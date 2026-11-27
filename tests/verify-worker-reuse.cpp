#include "framework/ddsp_pipeline.hpp"
#include "framework/sola_stitcher.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <limits>
#include <random>
static bool forbid_heap=false;
void* operator new(size_t n){if(forbid_heap)std::abort();if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{if(forbid_heap)std::abort();std::free(p);}
void operator delete[](void* p) noexcept{::operator delete(p);}
void operator delete(void* p,size_t) noexcept{::operator delete(p);}
void operator delete[](void* p,size_t) noexcept{::operator delete(p);}
int main(){
    for(size_t rate:{44100u,48000u}){
        ddsp::Analysis analysis(rate);
        std::vector<float> l(rate/50,.25f),r(l.size(),.5f);
        forbid_heap=true;
        for(int i=0;i<5;++i)analysis.push(l.data(),r.data());
        auto& a=analysis.push(l.data(),r.data());
        for(auto x:a)assert(std::abs(x-.375f)<1e-5f);
        forbid_heap=false;
        // Independently compute the old causal FIR from a raw input stream.
        // Exercise clipping, non-finite sanitization and hop history.
        ddsp::Analysis reference_test(rate);
        std::vector<float> raw(l.size()+48,0.f);
        std::array<float,1024> history{};
        std::mt19937 rng(1835);
        std::uniform_real_distribution<float> dist(-1.8f,1.8f);
        constexpr double pi=3.14159265358979323846;
        const double fc=7200./rate;
        for(int j=0;j<20;++j){
            for(size_t i=0;i<l.size();++i){l[i]=dist(rng);r[i]=dist(rng);}
            l[5]=std::numeric_limits<float>::quiet_NaN();
            r[14]=std::numeric_limits<float>::infinity();
            for(size_t i=0;i<l.size();++i){
                const float v=(l[i]+r[i])*.5f;
                raw[48+i]=std::isfinite(v)?std::clamp(v,-1.f,1.f):0.f;
            }
            std::move(history.begin()+320,history.end(),history.begin());
            for(size_t i=0;i<320;++i){
                const double pos=double(i)*l.size()/320.,frac=pos-std::floor(pos);
                std::array<float,48> weights{};
                double total=0;
                for(int k=0;k<48;++k){
                    const double t=k-23.5+frac;
                    const double v=(std::abs(t)<1e-8?2*fc:std::sin(2*pi*fc*t)/(pi*t))*(.5-.5*std::cos(2*pi*k/47.));
                    weights[k]=float(v);total+=v;
                }
                float sum=0;
                for(int k=0;k<48;++k)sum+=(weights[k]/float(total))*raw[size_t(int(i*l.size()/320)-k+48)];
                history[704+i]=sum;
            }
            forbid_heap=true;
            const auto& result=reference_test.push(l.data(),r.data());
            forbid_heap=false;
            assert(result==history);
            std::copy_n(raw.begin()+l.size(),48,raw.begin());
        }
        const size_t hop=rate/2,overlap=rate/25,search=rate/50;
        stts::SolaStitcher owned({hop,overlap,search}),reused({hop,overlap,search});
        std::vector<float> input(hop+overlap+search),out(hop);
        for(int j=0;j<10;++j){
            for(size_t i=0;i<input.size();++i)
                input[i]=j==3?0.f:float(.3*std::sin((i+j*hop+17*j)*.031)+.1*std::sin((i+j*hop)*.014));
            auto expected=owned.push(input.data(),input.size());
            forbid_heap=true;
            auto result=reused.push_into(input.data(),input.size(),out.data(),out.size());
            forbid_heap=false;
            assert(out==expected.pcm&&result.offset_frames==expected.offset_frames&&result.alignment_used==expected.alignment_used);
        }
        // A rejected output buffer must not alter the saved overlap.
        bool rejected=false;
        try{reused.push_into(input.data(),input.size(),out.data(),out.size()-1);}
        catch(const std::invalid_argument&){rejected=true;}
        assert(rejected);
        auto expected=owned.push(input.data(),input.size());
        auto actual=reused.push_into(input.data(),input.size(),out.data(),out.size());
        assert(out==expected.pcm&&actual.offset_frames==expected.offset_frames);
        reused.reset();
        forbid_heap=true;
        auto reset=reused.push_into(input.data(),input.size(),out.data(),out.size());
        forbid_heap=false;
        assert(!reset.alignment_used&&reset.offset_frames==0);
        assert(std::equal(out.begin(),out.end(),input.begin()));
    }
    puts("SOUND_WORKER_REUSE_PASS=1");
}
