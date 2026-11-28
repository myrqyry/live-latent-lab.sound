#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

namespace stts {
// Worker-owned rolling history. No allocation or per-sample deque operations
// while waiting for speech. Export in chronological order only at onset.
template<size_t Capacity>
class SttPreroll {
    static_assert(Capacity>0);
    std::array<float,Capacity> samples_{};
    size_t next_=0,count_=0;
public:
    void clear() noexcept {next_=count_=0;}
    void push(const float* data,size_t n) noexcept {
        if(!data||!n)return;
        if(n>=Capacity){
            std::copy_n(data+n-Capacity,Capacity,samples_.data());
            next_=0;count_=Capacity;return;
        }
        const size_t first=std::min(n,Capacity-next_);
        std::copy_n(data,first,samples_.data()+next_);
        if(first<n)std::copy_n(data+first,n-first,samples_.data());
        next_=(next_+n)%Capacity;count_=std::min(Capacity,count_+n);
    }
    void copy_to(std::vector<float>& out) const {
        out.resize(count_);
        if(!count_)return;
        const size_t start=(next_+Capacity-count_)%Capacity;
        const size_t first=std::min(count_,Capacity-start);
        std::copy_n(samples_.data()+start,first,out.data());
        if(first<count_)std::copy_n(samples_.data(),count_-first,out.data()+first);
    }
};
}
