#pragma once
#include <atomic>
#include <cstdint>
#include <cerrno>
#include <stdexcept>
#if defined(__linux__) && !defined(SOUND_RT_WAKE_POLLING)
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#else
#include <chrono>
#include <thread>
#endif

namespace stts {

// Audio/control producers and one sleeping worker. No mutex, allocation,
// or blocking syscall on the audio callback: eventfd writes are nonblocking
// and only attempted while the worker is parked.
#if defined(__linux__) && !defined(SOUND_RT_WAKE_POLLING)
class RtWake {
    int fd_=-1;
    std::atomic<uint64_t> sequence_{0};
    std::atomic<bool> waiting_{false};
public:
    static constexpr bool event_driven=true;
    RtWake() {
        fd_=::eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);
        if(fd_<0)throw std::runtime_error("Audio eventfd initialization failed");
    }
    RtWake(const RtWake&)=delete;
    RtWake& operator=(const RtWake&)=delete;
    ~RtWake(){::close(fd_);}

    uint64_t sequence() const noexcept {
        return sequence_.load(std::memory_order_seq_cst);
    }
    void notify() noexcept {
        sequence_.fetch_add(1,std::memory_order_seq_cst);
        if(waiting_.load(std::memory_order_seq_cst)){
            const uint64_t one=1;
            // EINTR could otherwise strand an indefinitely parked worker.
            // EAGAIN is safe: a saturated eventfd is already readable.
            ssize_t result;
            do {result=::write(fd_,&one,sizeof(one));}
            while(result<0&&errno==EINTR);
        }
    }
    bool wait(uint64_t observed,int timeout_ms) noexcept {
        waiting_.store(true,std::memory_order_seq_cst);
        bool signalled=sequence_.load(std::memory_order_seq_cst)!=observed;
        if(!signalled){
            pollfd pfd{fd_,POLLIN,0};
            signalled=::poll(&pfd,1,timeout_ms)>0;
        }
        waiting_.store(false,std::memory_order_seq_cst);
        // Drain any coalesced notifications before a future park attempt.
        uint64_t count=0;
        while(::read(fd_,&count,sizeof(count))==sizeof(count)){}
        return signalled;
    }
};
#else
// Keep the previous bounded worker polling on platforms where eventfd is
// unavailable. The callback only publishes a sequence, never a mutex/CV.
class RtWake {
    std::atomic<uint64_t> sequence_{0};
public:
    static constexpr bool event_driven=false;
    uint64_t sequence() const noexcept {return sequence_.load(std::memory_order_seq_cst);}
    void notify() noexcept {sequence_.fetch_add(1,std::memory_order_seq_cst);}
    bool wait(uint64_t observed,int timeout_ms) noexcept {
        if(sequence()!=observed)return true;
        if(timeout_ms>=0){
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
            return sequence()!=observed;
        }
        while(sequence()==observed)std::this_thread::sleep_for(std::chrono::milliseconds(4));
        return true;
    }
};
#endif
} // namespace stts
