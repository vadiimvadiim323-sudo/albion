#include "frame_channel/frame_channel.hpp"

#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace framechan {

namespace {
constexpr std::size_t kAlign = 64;  // cache line; also enough for common SIMD
}

const char* to_string(Status s) noexcept {
    switch (s) {
        case Status::Ok: return "Ok";
        case Status::Empty: return "Empty";
        case Status::Timeout: return "Timeout";
        case Status::Stopped: return "Stopped";
        case Status::Failed: return "Failed";
        case Status::Stale: return "Stale";
        case Status::LeaseLimit: return "LeaseLimit";
        case Status::InvalidArgument: return "InvalidArgument";
        case Status::InvalidState: return "InvalidState";
    }
    return "Unknown";
}

namespace detail {

struct Shared {
    enum class SlotState : std::uint8_t { Free, Writing, Queued, Reading };
    struct Slot {
        FrameInfo info{};
        SlotState state = SlotState::Free;
    };

    explicit Shared(const Config& c) : cfg(c) {
        if (c.frame_capacity == 0 || c.queue_capacity == 0 || c.max_read_leases == 0 ||
            c.max_write_leases == 0) {
            throw std::invalid_argument("framechan::Config: all sizes must be > 0");
        }
        constexpr std::size_t max = std::numeric_limits<std::size_t>::max();
        constexpr std::size_t max_slots = std::numeric_limits<std::uint32_t>::max();
        if (c.queue_capacity > max_slots || c.max_read_leases > max_slots ||
            c.max_write_leases > max_slots ||
            c.queue_capacity + c.max_read_leases + c.max_write_leases > max_slots) {
            throw std::length_error("framechan::Config: too many buffers");
        }
        pool = c.queue_capacity + c.max_read_leases + c.max_write_leases;
        if (c.frame_capacity > max - (kAlign - 1)) {
            throw std::length_error("framechan::Config: frame_capacity too large");
        }
        stride = (c.frame_capacity + kAlign - 1) / kAlign * kAlign;
        if (stride > max / pool) {
            throw std::length_error("framechan::Config: total size overflows");
        }

        slots.resize(pool);
        ring.resize(c.queue_capacity);
        free_slots.resize(pool);
        for (std::size_t i = 0; i < pool; ++i) {
            free_slots[i] = static_cast<std::uint32_t>(i);
        }
        free_count = pool;

        slab = static_cast<std::byte*>(::operator new(stride * pool, std::align_val_t{kAlign}));
        if (c.prefault) {
            std::memset(slab, 0, stride * pool);
        }
    }

    ~Shared() { ::operator delete(slab, std::align_val_t{kAlign}); }

    Shared(const Shared&) = delete;
    Shared& operator=(const Shared&) = delete;

    std::byte* data(std::uint32_t slot) const noexcept { return slab + stride * slot; }

    // Everything below requires `mu` to be held.

    void to_free(std::uint32_t slot) noexcept {
        slots[slot].state = SlotState::Free;
        free_slots[free_count++] = slot;
    }

    std::uint32_t pop_oldest() noexcept {
        const std::uint32_t slot = ring[head];
        head = (head + 1) % ring.size();
        --queued;
        return slot;
    }

    void discard_queue(std::uint64_t& counter) noexcept {
        while (queued > 0) {
            to_free(pop_oldest());
            ++counter;
        }
    }

    Status terminal_status() const noexcept {
        return state == State::Failed ? Status::Failed : Status::Stopped;
    }

    // Requires queued > 0 and reading < max_read_leases.
    std::uint32_t take(Take mode) noexcept {
        if (mode == Take::Newest) {
            while (queued > 1) {
                to_free(pop_oldest());
                ++stats.dropped_skipped;
            }
        }
        const std::uint32_t slot = pop_oldest();
        slots[slot].state = SlotState::Reading;
        ++reading;
        ++stats.delivered;
        if (queued == 0 && state == State::Draining) {
            state = State::Stopped;
        }
        return slot;
    }

    const Config cfg;
    std::size_t stride = 0;
    std::size_t pool = 0;
    std::byte* slab = nullptr;

    mutable std::mutex mu;
    std::condition_variable cv;

