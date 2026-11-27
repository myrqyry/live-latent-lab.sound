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
    static constexpr size_t capacity=1024;
    uint64_t epoch=0,timestamp=0;
    uint32_t count=0;
    std::array<float,capacity> samples{};
};
class SttCapture {
    std::vector<SttBlock> blocks_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
public:
    explicit SttCapture(size_t capacity=1024):blocks_(capacity+1){}
    // Producer fills the reserved slot directly. It is invisible to the
    // consumer until the release publication; no full packet copy or clear.
    template<class Writer>
    bool emplace(uint64_t epoch,uint64_t timestamp,uint32_t count,Writer&& write){
        if(!count || count>SttBlock::capacity)return false;
        const auto h=head_.load(std::memory_order_relaxed);
        const auto next=(h+1)%blocks_.size();
        if(next==tail_.load(std::memory_order_acquire))return false;
        auto& b=blocks_[h];
        b.epoch=epoch;b.timestamp=timestamp;b.count=count;
        write(b.samples.data());
        head_.store(next,std::memory_order_release);
        return true;
    }
    bool push(const SttBlock& b){
        return emplace(b.epoch,b.timestamp,b.count,[&](float* dst){
            std::copy_n(b.samples.data(),b.count,dst);
        });
    }
    bool pop(SttBlock& b){
        const auto t=tail_.load(std::memory_order_relaxed);
        if(t==head_.load(std::memory_order_acquire))return false;
        const auto& slot=blocks_[t];
        b.epoch=slot.epoch;b.timestamp=slot.timestamp;b.count=slot.count;
        std::copy_n(slot.samples.data(),slot.count,b.samples.data());
        tail_.store((t+1)%blocks_.size(),std::memory_order_release);return true;
    }
};
}
