#pragma once
// Worker-only context overlap and constant-sum crossfade. The wire window
// stays fixed; only fresh capture and published playback advance by the hop.
// One timeline for every plane, including future non-four-stem backends.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace stts {
class StemWindowOverlap {
public:
    StemWindowOverlap(size_t window,size_t planes)
        : window_(window),planes_(planes),hop_(window),left_(window),right_(window){
        if(window<2||planes<2||planes>12)throw std::invalid_argument("invalid stem window geometry");
    }
    void reset(size_t overlap){
        if(overlap>window_/2)throw std::invalid_argument("invalid stem overlap");
        overlap_=overlap;hop_=window_-overlap;
        have_tail_=false;captured_=false;
        if(overlap){
            tail_.resize(planes_*overlap);
            out_.resize(planes_*hop_);
            weights_.resize(overlap);
        }
        // Raised-cosine complementary weights sum to one. No equal-power
        // amplification when adjacent predictions describe the same signal.
        for(size_t i=0;i<overlap;++i)
            weights_[i]=overlap==1?.5f:float(.5-.5*std::cos(3.14159265358979323846*double(i)/double(overlap-1)));
    }
    size_t next_capture()const{return have_tail_?hop_:window_;}
    const float* left()const{return left_.data();}
    const float* right()const{return right_.data();}
    template<class Read>
    bool capture(Read&& read){
        if(captured_)return false;
        const size_t prefix=have_tail_?overlap_:0;
        // Previous publish retains the input overlap in the leading slots.
        captured_=read(left_.data()+prefix,right_.data()+prefix,window_-prefix);
        return captured_;
    }
    template<class Publish>
    bool publish(const float* batch,size_t count,Publish&& publish){
        if(!captured_||!batch||count!=planes_*window_)return false;
        if(!overlap_){
            if(!publish(batch,count,window_))return false;
        }else{
            for(size_t ch=0;ch<planes_;++ch){
                const auto* src=batch+ch*window_;
                auto* dst=out_.data()+ch*hop_;
                auto* tail=tail_.data()+ch*overlap_;
                if(have_tail_)for(size_t i=0;i<overlap_;++i){
                    const float w=weights_[i];
                    dst[i]=tail[i]*(1.f-w)+src[i]*w;
                }
                const size_t prefix=have_tail_?overlap_:0;
                std::copy_n(src+prefix,hop_-prefix,dst+prefix);
            }
            if(!publish(out_.data(),planes_*hop_,hop_))return false;
            for(size_t ch=0;ch<planes_;++ch)
                std::copy_n(batch+ch*window_+hop_,overlap_,tail_.data()+ch*overlap_);
            std::memmove(left_.data(),left_.data()+hop_,overlap_*sizeof(float));
            std::memmove(right_.data(),right_.data()+hop_,overlap_*sizeof(float));
        }
        have_tail_=true;captured_=false;
        return true;
    }
private:
    const size_t window_,planes_;
    size_t overlap_=0,hop_=0;
    bool have_tail_=false,captured_=false;
    std::vector<float> left_,right_,tail_,out_,weights_;
};
}
