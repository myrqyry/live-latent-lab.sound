#pragma once

#include "ring-buffer.hpp"
#include <cstdint>

namespace stts {

// Two SPSC queues, with a generation handshake to recover from overflow or
// model replacement. No actor resets the other actor's cursor. The callback
// never waits: while the worker acknowledges a generation it returns dry
// (or the caller can choose silence).
class DeepFilterTransport {
    RingBuffer input_, output_;
    // Start at generation 1 so the initial stream is acknowledged too.
    std::atomic<uint64_t> generation_{1}, audio_ack_{0}, worker_ack_{0};
    uint64_t audio_generation_=0, render_generation_=0; // audio thread only
    uint64_t worker_generation_=0; // worker thread only

    void invalidate(uint64_t expected) noexcept {
        generation_.compare_exchange_strong(expected,expected+1,
            std::memory_order_acq_rel,std::memory_order_acquire);
    }

public:
    explicit DeepFilterTransport(size_t input_capacity,size_t output_capacity)
        :input_(input_capacity),output_(output_capacity){}

    // Control thread: model changes invalidate the previous stream.
    void invalidate() noexcept { generation_.fetch_add(1,std::memory_order_acq_rel); }

    // Audio actor. First acknowledge that old input writes have stopped.
    // After the worker stops old output writes, discard that output before
    // publishing any new input. A short output queue never modifies audio.
    bool process(float* mono,size_t frames) noexcept {
        if(!mono||!frames)return false;
        const auto generation=generation_.load(std::memory_order_acquire);
        if(audio_generation_!=generation){
            audio_generation_=generation;
            audio_ack_.store(generation,std::memory_order_release);
        }
        if(worker_ack_.load(std::memory_order_acquire)!=generation)return false;
        if(render_generation_!=generation){
            output_.discard_available();
            render_generation_=generation;
        }
        if(generation_.load(std::memory_order_acquire)!=generation)return false;
        if(!input_.write_exact(mono,frames)){
            invalidate(generation);
            return false;
        }
        if(generation_.load(std::memory_order_acquire)!=generation ||
           output_.available()<frames)return false;
        return output_.read(mono,frames)==frames;
    }

    // Worker actor. Acknowledge only after no old input can be published,
    // and after resetting recurrent model state. New input cannot arrive
    // until this acknowledgement is visible to the audio actor.
    template<class Reset>
    uint64_t worker_prepare(Reset&& reset){
        const auto generation=generation_.load(std::memory_order_acquire);
        if(audio_ack_.load(std::memory_order_acquire)!=generation)return 0;
        if(worker_generation_!=generation){
            input_.discard_available();
            reset();
            worker_generation_=generation;
            worker_ack_.store(generation,std::memory_order_release);
        }
        return generation_.load(std::memory_order_acquire)==generation?generation:0;
    }

    bool worker_read(uint64_t generation,float* data,size_t frames) noexcept {
        if(!generation||generation!=worker_generation_||
           generation_.load(std::memory_order_acquire)!=generation||
           input_.available()<frames)return false;
        return input_.read(data,frames)==frames &&
               generation_.load(std::memory_order_acquire)==generation;
    }

    bool worker_publish(uint64_t generation,const float* data,size_t frames) noexcept {
        if(!generation||generation!=worker_generation_||
           generation_.load(std::memory_order_acquire)!=generation)return false;
        if(!output_.write_exact(data,frames)){
            invalidate(generation);
            return false;
        }
        return true;
    }

    // Worker observed a malformed result; recover on a new generation.
    void worker_fault(uint64_t generation) noexcept {if(generation)invalidate(generation);}
};

} // namespace stts