    State state = State::Running;
    std::uint64_t epoch = 1;
    std::uint64_t next_sequence = 0;
    std::error_code error;

    std::vector<Slot> slots;
    std::vector<std::uint32_t> free_slots;  // stack
    std::size_t free_count = 0;
    std::vector<std::uint32_t> ring;  // queued slots, oldest at `head`
    std::size_t head = 0;
    std::size_t queued = 0;
    std::size_t writing = 0;
    std::size_t reading = 0;
    Stats stats;
};

void release(Shared& s, std::uint32_t slot) noexcept {
    std::lock_guard lock(s.mu);
    if (s.slots[slot].state == Shared::SlotState::Writing) {
        --s.writing;
    } else {
        --s.reading;
    }
    s.to_free(slot);
}

}  // namespace detail

using detail::Shared;

// --- leases ---

void WriteLease::reset() noexcept {
    if (shared_) {
        detail::release(*shared_, slot_);
        shared_.reset();
        buffer_ = {};
    }
}

WriteLease& WriteLease::operator=(WriteLease&& other) noexcept {
    if (this != &other) {
        reset();
        shared_ = std::move(other.shared_);
        buffer_ = std::exchange(other.buffer_, {});
        slot_ = other.slot_;
    }
    return *this;
}

void ReadLease::reset() noexcept {
    if (shared_) {
        detail::release(*shared_, slot_);
        shared_.reset();
        data_ = {};
    }
}

ReadLease& ReadLease::operator=(ReadLease&& other) noexcept {
    if (this != &other) {
        reset();
        shared_ = std::move(other.shared_);
        data_ = std::exchange(other.data_, {});
        info_ = other.info_;
        slot_ = other.slot_;
    }
    return *this;
}

// --- channel ---

FrameChannel::FrameChannel(const Config& config) : shared_(std::make_shared<Shared>(config)) {}

FrameChannel::~FrameChannel() { stop(StopMode::Discard); }

AcquireResult FrameChannel::acquire() noexcept {
    Shared& s = *shared_;
    AcquireResult result;
    {
        std::lock_guard lock(s.mu);
        if (s.state != State::Running) {
            result.status = s.terminal_status();
            return result;
        }
        if (s.writing == s.cfg.max_write_leases) {
            result.status = Status::LeaseLimit;
            return result;
        }
        // pool = queue + readers + writers, so with writing < max_write_leases
        // at least one buffer is free.
        const std::uint32_t slot = s.free_slots[--s.free_count];
        s.slots[slot].state = Shared::SlotState::Writing;
        s.slots[slot].info.epoch = s.epoch;
        ++s.writing;
        result.lease.slot_ = slot;
    }
    result.lease.buffer_ = {s.data(result.lease.slot_), s.cfg.frame_capacity};
    result.lease.shared_ = shared_;
    result.status = Status::Ok;
    return result;
}

Status FrameChannel::publish(WriteLease&& lease, std::size_t size) noexcept {
    Shared& s = *shared_;
    if (lease.shared_ != shared_ || size > s.cfg.frame_capacity) {
        return Status::InvalidArgument;
    }
    const std::uint32_t slot = lease.slot_;
    Status status = Status::Ok;
    {
        std::lock_guard lock(s.mu);
        --s.writing;
        if (s.state != State::Running) {
            status = s.terminal_status();
        } else if (s.slots[slot].info.epoch != s.epoch) {
            status = Status::Stale;
        }
        if (status != Status::Ok) {
            s.to_free(slot);
            ++s.stats.rejected_publish;
        } else {
            if (s.queued == s.ring.size()) {
                s.to_free(s.pop_oldest());
                ++s.stats.dropped_overflow;
            }
            Shared::Slot& meta = s.slots[slot];
            meta.state = Shared::SlotState::Queued;
            meta.info.sequence = s.next_sequence++;
            meta.info.size = size;
            meta.info.published_at = std::chrono::steady_clock::now();
            s.ring[(s.head + s.queued) % s.ring.size()] = slot;
            ++s.queued;
            ++s.stats.published;
        }
    }
    // The buffer changed owner above; detach without releasing.
    lease.shared_.reset();
    lease.buffer_ = {};
    if (status == Status::Ok) {
        s.cv.notify_one();
    }
    return status;
}

