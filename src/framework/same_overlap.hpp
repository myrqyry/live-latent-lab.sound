#pragma once
#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <vector>
namespace same_overlap {
// A window consists of two equal hops. Blend the previous window's
// second hop with the next window's first hop at identical sample times.
inline void blend(const std::vector<float>& window,std::vector<float>& previous,
                  std::vector<float>& output,size_t hop){
    if(hop<2||window.size()!=4*hop||(!previous.empty()&&previous.size()!=2*hop))
        throw std::invalid_argument("Invalid overlap geometry");
    output.resize(2*hop);
    for(size_t ch=0;ch<2;++ch)for(size_t i=0;i<hop;++i){
        const float x=window[ch*2*hop+i];
        const float w=float(i)/float(hop-1);
        output[ch*hop+i]=previous.empty()?x:previous[ch*hop+i]*(1-w)+x*w;
    }
    previous.resize(2*hop);
    for(size_t ch=0;ch<2;++ch)std::copy_n(window.data()+ch*2*hop+hop,hop,previous.data()+ch*hop);
}
}
