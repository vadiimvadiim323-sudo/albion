#include "frame_channel/frame_channel.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <latch>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using framechan::Config;
using framechan::FrameChannel;
using framechan::State;
using framechan::Status;
using framechan::StopMode;
using framechan::Take;

namespace {

std::atomic<int> g_failures{0};

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            g_failures.fetch_add(1);                                             \
            std::fprintf(stderr, "  CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        }                                                                        \
    } while (false)

Config make_config(std::size_t frame, std::size_t queue, std::size_t readers = 1,
                   std::size_t writers = 1) {
    Config c;
    c.frame_capacity = frame;
    c.queue_capacity = queue;
    c.max_read_leases = readers;
    c.max_write_leases = writers;
    return c;
}

// Frame payload: 8-byte counter, then bytes derived from it.
void fill(std::span<std::byte> buffer, std::uint64_t counter) {
    std::memset(buffer.data(), static_cast<int>(counter & 0xff), buffer.size());
    std::memcpy(buffer.data(), &counter, sizeof(counter));
}

std::uint64_t counter_of(std::span<const std::byte> data) {
    std::uint64_t counter = 0;
    std::memcpy(&counter, data.data(), sizeof(counter));
    return counter;
}

bool intact(std::span<const std::byte> data) {
    const auto expected = static_cast<std::byte>(counter_of(data) & 0xff);
    for (std::size_t i = sizeof(std::uint64_t); i < data.size(); ++i) {
        if (data[i] != expected) {
            return false;
        }
    }
    return true;
}

Status publish_one(FrameChannel& ch, std::uint64_t counter) {
    auto [status, lease] = ch.acquire();
    if (status != Status::Ok) {
        return status;
    }
    fill(lease.buffer(), counter);
    return ch.publish(std::move(lease), lease.buffer().size());
}

bool balanced(const framechan::Stats& s) {
    return s.published ==
           s.delivered + s.dropped_overflow + s.dropped_skipped + s.dropped_on_stop + s.queued;
}

bool all_buffers_free(const FrameChannel& ch) {
    const auto& c = ch.config();
    return ch.stats().free_buffers == c.queue_capacity + c.max_read_leases + c.max_write_leases;
}

// --- single-threaded semantics ---

void test_roundtrip() {
    FrameChannel ch(make_config(256, 2));
    CHECK(ch.state() == State::Running);
    CHECK(ch.try_receive().status == Status::Empty);
    CHECK(ch.receive_for(5ms).status == Status::Timeout);

    auto [status, lease] = ch.acquire();
    CHECK(status == Status::Ok);
    CHECK(lease.buffer().size() == 256);
    CHECK(reinterpret_cast<std::uintptr_t>(lease.buffer().data()) % 64 == 0);
    fill(lease.buffer(), 7);
    CHECK(ch.publish(std::move(lease), 100) == Status::Ok);
    CHECK(!lease);

    auto r = ch.receive();
    CHECK(r.status == Status::Ok);
    CHECK(r.frame.data().size() == 100);
    CHECK(r.frame.info().sequence == 0);
    CHECK(r.frame.info().epoch == 1);
    CHECK(counter_of(r.frame.data()) == 7);
    r.frame.reset();
    CHECK(all_buffers_free(ch));
}

void test_overflow_drops_oldest() {
    FrameChannel ch(make_config(64, 3));
    for (std::uint64_t i = 0; i < 10; ++i) {
        CHECK(publish_one(ch, i) == Status::Ok);
    }
    auto s = ch.stats();
    CHECK(s.published == 10);
    CHECK(s.dropped_overflow == 7);
    CHECK(s.queued == 3);
    for (std::uint64_t expected = 7; expected < 10; ++expected) {
        auto r = ch.try_receive();
        CHECK(r.status == Status::Ok);
        CHECK(r.frame.info().sequence == expected);
        CHECK(counter_of(r.frame.data()) == expected);
    }
    CHECK(ch.try_receive().status == Status::Empty);
    CHECK(balanced(ch.stats()));
}

void test_take_newest() {
    FrameChannel ch(make_config(64, 4));
    for (std::uint64_t i = 0; i < 4; ++i) {
        CHECK(publish_one(ch, i) == Status::Ok);
    }
    auto r = ch.try_receive(Take::Newest);
    CHECK(r.status == Status::Ok);
    CHECK(r.frame.info().sequence == 3);
    CHECK(ch.stats().dropped_skipped == 3);
    CHECK(ch.stats().queued == 0);
    CHECK(balanced(ch.stats()));
}

void test_held_frame_is_never_overwritten() {
    FrameChannel ch(make_config(4096, 1, 2));
    CHECK(publish_one(ch, 100) == Status::Ok);
    auto first = ch.receive();
    CHECK(publish_one(ch, 101) == Status::Ok);
    auto second = ch.receive();
    CHECK(first.status == Status::Ok && second.status == Status::Ok);

    // Far more publishes than there are buffers in the pool.
    for (std::uint64_t i = 0; i < 1000; ++i) {
        CHECK(publish_one(ch, 1000 + i) == Status::Ok);
    }
    CHECK(counter_of(first.frame.data()) == 100 && intact(first.frame.data()));
    CHECK(counter_of(second.frame.data()) == 101 && intact(second.frame.data()));

    // The read-lease limit is enforced and leaves the queued frame in place.
    CHECK(ch.try_receive().status == Status::LeaseLimit);
    CHECK(ch.receive().status == Status::LeaseLimit);
    first.frame.reset();
    auto third = ch.try_receive();
    CHECK(third.status == Status::Ok);
    CHECK(counter_of(third.frame.data()) == 1999);
}

void test_write_lease_rules() {
    FrameChannel ch(make_config(64, 1));
    FrameChannel other(make_config(64, 1));

    auto a = ch.acquire();
    CHECK(a.status == Status::Ok);
    CHECK(ch.acquire().status == Status::LeaseLimit);

    // InvalidArgument keeps the lease usable.
    CHECK(ch.publish(std::move(a.lease), 65) == Status::InvalidArgument);
    CHECK(static_cast<bool>(a.lease));
    CHECK(other.publish(std::move(a.lease), 1) == Status::InvalidArgument);
    CHECK(static_cast<bool>(a.lease));
    framechan::WriteLease empty;
    CHECK(ch.publish(std::move(empty), 1) == Status::InvalidArgument);
    CHECK(ch.publish(std::move(a.lease), 64) == Status::Ok);

    // An abandoned lease returns its buffer.
    {
        auto b = ch.acquire();
        CHECK(b.status == Status::Ok);
    }
    CHECK(ch.stats().write_leases == 0);
    CHECK(ch.acquire().status == Status::Ok);
}

void test_invalid_config() {
    const auto throws_invalid = [](Config c) {
        try {
            FrameChannel ch(c);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    CHECK(throws_invalid(make_config(0, 1)));
    CHECK(throws_invalid(make_config(64, 0)));
    CHECK(throws_invalid(make_config(64, 1, 0)));
    CHECK(throws_invalid(make_config(64, 1, 1, 0)));

    bool length_error = false;
    try {
        FrameChannel ch(make_config(static_cast<std::size_t>(-1) / 2, 8));
    } catch (const std::length_error&) {
        length_error = true;
    }
    CHECK(length_error);
}

void test_stop_drain_and_discard() {
    {
        FrameChannel ch(make_config(64, 3));
        for (std::uint64_t i = 0; i < 3; ++i) {
            CHECK(publish_one(ch, i) == Status::Ok);
        }
        ch.stop(StopMode::Drain);
        CHECK(ch.state() == State::Draining);
        CHECK(publish_one(ch, 99) == Status::Stopped);
        for (std::uint64_t i = 0; i < 3; ++i) {
            auto r = ch.receive();
            CHECK(r.status == Status::Ok && r.frame.info().sequence == i);
        }
        CHECK(ch.state() == State::Stopped);
        CHECK(ch.receive().status == Status::Stopped);
        CHECK(ch.receive_for(1s).status == Status::Stopped);
    }
    {
        FrameChannel ch(make_config(64, 3));
        for (std::uint64_t i = 0; i < 3; ++i) {
            CHECK(publish_one(ch, i) == Status::Ok);
        }
        ch.stop(StopMode::Discard);
        ch.stop(StopMode::Drain);  // idempotent
        CHECK(ch.state() == State::Stopped);
        CHECK(ch.try_receive().status == Status::Stopped);
        CHECK(ch.stats().dropped_on_stop == 3);
        CHECK(balanced(ch.stats()));
        CHECK(all_buffers_free(ch));
    }
    {
        // Publishing a lease acquired before stop() is rejected and frees the buffer.
        FrameChannel ch(make_config(64, 1));
        auto a = ch.acquire();
        ch.stop();
        CHECK(ch.publish(std::move(a.lease), 1) == Status::Stopped);
        CHECK(!a.lease);
        CHECK(ch.stats().rejected_publish == 1);
        CHECK(all_buffers_free(ch));
    }
}

void test_restart() {
    FrameChannel ch(make_config(64, 2));
    CHECK(ch.restart() == Status::InvalidState);

    CHECK(publish_one(ch, 1) == Status::Ok);
    auto held = ch.receive();
    CHECK(publish_one(ch, 2) == Status::Ok);
    auto stale = ch.acquire();
    fill(stale.lease.buffer(), 3);

    ch.stop(StopMode::Drain);
    CHECK(ch.restart() == Status::Ok);
    CHECK(ch.state() == State::Running);
    CHECK(ch.try_receive().status == Status::LeaseLimit);  // `held` still counts

    // A frame held across the restart is intact; an old write lease is refused.
    CHECK(counter_of(held.frame.data()) == 1 && intact(held.frame.data()));
    CHECK(ch.publish(std::move(stale.lease), 64) == Status::Stale);
    held.frame.reset();
    CHECK(ch.try_receive().status == Status::Empty);  // frame 2 was dropped by restart

    CHECK(publish_one(ch, 4) == Status::Ok);
    auto r = ch.receive();
    CHECK(r.status == Status::Ok);
    CHECK(counter_of(r.frame.data()) == 4);
    CHECK(r.frame.info().epoch == 2);
    CHECK(r.frame.info().sequence == 2);  // sequence keeps counting
    r.frame.reset();
    CHECK(balanced(ch.stats()));
    CHECK(all_buffers_free(ch));
}

void test_fail() {
    FrameChannel ch(make_config(64, 2));
    CHECK(publish_one(ch, 1) == Status::Ok);
    const auto ec = std::make_error_code(std::errc::io_error);
    CHECK(ch.fail(ec));
    CHECK(!ch.fail(std::make_error_code(std::errc::timed_out)));  // first error wins
    ch.stop();                                                    // does not mask the failure
    CHECK(ch.state() == State::Failed);
    CHECK(ch.error() == ec);
    CHECK(ch.receive().status == Status::Failed);
    CHECK(ch.acquire().status == Status::Failed);

    CHECK(ch.restart() == Status::Ok);
    CHECK(!ch.error());
    CHECK(publish_one(ch, 2) == Status::Ok);
    CHECK(ch.receive().status == Status::Ok);
}

void test_leases_outlive_channel() {
    framechan::ReadLease read;
    framechan::WriteLease write;
    {
        FrameChannel ch(make_config(1024, 1));
        CHECK(publish_one(ch, 42) == Status::Ok);
        read = ch.receive().frame;
        write = ch.acquire().lease;
    }
    CHECK(counter_of(read.data()) == 42 && intact(read.data()));
    fill(write.buffer(), 1);  // still valid memory (checked under ASan)
}

// --- multi-threaded ---

void test_slow_consumer() {
    constexpr std::uint64_t kFrames = 20000;
    FrameChannel ch(make_config(4096, 2));
    std::uint64_t received = 0;

    std::thread consumer([&] {
        std::uint64_t last = 0;
        bool have_last = false;
        for (;;) {
            auto r = ch.receive();
            if (r.status != Status::Ok) {
                CHECK(r.status == Status::Stopped);
                return;
            }
            const std::uint64_t before = counter_of(r.frame.data());
            std::this_thread::sleep_for(200us);  // producer laps us many times here
            CHECK(counter_of(r.frame.data()) == before);
            CHECK(intact(r.frame.data()));
            CHECK(r.frame.info().sequence == before);
            CHECK(!have_last || before > last);
            last = before;
            have_last = true;
            ++received;
        }
    });

    // The producer must never be refused a buffer or made to wait.
    for (std::uint64_t i = 0; i < kFrames; ++i) {
        CHECK(publish_one(ch, i) == Status::Ok);
    }
    ch.stop(StopMode::Drain);
    consumer.join();

    const auto s = ch.stats();
    CHECK(s.published == kFrames);
    CHECK(s.delivered == received);
    CHECK(s.dropped_overflow > 0);
    CHECK(s.dropped_overflow + s.delivered == kFrames);
    CHECK(balanced(s));
    CHECK(all_buffers_free(ch));
}

void test_concurrent_stop() {
    for (int iter = 0; iter < 300; ++iter) {
        FrameChannel ch(make_config(128, 2));
        constexpr int kStoppers = 4;
        std::latch go(1);
        std::atomic<bool> consumer_done{false};

        std::thread consumer([&] {
            for (;;) {
                auto r = ch.receive();
                if (r.status != Status::Ok) {
                    CHECK(r.status == Status::Stopped);
                    break;
                }
                CHECK(intact(r.frame.data()));
            }
            consumer_done = true;
        });
        std::thread producer([&] {
            for (std::uint64_t i = 0;; ++i) {
                const Status st = publish_one(ch, i);
                if (st != Status::Ok) {
                    CHECK(st == Status::Stopped);
                    return;
                }
            }
        });
        std::vector<std::thread> stoppers;
        for (int i = 0; i < kStoppers; ++i) {
            stoppers.emplace_back([&, i] {
                go.wait();
                ch.stop((iter + i) % 2 == 0 ? StopMode::Drain : StopMode::Discard);
            });
        }
        if (iter % 3 == 0) {
            std::this_thread::sleep_for(100us);
        }
        go.count_down();
        for (auto& t : stoppers) {
            t.join();
        }
        producer.join();
        consumer.join();

        CHECK(consumer_done);
        CHECK(ch.state() == State::Stopped);
        CHECK(ch.acquire().status == Status::Stopped);
        CHECK(balanced(ch.stats()));
        CHECK(ch.stats().queued == 0);
        CHECK(all_buffers_free(ch));
    }
}

void test_stop_wakes_blocked_consumers() {
    FrameChannel ch(make_config(64, 1, 4));
    std::vector<std::thread> consumers;
    std::atomic<int> stopped{0};
    for (int i = 0; i < 4; ++i) {
        consumers.emplace_back([&] {
            if (ch.receive().status == Status::Stopped) {
                ++stopped;
            }
        });
    }
    std::this_thread::sleep_for(20ms);
    ch.stop();
    for (auto& t : consumers) {
        t.join();
    }
    CHECK(stopped == 4);
}

// A consumer blocked across stop()+restart() must not sleep through the stop.
// Whether the consumer is already blocked cannot be observed from outside, so
// a few late arrivals are tolerated (and rescued by a second stop).
void test_stop_restart_ends_pending_receive() {
    constexpr int kIterations = 50;
    int late = 0;
    for (int iter = 0; iter < kIterations; ++iter) {
        FrameChannel ch(make_config(64, 1));
        std::atomic<bool> done{false};
        Status result = Status::Ok;
        std::thread consumer([&] {
            result = ch.receive().status;
            done = true;
        });
        std::this_thread::sleep_for(2ms);
        ch.stop();
        CHECK(ch.restart() == Status::Ok);
        const auto deadline = std::chrono::steady_clock::now() + 500ms;
        while (!done && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(100us);
        }
        if (!done) {
            ++late;
            ch.stop();
        }
        consumer.join();
        CHECK(result == Status::Stopped);
    }
    CHECK(late <= kIterations / 10);
}

void test_restart_under_load() {
    FrameChannel ch(make_config(256, 2));
    std::atomic<bool> quit{false};

    std::thread consumer([&] {
        std::uint64_t last_seq = 0;
        bool have_last = false;
        while (!quit) {
            auto r = ch.receive_for(1ms);
            if (r.status != Status::Ok) {
                CHECK(r.status == Status::Stopped || r.status == Status::Failed ||
                      r.status == Status::Timeout);
                continue;
            }
            CHECK(intact(r.frame.data()));
            CHECK(!have_last || r.frame.info().sequence > last_seq);
            last_seq = r.frame.info().sequence;
            have_last = true;
        }
    });
    std::thread producer([&] {
        for (std::uint64_t i = 0; !quit; ++i) {
            const Status st = publish_one(ch, i);
            CHECK(st == Status::Ok || st == Status::Stopped || st == Status::Failed ||
                  st == Status::Stale);
        }
    });
    for (int i = 0; i < 500; ++i) {
        if (i % 5 == 0) {
            ch.fail(std::make_error_code(std::errc::io_error));
        } else {
            ch.stop(i % 2 == 0 ? StopMode::Drain : StopMode::Discard);
        }
        std::this_thread::sleep_for(20us);
        CHECK(ch.restart() == Status::Ok);
        std::this_thread::sleep_for(20us);
    }
    quit = true;
    ch.stop(StopMode::Discard);
    producer.join();
    consumer.join();
    CHECK(balanced(ch.stats()));
    CHECK(all_buffers_free(ch));
}

void test_multi_producer_multi_consumer() {
    constexpr int kProducers = 3;
    constexpr int kConsumers = 2;
    constexpr std::uint64_t kPerProducer = 5000;
    FrameChannel ch(make_config(512, 4, kConsumers, kProducers));
    std::atomic<std::uint64_t> received{0};

    std::vector<std::thread> consumers;
    for (int i = 0; i < kConsumers; ++i) {
        consumers.emplace_back([&] {
            for (;;) {
                auto r = ch.receive();
                if (r.status != Status::Ok) {
                    CHECK(r.status == Status::Stopped);
                    return;
                }
                CHECK(intact(r.frame.data()));
                ++received;
            }
        });
    }
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (std::uint64_t i = 0; i < kPerProducer; ++i) {
                CHECK(publish_one(ch, static_cast<std::uint64_t>(p) * 1000000 + i) == Status::Ok);
            }
        });
    }
    for (auto& t : producers) {
        t.join();
    }
    ch.stop(StopMode::Drain);
    for (auto& t : consumers) {
        t.join();
    }
    const auto s = ch.stats();
    CHECK(s.published == kProducers * kPerProducer);
    CHECK(s.delivered == received);
    CHECK(balanced(s));
    CHECK(all_buffers_free(ch));
}

struct TestCase {
    const char* name;
    void (*fn)();
};

}  // namespace

