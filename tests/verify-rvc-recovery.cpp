#include "framework/rvc_window.hpp"
#include "framework/sola_stitcher.hpp"
#include <cassert>
#include <cstdio>
#include <thread>
#include <atomic>
#include <cstdlib>
#include <new>
static thread_local bool realtime=false;
void* operator new(size_t n){if(realtime)std::abort();if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{if(realtime)std::abort();std::free(p);}
void operator delete[](void* p) noexcept{::operator delete(p);}
void operator delete(void* p,size_t) noexcept{::operator delete(p);}
void operator delete[](void* p,size_t) noexcept{::operator delete(p);}
int main(){
 for(size_t rate:{44100,48000}){
    rvc_native::Pipeline p(rate);
    const auto hop=rate/2,first=rate*3/4,delay=rate*3/2;
    std::vector<float> l(1025),r(1025),a(first),b(first),wet(hop);
    size_t clock=0,origin=0,consumed=0,produced=0,needed=first;
    auto signal=[](size_t i){return float((i%4093)+1)/16384.f;};
    auto feed=[&](size_t n,bool wet_expected,bool drain){
        for(size_t i=0;i<n;++i){l[i]=signal(clock+i);r[i]=-l[i];}
        realtime=true;p.process(l.data(),r.data(),n,1);realtime=false;
        for(size_t i=0;i<n;++i){
            const size_t pos=clock+i;
            float v=pos<delay?0:signal(pos-delay);
            assert(std::abs(l[i]-v)<1e-6);
            if(!wet_expected||pos<origin+delay)assert(std::abs(r[i]+v)<1e-6);
            else if(pos>=origin+delay+rate/100)assert(std::abs(r[i]-v)<1e-6);
        }
        clock+=n;
        if(drain)while(p.read(a.data(),b.data(),needed)){
            for(size_t i=0;i<needed;++i)assert(a[i]==signal(origin+consumed+i));
            consumed+=needed;needed=hop;
            for(size_t i=0;i<hop;++i)wet[i]=signal(origin+produced+i);
            assert(p.write(wet.data()));produced+=hop;
        }
    };
    for(size_t n=0;n<first-1;){const auto k=std::min<size_t>(1025,first-1-n);feed(k,true,false);n+=k;assert(!p.input_ready());}
    feed(1,true,true);assert(consumed==first&&!p.input_ready());
    for(int i=0;i<100;++i)for(auto n:{733u,17u,1025u,511u})feed(n,true,true);
    // A worker result already in flight cannot leak after the fault.
    p.fail();assert(!p.write(wet.data()));feed(511,false,false);
    assert(p.restart());assert(!p.worker_prepare());
    feed(37,false,false);assert(p.input_ready());assert(p.worker_prepare());
    origin=clock;consumed=produced=0;needed=first;
    for(size_t n=0;n<first-1;){const auto k=std::min<size_t>(1025,first-1-n);feed(k,true,false);n+=k;assert(!p.input_ready());}
    feed(1,true,true);assert(consumed==first);
    for(int i=0;i<100;++i)for(auto n:{1u,733u,1025u,511u})feed(n,true,true);
    p.fail();assert(p.restart());p.fail();feed(17,false,false);assert(!p.worker_prepare());
 }
 puts("RVC_GENERATION_RECOVERY_PASS=1");
}
