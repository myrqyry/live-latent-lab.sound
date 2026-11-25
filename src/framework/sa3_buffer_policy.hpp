#pragma once
#include <algorithm>
#include <cstdint>
namespace sa3buffer {
// A decaying high-water estimate includes model loading and inference queue
// wait. Keep at least a full second of margin, and keep two to three ready clips.
class Policy {
    uint64_t estimate_=0;
public:
    void observe(uint64_t ms){
        ms=std::min<uint64_t>(ms,3600000);
        estimate_=std::max(ms,estimate_*4/5);
    }
    unsigned target(uint64_t segment_ms)const{
        if(!segment_ms)return 3;
        return unsigned(std::clamp<uint64_t>(1+(estimate_+1000+segment_ms-1)/segment_ms,2,3));
    }
};
}
