#pragma once
#include "ring-buffer.hpp"
#include <cstdint>

namespace stts {
// Single audio producer, single inference consumer. Even epochs are paused;
// odd epochs admit new capture only after both actors acknowledge them.
// A model/result belongs to exactly one epoch. Never reset both ring cursors.
class ClassifierCapture {
    RingBuffer input_;
    std::atomic<uint64_t> epoch_{2},audio_ack_{0},worker_ack_{0};
public:
    explicit ClassifierCapture(size_t capacity):input_(capacity){}
    bool current(uint64_t epoch)const noexcept {
        return (epoch&1)&&epoch_.load(std::memory_order_acquire)==epoch;
    }
    uint64_t epoch()const noexcept{return epoch_.load(std::memory_order_acquire);}
    // Worker/control: invalidate only the observed generation.
    void fault(uint64_t epoch)noexcept{
        if(epoch&1)epoch_.compare_exchange_strong(epoch,epoch+1,
            std::memory_order_acq_rel,std::memory_order_acquire);
    }
    void pause()noexcept{fault(epoch());}
    // Worker: serialized with control revisions by the caller.
    uint64_t activate()noexcept{
        auto e=epoch();
        if(e&1)return 0;
        return epoch_.compare_exchange_strong(e,e+1,std::memory_order_acq_rel,
            std::memory_order_acquire)?e+1:0;
    }
    // Callback begins a packet once, so a concurrent restart cannot splice
    // its remaining samples into the next generation.
    uint64_t begin_packet()noexcept{
        auto e=epoch();
        if(!(e&1))return 0;
        if(audio_ack_.load(std::memory_order_relaxed)!=e)
            audio_ack_.store(e,std::memory_order_release);
        return worker_ack_.load(std::memory_order_acquire)==e&&current(e)?e:0;
    }
    bool worker_ready()const noexcept{
        auto e=epoch();
        return (e&1)&&audio_ack_.load(std::memory_order_acquire)==e&&
            (worker_ack_.load(std::memory_order_acquire)!=e||input_.available());
    }
    // Consumer only: the producer has finished all old writes before ack.
    bool prepare(uint64_t e)noexcept{
        if(!current(e)||audio_ack_.load(std::memory_order_acquire)!=e)return false;
        if(worker_ack_.load(std::memory_order_relaxed)!=e){
            input_.discard_available();
            worker_ack_.store(e,std::memory_order_release);
        }
        return current(e);
    }
    size_t write(uint64_t e,const float* data,size_t n)noexcept{
        if(!current(e))return 0;
        auto written=input_.write(data,n);
        if(written!=n)fault(e);
        return written;
    }
    size_t read(uint64_t e,float* data,size_t n)noexcept{
        if(!current(e)||worker_ack_.load(std::memory_order_acquire)!=e)return 0;
        auto count=input_.read(data,n);
        return current(e)?count:0;
    }
};
}
