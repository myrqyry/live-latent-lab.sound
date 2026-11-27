#pragma once
#include "live-stem-pipeline.hpp"
#include "ring-buffer.hpp"
#include <cmath>
#include <array>
namespace ddsp {
// All queues are SPSC. Fixed latency, with phase-aligned delayed dry on
// a missed deadline. No heap allocation or locking in process().
class Pipeline {
    const size_t rate_,hop_,delay_;
    stts::StereoSpscQueue input_;
    stts::RingBuffer wet_;
    std::vector<float> dry_l_,dry_r_;
    size_t cursor_=0;
    uint64_t count_=0;
    // Odd states admit audio, even states are faulted. A restart advances
    // to a fresh odd state. Each consumer discards only its own queue.
    std::atomic<uint64_t> state_{3},audio_ack_{3},worker_ack_{3};
    uint64_t worker_epoch_=3; // worker only
    uint64_t render_epoch_=3,wet_start_=0; // callback only
    float previous_mix_=0;
    void fail_epoch(uint64_t epoch) noexcept {
        if(epoch&1)state_.compare_exchange_strong(epoch,epoch+1,
            std::memory_order_acq_rel,std::memory_order_acquire);
    }
public:
    explicit Pipeline(size_t rate):rate_(rate),hop_(rate/50),delay_(rate/5),
        input_(rate/2),wet_(rate/2),dry_l_(delay_),dry_r_(delay_){}
    size_t hop()const{return hop_;}size_t delay()const{return delay_;}
    bool failed()const{return !(state_.load(std::memory_order_acquire)&1);}
    void fail(){fail_epoch(state_.load(std::memory_order_acquire));}
    // Worker only, after the model/recurrent state has been recreated.
    bool restart(){
        auto epoch=state_.load(std::memory_order_acquire);
        return !(epoch&1)&&state_.compare_exchange_strong(epoch,epoch+1,
            std::memory_order_acq_rel,std::memory_order_acquire);
    }
    bool input_ready()const{
        const auto epoch=state_.load(std::memory_order_acquire);
        return (epoch&1)&&audio_ack_.load(std::memory_order_acquire)==epoch&&
            (worker_ack_.load(std::memory_order_acquire)!=epoch||input_.available()>=hop_);
    }
    // Audio has stopped publishing old input before acknowledging the
    // new epoch. Only then may the worker discard it and permit capture.
    bool worker_prepare(){
        const auto epoch=state_.load(std::memory_order_acquire);
        if(!(epoch&1)||audio_ack_.load(std::memory_order_acquire)!=epoch)return false;
        if(worker_epoch_!=epoch){
            input_.discard_available();
            worker_epoch_=epoch;
            worker_ack_.store(epoch,std::memory_order_release);
        }
        return state_.load(std::memory_order_acquire)==epoch;
    }
    bool read(float* l,float* r){
        const auto epoch=worker_epoch_;
        return (epoch&1)&&state_.load(std::memory_order_acquire)==epoch&&
            input_.pop(l,r,hop_)&&state_.load(std::memory_order_acquire)==epoch;
    }
    bool write(const float* v){
        const auto epoch=worker_epoch_;
        if(!(epoch&1)||state_.load(std::memory_order_acquire)!=epoch)return false;
        if(!wet_.write_exact(v,hop_)){fail_epoch(epoch);return false;}
        return state_.load(std::memory_order_acquire)==epoch;
    }
    void process(float* l,float* r,size_t n,float mix){
        if(!l||!n)return;
        if(!r)r=l;
        const auto epoch=state_.load(std::memory_order_acquire);
        bool capture=false;
        if(epoch&1){
            if(audio_ack_.load(std::memory_order_relaxed)!=epoch)
                audio_ack_.store(epoch,std::memory_order_release);
            if(worker_ack_.load(std::memory_order_acquire)==epoch){
                if(render_epoch_!=epoch){
                    // Worker has stopped old output publication. The dry
                    // timeline is untouched; new wet starts at count_+delay_.
                    wet_.discard_available();
                    render_epoch_=epoch;wet_start_=count_;previous_mix_=0;
                }
                capture=state_.load(std::memory_order_acquire)==epoch;
                if(capture&&!input_.push(l,r,n)){fail_epoch(epoch);capture=false;}
            }
        }
        const float step=1.f/float(rate_/100);
        // A bounded stack block replaces per-sample atomic queue operations.
        // Do not consume wet audio during warmup; preserve a valid prefix
        // on underrun, then fall back to delayed dry until a fresh epoch is ready.
        std::array<float,256> wet_block;
        for(size_t offset=0;offset<n;){
            size_t block=std::min(wet_block.size(),n-offset);
            const bool warmup=count_-wet_start_<delay_;
            if(warmup)block=std::min(block,delay_-size_t(count_-wet_start_));
            const bool can_read=capture&&!warmup&&state_.load(std::memory_order_acquire)==epoch;
            size_t valid=can_read?wet_.read(wet_block.data(),block):0;
            if(can_read&&state_.load(std::memory_order_acquire)!=epoch)valid=0;
            if(can_read&&valid<block)fail_epoch(epoch);
            for(size_t j=0;j<block;++j){
                const size_t i=offset+j;
                const float a=dry_l_[cursor_],b=dry_r_[cursor_];
                dry_l_[cursor_]=std::isfinite(l[i])?l[i]:0;
                dry_r_[cursor_]=std::isfinite(r[i])?r[i]:0;
                if(++cursor_==delay_)cursor_=0;
                if(j<valid){
                    previous_mix_+=std::clamp(mix-previous_mix_,-step,step);
                    const float wet=wet_block[j];
                    l[i]=a+(wet-a)*previous_mix_;r[i]=b+(wet-b)*previous_mix_;
                }else{previous_mix_=0;l[i]=a;r[i]=b;}
            }
            count_+=block;offset+=block;
        }
    }
};
// Rational hop conversion from host sample rate to 16 kHz using a
// causal low-pass FIR. The window carries 704 samples across hops.
class Analysis {
    std::array<float,1024> history_{};
    std::vector<std::array<float,48>> weights_;
    size_t hop_;
    std::vector<float> mono_;
public:
    explicit Analysis(size_t rate):weights_(320),hop_(rate/50),mono_(hop_+48){
        constexpr double pi=3.14159265358979323846;
        const double fc=7200./rate;
        for(size_t i=0;i<320;++i){
            double pos=double(i)*hop_/320.,frac=pos-std::floor(pos),sum=0;
            for(int j=0;j<48;++j){
                double t=j-23.5+frac;
                double v=(std::abs(t)<1e-8?2*fc:std::sin(2*pi*fc*t)/(pi*t))*(.5-.5*std::cos(2*pi*j/47.));
                weights_[i][j]=float(v);sum+=v;
            }
            for(auto& v:weights_[i])v/=float(sum);
        }
    }
    const std::array<float,1024>& push(const float* l,const float* r){
        std::move(history_.begin()+320,history_.end(),history_.begin());
        // Sanitize each input sample once instead of repeating the stereo
        // downmix and finite check for every FIR tap. First 48 are history.
        for(size_t i=0;i<hop_;++i){
            float v=(l[i]+r[i])*.5f;
            mono_[48+i]=std::isfinite(v)?std::clamp(v,-1.f,1.f):0.f;
        }
        for(size_t i=0;i<320;++i){
            int pos=int(i*hop_/320);float sum=0;
            for(int j=0;j<48;++j){sum+=weights_[i][j]*mono_[size_t(pos-j+48)];}
            history_[704+i]=sum;
        }
        std::copy_n(mono_.begin()+hop_,48,mono_.begin());
        return history_;
    }
};
class Upsample {
    float previous_=0;
public:
    void run(const std::array<float,320>& in,float* out,size_t n){
        for(size_t i=0;i<n;++i){
            double p=double(i)*320/n;size_t k=size_t(p);float w=float(p-k);
            float a=k?in[k-1]:previous_;out[i]=a+(in[k]-a)*w;
        }
        previous_=in.back();
    }
};
}
