#pragma once
// Linux/glibc test executable only. Dynamic symbol interposition around
// registered filter_audio functions, never shipped inside Sound.
#include <obs.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>

namespace rt_probe {
using Callback=obs_audio_data*(*)(void*,obs_audio_data*);
struct Stats {
    Callback original=nullptr;
    const char* id=nullptr;
    std::array<uint64_t,8192> times{},cpu_times{};
    uint64_t calls=0,allocs=0,frees=0,locks=0,waits=0,late=0,max=0,cpu_max=0;
    uint64_t slow_wall=0,slow_mostly_off_cpu=0;
};
inline Stats stats[3]={{nullptr,"sound_stt_filter"},{nullptr,"sound_ddsp_filter"},
                       {nullptr,"stts_rvc_filter"}};
inline thread_local Stats* active=nullptr;
inline uint64_t thread_cpu_ns(){
    timespec ts{};
    if(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&ts)!=0)std::abort();
    return uint64_t(ts.tv_sec)*1000000000ULL+uint64_t(ts.tv_nsec);
}
template<int I> obs_audio_data* dispatch(void* state,obs_audio_data* p){
    auto& s=stats[I];
    const auto begin=std::chrono::steady_clock::now();
    const auto cpu_begin=thread_cpu_ns();
    auto* previous=active;active=&s;
    auto* out=s.original(state,p);
    active=previous;
    const auto cpu=thread_cpu_ns()-cpu_begin;
    const auto ns=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now()-begin).count());
    if(s.calls<s.times.size()){s.times[s.calls]=ns;s.cpu_times[s.calls]=cpu;}
    ++s.calls;s.max=std::max(s.max,ns);s.cpu_max=std::max(s.cpu_max,cpu);
    if(ns>=100000){++s.slow_wall;if(cpu<ns/2)++s.slow_mostly_off_cpu;}
    if(ns>=10000000)++s.late;
    return out;
}
inline bool report(size_t minimum){
    bool ok=true;
    for(auto& s:stats){
        if(!s.original){ok=false;continue;}
        const size_t n=std::min<uint64_t>(s.calls,s.times.size());
        std::sort(s.times.begin(),s.times.begin()+n);
        std::sort(s.cpu_times.begin(),s.cpu_times.begin()+n);
        auto p99=n?s.times[(n*99+99)/100-1]/1000.:0.;
        auto cpu99=n?s.cpu_times[(n*99+99)/100-1]/1000.:0.;
        std::printf("RT_CALLBACK id=%s calls=%llu p99_us=%.3f max_us=%.3f cpu_p99_us=%.3f cpu_max_us=%.3f slow_wall=%llu slow_mostly_off_cpu=%llu allocations=%llu frees=%llu locks=%llu waits=%llu over_10ms=%llu\n",
            s.id,(unsigned long long)s.calls,p99,s.max/1000.,cpu99,s.cpu_max/1000.,
            (unsigned long long)s.slow_wall,(unsigned long long)s.slow_mostly_off_cpu,
            (unsigned long long)s.allocs,(unsigned long long)s.frees,
            (unsigned long long)s.locks,(unsigned long long)s.waits,(unsigned long long)s.late);
        // Timing is diagnostic; a scheduler preemption is not a code defect.
        ok&=s.calls>=minimum && s.allocs==0 && s.frees==0 && s.locks==0 && s.waits==0;
    }
    return ok;
}
}
// glibc entrypoints avoid malloc/dlsym bootstrap recursion. Scope is only
// this test process, including dynamically linked libraries on the calling
// thread. Internal/hidden allocator symbols and custom allocators are not
// guaranteed to be intercepted.
extern "C" {
void* __libc_malloc(size_t) noexcept;
void* __libc_calloc(size_t,size_t) noexcept;
void* __libc_realloc(void*,size_t) noexcept;
void __libc_free(void*) noexcept;
void* __libc_memalign(size_t,size_t) noexcept;
void* malloc(size_t n) noexcept {if(rt_probe::active)++rt_probe::active->allocs;return __libc_malloc(n);}
void* calloc(size_t n,size_t s) noexcept {if(rt_probe::active)++rt_probe::active->allocs;return __libc_calloc(n,s);}
void* realloc(void* p,size_t n) noexcept {if(rt_probe::active)++rt_probe::active->allocs;return __libc_realloc(p,n);}
void free(void* p) noexcept {if(p&&rt_probe::active)++rt_probe::active->frees;__libc_free(p);}
void* aligned_alloc(size_t a,size_t n) noexcept {if(rt_probe::active)++rt_probe::active->allocs;return __libc_memalign(a,n);}
int posix_memalign(void** p,size_t a,size_t n) noexcept {
    if(rt_probe::active)++rt_probe::active->allocs;
    using Fn=int(*)(void**,size_t,size_t);
    static const auto real=reinterpret_cast<Fn>(dlsym(RTLD_NEXT,"posix_memalign"));
    return real(p,a,n);
}
int pthread_mutex_lock(pthread_mutex_t* p) noexcept {
    if(rt_probe::active)++rt_probe::active->locks;
    using Fn=int(*)(pthread_mutex_t*);
    static const auto real=reinterpret_cast<Fn>(dlsym(RTLD_NEXT,"pthread_mutex_lock"));
    return real(p);
}
int pthread_rwlock_rdlock(pthread_rwlock_t* p) noexcept {
    if(rt_probe::active)++rt_probe::active->locks;
    using Fn=int(*)(pthread_rwlock_t*);
    static const auto real=reinterpret_cast<Fn>(dlsym(RTLD_NEXT,"pthread_rwlock_rdlock"));
    return real(p);
}
int pthread_rwlock_wrlock(pthread_rwlock_t* p) noexcept {
    if(rt_probe::active)++rt_probe::active->locks;
    using Fn=int(*)(pthread_rwlock_t*);
    static const auto real=reinterpret_cast<Fn>(dlsym(RTLD_NEXT,"pthread_rwlock_wrlock"));
    return real(p);
}
int pthread_cond_wait(pthread_cond_t* c,pthread_mutex_t* m) {
    if(rt_probe::active)++rt_probe::active->waits;
    using Fn=int(*)(pthread_cond_t*,pthread_mutex_t*);
    static const auto real=reinterpret_cast<Fn>(dlsym(RTLD_NEXT,"pthread_cond_wait"));
    return real(c,m);
}
int pthread_cond_timedwait(pthread_cond_t* c,pthread_mutex_t* m,const timespec* t) {
    if(rt_probe::active)++rt_probe::active->waits;
    using Fn=int(*)(pthread_cond_t*,pthread_mutex_t*,const timespec*);
    static const auto real=reinterpret_cast<Fn>(dlsym(RTLD_NEXT,"pthread_cond_timedwait"));
    return real(c,m,t);
}
void obs_register_source_s(const obs_source_info* info,size_t size){
    using Fn=void(*)(const obs_source_info*,size_t);
    static const auto real=reinterpret_cast<Fn>(dlsym(RTLD_NEXT,"obs_register_source_s"));
    if(size==sizeof(obs_source_info)&&info->filter_audio){
        constexpr rt_probe::Callback wrappers[]={rt_probe::dispatch<0>,rt_probe::dispatch<1>,rt_probe::dispatch<2>};
        for(size_t i=0;i<3;++i){
            auto& s=rt_probe::stats[i];
            if(std::strcmp(info->id,s.id)==0){
                s.original=info->filter_audio;
                auto copy=*info;copy.filter_audio=wrappers[i];
                real(&copy,size);return;
            }
        }
    }
    real(info,size);
}
}
