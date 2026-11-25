#pragma once
#include <cstdint>
#include <cstddef>
namespace rvc_native {
inline constexpr uint32_t magic=0x52564331;
inline constexpr size_t input_frames=16000;
// Local little-endian IEEE float wire format, fixed 1s mono input.
struct Request {uint32_t magic,sequence,frames,rate;int32_t semitones;};
struct Reply {uint32_t magic,sequence,status,frames;};
static_assert(sizeof(Request)==20&&sizeof(Reply)==16);
inline bool valid(const Request& r){
    return r.magic==magic&&r.sequence&&r.frames==input_frames&&
        (r.rate==44100||r.rate==48000)&&r.semitones>=-24&&r.semitones<=24;
}
}
