#include "audiocpp.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <string.h>
#include <unistd.h>

static double ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec*1000.0 + t.tv_nsec/1000000.0;
}
static void mem(const char *phase) {
    FILE *f=fopen("/proc/self/status","r");
    if (!f) return;
    char line[256];
    long rss=0,hwm=0;
    while(fgets(line,sizeof(line),f)) {
        if(sscanf(line,"VmRSS: %ld kB",&rss)==1)continue;
        if(sscanf(line,"VmHWM: %ld kB",&hwm)==1)continue;
    }
    fclose(f);
    printf("PACED_MEMORY phase=%s rss_mb=%.1f peak_mb=%.1f\n",phase,rss/1024.0,hwm/1024.0);
    fflush(stdout);
}
static void sleep_until(double target_ms) {
    double remaining;
    while ((remaining=target_ms-ms())>0) {
        struct timespec req = { (time_t)(remaining/1000.0),
            (long)fmod(remaining,1000.0)*1000000L };
        if (nanosleep(&req,NULL)!=0) return;
    }
}
static void check(audiocpp_status s, const char *step) {
    if (s != AUDIOCPP_OK) {
        fprintf(stderr,"C_API_ERROR step=%s status=%d err=%s\n",
            step, (int)s, audiocpp_last_error());
        exit(4);
    }
}
static float *read_f32(const char *path, size_t *frames) {
    FILE *f=fopen(path,"rb");
    if(!f) {perror(path);exit(3);}
    fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
    if(size<=0 || size%sizeof(float))exit(3);
    float *buf=(float*)malloc((size_t)size);
    if(!buf || fread(buf,1,(size_t)size,f)!=(size_t)size)exit(3);
    fclose(f);*frames=(size_t)size/sizeof(float);return buf;
}
int main(int argc,char**argv) {
    if(argc!=4)return 2;
    size_t src_count=0, ref_count=0;
    float *in=read_f32(argv[2],&src_count);
    float *ref=read_f32(argv[3],&ref_count);
    const double begin=ms();
    audiocpp_registry *registry=NULL;
    audiocpp_model *model=NULL;
    audiocpp_session *session=NULL;
    audiocpp_request *request=NULL;
    audiocpp_result *result=NULL;
    audiocpp_model_config cfg={"meanvc2",NULL,NULL,NULL};
    audiocpp_backend_config backend={"cpu",0,4};
    check(audiocpp_registry_create(NULL,&registry),"registry");
    check(audiocpp_model_load(registry,argv[1],&cfg,NULL,&model),"load");
    mem("model_loaded");
    if(!audiocpp_model_supports(model,"vc","streaming")) {
        fprintf(stderr,"C_API_FAIL: vc streaming not supported\n");
        return 5;
    }
    const double loaded=ms();
    check(audiocpp_session_create(model,"vc","streaming",&backend,NULL,&session),"session");
    mem("session_constructed");
    audiocpp_stream_input_kind input_kind=AUDIOCPP_STREAM_INPUT_NONE;
    audiocpp_stream_output_kind output_kind=AUDIOCPP_STREAM_OUTPUT_FINAL_RESULT;
    int64_t preferred=0;double seconds=0;
    check(audiocpp_stream_policy(session,&input_kind,&output_kind,&preferred,&seconds),"policy");
    printf("C_API_POLICY input=%d output=%d preferred=%lld duration_sec=%g\n",
         input_kind,output_kind,(long long)preferred,seconds);
    request=audiocpp_request_create();
    check(audiocpp_request_set_voice_audio(request,ref,ref_count,16000,1),"voice");
    check(audiocpp_stream_start(session,request),"start");
    mem("stream_warmed");
    const double started=ms();
    const size_t chunk=preferred>0 ? (size_t)preferred : 2560;
    int pushes=0, audio_events=0, first_push=-1;
    size_t emitted_frames=0;
    double max_push_ms=0, sum_push_ms=0, first_output_ms=-1;
    double max_event_gap_ms=0, previous_event_ms=-1, first_end_to_end_ms=-1;
    long negative_lag_chunks=0;
    for(size_t pos=0;pos<src_count;pos+=chunk) {
        size_t count=src_count-pos;
        if(count>chunk) count=chunk;
        // Simulate capture cadence: we cannot submit a 160 ms chunk before
        // that many frames have actually arrived from the source.
        const double expected_input_ready = started + (pos+count)*1000.0/16000.0;
        sleep_until(expected_input_ready);
        audiocpp_event *event=NULL;
        const double p=ms();
        if(p>expected_input_ready+40.0)++negative_lag_chunks;
        check(audiocpp_stream_push(session,in+pos,count,16000,1,(int64_t)pos,&event),"push");
        const double duration=ms()-p;
        if(duration>max_push_ms) max_push_ms=duration;
        sum_push_ms+=duration;
        if(event) {
            const audiocpp_result *view=audiocpp_event_as_result(event);
            const float *audio=NULL;size_t frames=0;int rate=0,channels=0;
            if(audiocpp_result_audio(view,&audio,&frames,&rate,&channels)==AUDIOCPP_OK
                 && frames>0) {
                if(first_push<0) {
                    first_push=pushes;
                    first_output_ms=ms()-started;
                    first_end_to_end_ms=ms()-started;
                }
                const double event_ms=ms();
                if(previous_event_ms>0 && event_ms-previous_event_ms>max_event_gap_ms)
                    max_event_gap_ms=event_ms-previous_event_ms;
                previous_event_ms=event_ms;
                ++audio_events;emitted_frames+=frames;
                if(rate!=16000 || channels!=1)exit(7);
            }
            audiocpp_event_free(event);
        }
        ++pushes;
    }
    const double pushed=ms();
    mem("before_finish");
    check(audiocpp_stream_finish(session,&result),"finish");
    const float *out=NULL;size_t frames=0;int rate=0,channels=0;
    check(audiocpp_result_audio(result,&out,&frames,&rate,&channels),"result_audio");
    double sum=0,maxabs=0;size_t nonzero=0;
    for(size_t i=0;i<frames*(size_t)channels;++i) {
        const double val=out[i];
        if(!isfinite(val))exit(8);
        sum+=val*val;if(fabs(val)>maxabs)maxabs=fabs(val);
        if(fabs(val)>0.0001)++nonzero;
    }
    printf("C_API_AUDIO source_frames=%zu pushes=%d audio_events=%d first_push=%d "
      "event_frames=%zu output_frames=%zu sr=%d channels=%d rms=%g nonzero=%zu maxabs=%g\n",
      src_count,pushes,audio_events,first_push,emitted_frames,frames,rate,channels,
      sqrt(sum/(frames*(size_t)channels)),nonzero,maxabs);
    printf("C_API_TIMING model_load_ms=%.1f start_ms=%.1f push_total_ms=%.1f "
       "max_single_push_ms=%.1f mean_push_ms=%.1f first_output_after_start_ms=%.1f "
       "finalize_ms=%.1f end_to_end_ms=%.1f\n",
       loaded-begin,started-loaded,pushed-started,max_push_ms,
       sum_push_ms/pushes,first_output_ms,ms()-pushed,ms()-begin);
    printf("PACED_STREAM source_audio_ms=%.1f first_event_from_start_ms=%.1f max_event_gap_ms=%.1f behind_capture_chunks=%ld work_time_ms=%.1f expected_output_ms=%.1f\n",
        src_count*1000.0/16000.0,first_end_to_end_ms,max_event_gap_ms,
        negative_lag_chunks,sum_push_ms,frames*1000.0/16000.0);
    mem("finished");
    int good=(frames>50000 && nonzero>frames/4 && audio_events>0
           && first_push>=0 && first_push<pushes-1 && rate==16000 && channels==1);
    // Reuse the very same session, model and speaker reference. Validate
    // that the second stream is deterministic and releases stale stream state.
    float *first_audio=(float*)malloc(frames*sizeof(float));
    if (!first_audio) exit(9);
    memcpy(first_audio,out,frames*sizeof(float));
    const size_t first_frames=frames;
    audiocpp_result_free(result);
    result=NULL;
    const double restart_begin=ms();
    check(audiocpp_stream_start(session,request),"restart");
    const double restarted=ms();
    mem("restarted");
    size_t second_events=0,second_frames=0;
    for (size_t pos=0;pos<src_count;pos+=chunk) {
        const size_t count=(src_count-pos)>chunk?chunk:(src_count-pos);
        audiocpp_event *event=NULL;
        check(audiocpp_stream_push(session,in+pos,count,16000,1,(int64_t)pos,&event),"second_push");
        if(event) {
            const audiocpp_result *view=audiocpp_event_as_result(event);
            const float *samples=NULL;size_t count_out=0;int sr=0,ch=0;
            if(audiocpp_result_audio(view,&samples,&count_out,&sr,&ch)==AUDIOCPP_OK &&
               count_out>0) {++second_events;second_frames+=count_out;}
            audiocpp_event_free(event);
        }
    }
    check(audiocpp_stream_finish(session,&result),"second_finish");
    const float *second_out=NULL;size_t second_result_frames=0;int sr=0,ch=0;
    check(audiocpp_result_audio(result,&second_out,&second_result_frames,&sr,&ch),"second_result");
    double largest_diff=0;
    if(second_result_frames==first_frames && ch==1 && sr==16000) {
        for (size_t i=0;i<first_frames;++i) {
            const double diff=fabs(second_out[i]-first_audio[i]);
            if(diff>largest_diff) largest_diff=diff;
        }
    }
    const int repeat_good=(second_events>0 && second_frames>0 &&
       second_result_frames==first_frames && largest_diff<0.0001);
    printf("PACED_RESTART second_warmup_ms=%.1f audio_events=%zu event_frames=%zu "
           "result_frames=%zu max_audio_diff=%g pass=%d\n",
           restarted-restart_begin,second_events,second_frames,second_result_frames,
           largest_diff,repeat_good);
    mem("after_restart_finished");
    free(first_audio);
    good = good && repeat_good;
    printf("C_API_STREAM_CONVERSION_PASS=%d\n",good);
    audiocpp_result_free(result);
    audiocpp_request_free(request);
    audiocpp_session_free(session);
    audiocpp_model_free(model);
    audiocpp_registry_free(registry);
    mem("after_destroy");
    free(in);free(ref);
    return good?0:9;
}
