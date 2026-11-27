#include "framework/stt_capture.hpp"
#include <atomic>
#include <cassert>
#include <iostream>
#include <thread>
int main(){
    stts::SttCapture q(2);
    stts::SttBlock a{},b{},c{};
    a.epoch=1;a.timestamp=10;a.count=1;a.samples[0]=.25f;
    b=a;b.epoch=2;b.timestamp=20;b.samples[0]=.5f;
    assert(q.push(a)&&q.push(b)&&!q.push(b));
    assert(q.pop(c)&&c.epoch==1&&c.timestamp==10&&c.samples[0]==.25f);
    assert(q.push(a));
    assert(q.pop(c)&&c.epoch==2&&c.timestamp==20&&c.samples[0]==.5f);
    assert(q.pop(c)&&c.epoch==1&&!q.pop(c));
    // Direct writer sees a queue slot only when there is room. A rejected
    // writer never runs, and only the valid sample prefix is transferred.
    stts::SttCapture direct(1);
    int writes=0;
    assert(direct.emplace(3,30,2,[&](float* dst){++writes;dst[0]=.3f;dst[1]=-.4f;}));
    assert(!direct.emplace(4,40,2,[&](float*){++writes;}));
    assert(writes==1);
    c.samples.fill(99.f);
    assert(direct.pop(c)&&c.epoch==3&&c.timestamp==30&&c.count==2);
    assert(c.samples[0]==.3f&&c.samples[1]==-.4f&&c.samples[2]==99.f);
    assert(!direct.emplace(1,0,1025,[&](float*){++writes;}));
    assert(!direct.emplace(1,0,0,[&](float*){++writes;}));
    assert(writes==1);
    stts::SttCapture concurrent(8);
    constexpr uint64_t count=50000;
    std::thread producer([&]{
        stts::SttBlock data;
        data.count=4;
        for(uint64_t i=0;i<count;++i){
            data.epoch=i/1000;data.timestamp=i;
            while(!concurrent.emplace(data.epoch,data.timestamp,data.count,[&](float* dst){
                for(size_t j=0;j<data.count;++j)dst[j]=float(i+j);
            }))std::this_thread::yield();
        }
    });
    for(uint64_t i=0;i<count;++i){
        stts::SttBlock data;
        while(!concurrent.pop(data))std::this_thread::yield();
        assert(data.timestamp==i&&data.epoch==i/1000&&data.count==4&&data.samples[0]==float(i)&&data.samples[3]==float(i+3));
    }
    producer.join();
    std::cout<<"SOUND_STT_CAPTURE_PASS=1\n";
}
