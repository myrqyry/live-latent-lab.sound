#include "framework/ddsp_pipeline.hpp"
#include "framework/worker_recovery.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
#include <atomic>
static thread_local bool realtime=false;
void* operator new(size_t n){if(realtime)std::abort();if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{if(realtime)std::abort();std::free(p);}
void operator delete[](void* p) noexcept{::operator delete(p);}
void operator delete(void* p,size_t) noexcept{::operator delete(p);}
void operator delete[](void* p,size_t) noexcept{::operator delete(p);}
int main(){
    stts::WorkerRecovery budget(1500);
    assert(!budget.can_retry(false)); // invalid initial configuration is not transient
    assert(budget.can_retry(true)&&budget.delay_ms()==250);
    budget.on_retry();budget.on_success();
    assert(budget.can_retry(false)&&budget.delay_ms()==500);
    budget.on_retry();budget.on_fault();
    assert(budget.delay_ms()==1000);
    budget.on_retry();
    assert(!budget.can_retry(true)&&!budget.can_retry(false));
    for(int i=0;i<1499;++i)budget.on_success();
    assert(!budget.can_retry(true));budget.on_fault();
    for(int i=0;i<1500;++i)budget.on_success();
    assert(budget.can_retry(true)&&budget.delay_ms()==250&&!budget.can_retry(false));
    budget.on_retry();budget.reset();
    assert(!budget.can_retry(false)&&budget.delay_ms()==250);
 for(size_t rate:{44100,48000}){
    ddsp::Pipeline p(rate);auto h=p.hop();
    std::vector<float> l(h),r(h),a(h),b(h),wet(h);
    size_t clock=0;
    auto feed=[&](float mix){
        const float input=float(++clock)/1000;
        std::fill(l.begin(),l.end(),input);std::fill(r.begin(),r.end(),-input);
        realtime=true;p.process(l.data(),r.data(),h,mix);realtime=false;
    };
    auto dry=[&]{
        float v=clock<=10?0.f:float(clock-10)/1000;
        for(size_t i=0;i<h;++i)assert(l[i]==v&&r[i]==-v);
    };
    for(int i=0;i<15;++i){feed(0);dry();assert(p.read(a.data(),b.data()));wet.assign(h,.7f);assert(p.write(wet.data()));}
    // Simulate a model fault with old wet audio still queued.
    p.fail();feed(1);dry();assert(!p.read(a.data(),b.data()));assert(!p.write(wet.data()));
    // Worker requests a new epoch; audio must acknowledge before either
    // queue can be drained by its consumer. No old wet can cross the epoch.
    assert(p.restart());assert(!p.worker_prepare());
    feed(1);dry();assert(p.worker_prepare());
    for(int i=0;i<10;++i){feed(1);dry();assert(p.read(a.data(),b.data()));
        assert(a.front()==float(clock)/1000);wet.assign(h,-.6f);assert(p.write(wet.data()));}
    feed(1);assert(l.back()==-.6f&&r.back()==-.6f);
    // Repeated failures and a pending restart that is superseded.
    p.fail();assert(p.restart());p.fail();feed(1);dry();assert(!p.worker_prepare());
    assert(p.restart());feed(1);dry();assert(p.worker_prepare());
    for(int i=0;i<10;++i){feed(1);dry();assert(p.read(a.data(),b.data()));assert(a.front()==float(clock)/1000);wet.assign(h,.4f);assert(p.write(wet.data()));}
    feed(1);assert(std::abs(l.back()-.4f)<1e-6&&std::abs(r.back()-.4f)<1e-6);

    // Hold an actual worker in-flight across a control/audio invalidation.
    // Explicit phase gates make the ordering deterministic, not sleep-based.
    ddsp::Pipeline race(rate);
    std::atomic<unsigned> phase{0};
    auto wait_for=[&](unsigned target){
        while(phase.load(std::memory_order_acquire)<target)std::this_thread::yield();
    };
    auto publish=[&](unsigned target){phase.store(target,std::memory_order_release);};
    auto capture_block=[&](float x){
        std::fill(l.begin(),l.end(),x);std::fill(r.begin(),r.end(),-x);
        realtime=true;race.process(l.data(),r.data(),h,1);realtime=false;
    };
    capture_block(.1f);
    std::thread worker([&]{
        assert(race.read(a.data(),b.data())&&a.front()==.1f);
        publish(1);wait_for(2);
        std::fill(wet.begin(),wet.end(),.9f);
        assert(!race.write(wet.data())); // obsolete result is rejected
        assert(race.restart()&&!race.worker_prepare());
        publish(3);wait_for(4);
        assert(race.worker_prepare());publish(5);wait_for(6);
        assert(race.read(a.data(),b.data())&&a.front()==.4f&&b.front()==-.4f);
        std::fill(wet.begin(),wet.end(),-.5f);
        assert(race.write(wet.data()));publish(7);
    });
    wait_for(1);race.fail();capture_block(.2f);publish(2);
    wait_for(3);capture_block(.3f);publish(4);
    wait_for(5);capture_block(.4f);publish(6);
    wait_for(7);worker.join();
    // The new output has a fresh 200 ms warmup, not an earlier wet position.
    for(float x:l)assert(x==0.f);

    // Recover between arbitrary packet boundaries. The synthetic worker
    // returns the exact left input as mono wet, so sample-level alignment
    // can be checked independently of the neural model's synthesis phase.
    ddsp::Pipeline aligned(rate);
    size_t sample_clock=0,wet_origin=0;
    std::vector<float> left(1025),right(1025);
    auto signal=[](size_t i){return float((i%4093)+1)/16384.f;};
    auto advance=[&](size_t n,bool expect_wet,bool drain){
        for(size_t i=0;i<n;++i){left[i]=signal(sample_clock+i);right[i]=-left[i];}
        realtime=true;aligned.process(left.data(),right.data(),n,1);realtime=false;
        for(size_t i=0;i<n;++i){
            const auto pos=sample_clock+i;
            const auto expected=pos<aligned.delay()?0.f:signal(pos-aligned.delay());
            assert(std::abs(left[i]-expected)<1e-6);
            if(!expect_wet||pos<wet_origin+aligned.delay())
                assert(std::abs(right[i]+expected)<1e-6);
            else if(pos>=wet_origin+aligned.delay()+rate/100)
                assert(std::abs(right[i]-expected)<1e-6);
        }
        sample_clock+=n;
        if(drain)while(aligned.read(a.data(),b.data()))assert(aligned.write(a.data()));
    };
    for(int i=0;i<35;++i)for(auto n:{17u,511u,733u,1025u})advance(n,true,true);
    aligned.fail();
    for(auto n:{511u,17u,733u})advance(n,false,false);
    assert(aligned.restart());
    advance(37,false,false);assert(aligned.worker_prepare());
    wet_origin=sample_clock;
    for(int i=0;i<35;++i)for(auto n:{733u,1u,1025u,511u})advance(n,true,true);
 }
 puts("DDSP_GENERATION_RECOVERY_PASS=1");
}
