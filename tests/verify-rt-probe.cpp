#include "rt-callback-probe.hpp"
#include <cassert>
int main(int argc,char** argv){
    assert(argc==2);
    void* lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(lib);
    auto exercise=reinterpret_cast<void(*)()>(dlsym(lib,"exercise_rt_probe"));assert(exercise);
    exercise(); // resolve lazy symbols outside measurement
    rt_probe::Stats sentinel;
    rt_probe::active=&sentinel;
    exercise();
    rt_probe::active=nullptr;
    printf("RT_PROBE_DSO allocations=%llu frees=%llu locks=%llu waits=%llu\n",
        (unsigned long long)sentinel.allocs,(unsigned long long)sentinel.frees,
        (unsigned long long)sentinel.locks,(unsigned long long)sentinel.waits);
    assert(sentinel.allocs==5&&sentinel.frees==4&&sentinel.locks==3&&sentinel.waits==1);
    dlclose(lib);
    puts("RT_PROBE_DSO_PASS=1");
}
