// Reproducible benchmark: hand-off latency, frame age seen by a slow consumer,
// producer cost per frame and heap allocations per frame.
//
// Compares FrameChannel with a deliberately simple baseline that has the same
// drop-oldest policy but allocates a std::vector per frame.
//
//   frame_channel_bench [--frames N] [--bytes N] [--period-us N]
//                       [--slow-work-us N] [--runs N]
//
// Exit code is non-zero if FrameChannel allocated in steady state.
#include "frame_channel/frame_channel.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

// --- allocation counting (whole process) ---

namespace {
std::atomic<std::uint64_t> g_allocs{0};
std::atomic<std::uint64_t> g_alloc_bytes{0};

void* counted_alloc(std::size_t size, std::size_t align) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    g_alloc_bytes.fetch_add(size, std::memory_order_relaxed);
    if (size == 0) {
        size = 1;
    }
    void* p = nullptr;
    if (align <= alignof(std::max_align_t)) {
        p = std::malloc(size);
    } else if (posix_memalign(&p, align, size) != 0) {
        p = nullptr;
    }
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}
}  // namespace

void* operator new(std::size_t size) { return counted_alloc(size, 1); }
void* operator new[](std::size_t size) { return counted_alloc(size, 1); }
void* operator new(std::size_t size, std::align_val_t a) {
    return counted_alloc(size, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t size, std::align_val_t a) {
    return counted_alloc(size, static_cast<std::size_t>(a));
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::microseconds;
using std::chrono::nanoseconds;

struct Params {
    std::size_t frames = 2000;
    std::size_t bytes = 1280 * 720;  // one 8-bit 720p plane
    long period_us = 1000;
    long slow_work_us = 3500;
    int runs = 5;
};

struct Samples {
    std::vector<std::int64_t> latency_ns;   // publish -> receive
    std::vector<std::int64_t> producer_ns;  // get buffer + fill + publish
    std::uint64_t allocs = 0;
    std::uint64_t alloc_bytes = 0;
    std::uint64_t published = 0;
    std::uint64_t delivered = 0;
    std::uint64_t checksum = 0;
};

// Baseline: same policy (bounded, drop oldest, take newest), one heap
// allocation per frame, buffer ownership moved through the queue.
class VectorQueue {
public:
    struct Item {
        std::vector<std::byte> data;
        Clock::time_point published_at;
    };

    explicit VectorQueue(std::size_t capacity) : capacity_(capacity) {}

    void push(std::vector<std::byte>&& data) {
        std::vector<std::byte> evicted;
        {
            std::lock_guard lock(mu_);
            if (queue_.size() == capacity_) {
                evicted = std::move(queue_.front().data);
                queue_.pop_front();
            }
            queue_.push_back({std::move(data), Clock::now()});
        }
        cv_.notify_one();
    }

    bool pop_newest(Item& out) {
        std::unique_lock lock(mu_);
        cv_.wait(lock, [&] { return !queue_.empty() || closed_; });
        if (queue_.empty()) {
            return false;
        }
        out = std::move(queue_.back());
        queue_.clear();
        return true;
    }

    void close() {
        {
            std::lock_guard lock(mu_);
            closed_ = true;
        }
        cv_.notify_all();
    }

private:
    std::size_t capacity_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Item> queue_;
    bool closed_ = false;
};

// Reads one byte per page so the consumer really touches the frame.
std::uint64_t touch(std::span<const std::byte> data) {
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < data.size(); i += 4096) {
        sum += static_cast<std::uint64_t>(data[i]);
    }
    return sum;
}

void spin_for(long us) {
    const auto end = Clock::now() + microseconds(us);
    while (Clock::now() < end) {
    }
}

std::int64_t ns_between(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration_cast<nanoseconds>(b - a).count();
}

template <typename ProduceOne, typename ConsumeLoop, typename Close>
void run_threads(const Params& p, Samples& out, ProduceOne produce_one, ConsumeLoop consume_loop,
                 Close close) {
    std::vector<std::int64_t> latency;
    std::vector<std::int64_t> producer_cost;
    latency.reserve(p.frames);
    producer_cost.reserve(p.frames);
    std::atomic<bool> go{false};
    std::uint64_t delivered = 0;
    std::uint64_t checksum = 0;

    std::thread consumer([&] {
        while (!go.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        consume_loop(latency, delivered, checksum);
    });
    std::thread producer([&] {
        while (!go.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        const auto start = Clock::now();
        for (std::size_t i = 0; i < p.frames; ++i) {
            std::this_thread::sleep_until(start + microseconds(p.period_us) * i);
            const auto t0 = Clock::now();
            produce_one(i);
            producer_cost.push_back(ns_between(t0, Clock::now()));
        }
        close();
    });

    // Count only the steady state: everything above is already constructed.
    const std::uint64_t allocs0 = g_allocs.load();
    const std::uint64_t bytes0 = g_alloc_bytes.load();
    go.store(true, std::memory_order_release);
    producer.join();
    consumer.join();
    out.allocs += g_allocs.load() - allocs0;
    out.alloc_bytes += g_alloc_bytes.load() - bytes0;

    out.published += p.frames;
    out.delivered += delivered;
    out.checksum += checksum;
    out.latency_ns.insert(out.latency_ns.end(), latency.begin(), latency.end());
    out.producer_ns.insert(out.producer_ns.end(), producer_cost.begin(), producer_cost.end());
}

void run_channel(const Params& p, long work_us, Samples& out) {
    framechan::Config config;
    config.frame_capacity = p.bytes;
    config.queue_capacity = 1;
    framechan::FrameChannel channel(config);

    run_threads(
        p, out,
        [&](std::size_t i) {
            auto [status, lease] = channel.acquire();
            if (status != framechan::Status::Ok) {
                std::abort();
            }
            std::memset(lease.buffer().data(), static_cast<int>(i & 0xff), p.bytes);
            if (channel.publish(std::move(lease), p.bytes) != framechan::Status::Ok) {
                std::abort();
            }
        },
        [&](std::vector<std::int64_t>& latency, std::uint64_t& delivered, std::uint64_t& sum) {
            for (;;) {
                auto [status, frame] = channel.receive(framechan::Take::Newest);
                if (status != framechan::Status::Ok) {
                    return;
                }
                latency.push_back(ns_between(frame.info().published_at, Clock::now()));
                sum += touch(frame.data());
                ++delivered;
                spin_for(work_us);
            }
        },
        [&] { channel.stop(framechan::StopMode::Drain); });
}

void run_baseline(const Params& p, long work_us, Samples& out) {
    VectorQueue queue(1);

    run_threads(
        p, out,
        [&](std::size_t i) {
            std::vector<std::byte> data(p.bytes);
            std::memset(data.data(), static_cast<int>(i & 0xff), p.bytes);
            queue.push(std::move(data));
        },
        [&](std::vector<std::int64_t>& latency, std::uint64_t& delivered, std::uint64_t& sum) {
            VectorQueue::Item item;
            while (queue.pop_newest(item)) {
                latency.push_back(ns_between(item.published_at, Clock::now()));
                sum += touch(item.data);
                ++delivered;
                spin_for(work_us);
            }
        },
        [&] { queue.close(); });
}

double percentile_us(std::vector<std::int64_t>& v, double q) {
    if (v.empty()) {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    const auto idx = static_cast<std::size_t>(q * static_cast<double>(v.size() - 1));
    return static_cast<double>(v[idx]) / 1000.0;
}

void report(const char* scenario, const char* impl, Samples& s) {
    const double frames = static_cast<double>(s.published);
    std::printf("%-14s %-13s %9.1f %9.1f %9.1f %9.1f | %8.1f %8.1f | %9.3f %11.0f | %5.1f%%\n",
                scenario, impl, percentile_us(s.latency_ns, 0.50),
                percentile_us(s.latency_ns, 0.90), percentile_us(s.latency_ns, 0.99),
                percentile_us(s.latency_ns, 1.0), percentile_us(s.producer_ns, 0.50),
                percentile_us(s.producer_ns, 0.99), static_cast<double>(s.allocs) / frames,
                static_cast<double>(s.alloc_bytes) / frames,
                100.0 * static_cast<double>(s.published - s.delivered) / frames);
}

bool parse(int argc, char** argv, Params& p) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (i + 1 >= argc) {
            return false;
        }
        const long long value = std::atoll(argv[++i]);
        if (value <= 0) {
            return false;
        }
        if (arg == "--frames") {
            p.frames = static_cast<std::size_t>(value);
        } else if (arg == "--bytes") {
            p.bytes = static_cast<std::size_t>(value);
        } else if (arg == "--period-us") {
            p.period_us = static_cast<long>(value);
        } else if (arg == "--slow-work-us") {
            p.slow_work_us = static_cast<long>(value);
        } else if (arg == "--runs") {
            p.runs = static_cast<int>(value);
        } else {
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Params p;
    if (!parse(argc, argv, p)) {
        std::fprintf(stderr,
                     "usage: %s [--frames N] [--bytes N] [--period-us N] [--slow-work-us N] "
                     "[--runs N]\n",
                     argv[0]);
        return 2;
    }
    std::printf("frames=%zu bytes=%zu period_us=%ld slow_work_us=%ld runs=%d hw_threads=%u\n",
                p.frames, p.bytes, p.period_us, p.slow_work_us, p.runs,
                std::thread::hardware_concurrency());
    std::printf("latency = publish -> receive (for the slow consumer: age of the frame it gets)\n");
    std::printf("producer = get buffer + fill + publish; allocations are steady state only\n\n");
    std::printf("%-14s %-13s %9s %9s %9s %9s | %8s %8s | %9s %11s | %6s\n", "scenario", "impl",
                "lat p50", "lat p90", "lat p99", "lat max", "prod p50", "prod p99", "alloc/frm",
                "bytes/frm", "drops");
    std::printf("%-14s %-13s %9s %9s %9s %9s | %8s %8s | %9s %11s | %6s\n", "", "", "us", "us",
                "us", "us", "us", "us", "", "", "");

    Samples fast_channel, fast_baseline, slow_channel, slow_baseline;
    // Interleave implementations so drift of the machine hits both equally.
    for (int run = 0; run < p.runs; ++run) {
        run_channel(p, 0, fast_channel);
        run_baseline(p, 0, fast_baseline);
        run_channel(p, p.slow_work_us, slow_channel);
        run_baseline(p, p.slow_work_us, slow_baseline);
    }
    report("fast consumer", "FrameChannel", fast_channel);
    report("fast consumer", "vector queue", fast_baseline);
    report("slow consumer", "FrameChannel", slow_channel);
    report("slow consumer", "vector queue", slow_baseline);
    std::printf("\nchecksum=%llu\n",
                static_cast<unsigned long long>(fast_channel.checksum + fast_baseline.checksum +
                                                slow_channel.checksum + slow_baseline.checksum));

    const std::uint64_t channel_allocs = fast_channel.allocs + slow_channel.allocs;
    if (channel_allocs != 0) {
        std::fprintf(stderr, "FAIL: FrameChannel allocated %llu times in steady state\n",
                     static_cast<unsigned long long>(channel_allocs));
        return 1;
    }
    std::printf("FrameChannel steady-state allocations: 0\n");
    return 0;
}
