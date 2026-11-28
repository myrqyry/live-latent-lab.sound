#include "framework/stt_capture.hpp"
#include "framework/stt_preroll.hpp"
#include <deque>
#include <vector>
#include <atomic>
#include <cassert>
#include <iostream>
#include <thread>
static void preroll(){
    stts::SttPreroll<3200> window;
    std::deque<float> reference;
    std::vector<float> actual;
    std::array<float,4096> packet{};
    for(size_t i=0;i<200;++i){
        size_t n=i%11==0?4096:i%3==0?17:320;
        for(size_t j=0;j<n;++j)packet[j]=float(i*4096+j);
        window.push(packet.data(),n);
        for(size_t j=0;j<n;++j)reference.push_back(packet[j]);
        while(reference.size()>3200)reference.pop_front();
        window.copy_to(actual);
        assert(actual.size()==reference.size()&&std::equal(actual.begin(),actual.end(),reference.begin()));
        if(i%19==0){window.clear();reference.clear();window.copy_to(actual);assert(actual.empty());}
    }
    window.push(nullptr,0);window.clear();window.copy_to(actual);assert(actual.empty());
    std::cout<<"SOUND_STT_PREROLL_PASS=1\n";
}
int main(){
    preroll();
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
