#include "framework/ddsp_synthesis.hpp"
#include <cassert>
#include <cstdio>
#include <cmath>
int main(){
    ddsp::Synth synth;
    ddsp::Controls a{},b{};a.amplitude=.3f;a.harmonics[0]=1;b.amplitude=.2f;b.harmonics[1]=1;
    auto c=ddsp::blend(a,b,.5f);
    assert(std::abs(c.amplitude*c.harmonics[0]-.15f)<1e-6);
    assert(std::abs(c.amplitude*c.harmonics[1]-.1f)<1e-6);
    std::array<float,320> out{};
    synth.render(a,440,1,0,out);synth.render(a,440,1,0,out);
    double energy=0;int crossings=0;
    for(size_t i=0;i<out.size();++i){assert(std::isfinite(out[i]));energy+=out[i]*out[i];if(i&&out[i]>0&&out[i-1]<=0)++crossings;}
    assert(energy>10&&crossings>=8&&crossings<=9);
    auto prev=out.back();synth.render(a,440,1,0,out);assert(std::abs(out.front()-prev)<.06);
    ddsp::Controls silent{};synth.render(silent,440,0,0,out);synth.render(silent,440,0,0,out);
    for(float x:out)assert(x==0);
    b={};b.noise.fill(.1f);synth.render(b,440,0,1,out);synth.render(b,440,0,1,out);
    energy=0;for(float x:out){assert(std::isfinite(x));energy+=x*x;}assert(energy>.1);
    puts("DDSP_SYNTHESIS_PASS=1");
}
