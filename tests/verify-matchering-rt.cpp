#include "engines/matchering_engine.hpp"
#include <array>
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <new>
#include <thread>
#include <cstdio>

static thread_local bool realtime = false;
void* operator new(std::size_t n) {
    if (realtime) std::abort();
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { if (realtime) std::abort(); std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

int main() {
    stts::MatcheringEngine engine;
    stts::MatcheringControl control;
    stts::MatcheringControl::Snapshot snapshot;
    snapshot.reference={true,.2f,{.1f,.12f,.05f}};
    snapshot.generation=1;
    snapshot.parameters={.5f,.7f,0.f};
    control.publish(snapshot);
    std::array<float,2048> l{},r{};
    for(size_t i=0;i<l.size();++i){l[i]=.25f*std::sin(float(i)*.1f);r[i]=-.1f*std::cos(float(i)*.07f);}
    auto dry_l=l,dry_r=r;
    realtime=true;
    assert(control.try_read(snapshot));
    engine.apply_reference(snapshot.reference);
    engine.set_parameters(snapshot.parameters);
    engine.update_analysis(l.data(),r.data(),l.size(),48000);
    engine.process_channel(l.data(),l.size(),0);
    engine.process_channel(r.data(),r.size(),1);
    assert(l==dry_l && r==dry_r);
    snapshot.parameters.mix=.8f;
    engine.set_parameters(snapshot.parameters);
    engine.update_analysis(l.data(),r.data(),l.size(),48000);
    engine.process_channel(l.data(),l.size(),0);
    engine.process_channel(r.data(),r.size(),1);
    for(size_t i=0;i<l.size();++i) assert(std::isfinite(l[i]) && std::isfinite(r[i]));
    assert(l!=dry_l && r!=dry_r);
    realtime=false;

    std::atomic<bool> done{false};
    std::thread writer([&]{
        for(unsigned i=2;i<50000;++i){
            stts::MatcheringControl::Snapshot next;
            next.generation=i;
            next.reference={true,float(i),{float(i),float(i),float(i)}};
            next.parameters={float(i),float(i),float(i)};
            control.publish(next);
        }
        done=true;
    });
    realtime=true;
    do{
        if(control.try_read(snapshot) && snapshot.generation>=2){
            float expected=float(snapshot.generation);
            assert(snapshot.reference.rms==expected);
            for(float e:snapshot.reference.energy) assert(e==expected);
            assert(snapshot.parameters.mix==expected && snapshot.parameters.smoothing==expected && snapshot.parameters.eq_amount==expected);
        }
    }while(!done);
    realtime=false;
    writer.join();
    assert(control.try_read(snapshot) && snapshot.generation==49999);
    puts("MATCHERING_RT_PASS=1");
}