ReceiveResult FrameChannel::try_receive(Take take) noexcept {
    return receive_for(std::chrono::nanoseconds{-1}, take);
}

ReceiveResult FrameChannel::receive(Take take) noexcept {
    return receive_for(std::chrono::nanoseconds::max(), take);
}

// timeout < 0: do not wait; nanoseconds::max(): wait without deadline.
ReceiveResult FrameChannel::receive_for(std::chrono::nanoseconds timeout, Take take) noexcept {
    using clock = std::chrono::steady_clock;
    Shared& s = *shared_;
    ReceiveResult result;
    {
        std::unique_lock lock(s.mu);
        if (s.reading == s.cfg.max_read_leases) {
            result.status = Status::LeaseLimit;
            return result;
        }
        if (s.queued == 0 && s.state == State::Running && timeout.count() >= 0) {
            // A stop() followed by restart() must still end this wait, hence
            // the epoch in the predicate.
            const std::uint64_t epoch = s.epoch;
            const auto ready = [&] {
                return s.queued > 0 || s.state != State::Running || s.epoch != epoch;
            };
            const auto now = clock::now();
            if (timeout == std::chrono::nanoseconds::max() ||
                timeout > clock::time_point::max() - now) {
                s.cv.wait(lock, ready);
            } else if (!s.cv.wait_until(lock, now + timeout, ready)) {
                result.status = Status::Timeout;
                return result;
            }
            if (s.epoch != epoch) {
                result.status = Status::Stopped;
                return result;
            }
        }
        if (s.queued == 0) {
            result.status = s.state == State::Running ? Status::Empty : s.terminal_status();
            return result;
        }
        // Another consumer may have taken the last lease while we waited.
        if (s.reading == s.cfg.max_read_leases) {
            result.status = Status::LeaseLimit;
            return result;
        }
        const std::uint32_t slot = s.take(take);
        result.frame.slot_ = slot;
        result.frame.info_ = s.slots[slot].info;
    }
    result.frame.data_ = {s.data(result.frame.slot_), result.frame.info_.size};
    result.frame.shared_ = shared_;
    result.status = Status::Ok;
    return result;
}

void FrameChannel::stop(StopMode mode) noexcept {
    Shared& s = *shared_;
    {
        std::lock_guard lock(s.mu);
        if (s.state == State::Running) {
            s.state = State::Draining;
        }
        if (s.state == State::Draining) {
            if (mode == StopMode::Discard) {
                s.discard_queue(s.stats.dropped_on_stop);
            }
            if (s.queued == 0) {
                s.state = State::Stopped;
            }
        }
    }
    s.cv.notify_all();
}

bool FrameChannel::fail(std::error_code ec) noexcept {
    Shared& s = *shared_;
    {
        std::lock_guard lock(s.mu);
        if (s.state == State::Failed) {
            return false;
        }
        s.state = State::Failed;
        s.error = ec;
        s.discard_queue(s.stats.dropped_on_stop);
    }
    s.cv.notify_all();
    return true;
}

Status FrameChannel::restart() noexcept {
    Shared& s = *shared_;
    {
        std::lock_guard lock(s.mu);
        if (s.state == State::Running) {
            return Status::InvalidState;
        }
        s.discard_queue(s.stats.dropped_on_stop);
        s.error.clear();
        ++s.epoch;
        s.state = State::Running;
    }
    // Waiters from the previous epoch must observe the epoch change.
    s.cv.notify_all();
    return Status::Ok;
}

State FrameChannel::state() const noexcept {
    std::lock_guard lock(shared_->mu);
    return shared_->state;
}

std::error_code FrameChannel::error() const noexcept {
    std::lock_guard lock(shared_->mu);
    return shared_->error;
}

Stats FrameChannel::stats() const noexcept {
    const Shared& s = *shared_;
    std::lock_guard lock(s.mu);
    Stats out = s.stats;
    out.queued = s.queued;
    out.free_buffers = s.free_count;
    out.read_leases = s.reading;
    out.write_leases = s.writing;
    return out;
}

const Config& FrameChannel::config() const noexcept { return shared_->cfg; }

std::size_t FrameChannel::memory_bytes() const noexcept {
    return shared_->stride * shared_->pool;
}

}  // namespace framechan
