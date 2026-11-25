#pragma once
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace same_texture {
enum class Mode { Freeze, Smear, Grain };
// Latents use frame-major storage, as returned by SAME. All operations
// read the original sequence, never previously modified frames.
inline void process_into(const std::vector<float>& input,std::vector<float>& out,int dim,int frames,
                                  Mode mode,float amount,float position,int radius,int grain){
    if(dim<1||dim>256||frames<1||frames>130||input.size()!=size_t(dim)*frames||
       !std::isfinite(amount)||amount<0||amount>1||
       !std::isfinite(position)||position<0||position>1||radius<1||radius>16||
       grain<1||grain>16)throw std::invalid_argument("Invalid latent texture parameters");
    if(&input==&out)throw std::invalid_argument("Texture output must not alias input");
    out=input;
    if(amount==0)return;
    int anchor=int(std::lround(position*(frames-1)));
    grain=std::min(grain,frames);
    const int start=std::min(anchor,frames-grain);
    for(int t=0;t<frames;++t){
        const int lo=std::max(0,t-radius),hi=std::min(frames-1,t+radius);
        for(int k=0;k<dim;++k){
            float value=0;
            switch(mode){
            case Mode::Freeze:value=input[size_t(anchor)*dim+k];break;
            case Mode::Smear:{
                double sum=0;
                for(int j=lo;j<=hi;++j)sum+=input[size_t(j)*dim+k];
                value=float(sum/(hi-lo+1));break;
            }
            case Mode::Grain:value=input[size_t(start+t%grain)*dim+k];break;
            }
            const auto i=size_t(t)*dim+k;
            out[i]=amount==1?value:(1-amount)*input[i]+amount*value;
        }
    }
}
inline std::vector<float> process(const std::vector<float>& input,int dim,int frames,
                                  Mode mode,float amount,float position,int radius,int grain){
    std::vector<float> out;process_into(input,out,dim,frames,mode,amount,position,radius,grain);
    return out;
}
}
