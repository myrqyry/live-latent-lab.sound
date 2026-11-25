#include "engines/stt_parakeet.hpp"
#include <fstream>
#include <iostream>
#include <chrono>
#include <algorithm>
#include <cctype>
int main(int argc,char** argv){
    if(argc!=4&&argc!=5)return 2;
    const std::string device=argc==5?argv[4]:"auto";
    std::ifstream f(argv[3],std::ios::binary|std::ios::ate);
    auto bytes=f.tellg();if(bytes<=0||bytes%4)return 3;
    std::vector<float> pcm(size_t(bytes)/4);
    f.seekg(0);f.read(reinterpret_cast<char*>(pcm.data()),bytes);
    stts::SttParakeet p;
    if(p.open(argv[1],"/nonexistent/model.litertlm"))return 4;
    if(!p.open(argv[1],argv[2],device)){std::cerr<<p.error()<<'\n';return 5;}
    for(int i=0;i<2;++i){
        std::string t;
        auto begin=std::chrono::steady_clock::now();
        if(!p.transcribe(pcm,t)){std::cerr<<p.error()<<'\n';return 6;}
        std::cout<<"PARAKEET_NATIVE trial="<<i<<" ms="<<std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-begin).count()<<" text="<<t<<std::endl;
        std::transform(t.begin(),t.end(),t.begin(),[](unsigned char c){return std::tolower(c);});
        if(t.find("country")==std::string::npos||t.find("ask")==std::string::npos)return 7;
    }
    p.close();
    if(!p.open(argv[1],argv[2],device))return 8;
    std::string t;
    if(!p.transcribe(pcm,t)||t.empty())return 9;
    std::cout<<"SOUND_STT_PARAKEET_NATIVE_PASS=1\n";
}
