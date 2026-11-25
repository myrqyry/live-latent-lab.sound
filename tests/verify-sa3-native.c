#define _POSIX_C_SOURCE 200809L
#include "libsa3_v1.h"
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return t.tv_sec+t.tv_nsec/1e9;
}
static int cancel_now(void* p) { (void)p; return 1; }
static void progress(void* p,const sa3_progress_v1* v) {
    (void)p;
    if(v) { printf("PROGRESS %s %d/%d %.3f\n",v->stage_name?v->stage_name:"?",v->step,v->total,v->fraction); fflush(stdout); }
}
static int pcm_ok(const sa3_result_v1* r,double seconds) {
    if(!r->samples||r->sample_rate!=44100||r->n_channels!=2||
       r->layout!=SA3_AUDIO_PLANAR_V1||r->n_samples!=(uint64_t)llround(seconds*44100))return 0;
    double sum=0,peak=0;
    for(uint64_t i=0;i<r->n_samples*r->n_channels;i++){
        double x=r->samples[i]; if(!isfinite(x))return 0;
        sum+=x*x; if(fabs(x)>peak)peak=fabs(x);
    }
    double rms=sqrt(sum/(r->n_samples*r->n_channels));
    printf("PCM seconds=%.2f frames=%llu channels=%u rate=%u rms=%.5f peak=%.5f\n",
        seconds,(unsigned long long)r->n_samples,r->n_channels,r->sample_rate,rms,peak);
    return rms>0.0001&&peak<=1.001;
}
static int wav(const char* path,const sa3_result_v1* r) {
    FILE* f=fopen(path,"wb"); if(!f)return 0;
    uint32_t data=(uint32_t)(r->n_samples*r->n_channels*2),riff=data+36,fmt=16,rate=r->sample_rate,bytes=rate*4;
    uint16_t one=1,ch=2,align=4,bits=16;
    fwrite("RIFF",1,4,f); fwrite(&riff,4,1,f); fwrite("WAVEfmt ",1,8,f);
    fwrite(&fmt,4,1,f); fwrite(&one,2,1,f); fwrite(&ch,2,1,f); fwrite(&rate,4,1,f);
    fwrite(&bytes,4,1,f); fwrite(&align,2,1,f); fwrite(&bits,2,1,f); fwrite("data",1,4,f); fwrite(&data,4,1,f);
    for(uint64_t i=0;i<r->n_samples;i++)for(unsigned c=0;c<2;c++){
        float v=fmaxf(-1,fminf(1,r->samples[c*r->n_samples+i]));
        int16_t x=(int16_t)lrintf(v*32767); fwrite(&x,2,1,f);
    }
    int ok=!ferror(f); return fclose(f)==0&&ok;
}
int main(int argc,char** argv) {
    if(argc!=5){fprintf(stderr,"usage: probe LIBSA3 MODELS DEVICE OUTPUT.wav\n");return 2;}
    setvbuf(stdout,NULL,_IOLBF,0);
    void* lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
    if(!lib){fprintf(stderr,"%s\n",dlerror());return 3;}
    const sa3_api_v1* (*get)(uint32_t)=(const sa3_api_v1*(*)(uint32_t))dlsym(lib,"sa3_get_api");
    const sa3_api_v1* api=get?get(SA3_ABI_VERSION_1):NULL;
    if(!api||api->abi_version!=1||api->size<offsetof(sa3_api_v1,convert_lora)+sizeof(api->convert_lora))return 4;
    printf("ABI %u runtime=%s\n",api->abi_version,api->runtime_version());
    sa3_context_config_v1 c={.size=sizeof c}; api->context_config_init(&c);
    c.models_dir=argv[2];c.variant="small-music";c.dit_encoding="q4_k_m";
    c.text_encoder_encoding="f16";c.autoencoder_encoding="f16";c.device=argv[3];c.cpu_threads=8;
    sa3_error_v1 err={.size=sizeof err};api->error_init(&err);
    sa3_context* ctx=NULL;sa3_result_v1 a={.size=sizeof a},b={.size=sizeof b};
    api->result_init(&a);api->result_init(&b);
    int ok=0;
    double opened=now();
    if(api->context_create(&c,&ctx,&err)!=SA3_STATUS_OK_V1){fprintf(stderr,"context: %s\n",err.message);goto done;}
    printf("CONTEXT_CREATE elapsed=%.3f\n",now()-opened);
    sa3_request_v1 r={.size=sizeof r};api->request_init(&r);
    r.prompt="mellow bedroom pop instrumental, toy keyboard arpeggios, gentle drums, warm bass, seamless groove";
    r.duration_seconds=12;r.steps=8;r.seed=42;r.cfg_scale=1;r.residency=SA3_RESIDENCY_FRUGAL_V1;
    r.on_progress=progress;
    double start=now();
    int status=api->generate(ctx,&r,&a,&err);
    printf("GENERATE status=%d elapsed=%.3f\n",status,now()-start);
    if(status||!pcm_ok(&a,12)||!wav(argv[4],&a)){fprintf(stderr,"generate: %s\n",err.message);goto done;}
    r.operation=SA3_OPERATION_CONTINUE_V1;r.duration_seconds=6;
    api->audio_view_init(&r.input_audio);
    r.input_audio.samples=a.samples;r.input_audio.n_samples=a.n_samples;r.input_audio.n_channels=a.n_channels;
    r.input_audio.sample_rate=a.sample_rate;r.input_audio.layout=a.layout;
    r.encode_chunk_size=128;r.encode_overlap=32;r.decode_chunk_size=128;r.decode_overlap=32;
    start=now();status=api->generate(ctx,&r,&b,&err);
    printf("CONTINUE status=%d elapsed=%.3f splice=%d crossfade=%.3f\n",status,now()-start,b.splice_applied,b.splice_crossfade_seconds);
    if(status||!pcm_ok(&b,18)||!b.splice_applied){fprintf(stderr,"continue: %s\n",err.message);goto done;}
    // Upstream explicitly multiplies the restored prefix by splice gain and
    // applies the final normalization/soft limiter to the entire result.
    // Compare the actual documented transform rather than silently accepting
    // arbitrary changes to the already-generated audio.
    double delta=0,raw_delta=0;
    const float ceiling=powf(10.f,r.loudness.limiter_ceiling_db/20.f);
    const float knee=ceiling*r.loudness.limiter_knee;
    for(unsigned ch=0;ch<2;ch++)for(uint64_t i=0;i<44100;i++){
        float original=a.samples[ch*a.n_samples+i], expected=original*b.splice_gain;
        if(b.peak_normalize_gain_set)expected*=b.peak_normalize_gain;
        if(r.loudness.limiter&&fabsf(expected)>knee){
            float mag=knee+(ceiling-knee)*tanhf((fabsf(expected)-knee)/(ceiling-knee));
            expected=copysignf(mag,expected);
        }
        if(b.safety_gain_set)expected*=b.safety_gain;
        float actual=b.samples[ch*b.n_samples+i];
        delta=fmax(delta,fabs(expected-actual));
        raw_delta=fmax(raw_delta,fabs(original-actual));
    }
    printf("CONTINUE_PREFIX expected_max_delta=%.8f unprocessed_max_delta=%.8f\n",delta,raw_delta);
    if(delta>0.00001)goto done;
    api->result_free(&b);
    // Streaming hosts should not renormalize audio that was already played.
    r.continuation.gain_match=0;r.loudness.peak_normalize=0;r.loudness.limiter=0;
    start=now();status=api->generate(ctx,&r,&b,&err);
    printf("CONTINUE_RAW status=%d elapsed=%.3f splice=%d peak=%.5f\n",status,now()-start,b.splice_applied,b.final_peak);
    if(status||!b.samples||b.n_samples!=793800||b.n_channels!=2||!b.splice_applied)goto done;
    delta=0;for(unsigned ch=0;ch<2;ch++)for(uint64_t i=0;i<44100;i++)
        delta=fmax(delta,fabs(a.samples[ch*a.n_samples+i]-b.samples[ch*b.n_samples+i]));
    printf("CONTINUE_RAW_PREFIX max_delta=%.8f\n",delta);
    if(delta>0.00001)goto done;
    api->result_free(&b);
    r.loudness.peak_normalize=1;r.loudness.limiter=1;r.continuation.gain_match=1;
    r.operation=SA3_OPERATION_TRANSFORM_V1;r.transform_noise_level=0.5f;
    start=now();status=api->generate(ctx,&r,&b,&err);
    printf("TRANSFORM status=%d elapsed=%.3f\n",status,now()-start);
    if(status||!pcm_ok(&b,12)){fprintf(stderr,"transform: %s\n",err.message);goto done;}
    api->result_free(&b);
    api->request_init(&r);r.prompt="soft piano";r.duration_seconds=2;r.steps=8;r.should_cancel=cancel_now;
    status=api->generate(ctx,&r,&b,&err);
    printf("CANCEL status=%d samples=%llu\n",status,(unsigned long long)b.n_samples);
    if(status!=SA3_STATUS_CANCELLED_V1||b.samples)goto done;
    api->result_free(&b);
    api->request_init(&r);r.prompt="soft piano";r.duration_seconds=-1;
    status=api->generate(ctx,&r,&b,&err);
    printf("INVALID_DURATION status=%d\n",status);
    if(status!=SA3_STATUS_INVALID_ARGUMENT_V1||b.samples)goto done;
    api->result_free(&b);
    api->request_init(&r);r.prompt="mellow instrumental synth-pop with warm bass and delicate percussion";
    r.steps=8;r.seed=43;r.residency=SA3_RESIDENCY_FRUGAL_V1;
    for(int seconds=30;seconds<=60;seconds+=30){
        r.duration_seconds=seconds;start=now();status=api->generate(ctx,&r,&b,&err);
        printf("LONG_GENERATE seconds=%d status=%d elapsed=%.3f\n",seconds,status,now()-start);
        if(status||!pcm_ok(&b,seconds))goto done;
        api->result_free(&b);
    }
    ok=1;
done:
    api->result_free(&a);api->result_free(&b);if(ctx)api->context_destroy(ctx);
    dlclose(lib);printf("SA3_NATIVE_PROBE_PASS=%d\n",ok);return ok?0:1;
}
