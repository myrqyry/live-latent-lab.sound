#include "framework/live-stem-pipeline.hpp"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace stts;
int main(){
 for(unsigned rate:{44100u,48000u}){
    LiveStemMixPipeline p(rate);
    const size_t window=p.window_frames(),delay=p.delay_frames();
    std::vector<float> batch(8*window),in_l(window),in_r(window),l(733),r(733);
    for(size_t stem=0;stem<4;++stem){
        std::fill_n(batch.data()+stem*2*window,window,float(stem+1)/16);
        std::fill_n(batch.data()+(stem*2+1)*window,window,-float(stem+1)/32);
    }
    std::array<std::vector<float>,8> raw;
    std::array<float*,8> planes{};
    for(size_t i=0;i<8;++i){raw[i].resize(l.size());planes[i]=raw[i].data();}
    size_t clock=0,origin=0,windows=0;
    auto feed=[&](size_t n,const StemGains& gains,bool active,bool drain,float expected){
        std::fill_n(l.data(),n,.75f);std::fill_n(r.data(),n,-.5f);
        p.process_mix_and_stems(l.data(),r.data(),n,gains,planes);
        for(size_t i=0;i<n;++i){
            bool wet=active&&clock+i>=origin+delay;
            assert(l[i]==(wet?expected:0.f));assert(r[i]==(wet?-expected/2:0.f));
            for(size_t ch=0;ch<8;++ch)assert(raw[ch][i]==(wet?batch[ch*window]:0.f));
        }
        clock+=n;
        if(drain)while(p.read_input_window(in_l.data(),in_r.data())){
            assert(in_l.front()==.75f&&in_r.back()==-.5f);
            assert(p.push_output_batch(batch.data(),batch.size()));++windows;
        }
    };
    auto run=[&](size_t n,StemGains gains,bool active,bool drain,float expected){
        while(n){size_t k=std::min(n,l.size());feed(k,gains,active,drain,expected);n-=k;}
    };
    const StemGains vocals{0,0,0,1},instrumental{1,1,1,0},custom{.5f,0,1.5f,0},muted{};
    assert(resolve_stem_gains(0,3,custom,0,0)==vocals);
    assert(resolve_stem_gains(1,3,custom,0,0)==instrumental);
    assert(resolve_stem_gains(3,3,custom,0,0)==muted);
    assert(resolve_stem_gains(2,3,custom,1,0)==(StemGains{0,0,1.5f,0}));
    assert(resolve_stem_gains(2,3,custom,0,1)==(StemGains{.5f,0,0,0}));
    assert(resolve_stem_gains(2,3,custom,1,1)==muted);
    assert(resolve_stem_gains(99,99,custom,0,0)==vocals);
    assert(unpack_stem_gains(pack_stem_gains(custom))==custom);
    assert(unpack_stem_gains(pack_stem_gains(StemGains{-1,NAN,INFINITY,3}))==(StemGains{0,0,0,2}));
    run(delay+rate,vocals,true,true,.25f);
    auto epoch=p.epoch();
    run(rate,instrumental,true,true,.375f);
    run(rate,custom,true,true,.3125f);
    run(rate,muted,true,true,0.f);
    run(rate,vocals,true,true,.25f);
    assert(p.epoch()==epoch&&windows>=2);
    p.invalidate();assert(!p.push_output_batch(batch.data(),batch.size()));
    run(rate,custom,false,false,0);
    assert(p.restart());feed(37,custom,false,false,0);assert(p.worker_prepare());
    origin=clock;
    // Recovered batches have different samples. All four old stems must be discarded together.
    for(float& sample:batch)sample*=.5f;
    run(delay+rate,custom,true,true,.15625f);
    assert(!p.push_output_batch(batch.data(),batch.size()-1));assert(!p.ok());
 }
 puts("STEM_SHARED_MIX_PASS=1");
}
