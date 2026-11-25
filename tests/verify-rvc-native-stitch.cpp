// Actual Sound SOLA code, driven by the native RVC qualification harness.
#include "framework/sola_stitcher.hpp"
#include <array>
#include <cstdio>
#include <exception>
int main(){
    constexpr size_t rate=48000,hop=rate/2,overlap=rate/25,search=rate/50;
    stts::SolaStitcher stitch({hop,overlap,search});
    std::array<float,hop+overlap+search> window{};
    for(;;){
        size_t count=std::fread(window.data(),sizeof(float),window.size(),stdin);
        if(!count&&std::feof(stdin))return 0;
        if(count!=window.size())return 2;
        try{
            auto out=stitch.push(window.data(),count);
            if(out.pcm.size()!=hop)return 3;
            if(std::fwrite(out.pcm.data(),sizeof(float),hop,stdout)!=hop||std::fflush(stdout))return 4;
        }catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 5;}
    }
}
