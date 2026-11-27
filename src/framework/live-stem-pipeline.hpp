#pragma once
// Fixed-delay, stereo, single-producer/single-consumer bridge for the opt-in
// HTDemucs sidecar. OBS audio callbacks never allocate, wait or use sockets.
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace stts {
class StereoSpscQueue {
public:
    explicit StereoSpscQueue(size_t max_frames)
        : capacity_(max_frames), left_(max_frames), right_(max_frames) {}
    size_t available() const {
        const size_t head=head_.load(std::memory_order_acquire);
        const size_t tail=tail_.load(std::memory_order_acquire);
        return head-tail;
    }
    bool push(const float* l,const float* r,size_t frames) {
        if(!l || !r || !frames || frames>capacity_)return false;
        const size_t head=head_.load(std::memory_order_relaxed);
        const size_t tail=tail_.load(std::memory_order_acquire);
        if(head-tail>capacity_-frames)return false;
        const size_t offset=head%capacity_;
        const size_t first=std::min(frames,capacity_-offset);
        std::memcpy(left_.data()+offset,l,first*sizeof(float));
        std::memcpy(right_.data()+offset,r,first*sizeof(float));
        if(first<frames){
            std::memcpy(left_.data(),l+first,(frames-first)*sizeof(float));
            std::memcpy(right_.data(),r+first,(frames-first)*sizeof(float));
        }
        head_.store(head+frames,std::memory_order_release);
        return true;
    }
    bool pop(float* l,float* r,size_t frames){
        if(!l || !r || !frames || frames>capacity_)return false;
        const size_t tail=tail_.load(std::memory_order_relaxed);
        const size_t head=head_.load(std::memory_order_acquire);
        if(head-tail<frames)return false;
        const size_t offset=tail%capacity_;
        const size_t first=std::min(frames,capacity_-offset);
        std::memcpy(l,left_.data()+offset,first*sizeof(float));
        std::memcpy(r,right_.data()+offset,first*sizeof(float));
        if(first<frames){
            std::memcpy(l+first,left_.data(),(frames-first)*sizeof(float));
            std::memcpy(r+first,right_.data(),(frames-first)*sizeof(float));
        }
        tail_.store(tail+frames,std::memory_order_release);
        return true;
    }
private:
    const size_t capacity_;
    std::vector<float> left_,right_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

class LiveStemPipeline {
public:
    explicit LiveStemPipeline(uint32_t sample_rate)
        : rate_(sample_rate), inputs_(size_t(sample_rate)*32),
          outputs_(size_t(sample_rate)*24),
          valid_(sample_rate==44100 || sample_rate==48000) {}
    bool ok() const{return valid_.load(std::memory_order_acquire);}
    void invalidate(){valid_.store(false,std::memory_order_release);}
    size_t window_frames() const{return size_t(rate_)*8;}
    size_t delay_frames() const{return size_t(rate_)*12;}
    bool push_input(const float* left,const float* right,size_t frames){
        if(!ok())return false;
        if(!inputs_.push(left,right,frames)){
            valid_.store(false,std::memory_order_release);
            return false;
        }
        return true;
    }
    bool input_ready()const{return ok()&&inputs_.available()>=window_frames();}
    bool read_input_window(float* left,float* right){
        return ok() && inputs_.pop(left,right,window_frames());
    }
    bool push_output_window(const float* left,const float* right){
        if(!ok())return false;
        if(!outputs_.push(left,right,window_frames())){
            valid_.store(false,std::memory_order_release);
            return false;
        }
        return true;
    }
    // Audio callback, exactly one producer and exactly one consumer per queue.
    // The first 12s are deliberately silent, not copied dry music.
    void process(float* left,float* right,size_t frames){
        if(!left || !right || !frames)return;
        if(!push_input(left,right,frames)){
            std::fill_n(left,frames,0.f);
            std::fill_n(right,frames,0.f);
            return;
        }
        const auto delay=delay_frames();
        const auto remaining=played_<delay?delay-played_:0;
        const size_t silence=std::min(frames,remaining);
        std::fill_n(left,silence,0.f);
        std::fill_n(right,silence,0.f);
        const size_t to_render=frames-silence;
        if(to_render && !outputs_.pop(left+silence,right+silence,to_render)){
            valid_.store(false,std::memory_order_release);
            std::fill_n(left+silence,to_render,0.f);
            std::fill_n(right+silence,to_render,0.f);
        }
        played_+=frames;
    }
private:
    uint32_t rate_;
    StereoSpscQueue inputs_;
    StereoSpscQueue outputs_;
    std::atomic<bool> valid_;
    uint64_t played_{0}; // owned by OBS audio thread only
};
} // namespace stts
