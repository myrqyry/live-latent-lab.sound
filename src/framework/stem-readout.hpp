#pragma once
// Worker-only sliding context with short central readouts. At stream start
// history is zero-padded; every published sample corresponds to real capture.
// Future tail is borrowed from the preceding prediction, never emitted twice.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace stts {
class StemReadout {
public:
    StemReadout(size_t context,size_t hop,size_t future,size_t blend,size_t planes)
        : context_(context),hop_(hop),future_(future),blend_(blend),planes_(planes),
          history_(checked_history(context,hop,future,blend,planes)),
          left_(context),right_(context),tail_(planes*blend),out_(planes*hop),weights_(blend){
        for(size_t i=0;i<blend_;++i)
            weights_[i]=blend_==1?.5f:float(.5-.5*std::cos(3.14159265358979323846*double(i)/double(blend_-1)));
        reset();
    }
    void reset(){
        std::fill(left_.begin(),left_.end(),0.f);
        std::fill(right_.begin(),right_.end(),0.f);
        captured_=false;have_tail_=false;
    }
    const float* left()const{return left_.data();}
    const float* right()const{return right_.data();}
    size_t next_capture()const{return have_tail_?hop_:hop_+future_;}
    template<class Read>
    bool capture(Read&& read){
        if(captured_)return false;
        size_t n=next_capture(),prefix=context_-n;
        captured_=read(left_.data()+prefix,right_.data()+prefix,n);
        return captured_;
    }
    template<class Publish>
    bool publish(const float* batch,size_t count,Publish&& publish){
        if(!captured_||!batch||count!=planes_*context_)return false;
        for(size_t ch=0;ch<planes_;++ch){
            auto* src=batch+ch*context_+history_;
            auto* dst=out_.data()+ch*hop_;
            size_t n=have_tail_?blend_:0;
            for(size_t i=0;i<n;++i)dst[i]=tail_[ch*blend_+i]*(1.f-weights_[i])+src[i]*weights_[i];
            std::copy_n(src+n,hop_-n,dst+n);
        }
        if(!publish(out_.data(),out_.size(),hop_))return false;
        if(blend_)for(size_t ch=0;ch<planes_;++ch)
            std::copy_n(batch+ch*context_+history_+hop_,blend_,tail_.data()+ch*blend_);
        std::memmove(left_.data(),left_.data()+hop_,(context_-hop_)*sizeof(float));
        std::memmove(right_.data(),right_.data()+hop_,(context_-hop_)*sizeof(float));
        have_tail_=true;captured_=false;
        return true;
    }
private:
    static size_t checked_history(size_t c,size_t h,size_t f,size_t b,size_t p){
        if(!h||h>c||f>c-h||b>h||b>f||p<2||p>12||p%2)
            throw std::invalid_argument("invalid stem readout geometry");
        return c-h-f;
    }
    const size_t context_,hop_,future_,blend_,planes_,history_;
    bool captured_=false,have_tail_=false;
    std::vector<float> left_,right_,tail_,out_,weights_;
};
}
