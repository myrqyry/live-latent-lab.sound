#include "framework/stt_wake.hpp"
#include "framework/stt_capture.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
using namespace std::chrono_literals;

int main() {
    stts::SttWake wake;

    // Waiting with no work must time out rather than spin.
    auto token = wake.sequence();
    auto before = std::chrono::steady_clock::now();
    bool ready = wake.wait(token, 35);
    auto elapsed = std::chrono::steady_clock::now() - before;
    assert(!ready);
    assert(elapsed >= 20ms && elapsed < 2s);

    // A notification between checking the queue and entering the wait
    // must never disappear, even if no other producer event follows it.
    token = wake.sequence();
    wake.notify();
    before = std::chrono::steady_clock::now();
    assert(wake.wait(token, 250));
    assert(std::chrono::steady_clock::now() - before < 100ms);

    // Real sleep with no future polling: another thread wakes the waiter.
    std::atomic<bool> woke{false};
    token = wake.sequence();
    std::thread waiter([&] { woke.store(wake.wait(token, 1500)); });
    std::this_thread::sleep_for(25ms);
    wake.notify();
    waiter.join();
    assert(woke.load());

    // A burst of producer notifications while idle must coalesce, not
    // accumulate a long backlog of wake tokens to drain.
    for (int i=0;i<50000;++i) wake.notify();
    token = wake.sequence();
    before = std::chrono::steady_clock::now();
    assert(!wake.wait(token, 30));
    assert(std::chrono::steady_clock::now()-before >= 20ms);

    // Simulates shutdown: indefinite wait must be released explicitly.
    token = wake.sequence();
    std::thread shutdown([&] { woke.store(wake.wait(token, -1)); });
    std::this_thread::sleep_for(25ms);
    wake.notify();
    shutdown.join();
    assert(woke.load());

    // Exercise the real SPSC queue's producer/consumer handoff under burst
    // pressure. A missed notification would strand the final audio block.
    stts::SttCapture queue(8);
    constexpr uint64_t blocks=5000;
    std::atomic<int> unexpected_timeouts{0};
    std::thread reader([&]{
        for(uint64_t i=0;i<blocks;){
            const auto ticket=wake.sequence();
            stts::SttBlock b;
            if(queue.pop(b)){assert(b.timestamp==i);++i;}
            else if(!wake.wait(ticket,500))++unexpected_timeouts;
        }
    });
    for(uint64_t i=0;i<blocks;++i){
        stts::SttBlock b;b.timestamp=i;b.count=1;
        while(!queue.push(b))std::this_thread::yield();
        wake.notify();
    }
    reader.join();
    assert(unexpected_timeouts.load()==0);
    std::cout << "SOUND_STT_WAKE_PASS=1\n";
}
