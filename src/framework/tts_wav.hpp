#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
namespace stts {
struct TtsPcm { uint32_t rate=0; std::vector<float> samples; };
inline uint32_t wav_u32(const uint8_t* p) {
    return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
}
inline uint16_t wav_u16(const uint8_t* p){return uint16_t(p[0])|(uint16_t(p[1])<<8);}
inline bool decode_tts_wav(const std::vector<uint8_t>& b,TtsPcm& out){
    out={};
    if(b.size()<44||memcmp(b.data(),"RIFF",4)||memcmp(b.data()+8,"WAVE",4))return false;
    const auto riff=wav_u32(b.data()+4);
    if(riff!=b.size()-8)return false;
    uint32_t rate=0,bytes=0;
    const uint8_t* data=nullptr;
    for(size_t pos=12;pos+8<=b.size();){
        const auto* chunk=b.data()+pos;
        const auto n=wav_u32(chunk+4);pos+=8;
        if(n>b.size()-pos)return false;
        if(!memcmp(chunk,"fmt ",4)){
            if(rate||n<16||wav_u16(b.data()+pos)!=1||wav_u16(b.data()+pos+2)!=1||
                wav_u16(b.data()+pos+12)!=2||wav_u16(b.data()+pos+14)!=16)return false;
            rate=wav_u32(b.data()+pos+4);
            if(rate<8000||rate>48000||wav_u32(b.data()+pos+8)!=rate*2)return false;
        } else if(!memcmp(chunk,"data",4)){
            if(data)return false;
            data=b.data()+pos;bytes=n;
        }
        pos+=n;
        if(n%2){if(pos==b.size())return false;++pos;}
    }
    if(!rate||!data||!bytes||bytes%2||bytes/2>rate*90)return false;
    out.rate=rate;out.samples.resize(bytes/2);
    bool audible=false;
    for(size_t i=0;i<out.samples.size();++i){
        const auto v=wav_u16(data+2*i);
        out.samples[i]=float(int32_t(v)-(v>=32768?65536:0))/32768.f;
        audible|=v!=0;
    }
    if(!audible){out={};return false;}
    return true;
}
}
