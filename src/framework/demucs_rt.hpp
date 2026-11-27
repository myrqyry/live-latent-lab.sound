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
// Worker publishes complete planar batches into preallocated slots. The audio
// reader owns the partial-block cursor and can discard stale generations in
// bounded time without freeing or moving any storage.
class DemucsFilterQueue {
    static constexpr size_t slots=3;
    struct Block {
        uint64_t tag=0;
        size_t frames=0,channels=0;
        std::vector<float> samples;
    };
    const size_t max_frames_;
    std::array<Block,slots> blocks_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    size_t offset_=0; // audio consumer only
public:
    explicit DemucsFilterQueue(size_t max_frames):max_frames_(max_frames){
        for(auto& b:blocks_)b.samples.resize(max_frames_*4);
    }
    bool publish(uint64_t tag,const float* const* data,size_t channels,size_t frames){
        if(!data || (channels!=1 && channels!=4) || !frames || frames>max_frames_)return false;
        for(size_t ch=0;ch<channels;++ch)if(!data[ch])return false;
        const auto h=head_.load(std::memory_order_relaxed);
        const auto t=tail_.load(std::memory_order_acquire);
        if(h-t>=slots)return false;
        auto& b=blocks_[h%slots];
        b.tag=tag;b.frames=frames;b.channels=channels;
        for(size_t ch=0;ch<channels;++ch)
            std::memcpy(b.samples.data()+ch*max_frames_,data[ch],frames*sizeof(float));
        head_.store(h+1,std::memory_order_release);
        return true;
    }
    void render(uint64_t tag,float* const* output,size_t channels,size_t frames){
        if(!output || !frames)return;
        for(size_t ch=0;ch<channels;++ch)
            if(output[ch])std::fill_n(output[ch],frames,0.f);
        auto t=tail_.load(std::memory_order_relaxed);
        const auto h=head_.load(std::memory_order_acquire);
        size_t written=0;
        const bool multi=(tag&7u)==5u;
        while(t!=h && written<frames){
            const auto& b=blocks_[t%slots];
            if(b.tag==tag && b.channels==(multi?4u:1u)){
                const size_t n=std::min(frames-written,b.frames-offset_);
                for(size_t ch=0;ch<channels;++ch){
                    const size_t plane=multi?ch/2:0;
                    if(output[ch] && plane<b.channels)
                        std::memcpy(output[ch]+written,b.samples.data()+plane*max_frames_+offset_,n*sizeof(float));
                }
                written+=n;offset_+=n;
                if(offset_<b.frames)break;
            }
            offset_=0;++t;
            tail_.store(t,std::memory_order_release);
        }
    }
};
}
