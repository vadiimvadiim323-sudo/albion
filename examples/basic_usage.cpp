// A camera-like producer at ~200 fps and a consumer that needs ~12 ms per
// frame. The consumer always works on the freshest frame; the producer never
// waits for it.
#include "frame_channel/frame_channel.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <system_error>
#include <thread>

using namespace std::chrono_literals;
using framechan::Status;

namespace {

void produce(framechan::FrameChannel& channel, int frames) {
    for (int i = 0; i < frames; ++i) {
        auto [status, lease] = channel.acquire();
        if (status != Status::Ok) {
            std::printf("producer: acquire -> %s, leaving\n", framechan::to_string(status));
            return;
        }
        // Fill the buffer in place: no copy between producer and consumer.
        auto buffer = lease.buffer();
        std::memset(buffer.data(), i & 0xff, buffer.size());

        status = channel.publish(std::move(lease), buffer.size());
        if (status != Status::Ok) {
            std::printf("producer: publish -> %s, leaving\n", framechan::to_string(status));
            return;
        }
        std::this_thread::sleep_for(5ms);
    }
}

void consume(framechan::FrameChannel& channel) {
    for (;;) {
        auto [status, frame] = channel.receive(framechan::Take::Newest);
        if (status == Status::Failed) {
            std::printf("consumer: failed: %s\n", channel.error().message().c_str());
            return;
        }
        if (status != Status::Ok) {
            std::printf("consumer: %s\n", framechan::to_string(status));
            return;
        }
        // `frame` owns the buffer until it goes out of scope; the producer
        // cannot overwrite it however many frames it publishes meanwhile.
        const auto age = std::chrono::steady_clock::now() - frame.info().published_at;
        std::printf("consumer: seq=%llu size=%zu age=%lld us\n",
                    static_cast<unsigned long long>(frame.info().sequence), frame.data().size(),
                    static_cast<long long>(
                        std::chrono::duration_cast<std::chrono::microseconds>(age).count()));
        std::this_thread::sleep_for(12ms);
    }
}

void print_stats(const framechan::FrameChannel& channel) {
    const auto s = channel.stats();
    std::printf("stats: published=%llu delivered=%llu dropped(overflow=%llu skipped=%llu "
                "stop=%llu) rejected=%llu\n",
                static_cast<unsigned long long>(s.published),
                static_cast<unsigned long long>(s.delivered),
                static_cast<unsigned long long>(s.dropped_overflow),
                static_cast<unsigned long long>(s.dropped_skipped),
                static_cast<unsigned long long>(s.dropped_on_stop),
                static_cast<unsigned long long>(s.rejected_publish));
}

}  // namespace

int main() {
    framechan::Config config;
    config.frame_capacity = 640 * 480;
    config.queue_capacity = 2;
    framechan::FrameChannel channel(config);
    std::printf("channel owns %zu bytes\n", channel.memory_bytes());

    // 1. Normal run, then a graceful stop: the consumer drains what is queued.
    {
        std::thread consumer(consume, std::ref(channel));
        std::thread producer(produce, std::ref(channel), 40);
        producer.join();
        channel.stop(framechan::StopMode::Drain);
        consumer.join();
        print_stats(channel);
    }

    // 2. Restart the same channel (same memory), then report a producer error.
    if (channel.restart() != Status::Ok) {
        return 1;
    }
    {
        std::thread consumer(consume, std::ref(channel));
        std::thread producer(produce, std::ref(channel), 10);
        producer.join();
        channel.fail(std::make_error_code(std::errc::io_error));
        consumer.join();
        print_stats(channel);
    }
    return 0;
}
