#include "framework/rt_wake.hpp"
#include "framework/live-stem-pipeline.hpp"
#include "framework/ddsp_pipeline.hpp"
#include "framework/rvc_window.hpp"
#include "framework/deepfilter_transport.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
using namespace std::chrono_literals;
int main(){
    // Only wake the consumer for a complete unit of work, not every packet.
    for(size_t rate:{44100u,48000u}){
        ddsp::Pipeline ddsp(rate);
        rvc_native::Pipeline rvc(rate);
        stts::LiveStemPipeline stems(rate);
        const auto n=rate/100;
        std::vector<float> l(n),r(n);
        assert(!ddsp.input_ready() && !rvc.input_ready() && !stems.input_ready());
        ddsp.process(l.data(),r.data(),n,0);
        assert(!ddsp.input_ready());
        ddsp.process(l.data(),r.data(),n,0);
        assert(ddsp.input_ready());
        std::vector<float> dl(ddsp.hop()),dr(ddsp.hop());
        assert(ddsp.read(dl.data(),dr.data()) && !ddsp.input_ready());
        for(size_t i=0;i<50;++i){
            rvc.process(l.data(),r.data(),n,0);
            assert(rvc.input_ready()==(i==49));
        }
        for(size_t i=0;i<800;++i){
            stems.process(l.data(),r.data(),n);
            assert(stems.input_ready()==(i==799));
        }
    }
    // A generation acknowledgement is actionable even without a full block.
    stts::DeepFilterTransport df(16,16);
    float packet[4]={1,2,3,4},out[4]{};
    assert(!df.worker_ready(4));
    assert(!df.process(packet,2));
    assert(df.worker_ready(4));
    auto epoch=df.worker_prepare([]{});
    assert(epoch && !df.worker_ready(4));
    df.process(packet,2);
    assert(!df.worker_ready(4));
    df.process(packet+2,2);
    assert(df.worker_ready(4));
    assert(df.worker_read(epoch,out,4) && !df.worker_ready(4));
    df.invalidate();
    assert(!df.worker_ready(4));
    assert(!df.process(packet,4) && df.worker_ready(4));
    auto next=df.worker_prepare([]{});
    assert(next!=epoch && !df.worker_ready(4));
    stts::RtWake wake;
    auto token=wake.sequence();
    wake.notify();
    assert(wake.wait(token,250));
    token=wake.sequence();
    assert(!wake.wait(token,25));
    if constexpr(!stts::RtWake::event_driven){
        // The compatibility path retains timed polling, not native signaling.
        const auto observed=wake.sequence();
        std::thread producer([&]{wake.notify();});
        producer.join();
        assert(wake.wait(observed,4));
        assert(!wake.wait(wake.sequence(),4));
        std::cout<<"SOUND_RT_WAKE_POLLING_PASS=1\n";
        return 0;
    }
    stts::StereoSpscQueue queue(64);
    constexpr size_t blocks=10000;
    std::atomic<bool> stop{false};
    std::atomic<size_t> received{0},timeouts{0};
    std::thread reader([&]{
        float l[4],r[4];
        for(;;){
            auto ticket=wake.sequence();
            if(queue.pop(l,r,4)){
                const auto i=received.fetch_add(1);
                for(size_t j=0;j<4;++j){assert(l[j]==float(i));assert(r[j]==-float(i));}
            }else if(stop.load())break;
            else if(!wake.wait(ticket,500))++timeouts;
        }
    });
    for(size_t i=0;i<blocks;++i){
        float l[4],r[4];
        for(size_t j=0;j<4;++j){l[j]=float(i);r[j]=-float(i);}
        while(!queue.push(l,r,4))std::this_thread::yield();
        wake.notify();
    }
    while(received.load()!=blocks)std::this_thread::yield();
    stop=true;wake.notify();reader.join();
    assert(received==blocks && timeouts==0);
    std::cout<<"SOUND_RT_WAKE_PASS=1\n";
}
