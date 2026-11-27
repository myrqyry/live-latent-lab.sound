#pragma once
#include "live-stem-pipeline.hpp"
#include "ring-buffer.hpp"
#include <cmath>
#include <array>
namespace ddsp {
// All queues are SPSC. Fixed latency, with phase-aligned delayed dry on
// a missed deadline. No heap allocation or locking in process().
class Pipeline {
    const size_t rate_,hop_,delay_;
    stts::StereoSpscQueue input_;
    stts::RingBuffer wet_;
    std::vector<float> dry_l_,dry_r_;
    size_t cursor_=0;
    uint64_t count_=0;
    std::atomic<bool> fault_{false};
    float previous_mix_=0;
public:
    explicit Pipeline(size_t rate):rate_(rate),hop_(rate/50),delay_(rate/5),
        input_(rate/2),wet_(rate/2),dry_l_(delay_),dry_r_(delay_){}
    size_t hop()const{return hop_;}size_t delay()const{return delay_;}
    bool failed()const{return fault_.load(std::memory_order_acquire);}
    void fail(){fault_.store(true,std::memory_order_release);}
    bool read(float* l,float* r){return !failed()&&input_.pop(l,r,hop_);}
    bool write(const float* v){if(failed())return false;if(wet_.write(v,hop_)!=hop_){fail();return false;}return true;}
    void process(float* l,float* r,size_t n,float mix){
        if(!l||!n)return;
        if(!r)r=l;
        if(!failed()&&!input_.push(l,r,n))fail();
        const float step=1.f/float(rate_/100);
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
                const float a=dry_l_[cursor_],b=dry_r_[cursor_];
                dry_l_[cursor_]=std::isfinite(l[i])?l[i]:0;
                dry_r_[cursor_]=std::isfinite(r[i])?r[i]:0;
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
// Rational hop conversion from host sample rate to 16 kHz using a
// causal low-pass FIR. The window carries 704 samples across hops.
class Analysis {
    std::array<float,1024> history_{};
    std::array<float,48> tail_{};
    std::vector<std::array<float,48>> weights_;
    size_t hop_;
public:
    explicit Analysis(size_t rate):weights_(320),hop_(rate/50){
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
        auto mono=[&](size_t i){float v=(l[i]+r[i])*.5f;return std::isfinite(v)?std::clamp(v,-1.f,1.f):0.f;};
        for(size_t i=0;i<320;++i){
            int pos=int(i*hop_/320);float sum=0;
            for(int j=0;j<48;++j){int k=pos-j;sum+=weights_[i][j]*(k<0?tail_[size_t(k+48)]:mono(size_t(k)));}
            history_[704+i]=sum;
        }
        for(size_t i=0;i<48;++i)tail_[i]=mono(hop_-48+i);
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
