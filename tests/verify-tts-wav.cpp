#include "framework/tts_wav.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
int main(int argc,char** argv){
    if(argc!=5)return 2;
    for(int i=1;i<argc;++i){
        std::ifstream f(argv[i],std::ios::binary);
        std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)),{});
        stts::TtsPcm out;
        if(!stts::decode_tts_wav(b,out)||out.samples.empty())return 3;
        auto truncated=b; truncated.pop_back();
        if(stts::decode_tts_wav(truncated,out))return 4;
        auto malformed=b;malformed[0]='X';
        if(stts::decode_tts_wav(malformed,out))return 5;
        auto badrate=b;
        // Locate the actual fmt chunk, including files with extra metadata.
        for(size_t pos=12;pos+24<=badrate.size();){
            auto n=stts::wav_u32(badrate.data()+pos+4);
            if(!memcmp(badrate.data()+pos,"fmt ",4)){
                badrate[pos+12]=0;badrate[pos+13]=0;badrate[pos+14]=0;badrate[pos+15]=0;
                break;
            }
            if(n>badrate.size()-pos-8)break;
            pos+=8+n+(n%2);
        }
        if(stts::decode_tts_wav(badrate,out))return 6;
    }
    std::cout<<"SOUND_TTS_WAV_REAL_FILES_AND_INVALID_PASS=1\n";
}
