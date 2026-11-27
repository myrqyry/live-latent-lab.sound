#include "framework/live-stem-pipeline.hpp"
#include <cassert>
#include <atomic>
#include <thread>
#include <cstdio>
int main(){
 for(unsigned rate:{44100u,48000u}){
    stts::LiveStemPipeline p(rate);
    auto window=p.window_frames(),delay=p.delay_frames();
    std::vector<float> l(1025),r(1025),in_l(window),in_r(window),out_l(window,.125f),out_r(window,-.25f);
    size_t clock=0,origin=0,captured=0;
    auto feed=[&](size_t n,bool active,bool drain,float expected_l,float expected_r){
        std::fill_n(l.data(),n,.75f);std::fill_n(r.data(),n,-.5f);
        p.process(l.data(),r.data(),n);
        for(size_t i=0;i<n;++i){
            bool wet=active&&clock+i>=origin+delay;
            assert(l[i]==(wet?expected_l:0.f)&&r[i]==(wet?expected_r:0.f));
        }
        clock+=n;
        if(drain)while(p.read_input_window(in_l.data(),in_r.data())){
            assert(in_l.front()==.75f&&in_r.back()==-.5f);
            captured+=window;
            assert(p.push_output_window(out_l.data(),out_r.data()));
        }
    };
    auto run=[&](size_t n,bool active,bool drain,float a,float b){
        while(n){size_t k=std::min(n,size_t(733));feed(k,active,drain,a,b);n-=k;}
    };
    run(delay+rate,true,true,.125f,-.25f);
    // The worker has published a result before the fault, and still has
    // unread old capture. No old audio may be played on a fresh epoch.
    auto old=p.epoch();p.invalidate();assert(!p.ok());
    assert(!p.push_output_window(out_l.data(),out_r.data()));
    run(rate,false,false,0,0);
    assert(p.restart());auto next=p.epoch();assert(next!=old);
    assert(!p.worker_prepare());
    feed(37,false,false,0,0);
    assert(p.input_ready()&&p.worker_prepare());
    origin=clock;captured=0;
    std::fill(out_l.begin(),out_l.end(),-.125f);std::fill(out_r.begin(),out_r.end(),.375f);
    run(window-1,true,false,-.125f,.375f);assert(!p.input_ready());
    feed(1,true,true,-.125f,.375f);assert(captured==window);
    run(delay-window+rate,true,true,-.125f,.375f);
    p.invalidate();assert(!p.push_output_window(out_l.data(),out_r.data()));
    run(rate,false,false,0,0);
    assert(p.restart());p.invalidate();assert(!p.worker_prepare());
 }
 puts("STEM_GENERATION_RECOVERY_PASS=1");
}
