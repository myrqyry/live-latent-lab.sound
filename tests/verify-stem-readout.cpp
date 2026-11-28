#include "framework/stem-readout.hpp"
#include "framework/live-stem-pipeline.hpp"
#include <cassert>
#include <cstdio>
#include <cmath>
#include <vector>
using namespace stts;
void verify(unsigned rate){
    const size_t context=rate*28/10,hop=rate*7/10,future=rate*35/100,blend=rate*14/100,delay=rate*21/10;
    StemReadout worker(context,hop,future,blend,4);
    LiveStemMixPipeline pipe(rate,context,delay,2);
    assert(pipe.delay_frames()==delay&&pipe.window_frames()==context);
    std::vector<float> l(733),r(733),batch(4*context);
    std::array<std::vector<float>,8> raw;
    std::array<float*,8> planes{};
    for(size_t ch=0;ch<8;++ch){raw[ch].resize(l.size());planes[ch]=raw[ch].data();}
    size_t clock=0,origin=0,read=0,windows=0;
    auto sample=[](int64_t n){return n<0?0.f:float((n%4096)+1)/8192;};
    auto feed=[&](size_t n,bool active,bool drain){
        for(size_t i=0;i<n;++i){l[i]=sample(clock+i-origin);r[i]=-l[i];}
        pipe.process_mix_and_stems(l.data(),r.data(),n,{1,1,0,0},planes);
        for(size_t i=0;i<n;++i){
            float x=active&&clock+i>=origin+delay?sample(clock+i-origin-delay):0;
            assert(std::abs(l[i]-x)<1e-6&&std::abs(r[i]+x)<1e-6);
            assert(std::abs(raw[0][i]-x*.25f)<1e-6&&std::abs(raw[2][i]-x*.75f)<1e-6);
            for(size_t ch=4;ch<8;++ch)assert(raw[ch][i]==0);
        }
        clock+=n;
        if(drain)while(worker.capture([&](float* a,float* b,size_t k){return pipe.read_input_frames(a,b,k);})){
            for(size_t i=0;i<context;++i){
                float x=sample(int64_t(read)+int64_t(i)-int64_t(context-hop-future));
                assert(worker.left()[i]==x&&worker.right()[i]==-x);
                batch[i]=x*.25f;batch[context+i]=-x*.25f;
                batch[2*context+i]=x*.75f;batch[3*context+i]=-x*.75f;
            }
            assert(worker.publish(batch.data(),batch.size(),[&](const float* data,size_t count,size_t n){
                assert(n==hop&&count==4*hop);
                return pipe.push_output_frames(data,count,n);
            }));
            read+=hop;++windows;
        }
    };
    auto run=[&](size_t n,bool active,bool drain){while(n){auto k=std::min(n,l.size());feed(k,active,drain);n-=k;}};
    run(rate*15,true,true);
    assert(windows==(rate*15-future)/hop);
    pipe.invalidate();run(rate,false,false);
    assert(pipe.restart());feed(37,false,false);assert(pipe.worker_prepare());
    origin=clock;read=0;worker.reset();
    run(rate*12,true,true);
}
int main(){
    for(unsigned rate:{44100u,48000u})verify(rate);
    assert(stem_output_index(true,3)==0&&stem_output_index(true,4)==1);
    assert(stem_output_index(true,0)==-1&&stem_output_index(false,4)==-1);
    assert(stem_output_index(false,3)==3);
    assert(resolve_melband_gains(1,3,{},0,0)==(StemGains{0,1,0,0}));
    assert(resolve_melband_gains(2,3,{1,1,1,1},0,1)==(StemGains{1,0,0,0}));
    assert(resolve_melband_gains(2,3,{1,1,1,1},1,1)==(StemGains{}));
    StemReadout w(20,5,3,2,4);
    auto capture=[](float* l,float* r,size_t n){std::fill_n(l,n,0);std::fill_n(r,n,0);return true;};
    std::vector<float> a(80,1),b(80,3);
    assert(w.capture(capture));assert(w.publish(a.data(),a.size(),[](const float* p,size_t n,size_t){return n==20&&p[0]==1;}));
    assert(w.capture(capture));assert(w.publish(b.data(),b.size(),[](const float* p,size_t,size_t){return p[0]==1&&p[1]==3;}));
    w.reset();assert(w.capture(capture));assert(w.publish(b.data(),b.size(),[](const float* p,size_t,size_t){return p[0]==3;}));
    puts("STEM_SHORT_READOUT_PASS=1");
}
