// Bounded-memory frame hand-off between producers and consumers with a
// freshness-first overflow policy (the oldest queued frame is dropped).
//
// All memory is allocated once, in the FrameChannel constructor. Every buffer
// is always in exactly one of four states:
//
//   Free -> Writing (WriteLease) -> Queued -> Reading (ReadLease) -> Free
//
// Only Queued buffers are ever evicted. A buffer held through a lease is never
// handed to anybody else until the lease is destroyed or reset.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <system_error>

namespace framechan {

enum class Status : std::uint8_t {
    Ok,
    Empty,            // try_receive: nothing queued
    Timeout,          // receive_for: deadline passed
    Stopped,          // channel stopped (or stopped and restarted while waiting)
    Failed,           // channel failed, see FrameChannel::error()
    Stale,            // lease was acquired before the last restart
    LeaseLimit,       // caller already holds the configured number of leases
    InvalidArgument,  // empty/foreign lease or size > frame_capacity
    InvalidState,     // restart() on a running channel
};

const char* to_string(Status s) noexcept;

enum class State : std::uint8_t {
    Running,
    Draining,  // stop(Drain): no new frames, queued frames still deliverable
    Stopped,
    Failed,
};

enum class StopMode : std::uint8_t {
    Drain,    // consumers may still receive what is queued
    Discard,  // queued frames are dropped immediately
};

enum class Take : std::uint8_t {
    Oldest,  // FIFO among the frames that survived overflow
    Newest,  // newest queued frame; older queued frames are dropped
};

struct Config {
    std::size_t frame_capacity = 0;    // bytes per buffer, > 0
    std::size_t queue_capacity = 1;    // max published-but-not-received frames, > 0
    std::size_t max_read_leases = 1;   // frames consumers may hold at once, > 0
    std::size_t max_write_leases = 1;  // buffers producers may hold at once, > 0
    // Touch every page in the constructor so the first frames do not pay for
    // page faults.
    bool prefault = true;
};

struct FrameInfo {
    std::uint64_t sequence = 0;  // monotonic over the channel lifetime, gaps = drops
    std::uint64_t epoch = 0;     // incremented by every restart()
    std::size_t size = 0;        // payload bytes
    std::chrono::steady_clock::time_point published_at{};
};

// Snapshot. Invariant:
//   published == delivered + dropped_overflow + dropped_skipped
//                + dropped_on_stop + queued
struct Stats {
    std::uint64_t published = 0;
    std::uint64_t delivered = 0;
    std::uint64_t dropped_overflow = 0;  // evicted by a newer publish
    std::uint64_t dropped_skipped = 0;   // skipped by Take::Newest
    std::uint64_t dropped_on_stop = 0;   // discarded by stop/fail/restart
    std::uint64_t rejected_publish = 0;  // publish() on a stopped channel or stale lease
    std::size_t queued = 0;
    std::size_t free_buffers = 0;
    std::size_t read_leases = 0;
    std::size_t write_leases = 0;
};

namespace detail {
struct Shared;
void release(Shared& shared, std::uint32_t slot) noexcept;
}  // namespace detail

class FrameChannel;

// Exclusive ownership of one buffer being filled. Move-only. Destroying an
// unpublished lease returns the buffer to the pool. A lease keeps the channel's
// memory alive, so it may outlive the FrameChannel object.
// A lease object is not thread-safe, but may be moved to another thread.
class WriteLease {
public:
    WriteLease() noexcept = default;
    WriteLease(WriteLease&&) noexcept = default;
    WriteLease& operator=(WriteLease&& other) noexcept;
    WriteLease(const WriteLease&) = delete;
    WriteLease& operator=(const WriteLease&) = delete;
    ~WriteLease() { reset(); }

