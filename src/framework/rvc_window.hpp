#pragma once
#include "fixed_delay_mono_pipeline.hpp"
#include <array>
#include <cmath>
#include <stdexcept>
namespace rvc_native {
// Bounded host-rate capture, fixed delayed stereo dry, mono wet.
// No lock, allocation, I/O or inference on the audio callback.
class Pipeline : public stts::FixedDelayMonoPipeline {
public:
    explicit Pipeline(size_t rate):FixedDelayMonoPipeline(
        rate/2,rate*3/2,rate*3/4,rate*4,rate*3,100.f/rate){}
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
