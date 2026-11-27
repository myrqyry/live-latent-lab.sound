#include "framework/demucs_rt.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <new>

static bool realtime=false;
void* operator new(std::size_t n) {
    if(realtime) std::abort();
    if(auto* p=std::malloc(n?n:1))return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept {if(realtime)std::abort();std::free(p);}
void operator delete[](void* p) noexcept {::operator delete(p);}
void operator delete(void* p,std::size_t) noexcept {::operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept {::operator delete(p);}

int main(){
    stts::DemucsCapture capture(7);
    stts::DemucsStemOutput stem(7);
    std::array<float,7> input{1,2,3,4,5,6,7},out{};
    uint64_t epoch=0;
    assert(stem.claim() && !stem.claim());
    realtime=true;
    assert(capture.push(input.data(),7));
    assert(!capture.push(input.data(),1)); // no partial write, gap recorded
    assert(!capture.read_chunk(out.data(),7,epoch)); // stale queued samples dropped
    assert(epoch==1);
    assert(capture.push(input.data(),7));
    assert(capture.read_chunk(out.data(),7,epoch) && out==input);
    capture.mark_gap();
    assert(!capture.read_chunk(out.data(),7,epoch) && epoch==2);
    assert(stem.publish(input.data(),7));
    assert(!stem.publish(input.data(),1)); // no partial batch
    stem.render(out.data(),3);
    assert(out[0]==1 && out[1]==2 && out[2]==3);
    stem.render(out.data(),7);
    assert((out==std::array<float,7>{4,5,6,7,0,0,0}));
    assert(stem.publish(input.data(),7));
    stem.close();
    stem.render(out.data(),7);
    assert((out==std::array<float,7>{}));
    assert(!stem.publish(input.data(),7));
    realtime=false;
    stem.release();
    assert(!stem.claim()); // closed parent cannot be rebound
    puts("DEMUCS_RT_CAPTURE_AND_STEM_PASS=1");
}
