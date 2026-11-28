#pragma once
// Fixed-delay, stereo, single-producer/single-consumer bridge for the opt-in
// HTDemucs sidecar. OBS audio callbacks never allocate, wait or use sockets.
#include <algorithm>
#include <array>
#include <cmath>
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
    // Consumer only: discard a snapshot after the producer is quiescent.
    size_t discard_available(){
        const auto tail=tail_.load(std::memory_order_relaxed);
        const auto head=head_.load(std::memory_order_acquire);
        tail_.store(head,std::memory_order_release);
        return head-tail;
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

using StemGains = std::array<float,4>;
// Mode 0 is the original single-stem selection; 1 is instrumental;
// 2 is a custom linear mix. Explicit mute always overrides solo.
inline StemGains resolve_stem_gains(int mode,int selected,StemGains gains,unsigned muted,unsigned solo){
    if(mode==2){
        for(size_t i=0;i<4;++i)
            if((muted&(1u<<i))||((solo&15u)&&!(solo&(1u<<i))))gains[i]=0.f;
        return gains;
    }
    if(mode==1)return {1,1,1,0};
    if(mode==3)return {}; // independent OBS outputs; mute parent
    StemGains result{};
    result[selected>=0&&selected<4?selected:3]=1.f;
    return result;
}
// A single atomic word publishes the complete mix (Q12, 0..2 gain).
// No torn mute/solo/gain snapshot and no mutex on the audio thread.
inline uint64_t pack_stem_gains(const StemGains& gains){
    uint64_t bits=0;
    for(size_t i=0;i<gains.size();++i){
        float gain=std::isfinite(gains[i])?std::clamp(gains[i],0.f,2.f):0.f;
        bits|=uint64_t(std::lround(gain*4096.f))<<(16*i);
    }
    return bits;
}
inline StemGains unpack_stem_gains(uint64_t bits){
    StemGains gains{};
    for(size_t i=0;i<gains.size();++i)gains[i]=float((bits>>(16*i))&0xffff)/4096.f;
    return gains;
}

// Four stereo stems occupy one batch ring with a common publication and
// consumption cursor. A failed batch cannot skew stems relative to each other.
// The input layout is [stem][channel][frame], matching LLSTEM01.
class StemBatchQueue {
public:
    explicit StemBatchQueue(size_t capacity):capacity_(capacity),planes_(8*capacity){}
    bool push_batch(const float* batch,size_t count,size_t frames){
        if(!batch||!frames||frames>capacity_||count!=8*frames)return false;
        auto head=head_.load(std::memory_order_relaxed);
        auto tail=tail_.load(std::memory_order_acquire);
        if(head-tail>capacity_-frames)return false;
        const size_t offset=head%capacity_,first=std::min(frames,capacity_-offset);
        for(size_t ch=0;ch<8;++ch){
            auto* dst=planes_.data()+ch*capacity_;
            const auto* src=batch+ch*frames;
            std::memcpy(dst+offset,src,first*sizeof(float));
            if(first<frames)std::memcpy(dst,src+first,(frames-first)*sizeof(float));
        }
        head_.store(head+frames,std::memory_order_release);
        return true;
    }
    size_t discard_available(){
        auto tail=tail_.load(std::memory_order_relaxed);
        auto head=head_.load(std::memory_order_acquire);
        tail_.store(head,std::memory_order_release);
        return head-tail;
    }
    bool pop_mix(float* l,float* r,size_t frames,const StemGains& gains,float* const* raw=nullptr){
        if(!l||!r||!frames||frames>capacity_)return false;
        auto tail=tail_.load(std::memory_order_relaxed);
        auto head=head_.load(std::memory_order_acquire);
        if(head-tail<frames)return false;
        size_t offset=tail%capacity_,first=std::min(frames,capacity_-offset);
        std::fill_n(l,frames,0.f);std::fill_n(r,frames,0.f);
        if(raw)for(size_t ch=0;ch<8;++ch){
            const auto* src=planes_.data()+ch*capacity_;
            std::memcpy(raw[ch],src+offset,first*sizeof(float));
            if(first<frames)std::memcpy(raw[ch]+first,src,(frames-first)*sizeof(float));
        }
        // Skip muted stems entirely; each span is contiguous for SIMD.
        for(size_t stem=0;stem<4;++stem){
            const float gain=gains[stem];
            if(gain==0.f)continue;
            auto* sl=planes_.data()+stem*2*capacity_;
            auto* sr=sl+capacity_;
            for(size_t i=0;i<first;++i){l[i]+=sl[offset+i]*gain;r[i]+=sr[offset+i]*gain;}
            for(size_t i=first;i<frames;++i){l[i]+=sl[i-first]*gain;r[i]+=sr[i-first]*gain;}
        }
        tail_.store(tail+frames,std::memory_order_release);
        return true;
    }
private:
    const size_t capacity_;
    std::vector<float> planes_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

template<class OutputQueue>
class BasicLiveStemPipeline {
public:
    explicit BasicLiveStemPipeline(uint32_t sample_rate)
        : rate_(sample_rate),inputs_(size_t(sample_rate)*32),
          outputs_(size_t(sample_rate)*24),
          state_(sample_rate==44100||sample_rate==48000?3:0) {}
    uint64_t epoch()const{return state_.load(std::memory_order_acquire);}
    bool current(uint64_t e)const{return (e&1)&&epoch()==e;}
    bool ok()const{return epoch()&1;}
    void fault(uint64_t e){
        if(e&1)state_.compare_exchange_strong(e,e+1,std::memory_order_acq_rel,
            std::memory_order_acquire);
    }
    void invalidate(){fault(epoch());}
    // Worker only, serialized with control revisions by the caller.
    bool restart(){
        auto e=epoch();
        return e&&!(e&1)&&state_.compare_exchange_strong(e,e+1,
            std::memory_order_acq_rel,std::memory_order_acquire);
    }
    size_t window_frames()const{return size_t(rate_)*8;}
    size_t delay_frames()const{return size_t(rate_)*12;}
    bool input_ready()const{
        auto e=epoch();
        return (e&1)&&audio_ack_.load(std::memory_order_acquire)==e&&
            (worker_ack_.load(std::memory_order_acquire)!=e||
             inputs_.available()>=window_frames());
    }
    // Worker must stop publishing old output before acknowledging new
    // capture. Each consumer alone advances its own queue read cursor.
    bool worker_prepare(){
        auto e=epoch();
        if(!(e&1)||audio_ack_.load(std::memory_order_acquire)!=e)return false;
        if(worker_epoch_!=e){
            inputs_.discard_available();
            worker_epoch_=e;worker_ack_.store(e,std::memory_order_release);
        }
        return current(e);
    }
    bool push_input(const float* l,const float* r,size_t frames){
        auto e=epoch();
        if(!current(e)||worker_ack_.load(std::memory_order_acquire)!=e)return false;
        if(!inputs_.push(l,r,frames)){fault(e);return false;}
        return current(e);
    }
    bool read_input_window(float* l,float* r){
        auto e=worker_epoch_;
        return current(e)&&inputs_.pop(l,r,window_frames())&&current(e);
    }
    bool push_output_window(const float* l,const float* r){
        auto e=worker_epoch_;
        if(!current(e))return false;
        if(!outputs_.push(l,r,window_frames())){fault(e);return false;}
        return current(e);
    }
    bool push_output_batch(const float* data,size_t count){
        auto e=worker_epoch_;
        if(!current(e))return false;
        if(!outputs_.push_batch(data,count,window_frames())){fault(e);return false;}
        return current(e);
    }
    void process(float* left,float* right,size_t frames){
        process_with(left,right,frames,[&](float* l,float* r,size_t n){return outputs_.pop(l,r,n);});
    }
    void process_mix(float* left,float* right,size_t frames,const StemGains& gains){
        process_with(left,right,frames,[&](float* l,float* r,size_t n){return outputs_.pop_mix(l,r,n,gains);});
    }
    void process_mix_and_stems(float* left,float* right,size_t frames,const StemGains& gains,
                               const std::array<float*,8>& stems){
        auto clear=[&]{for(auto* ch:stems)std::fill_n(ch,frames,0.f);};
        clear();
        if(!process_with(left,right,frames,[&](float* l,float* r,size_t n){
            auto planes=stems;
            for(auto& ch:planes)ch+=l-left;
            return outputs_.pop_mix(l,r,n,gains,planes.data());
        }))clear();
    }
private:
    // A recovered stream starts a new 12-second prefill on the first
    // captured packet. Never emit original dry audio, even during faults.
    template<class Render>
    bool process_with(float* left,float* right,size_t frames,Render render){
        if(!left||!right||!frames)return false;
        auto e=epoch();
        auto silence=[&]{std::fill_n(left,frames,0.f);std::fill_n(right,frames,0.f);};
        if(!(e&1)){silence();return false;}
        if(audio_ack_.load(std::memory_order_relaxed)!=e)
            audio_ack_.store(e,std::memory_order_release);
        if(worker_ack_.load(std::memory_order_acquire)!=e){silence();return false;}
        if(render_epoch_!=e){
            outputs_.discard_available();render_epoch_=e;played_=0;
        }
        if(!current(e)){silence();return false;}
        if(!inputs_.push(left,right,frames)){fault(e);silence();return false;}
        auto delay=delay_frames();
        size_t prefix=std::min<uint64_t>(frames,played_<delay?delay-played_:0);
        std::fill_n(left,prefix,0.f);std::fill_n(right,prefix,0.f);
        size_t n=frames-prefix;
        if(n&&!render(left+prefix,right+prefix,n)){
            fault(e);silence();return false;
        }
        if(!current(e)){silence();return false;}
        played_+=frames;
        return true;
    }
private:
    const uint32_t rate_;
    StereoSpscQueue inputs_;
    OutputQueue outputs_;
    std::atomic<uint64_t> state_,audio_ack_{3},worker_ack_{3};
    uint64_t worker_epoch_=3,render_epoch_=3,played_=0;
};
using LiveStemPipeline = BasicLiveStemPipeline<StereoSpscQueue>;
using LiveStemMixPipeline = BasicLiveStemPipeline<StemBatchQueue>;
} // namespace stts
