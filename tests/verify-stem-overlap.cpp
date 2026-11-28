#include "framework/stem-window-overlap.hpp"
#include "framework/live-stem-pipeline.hpp"
#include <cassert>
#include <cstdio>
#include <cmath>
#include <vector>
using namespace stts;
static void exact_stream(unsigned rate,bool overlap){
    LiveStemMixPipeline p(rate);
    const size_t win=p.window_frames(),ov=overlap?rate*2:0,hop=win-ov;
    StemWindowOverlap blend(win,8);
    blend.reset(ov);
    std::vector<float> l(733),r(733),batch(8*win);
    size_t clock=0,requests=0,origin=0,read_clock=0;
    auto feed=[&](size_t n,bool active,bool drain){
        for(size_t i=0;i<n;++i){l[i]=float((clock+i-origin)%4096)/4096;r[i]=-l[i];}
        p.process_mix(l.data(),r.data(),n,{0,0,0,1});
        for(size_t i=0;i<n;++i){
            float expected=active&&clock+i>=origin+p.delay_frames()?float((clock+i-origin-p.delay_frames())%4096)/4096:0;
            assert(std::abs(l[i]-expected)<1e-6f&&std::abs(r[i]+expected)<1e-6f);
        }
        clock+=n;
        if(drain)while(blend.capture([&](float* a,float* b,size_t count){
            return p.read_input_frames(a,b,count);
        })){
            for(size_t i=0;i<win;++i){
                float expected=float((read_clock+i)%4096)/4096;
                assert(blend.left()[i]==expected&&blend.right()[i]==-expected);
            }
            for(size_t ch=0;ch<8;++ch)
                for(size_t i=0;i<win;++i)batch[ch*win+i]=(ch%2?-1.f:1.f)*blend.left()[i];
            assert(blend.publish(batch.data(),batch.size(),[&](const float* data,size_t count,size_t frames){
                assert(frames==hop);
                return p.push_output_frames(data,count,frames);
            }));
            read_clock+=hop;++requests;
        }
    };
    auto run=[&](size_t n,bool active,bool drain){while(n){auto k=std::min(n,l.size());feed(k,active,drain);n-=k;}};
    run(rate*38,true,true);
    assert(requests==(rate*38-win)/hop+1);
    p.invalidate();run(rate,false,false);
    assert(p.restart());feed(37,false,false);assert(p.worker_prepare());
    origin=clock;read_clock=0;blend.reset(ov);
    run(rate*31,true,true);
}
int main(){
    for(unsigned rate:{44100u,48000u})for(bool overlap:{false,true})exact_stream(rate,overlap);
    // Distinct planes and disagreeing predictions: constant-sum crossfade
    // starts at previous estimate and ends at new estimate, no amplitude boost.
    StemWindowOverlap b(8,4);b.reset(2);
    float data[32]{};
    auto capture=[](float* l,float* r,size_t n){std::fill_n(l,n,0);std::fill_n(r,n,0);return true;};
    assert(b.capture(capture));
    for(size_t ch=0;ch<4;++ch)std::fill_n(data+ch*8,8,float(ch+1));
    assert(b.publish(data,32,[](const float* out,size_t count,size_t n){assert(n==6&&count==24);return out[0]==1&&out[18]==4;}));
    assert(b.capture(capture));
    for(size_t ch=0;ch<4;++ch)std::fill_n(data+ch*8,8,float(ch+5));
    assert(b.publish(data,32,[](const float* out,size_t,size_t n){
        for(size_t ch=0;ch<4;++ch){assert(out[ch*n]==float(ch+1));assert(out[ch*n+1]==float(ch+5));}
        return true;
    }));
    b.reset(2);assert(b.capture(capture));
    assert(b.publish(data,32,[](const float* out,size_t,size_t){return out[0]==5;}));
    // Model layouts may vary, but a malformed response may never advance
    // the common timeline. Reset must discard both context and output tail.
    for(size_t planes:{4u,8u,12u}){
        StemWindowOverlap x(8,planes);
        x.reset(4);
        std::vector<float> batch(planes*8,2.f);
        assert(x.next_capture()==8);
        assert(x.capture(capture));
        auto reject=[](const float*,size_t,size_t){return false;};
        assert(!x.publish(batch.data(),batch.size()-1,reject));
        assert(!x.publish(batch.data(),batch.size(),reject));
        x.reset(2);
        assert(x.next_capture()==8);
        assert(x.capture(capture));
        assert(x.publish(batch.data(),batch.size(),[&](const float* samples,size_t count,size_t n){
            return count==planes*6&&n==6&&std::all_of(samples,samples+count,[](float v){return v==2.f;});
        }));
        assert(x.next_capture()==6);
        x.reset(0);
        assert(x.next_capture()==8);
        assert(x.capture(capture));
        assert(x.publish(batch.data(),batch.size(),[&](const float* samples,size_t count,size_t n){
            return samples==batch.data()&&count==planes*8&&n==8;
        }));
    }
    puts("STEM_WINDOW_OVERLAP_PASS=1");
}
