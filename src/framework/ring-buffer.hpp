/**
 * @file ring-buffer.hpp
 * @brief Bounded lock-free single-producer/single-consumer sample queue.
 *
 * The producer exclusively owns head_; the consumer exclusively owns tail_.
 * A full queue drops NEW samples (write() returns the accepted count).
 *
 * Never advance tail_ from the producer: doing so races with an in-flight read,
 * and can overwrite samples while the consumer is copying them.
 * A separate, explicit latency policy belongs at each audio-pipeline caller.
 *
 * reset() is only safe while both producer and consumer are stopped.
 */
#pragma once

#include <vector>
#include <atomic>
#include <algorithm>
#include <cstddef>
#include <cstring>

namespace stts {

namespace detail {

inline size_t round_up_pow2(size_t n)
{
    if (n <= 1) return 1;
    --n;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
#if __SIZE_MAX__ > 0xFFFFFFFFULL
    n |= n >> 32;
#endif
    return ++n;
}

} // namespace detail

class RingBuffer {
public:
    explicit RingBuffer(size_t requested_samples)
        : buffer_(detail::round_up_pow2(std::max<size_t>(requested_samples, 1) + 1)),
          mask_(buffer_.size() - 1),
          usable_(mask_)
    {}

    // Non-blocking, no allocations. Drops the newest samples when full.
    // Only the producer may call this method.
    size_t write(const float* data, size_t samples)
    {
        if (!data || samples == 0) return 0;

        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_acquire);
        const size_t occupied = head - tail;
        const size_t count = std::min(samples, occupied < usable_ ? usable_ - occupied : size_t(0));

        if (!count) return 0;
        const size_t offset = head & mask_;
        if (count == 1) {
            buffer_[offset] = *data;
        } else {
            const size_t first = std::min(count, buffer_.size() - offset);
            std::memcpy(buffer_.data() + offset, data, first * sizeof(float));
            if (first < count)
                std::memcpy(buffer_.data(), data + first, (count - first) * sizeof(float));
        }

        head_.store(head + count, std::memory_order_release);
        return count;
    }

    // Only the consumer may call this method.
    size_t read(float* output, size_t samples)
    {
        if (!output || samples == 0) return 0;

        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t count = std::min(samples, head - tail);

        if (!count) return 0;
        const size_t offset = tail & mask_;
        if (count == 1) {
            *output = buffer_[offset];
        } else {
            const size_t first = std::min(count, buffer_.size() - offset);
            std::memcpy(output, buffer_.data() + offset, first * sizeof(float));
            if (first < count)
                std::memcpy(output + first, buffer_.data(), (count - first) * sizeof(float));
        }

        tail_.store(tail + count, std::memory_order_release);
        return count;
    }

    // A snapshot, not a reservation. On either thread, the count may change
    // immediately; callers must still respect the return count of read/write.
    size_t available() const
    {
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t tail = tail_.load(std::memory_order_acquire);
        return head - tail;
    }

    size_t capacity() const { return usable_; }

    // Only while stopped, before starting a new producer/consumer.
    void reset()
    {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

private:
    std::vector<float> buffer_;
    const size_t mask_;
    const size_t usable_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

} // namespace stts
