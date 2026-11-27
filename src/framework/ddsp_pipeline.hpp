#pragma once
#include "fixed_delay_mono_pipeline.hpp"
#include <cmath>
#include <array>
namespace ddsp {
// All queues are SPSC. Fixed latency, with phase-aligned delayed dry on
// a missed deadline. No heap allocation or locking in process().
class Pipeline : public stts::FixedDelayMonoPipeline {
public:
    explicit Pipeline(size_t rate):FixedDelayMonoPipeline(
        rate/50,rate/5,rate/50,rate/2,rate/2,1.f/float(rate/100)){}
    bool read(float* l,float* r){return FixedDelayMonoPipeline::read(l,r,hop());}
};
// Rational hop conversion from host sample rate to 16 kHz using a
// causal low-pass FIR. The window carries 704 samples across hops.
class Analysis {
    std::array<float,1024> history_{};
    std::vector<std::array<float,48>> weights_;
    size_t hop_;
    std::vector<float> mono_;
public:
    explicit Analysis(size_t rate):weights_(320),hop_(rate/50),mono_(hop_+48){
        constexpr double pi=3.14159265358979323846;
        const double fc=7200./rate;
        for(size_t i=0;i<320;++i){
            double pos=double(i)*hop_/320.,frac=pos-std::floor(pos),sum=0;
            for(int j=0;j<48;++j){
                double t=j-23.5+frac;
                double v=(std::abs(t)<1e-8?2*fc:std::sin(2*pi*fc*t)/(pi*t))*(.5-.5*std::cos(2*pi*j/47.));
                weights_[i][j]=float(v);sum+=v;
            }
            for(auto& v:weights_[i])v/=float(sum);
        }
    }
    const std::array<float,1024>& push(const float* l,const float* r){
        std::move(history_.begin()+320,history_.end(),history_.begin());
        // Sanitize each input sample once instead of repeating the stereo
        // downmix and finite check for every FIR tap. First 48 are history.
        for(size_t i=0;i<hop_;++i){
            float v=(l[i]+r[i])*.5f;
            mono_[48+i]=std::isfinite(v)?std::clamp(v,-1.f,1.f):0.f;
        }
        for(size_t i=0;i<320;++i){
            int pos=int(i*hop_/320);float sum=0;
            for(int j=0;j<48;++j){sum+=weights_[i][j]*mono_[size_t(pos-j+48)];}
            history_[704+i]=sum;
        }
        std::copy_n(mono_.begin()+hop_,48,mono_.begin());
        return history_;
    }
};
class Upsample {
    float previous_=0;
public:
    void run(const std::array<float,320>& in,float* out,size_t n){
        for(size_t i=0;i<n;++i){
            double p=double(i)*320/n;size_t k=size_t(p);float w=float(p-k);
            float a=k?in[k-1]:previous_;out[i]=a+(in[k]-a)*w;
        }
        previous_=in.back();
    }
};
}
