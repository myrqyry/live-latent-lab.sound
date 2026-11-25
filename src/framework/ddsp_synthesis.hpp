#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace ddsp {
struct Controls {float amplitude=0;std::array<float,60> harmonics{};std::array<float,65> noise{};};
inline Controls blend(const Controls& a,const Controls& b,float mix){
    mix=std::clamp(mix,0.f,1.f);Controls c;
    c.amplitude=(1-mix)*a.amplitude+mix*b.amplitude;
    for(size_t k=0;k<60;++k)c.harmonics[k]=c.amplitude>1e-9f?((1-mix)*a.amplitude*a.harmonics[k]+mix*b.amplitude*b.harmonics[k])/c.amplitude:0;
    for(size_t k=0;k<65;++k)c.noise[k]=(1-mix)*a.noise[k]+mix*b.noise[k];
    return c;
}
// Fixed 16 kHz model domain. Independent implementation of phase-continuous
// additive synthesis and frequency-sampled Hann-windowed FIR noise shaping.
class Synth {
    static constexpr double tau=6.2831853071795864769;
    std::array<float,60> previous{};
    std::array<float,128> fir{},history{};
    std::array<std::array<float,65>,128> basis{};
    size_t cursor=0;uint32_t rng=0x98d321u;
    double phase=0;float previous_hz=0;
public:
    Synth(){
        for(size_t n=0;n<128;++n){
            const double offset=double(n)-64;
            const double window=.5+.5*std::cos(tau*offset/128.);
            for(size_t k=0;k<65;++k)basis[n][k]=float(window*std::cos(tau*offset*k/128.)*(k==0||k==64?1:2)/128.);
        }
    }
    void render(const Controls& c,float hz,float harmonic_gain,float noise_gain,std::array<float,320>& out){
        hz=std::clamp(hz,20.f,4000.f);
        std::array<float,60> next{};
        float sum=0;
        for(size_t k=0;k<60;++k)if(hz*(k+1)<7900)sum+=std::max(0.f,c.harmonics[k]);
        if(sum>1e-9f)for(size_t k=0;k<60;++k)if(hz*(k+1)<7900)next[k]=std::max(0.f,c.harmonics[k])*std::clamp(c.amplitude,0.f,2.f)*harmonic_gain/sum;
        std::array<float,128> target{};
        for(size_t n=0;n<128;++n)for(size_t k=0;k<65;++k)target[n]+=basis[n][k]*std::clamp(c.noise[k],0.f,2.f)*noise_gain;
        const float old_hz=previous_hz>0?previous_hz:hz;
        for(size_t i=0;i<out.size();++i){
            float w=float(i+1)/out.size(),f=old_hz+(hz-old_hz)*w;
            phase+=tau*f/16000.;if(phase>=tau)phase-=tau;
            float y=0;
            for(size_t k=0;k<60;++k)if(f*(k+1)<7900)y+=(previous[k]+(next[k]-previous[k])*w)*float(std::sin(phase*(k+1)));
            rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;
            history[cursor]=float(double(rng)/2147483648.-1.);
            for(size_t n=0;n<128;++n)y+=(fir[n]+(target[n]-fir[n])*w)*history[(cursor+128-n)%128];
            cursor=(cursor+1)%128;
            out[i]=std::clamp(y,-.95f,.95f);
        }
        previous=next;fir=target;previous_hz=hz;
    }
};
}
