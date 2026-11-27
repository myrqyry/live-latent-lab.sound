#include "framework/deepfilter_transport.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <atomic>
#include <thread>
#include <chrono>
static bool realtime=false;
void* operator new(size_t n){if(realtime)std::abort();if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{if(realtime)std::abort();std::free(p);}
void operator delete[](void* p) noexcept{::operator delete(p);}
void operator delete(void* p,size_t) noexcept{::operator delete(p);}
void operator delete[](void* p,size_t) noexcept{::operator delete(p);}

int main(){
    stts::DeepFilterTransport q(7,7);
    std::array<float,7> dry{1,2,3,4,5,6,7}, wet{-1,-2,-3,-4,-5,-6,-7}, audio=dry, in{};
    int resets=0;
    auto prepare=[&]{return q.worker_prepare([&]{++resets;});};
    auto callback=[&](size_t n){realtime=true;bool result=q.process(audio.data(),n);realtime=false;return result;};
    // Wait for worker acknowledgement before capturing the new stream.
    assert(!prepare());
    assert(!callback(7)&&audio==dry);
    auto epoch=prepare();assert(epoch&&resets==1);
    assert(!callback(7)&&audio==dry);
    assert(q.worker_read(epoch,in.data(),7)&&in==dry);
    assert(q.worker_publish(epoch,wet.data(),7));
    assert(callback(7)&&audio==wet);
    assert(q.worker_read(epoch,in.data(),7)&&in==dry);
    // Overflow: preserve dry input, invalidate outstanding old output.
    assert(q.worker_publish(epoch,wet.data(),7));
    audio=dry;
    // First callback fills input and consumes existing output.
    assert(callback(7)&&audio==wet);
    assert(q.worker_publish(epoch,wet.data(),7));
    audio=dry;
    assert(!callback(7)&&audio==dry);
    assert(!q.worker_publish(epoch,wet.data(),1));
    assert(!prepare()); // worker cannot drain until audio acknowledges
    assert(!callback(7)&&audio==dry);
    auto next=prepare();assert(next!=epoch&&resets==2);
    assert(!callback(7)&&audio==dry); // old wet output was discarded
    assert(q.worker_read(next,in.data(),7)&&in==dry);
    assert(q.worker_publish(next,wet.data(),7));
    // Worker output overflow also invalidates the stream.
    assert(!q.worker_publish(next,wet.data(),1));
    assert(!callback(7)&&audio==dry);
    auto third=prepare();assert(third!=next&&resets==3);
    assert(!callback(7)&&audio==dry);
    assert(q.worker_read(third,in.data(),7));
    // Model reload / clear must reject pre-reload output.
    assert(q.worker_publish(third,wet.data(),7));
    q.invalidate();
    assert(!callback(7)&&audio==dry);
    auto fourth=prepare();assert(fourth!=third&&resets==4);
    assert(!callback(3)&&audio==dry);
    assert(q.worker_read(fourth,in.data(),3));
    assert(q.worker_publish(fourth,wet.data(),3));
    assert(!callback(4)&&audio==dry); // no partial overwrite on underrun
    assert(q.worker_read(fourth,in.data(),4));
    assert(q.worker_publish(fourth,wet.data()+3,4));
    assert(callback(7)&&audio==wet);
    // Real concurrent SPSC actors with rapid generation changes and overload.
    // A successful callback may only expose wet samples from its current phase.
    stts::DeepFilterTransport concurrent(31,31);
    std::atomic<bool> stop{false};
    std::thread worker([&]{
        std::array<float,3> block;
        while(!stop.load(std::memory_order_relaxed)){
            const auto generation=concurrent.worker_prepare([]{});
            if(concurrent.worker_read(generation,block.data(),block.size())){
                for(auto& v:block)v=-v;
                concurrent.worker_publish(generation,block.data(),block.size());
            }else std::this_thread::yield();
        }
    });
    size_t successful=0;
    for(int phase=1;phase<=200;++phase){
        concurrent.invalidate();
        bool got_wet=false;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        do {
            std::array<float,7> block;block.fill(float(phase));
            if(concurrent.process(block.data(),block.size())){
                for(auto v:block)assert(v==-float(phase));
                got_wet=true;++successful;
            }else for(auto v:block)assert(v==float(phase));
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }while(!got_wet&&std::chrono::steady_clock::now()<deadline);
        assert(got_wet);
        // Force burst pressure with no waits. Either a complete same-phase wet
        // packet or bit-identical dry is acceptable, never a mixed generation.
        for(int i=0;i<20;++i){
            std::array<float,7> block;block.fill(float(phase));
            const bool wet=concurrent.process(block.data(),block.size());
            for(auto v:block)assert(v==(wet?-float(phase):float(phase)));
        }
    }
    stop.store(true,std::memory_order_relaxed);worker.join();
    assert(successful==200);
    puts("DEEPFILTER_TRANSPORT_PASS=1");
}
