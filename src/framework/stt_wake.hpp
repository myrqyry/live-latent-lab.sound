#pragma once
#include <atomic>
#include <cstdint>
#include <cerrno>
#include <stdexcept>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace stts {

// One producer (OBS audio) and one sleeping worker. No mutex, allocation,
// or blocking syscall on the audio callback: eventfd writes are nonblocking
// and only attempted while the worker is parked.
class SttWake {
    int fd_=-1;
    std::atomic<uint64_t> sequence_{0};
    std::atomic<bool> waiting_{false};
public:
    SttWake() {
        fd_=::eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);
        if(fd_<0)throw std::runtime_error("STT eventfd initialization failed");
    }
    SttWake(const SttWake&)=delete;
    SttWake& operator=(const SttWake&)=delete;
    ~SttWake(){::close(fd_);}

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
} // namespace stts
