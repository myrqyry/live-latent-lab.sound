#pragma once
#include <cstdint>
#include <cstddef>
#include <cmath>
namespace rvc_native {
inline constexpr uint32_t magic=0x52564333;
inline constexpr size_t input_frames=16000;
// Local little-endian IEEE float wire format, fixed 1s mono input.
struct Request {uint32_t magic,sequence,frames,rate;int32_t semitones;float retrieval,rms_mix,protection;uint32_t pitch_median;};
struct Reply {uint32_t magic,sequence,status,frames;};
static_assert(sizeof(Request)==36&&sizeof(Reply)==16);
inline bool valid(const Request& r){
    return r.magic==magic&&r.sequence&&r.frames==input_frames&&
        (r.rate==44100||r.rate==48000)&&r.semitones>=-24&&r.semitones<=24&&std::isfinite(r.retrieval)&&r.retrieval>=0&&r.retrieval<=1&&std::isfinite(r.rms_mix)&&r.rms_mix>=0&&r.rms_mix<=1&&
        std::isfinite(r.protection)&&r.protection>=0&&r.protection<=1&&r.pitch_median<=1;
}
}
