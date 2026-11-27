#pragma once
#include <cstddef>

namespace stts {
// Worker-owned retry policy. Only actual successful work replenishes the
// budget; merely loading a model or waiting idle does not count as recovery.
class WorkerRecovery {
    const size_t stable_work_;
    unsigned attempts_=0;
    size_t healthy_=0;
public:
    explicit WorkerRecovery(size_t stable_work):stable_work_(stable_work?stable_work:1){}
    void reset() noexcept {attempts_=0;healthy_=0;}
    bool can_retry(bool initialized) const noexcept {
        return (initialized||attempts_>0)&&attempts_<3;
    }
    unsigned delay_ms() const noexcept {return 250u<<(attempts_<3?attempts_:2);}
    void on_fault() noexcept {healthy_=0;}
    void on_retry() noexcept {if(attempts_<3)++attempts_;healthy_=0;}
    void on_success() noexcept {
        if(attempts_&&++healthy_>=stable_work_)reset();
    }
};
}
