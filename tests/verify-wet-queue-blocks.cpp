#include "framework/ddsp_pipeline.hpp"
#include "framework/rvc_window.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <new>
static bool realtime=false;
void* operator new(size_t n){if(realtime)std::abort();if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{if(realtime)std::abort();std::free(p);}
void operator delete[](void* p) noexcept{::operator delete(p);}
void operator delete(void* p,size_t) noexcept{::operator delete(p);}
void operator delete[](void* p,size_t) noexcept{::operator delete(p);}

template<class Pipeline,class Read>
void test(size_t rate,size_t delay,size_t hop,float step,Read read){
    Pipeline p(rate);
    std::vector<float> l(4097),r(4097),a(delay+4097),b(delay+4097),wet(hop,.6f);
    // Feed warmup in uneven packets; drain input so only wet underrun can fail.
    size_t count=0,read_count=0;
    auto process=[&](size_t n,float mix){
        std::fill_n(l.data(),n,.2f);std::fill_n(r.data(),n,-.1f);
        realtime=true;p.process(l.data(),r.data(),n,mix);realtime=false;
        count+=n;
        while(count-read_count>=hop && read(p,a.data(),b.data(),hop))read_count+=hop;
    };
    while(count<delay-17){auto n=std::min<size_t>(777,delay-17-count);process(n,1);
        for(size_t i=0;i<n;++i)assert(l[i]==0&&r[i]==0);}
    assert(p.write(wet.data()));
    float previous=0;
    size_t played=0;
    // Cross delay and then drain exactly one wet chunk, including partial block.
    for(size_t n:{size_t(53),size_t(511),size_t(2049),size_t(4097)}){
        size_t start=count;process(n,1);
        for(size_t i=0;i<n;++i){
            const auto pos=start+i;
            if(pos<delay){assert(l[i]==0&&r[i]==0);continue;}
            if(played<hop){
                previous+=std::clamp(1.f-previous,-step,step);
                assert(std::abs(l[i]-(.2f+(.6f-.2f)*previous))<1e-6f);
                assert(std::abs(r[i]-(-.1f+(.6f+.1f)*previous))<1e-6f);
            }else{assert(l[i]==.2f&&r[i]==-.1f);}
            ++played;
        }
        if(played>=hop)break;
    }
    // RVC wet chunk is longer; keep advancing to the underrun.
    while(played<hop+1){
        size_t start=count;process(std::min<size_t>(4097,hop+1-played),1);
        size_t n=count-start;
        for(size_t i=0;i<n;++i){
            if(played<hop){
                previous+=std::clamp(1.f-previous,-step,step);
                assert(std::abs(l[i]-(.2f+(.6f-.2f)*previous))<1e-6f);
                assert(std::abs(r[i]-(-.1f+(.6f+.1f)*previous))<1e-6f);
            }else{assert(l[i]==.2f&&r[i]==-.1f);}
            ++played;
        }
    }
    assert(p.failed());assert(!p.write(wet.data()));
    process(37,1);for(size_t i=0;i<37;++i)assert(l[i]==.2f&&r[i]==-.1f);
}
int main(){
 for(size_t rate:{44100,48000}){
    test<ddsp::Pipeline>(rate,rate/5,rate/50,1.f/float(rate/100),[](auto& p,float* l,float* r,size_t){return p.read(l,r);});
    test<rvc_native::Pipeline>(rate,rate*3/2,rate/2,100.f/rate,[](auto& p,float* l,float* r,size_t n){return p.read(l,r,n);});
 }
 puts("WET_QUEUE_BLOCKS_PASS=1");
}
