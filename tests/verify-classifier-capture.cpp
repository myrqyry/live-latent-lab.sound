#include "framework/classifier_capture.hpp"
#include <cassert>
#include <thread>
#include <atomic>
#include <cstdio>
int main(){
    stts::ClassifierCapture capture(16);
    float old[8]={1,1,1,1,1,1,1,1}, fresh[8]={2,2,2,2,2,2,2,2}, out[8]{};
    assert(!capture.begin_packet());
    auto first=capture.activate();assert(first&&capture.current(first));
    assert(!capture.begin_packet());assert(capture.prepare(first));
    assert(capture.begin_packet()==first&&capture.write(first,old,8)==8);
    assert(capture.read(first,out,8)==8&&out[0]==1);
    assert(capture.write(first,old,8)==8);
    std::atomic<int> stage{0};
    std::thread worker([&]{
        assert(capture.read(first,out,4)==4);
        stage=1;while(stage.load()!=2)std::this_thread::yield();
        assert(!capture.current(first));
        assert(!capture.read(first,out,4));
    });
    while(stage.load()!=1)std::this_thread::yield();
    capture.pause();stage=2;worker.join();
    auto second=capture.activate();assert(second!=first);
    assert(!capture.prepare(second));
    assert(!capture.begin_packet());assert(capture.prepare(second));
    assert(!capture.read(second,out,8)); // discarded old tail
    assert(capture.begin_packet()==second&&capture.write(second,fresh,8)==8);
    assert(capture.read(second,out,8)==8&&out[0]==2);
    assert(capture.write(second,fresh,8)==8);
    assert(capture.write(second,fresh,8)==8);
    assert(capture.write(second,fresh,8)==8);
    assert(capture.write(second,fresh,8)<8);
    assert(!capture.current(second)&&!capture.begin_packet());
    auto third=capture.activate();assert(third!=second);
    capture.pause();assert(!capture.prepare(third));
    puts("CLASSIFIER_CAPTURE_RECOVERY_PASS=1");
}
