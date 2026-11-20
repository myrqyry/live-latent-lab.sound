#pragma once
#include <array>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <vector>

namespace stts {
// Fixed packet storage. Audio callback owns write cursor, worker owns read
// cursor. Epoch tags prevent late packets crossing a config/stream boundary.
struct SttBlock {
    uint64_t epoch=0,timestamp=0;
    uint32_t count=0;
    std::array<float,1024> samples{};
};
class SttCapture {
    std::vector<SttBlock> blocks_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
public:
    explicit SttCapture(size_t capacity=1024):blocks_(capacity+1){}
    bool push(const SttBlock& b){
        const auto h=head_.load(std::memory_order_relaxed);
        const auto next=(h+1)%blocks_.size();
        if(next==tail_.load(std::memory_order_acquire))return false;
        blocks_[h]=b;head_.store(next,std::memory_order_release);return true;
    }
    bool pop(SttBlock& b){
        const auto t=tail_.load(std::memory_order_relaxed);
        if(t==head_.load(std::memory_order_acquire))return false;
        b=blocks_[t];tail_.store((t+1)%blocks_.size(),std::memory_order_release);return true;
    }
};
}
