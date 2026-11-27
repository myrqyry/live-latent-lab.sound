#include "framework/ring-buffer.hpp"
#include "framework/live-stem-pipeline.hpp"
#include <cassert>
#include <cstdlib>
#include <new>
#include <cstdio>
#include <array>

// Fail on C++ heap activity inside the isolated queue operation window.
static bool realtime = false;
void* operator new(std::size_t n) {
    if (realtime) std::abort();
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { if (realtime) std::abort(); std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

int main() {
    stts::RingBuffer mono(7);
    stts::StereoSpscQueue stereo(7);
    const float left[] = {1,2,3,4,5,6,7};
    const float right[] = {-1,-2,-3,-4,-5,-6,-7};
    float a[7]{}, b[7]{};
    realtime = true;
    for (int round=0; round<500; ++round) {
        assert(mono.write(left,7)==7);
        assert(stereo.push(left,right,7));
        assert(mono.read(a,3)==3);
        for(int i=0;i<3;++i) assert(a[i]==left[i]);
        assert(stereo.pop(a,b,3));
        for(int i=0;i<3;++i) assert(a[i]==left[i] && b[i]==right[i]);
        assert(mono.write(left,4)==3);
        assert(!stereo.push(left,right,4));
        assert(stereo.push(left,right,3));
        assert(mono.read(a,7)==7);
        for(int i=0;i<7;++i) assert(a[i]==left[(i+3)%7]);
        assert(stereo.pop(a,b,7));
        for(int i=0;i<7;++i) assert(a[i]==left[(i+3)%7] && b[i]==right[(i+3)%7]);
        assert(mono.read(a,1)==0 && !stereo.pop(a,b,1));
    }
    realtime = false;
    std::puts("RT_QUEUE_WRAP_AND_CPP_HEAP_PASS=1");
}
