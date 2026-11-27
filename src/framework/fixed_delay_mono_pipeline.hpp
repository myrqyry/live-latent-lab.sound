#pragma once
#include "live-stem-pipeline.hpp"
#include "ring-buffer.hpp"
#include <array>
#include <cmath>

namespace stts {
// Recoverable fixed-delay stereo dry / mono wet transport shared by DDSP
// and RVC. Each worker chooses its own first read, hop, capacities and delay.
class FixedDelayMonoPipeline {
    const size_t hop_,delay_,initial_read_;
    const float mix_step_;
    std::atomic<size_t> read_threshold_;
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
    FixedDelayMonoPipeline(size_t hop,size_t delay,size_t initial_read,
                          size_t input_capacity,size_t wet_capacity,float mix_step)
        :hop_(hop),delay_(delay),initial_read_(initial_read),mix_step_(mix_step),
         read_threshold_(initial_read),input_(input_capacity),wet_(wet_capacity),
         dry_l_(delay_),dry_r_(delay_){}
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
            (worker_ack_.load(std::memory_order_acquire)!=epoch||input_.available()>=read_threshold_.load(std::memory_order_acquire));
    }
    // Audio has stopped publishing old input before acknowledging the
    // new epoch. Only then may the worker discard it and permit capture.
    bool worker_prepare(){
        const auto epoch=state_.load(std::memory_order_acquire);
        if(!(epoch&1)||audio_ack_.load(std::memory_order_acquire)!=epoch)return false;
        if(worker_epoch_!=epoch){
            input_.discard_available();
            read_threshold_.store(initial_read_,std::memory_order_release);
            worker_epoch_=epoch;
            worker_ack_.store(epoch,std::memory_order_release);
        }
        return state_.load(std::memory_order_acquire)==epoch;
    }
    bool read(float* l,float* r,size_t n){
        const auto epoch=worker_epoch_;
        if(!(epoch&1)||state_.load(std::memory_order_acquire)!=epoch||
           !input_.pop(l,r,n)||state_.load(std::memory_order_acquire)!=epoch)return false;
        read_threshold_.store(hop_,std::memory_order_release);
        return true;
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
        const float step=mix_step_;
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
} // namespace stts
