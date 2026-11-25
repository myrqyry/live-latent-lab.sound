#include "framework/ddsp_pipeline.hpp"
#include <cassert>
#include <cstdio>
int main(){
 for(size_t rate:{44100,48000}){
    ddsp::Pipeline p(rate);auto h=p.hop();
    std::vector<float> l(h),r(h),a(h),b(h),wet(h,.25f);
    for(size_t j=0;j<30;++j){
        std::fill(l.begin(),l.end(),float(j+1)/100);std::fill(r.begin(),r.end(),-float(j+1)/100);
        p.process(l.data(),r.data(),h,0);
        assert(p.read(a.data(),b.data()));assert(std::abs(a[0]-float(j+1)/100)<1e-6);
        assert(p.write(wet.data()));
        const float expected=j<10?0.f:float(j-9)/100;
        for(size_t i=0;i<h;++i){assert(std::abs(l[i]-expected)<1e-6);assert(std::abs(r[i]+expected)<1e-6);}
    }
    p.fail();std::fill(l.begin(),l.end(),.8f);std::fill(r.begin(),r.end(),-.8f);
    p.process(l.data(),r.data(),h,1);
    assert(std::abs(l[0]-.21f)<1e-6);assert(std::abs(r[0]+.21f)<1e-6);
    ddsp::Analysis analysis(rate);ddsp::Upsample up;
    std::array<float,320> ones{};ones.fill(.5f);up.run(ones,a.data(),h);up.run(ones,a.data(),h);
    for(float x:a)assert(std::abs(x-.5f)<1e-6);
    std::fill(l.begin(),l.end(),.25f);std::fill(r.begin(),r.end(),.25f);
    for(int i=0;i<4;++i)analysis.push(l.data(),r.data());
    for(float x:analysis.push(l.data(),r.data()))assert(std::abs(x-.25f)<1e-5);
 }
 puts("DDSP_PIPELINE_PASS=1");
}
