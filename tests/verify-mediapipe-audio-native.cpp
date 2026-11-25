#include "engines/mediapipe_audio_123.hpp"
#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

int main(int argc,char** argv) {
    if(argc!=3&&argc!=4) {
        std::cerr<<"usage: native-test LIB MODEL [auto|cpu|gpu]\n";
        return 2;
    }
    const std::string device=argc==4?argv[3]:"cpu";
    stts::mp_audio_123::Classifier classifier;
    std::string error;
    // Wrong paths must fail closed; neither fabricated categories nor a
    // silently substituted runtime is permitted.
    assert(!classifier.open("/path/not/installed.so",argv[2],error));
    assert(!classifier.ready()&&!error.empty());
    error.clear();
    assert(!classifier.open(argv[1],"/path/no-model.tflite",error));
    assert(!classifier.ready()&&!error.empty());

    std::vector<float> silent(16000);
    std::vector<stts::mp_audio_123::Prediction> results;
    assert(!classifier.classify(silent.data(),silent.size(),16000,results,error));
    assert(results.empty());

    error.clear();
    const auto start=std::chrono::steady_clock::now();
    if(!classifier.open(argv[1],argv[2],error,1024,device)) {
        std::cerr<<"Failed to open YAMNet: "<<error<<"\n";
        return 3;
    }
    std::cout<<"YAMNET_DELEGATE="<<classifier.delegate()<<"\n";
    assert(device=="auto"||classifier.delegate()==device);
    std::cout<<"YAMNET_INIT_MS="<<std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now()-start).count()<<"\n";
    auto infer=[&](const char* name,const std::vector<float>& pcm) {
        results.clear();
        error.clear();
        auto begin=std::chrono::steady_clock::now();
        if(!classifier.classify(pcm.data(),pcm.size(),16000,results,error)) {
            std::cerr<<name<<" failure: "<<error<<"\n";return std::string{};
        }
        std::cout<<"INPUT="<<name<<" INFERENCE_MS="
                 <<std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now()-begin).count()
                 <<" TOP_LABEL="<<results.front().category
                 <<" TOP_SCORE="<<results.front().score
                 <<" COUNT="<<results.size()<<"\n";
        for(const auto& c:results)
            assert(std::isfinite(c.score)&&c.score>=0&&c.score<=1);
        return results.front().category;
    };
    const auto a=infer("SILENCE",silent);
    std::vector<float> wave(16000);
    for(size_t i=0;i<wave.size();++i)
        wave[i]=.3f*std::sin(2*3.14159265358979323846*440.*double(i)/16000.);
    const auto b=infer("440_HZ_SINE",wave);
    assert(a=="Silence");
    assert(b=="Sine wave");
    classifier.close();
    assert(!classifier.ready());
    error.clear();
    assert(classifier.open(argv[1],argv[2],error,1024,device));
    assert(classifier.ready());
    classifier.close();
    std::cout<<"MEDIAPIPE_YAMNET_NATIVE_SEMANTIC_PASS=YES\n";
    return 0;
}
