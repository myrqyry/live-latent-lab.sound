#pragma once
#include "live-stem-pipeline.hpp"
#include "ring-buffer.hpp"
#include <array>
#include <cmath>
#include <stdexcept>
namespace rvc_native {
// Bounded host-rate capture, fixed delayed stereo dry, mono wet.
// No lock, allocation, I/O or inference on the audio callback.
class Pipeline {
    const size_t rate_,delay_;
    stts::StereoSpscQueue in_;
    stts::RingBuffer wet_;
    std::vector<float> l_,r_;
    size_t cursor_=0,count_=0;
    std::atomic<bool> failed_{false};
    float previous_mix_=0;
public:
    explicit Pipeline(size_t rate):rate_(rate),delay_(rate*3/2),in_(rate*4),wet_(rate*3),l_(delay_),r_(delay_){}
    bool failed()const{return failed_.load(std::memory_order_acquire);}
    void fail(){failed_.store(true,std::memory_order_release);}
    // Producer checks the minimum subsequent hop; the worker checks its exact window.
    bool input_ready()const{return !failed()&&in_.available()>=rate_/2;}
    bool read(float* l,float* r,size_t n){return !failed()&&in_.pop(l,r,n);}
    bool write(const float* x){if(failed()||wet_.write(x,rate_/2)!=rate_/2){fail();return false;}return true;}
    void process(float* l,float* r,size_t n,float mix){
        if(!l||!n)return;
        if(!r)r=l;
        if(!failed()&&!in_.push(l,r,n))fail();
        const float step=100.f/rate_;
        // A bounded stack block replaces per-sample atomic queue operations.
        // Do not consume wet audio during warmup; preserve a valid prefix
        // on underrun, then permanently fall back to delayed dry.
        std::array<float,256> wet_block;
        for(size_t offset=0;offset<n;){
            size_t block=std::min(wet_block.size(),n-offset);
            const bool warmup=count_<delay_;
            if(warmup)block=std::min(block,delay_-size_t(count_));
            const bool can_read=!warmup&&!failed();
            size_t valid=can_read?wet_.read(wet_block.data(),block):0;
            if(can_read&&failed())valid=0;
            if(can_read&&valid<block)fail();
            for(size_t j=0;j<block;++j){
                const size_t i=offset+j;
                const float a=l_[cursor_],b=r_[cursor_];
                l_[cursor_]=std::isfinite(l[i])?l[i]:0;
                r_[cursor_]=std::isfinite(r[i])?r[i]:0;
                if(++cursor_==delay_)cursor_=0;
                if(j<valid){
                    previous_mix_+=std::clamp(mix-previous_mix_,-step,step);
                    const float wet=wet_block[j];
                    l[i]=a+(wet-a)*previous_mix_;r[i]=b+(wet-b)*previous_mix_;
                }else{previous_mix_=0;l[i]=a;r[i]=b;}
            }
            count_+=block;offset+=block;
        }
    }
};
// One-second causal overlap window; initial 250 ms is zero history.
// Each subsequent window advances exactly 500 ms. Windowed-sinc lowpass
// resampling to 16 kHz, with coefficient table built off the audio thread.
class Window {
    size_t rate_,needed_;
    std::vector<float> history_;
    std::vector<std::array<float,48>> coeff_;
public:
    explicit Window(size_t rate):rate_(rate),needed_(rate*3/4),history_(rate),coeff_(16000){
        constexpr double pi=3.14159265358979323846;double fc=7200./rate;
        for(size_t i=0;i<coeff_.size();++i){
            double pos=double(i)*rate/16000,frac=pos-std::floor(pos),sum=0;
            for(int j=0;j<48;++j){double t=j-23-frac;double v=(std::abs(t)<1e-9?2*fc:std::sin(2*pi*fc*t)/(pi*t))*(.5-.5*std::cos(2*pi*j/47.));coeff_[i][j]=float(v);sum+=v;}
            for(float& v:coeff_[i])v/=float(sum);
        }
    }
    size_t needed()const{return needed_;}
    std::array<float,16000> push(const float* l,const float* r,size_t n){
        if(n!=needed_)throw std::invalid_argument("RVC window size");
        if(n==rate_/2)std::move(history_.begin()+n,history_.end(),history_.begin());
        for(size_t i=0;i<n;++i){float v=(l[i]+r[i])*.5f;history_[rate_-n+i]=std::isfinite(v)?std::clamp(v,-1.f,1.f):0;}
        needed_=rate_/2;
        std::array<float,16000> out{};
        for(size_t i=0;i<out.size();++i){
            int pos=int(i*rate_/16000);float sum=0;
            for(int j=0;j<48;++j){int k=std::clamp(pos+j-23,0,int(rate_-1));sum+=history_[size_t(k)]*coeff_[i][j];}
            out[i]=std::clamp(sum,-1.f,1.f);
        }
        return out;
    }
};
}
