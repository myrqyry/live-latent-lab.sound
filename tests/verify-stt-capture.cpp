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
    stts::SttCapture concurrent(8);
    constexpr uint64_t count=50000;
    std::thread producer([&]{
        stts::SttBlock data;
        data.count=4;
        for(uint64_t i=0;i<count;++i){
            data.epoch=i/1000;data.timestamp=i;
            data.samples[0]=float(i);
            while(!concurrent.push(data))std::this_thread::yield();
        }
    });
    for(uint64_t i=0;i<count;++i){
        stts::SttBlock data;
        while(!concurrent.pop(data))std::this_thread::yield();
        assert(data.timestamp==i&&data.epoch==i/1000&&data.count==4&&data.samples[0]==float(i));
    }
    producer.join();
    std::cout<<"SOUND_STT_CAPTURE_PASS=1\n";
}
