#include "framework/live-stem-dispatch.hpp"
#include <cassert>
#include <thread>
#include <cstdio>
using namespace stts;
int main(){
    StemDispatchQueue q(3);
    for(unsigned i=0;i<3;++i){
        auto* p=q.reserve();assert(p);p->timestamp=i;p->frames=1;p->epoch=3;
        for(size_t ch=0;ch<8;++ch)p->planes[ch][0]=float(i*8+ch);
        q.publish();
    }
    assert(!q.reserve());
    for(unsigned i=0;i<3;++i){
        auto* p=q.peek();assert(p&&p->timestamp==i&&p->frames==1&&p->epoch==3);
        for(size_t ch=0;ch<8;++ch)assert(p->planes[ch][0]==float(i*8+ch));
        q.consume();
    }
    assert(!q.peek());
    std::thread producer([&]{
        for(unsigned i=0;i<20000;++i){
            StemDispatchPacket* p;
            while(!(p=q.reserve()))std::this_thread::yield();
            p->timestamp=i*1000000ULL;p->epoch=5;p->frames=7;
            for(size_t ch=0;ch<8;++ch)for(size_t f=0;f<7;++f)p->planes[ch][f]=float(i*64+ch*8+f);
            q.publish();
        }
    });
    for(unsigned i=0;i<20000;++i){
        const StemDispatchPacket* p;
        while(!(p=q.peek()))std::this_thread::yield();
        assert(p->timestamp==i*1000000ULL&&p->epoch==5&&p->frames==7);
        for(size_t ch=0;ch<8;++ch)for(size_t f=0;f<7;++f)assert(p->planes[ch][f]==float(i*64+ch*8+f));
        q.consume();
    }
    producer.join();assert(!q.peek());
    puts("STEM_DISPATCH_QUEUE_PASS=1");
}
