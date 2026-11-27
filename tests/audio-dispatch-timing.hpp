#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdint>

// Test-only measurement of synchronous obs_source_output_audio dispatch,
// including libobs and fixture capture. Not an isolated plugin callback.
class AudioDispatchTiming {
    using Clock=std::chrono::steady_clock;
    std::array<uint64_t,16384> times_{};
    size_t used_=0, omitted_=0, late_=0;
public:
    template<class F> void measure(uint64_t budget_ns,F&& dispatch){
        const auto start=Clock::now();
        dispatch();
        const auto ns=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count());
        if(ns>=budget_ns)++late_;
        if(used_<times_.size())times_[used_++]=ns;else ++omitted_;
    }
    void report(const char* label){
        if(!used_)return;
        std::sort(times_.begin(),times_.begin()+used_);
        auto percentile=[&](size_t p){return times_[(used_*p+99)/100-1]/1000.;};
        std::printf("SOUND_DISPATCH_TIMING effect=%s count=%zu omitted=%zu p50_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f over_packet_budget=%zu\n",
                    label,used_,omitted_,percentile(50),percentile(95),percentile(99),times_[used_-1]/1000.,late_);
    }
};
