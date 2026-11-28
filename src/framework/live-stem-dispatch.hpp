#pragma once
// One bounded queue of synchronized OBS-ready packets, not four queues.
// Producer = parent filter callback, consumer = non-audio dispatch worker.
#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <vector>

namespace stts {
struct StemDispatchPacket {
    static constexpr size_t max_frames=1024;
    uint64_t timestamp=0,epoch=0;
    uint32_t frames=0;
    std::array<std::array<float,max_frames>,8> planes{};
};
class StemDispatchQueue {
public:
    explicit StemDispatchQueue(size_t capacity=64):slots_(capacity){}
    StemDispatchPacket* reserve(){
        auto head=head_.load(std::memory_order_relaxed);
        if(head-tail_.load(std::memory_order_acquire)>=slots_.size())return nullptr;
        return &slots_[head%slots_.size()];
    }
    void publish(){head_.fetch_add(1,std::memory_order_release);}
    const StemDispatchPacket* peek()const{
        auto tail=tail_.load(std::memory_order_relaxed);
        if(head_.load(std::memory_order_acquire)==tail)return nullptr;
        return &slots_[tail%slots_.size()];
    }
    void consume(){tail_.fetch_add(1,std::memory_order_release);}
private:
    std::vector<StemDispatchPacket> slots_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};
}
