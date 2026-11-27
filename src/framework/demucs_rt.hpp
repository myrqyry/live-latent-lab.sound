#pragma once
#include "ring-buffer.hpp"
#include <array>
#include <atomic>
#include <cstdint>

namespace stts {
// Capture producer and worker consumer each own one cursor. Overflow drops a
// whole incoming packet and marks a discontinuity; the worker discards the
// old backlog before accepting a new contiguous chunk.
class DemucsCapture {
    RingBuffer ring_;
    const size_t limit_;
    std::atomic<uint64_t> epoch_{0};
public:
    explicit DemucsCapture(size_t samples):ring_(samples),limit_(samples){}
    uint64_t epoch()const noexcept{return epoch_.load(std::memory_order_acquire);}
    void mark_gap() noexcept{epoch_.fetch_add(1,std::memory_order_release);}
    bool push(const float* samples,size_t count){
        if(!samples || !count)return false;
        const auto used=ring_.available();
        if(count>limit_ || used>limit_-count){
            mark_gap();return false;
        }
        return ring_.write(samples,count)==count;
    }
    // Only the worker may drain or read. The caller owns seen_epoch.
    bool read_chunk(float* output,size_t count,uint64_t& seen_epoch){
        const auto current=epoch();
        if(current!=seen_epoch){
            std::array<float,1024> discard;
            size_t remaining=ring_.available();
            while(remaining){
                const auto n=ring_.read(discard.data(),std::min(remaining,discard.size()));
                if(!n)break;
                remaining-=n;
            }
            seen_epoch=current;
            return false;
        }
        return output && count && ring_.available()>=count &&
               ring_.read(output,count)==count && epoch()==current;
    }
};

// Parent and child hold this object across their lifetimes. The audio render
// path only dereferences its existing owner; no registry lock or refcount
// operations, and no possibility of accessing a deleted parent object.
class DemucsStemOutput {
    RingBuffer ring_;
    const size_t limit_;
    std::atomic<bool> active_{true},claimed_{false};
public:
    explicit DemucsStemOutput(size_t samples):ring_(samples),limit_(samples){}
    bool claim() noexcept{
        if(!active_.load(std::memory_order_acquire))return false;
        bool expected=false;
        return claimed_.compare_exchange_strong(expected,true);
    }
    void release() noexcept{claimed_.store(false,std::memory_order_release);}
    void close() noexcept{active_.store(false,std::memory_order_release);}
    bool publish(const float* samples,size_t count){
        if(!active_.load(std::memory_order_acquire) || !samples || !count)return false;
        const auto used=ring_.available();
        if(count>limit_ || used>limit_-count)return false;
        return ring_.write(samples,count)==count;
    }
    void render(float* output,size_t count){
        if(!output || !count)return;
        size_t read=0;
        if(active_.load(std::memory_order_acquire))read=ring_.read(output,count);
        std::fill_n(output+read,count-read,0.f);
    }
};
}
