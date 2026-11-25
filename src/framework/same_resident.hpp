#pragma once
#include <cstdint>
#include <limits>
namespace same_resident {
constexpr uint32_t magic=0x53414d45;
constexpr uint32_t max_frames=12*44100,min_frames=8192;
struct Request {uint32_t magic,sequence,frames,mode;float amount,position;uint32_t radius,grain;};
struct Reply {uint32_t magic,sequence,status,frames,builds,processing_ms;};
static_assert(sizeof(Request)==32&&sizeof(Reply)==24&&sizeof(float)==4);
static_assert(__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__&&std::numeric_limits<float>::is_iec559);
}