int main() {
    const TestCase tests[] = {
        {"roundtrip", test_roundtrip},
        {"overflow_drops_oldest", test_overflow_drops_oldest},
        {"take_newest", test_take_newest},
        {"held_frame_is_never_overwritten", test_held_frame_is_never_overwritten},
        {"write_lease_rules", test_write_lease_rules},
        {"invalid_config", test_invalid_config},
        {"stop_drain_and_discard", test_stop_drain_and_discard},
        {"restart", test_restart},
        {"fail", test_fail},
        {"leases_outlive_channel", test_leases_outlive_channel},
        {"slow_consumer", test_slow_consumer},
        {"concurrent_stop", test_concurrent_stop},
        {"stop_wakes_blocked_consumers", test_stop_wakes_blocked_consumers},
        {"stop_restart_ends_pending_receive", test_stop_restart_ends_pending_receive},
        {"restart_under_load", test_restart_under_load},
        {"multi_producer_multi_consumer", test_multi_producer_multi_consumer},
    };
    int failed_tests = 0;
    for (const auto& t : tests) {
        const int before = g_failures.load();
        t.fn();
        const bool ok = g_failures.load() == before;
        std::printf("[%s] %s\n", ok ? " OK " : "FAIL", t.name);
        std::fflush(stdout);
        failed_tests += ok ? 0 : 1;
    }
    std::printf("%d test(s) failed\n", failed_tests);
    return failed_tests == 0 ? 0 : 1;
}
