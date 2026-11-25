#include "engines/ddsp_litert.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <chrono>
int main(int argc,char** argv){
    assert(argc==3);
    ddsp::Runtime runtime(argv[1]);
    ddsp::Features features(runtime,std::string(argv[2])+"/extract_features_micro.tflite");
    ddsp::Decoder violin(runtime,std::string(argv[2])+"/Violin.tflite"),flute(runtime,std::string(argv[2])+"/Flute.tflite");
    std::array<float,1024> pcm{};
    double ms=0,dist=0;
    for(int j=0;j<150;++j){
        float hz=220.f+j;
        for(size_t i=0;i<pcm.size();++i)pcm[i]=.3f*std::sin(6.28318530718*hz*i/16000.);
        auto t=std::chrono::steady_clock::now();
        auto f=features.run(pcm);
        assert(std::abs(f.hz-hz)/hz<.03f);
        auto a=violin.run(f),b=flute.run(f);
        for(size_t i=0;i<60;++i)dist+=std::abs(a.harmonics[i]*a.amplitude-b.harmonics[i]*b.amplitude);
        ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count();
    }
    assert(dist>.1&&ms/150<20);
    violin.reset();auto f=features.run(pcm);auto a=violin.run(f);violin.reset();auto b=violin.run(f);
    assert(std::abs(a.amplitude-b.amplitude)<1e-6);
    printf("DDSP_NATIVE_PASS=1 mean_ms=%.3f difference=%.3f\n",ms/150,dist);
}