    explicit operator bool() const noexcept { return static_cast<bool>(shared_); }
    // Whole buffer (frame_capacity bytes). Contents are unspecified: whatever
    // a previous frame left there.
    std::span<std::byte> buffer() const noexcept { return buffer_; }
    void reset() noexcept;

private:
    friend class FrameChannel;
    std::shared_ptr<detail::Shared> shared_;
    std::span<std::byte> buffer_{};
    std::uint32_t slot_ = 0;
};

// Exclusive read-only ownership of one received frame. The bytes do
// not change while the lease is alive, across stop() and restart() as well.
class ReadLease {
public:
    ReadLease() noexcept = default;
    ReadLease(ReadLease&&) noexcept = default;
    ReadLease& operator=(ReadLease&& other) noexcept;
    ReadLease(const ReadLease&) = delete;
    ReadLease& operator=(const ReadLease&) = delete;
    ~ReadLease() { reset(); }

    explicit operator bool() const noexcept { return static_cast<bool>(shared_); }
    std::span<const std::byte> data() const noexcept { return data_; }
    const FrameInfo& info() const noexcept { return info_; }
    void reset() noexcept;

private:
    friend class FrameChannel;
    std::shared_ptr<detail::Shared> shared_;
    std::span<const std::byte> data_{};
    FrameInfo info_{};
    std::uint32_t slot_ = 0;
};

struct AcquireResult {
    Status status = Status::Stopped;
    WriteLease lease;
};

struct ReceiveResult {
    Status status = Status::Stopped;
    ReadLease frame;
};

// Thread-safe for any number of producer and consumer threads. Methods are
// noexcept: the only thing that can fail inside is locking a mutex, which is
// treated as fatal.
class FrameChannel {
public:
    // Allocates (queue_capacity + max_read_leases + max_write_leases) buffers.
    // Throws std::invalid_argument, std::length_error or std::bad_alloc.
    explicit FrameChannel(const Config& config);
    // Stops the channel (Discard). Outstanding leases stay valid.
    ~FrameChannel();

    FrameChannel(const FrameChannel&) = delete;
    FrameChannel& operator=(const FrameChannel&) = delete;

    // --- producer side: never blocks ---

    // Ok, Stopped, Failed or LeaseLimit. Because of the pool sizing, a free
    // buffer always exists while the write-lease limit is respected.
    AcquireResult acquire() noexcept;

    // Publishes the first `size` bytes. If the queue is full, the oldest queued
    // frame is dropped. On InvalidArgument the lease is left untouched so the
    // caller can retry; on every other status it is consumed (the buffer goes
    // to the queue on Ok, back to the pool on Stopped/Failed/Stale).
    Status publish(WriteLease&& lease, std::size_t size) noexcept;

    // --- consumer side ---

    // Ok, Empty, Stopped, Failed or LeaseLimit.
    ReceiveResult try_receive(Take take = Take::Oldest) noexcept;
    // Blocks until a frame, stop or failure. Returns LeaseLimit immediately
    // instead of waiting when the caller side holds max_read_leases frames.
    ReceiveResult receive(Take take = Take::Oldest) noexcept;
    // As receive(), plus Timeout.
    ReceiveResult receive_for(std::chrono::nanoseconds timeout,
                              Take take = Take::Oldest) noexcept;

    // --- lifecycle: callable from any thread, idempotent ---

    // Wakes all blocked consumers. Later publishes are rejected.
    void stop(StopMode mode = StopMode::Drain) noexcept;
    // Like stop(Discard), but consumers get Status::Failed and error() returns
    // `ec`. Returns false if the channel had already failed (first error wins).
    bool fail(std::error_code ec) noexcept;
    // Stopped/Draining/Failed -> Running. Drops queued frames, clears the
    // error, invalidates write leases acquired earlier (publish -> Stale).
    // Consumers that were blocked across stop+restart return Stopped.
    Status restart() noexcept;

    State state() const noexcept;
    std::error_code error() const noexcept;
    Stats stats() const noexcept;
    const Config& config() const noexcept;
    // Total bytes of frame storage owned by the channel.
    std::size_t memory_bytes() const noexcept;

private:
    std::shared_ptr<detail::Shared> shared_;
};

}  // namespace framechan
