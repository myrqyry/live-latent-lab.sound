// Concurrent SPSC regression using real threads and exact sample sequences.
#include "framework/ring-buffer.hpp"
#include <atomic>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <thread>

void boundaries() {
    stts::RingBuffer ring(3);
    const float first[] = {1, 2, 3}, second[] = {4, 5, 6, 7};
    float out[8] = {};
    assert(ring.capacity() >= 3);
    assert(ring.write(first, 3) == 3);
    assert(ring.write(second, 4) == 0); // Full: no overwrite of unread data
    assert(ring.read(out, 2) == 2 && out[0] == 1 && out[1] == 2);
    assert(ring.write(second, 4) == 2);
    assert(ring.read(out, 8) == 3);
    assert(out[0] == 3 && out[1] == 4 && out[2] == 5);
    assert(ring.read(out, 1) == 0);
    ring.reset(); // Both actors are stopped
    assert(ring.available() == 0 && ring.write(first, 3) == 3);
    assert(ring.read(out, 3) == 3 && out[0] == 1);
    assert(ring.write(nullptr, 3) == 0 && ring.read(nullptr, 3) == 0);
}

void threaded() {
    constexpr size_t N = 250000;
    stts::RingBuffer ring(31);
    std::atomic<bool> done{false}, ok{true};
    std::atomic<size_t> count{0};
    std::thread consumer([&] {
        size_t expected = 1;
        float samples[13];
        while (expected <= N) {
            size_t n = ring.read(samples, 13);
            if (!n) {
                if (done.load(std::memory_order_acquire) && ring.available() == 0) break;
                std::this_thread::yield();
                continue;
            }
            for (size_t i = 0; i < n; ++i)
                if (samples[i] != float(expected++)) ok.store(false);
            count.fetch_add(n);
        }
        if (expected != N + 1) ok.store(false);
    });
    std::thread producer([&] {
        for (size_t next = 1; next <= N;) {
            float samples[7];
            size_t n = std::min<size_t>(7, N - next + 1);
            for (size_t i = 0; i < n; ++i) samples[i] = float(next + i);
            size_t written = ring.write(samples, n);
            next += written;
            if (!written) std::this_thread::yield();
        }
        done.store(true, std::memory_order_release);
    });
    producer.join();
    consumer.join();
    assert(ok.load() && count.load() == N && ring.available() == 0);
}

int main() {
    boundaries();
    threaded();
    std::puts("RING_BUFFER_SPSC_PASS=1");
}
