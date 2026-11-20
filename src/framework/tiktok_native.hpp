#pragma once
#include "tts_wav.hpp"
#include "tts_text.hpp"
#include <obs.h>
#include <cmath>
#include <string>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/base64.h>
#include <libswresample/swresample.h>
}
namespace stts {
inline constexpr char TikTokEndpoint[]="https://ottsy.weilbyte.dev/api/generation";
inline constexpr size_t TikTokMaxResponse=4000000;
inline constexpr size_t TikTokChunkBytes=280;
namespace tiktok_detail {
struct Memory {
    const std::vector<uint8_t>& bytes;
    size_t pos=0;
    static int read(void* opaque,uint8_t* buf,int size){
        auto& m=*static_cast<Memory*>(opaque);
        if(m.pos==m.bytes.size())return AVERROR_EOF;
        const auto n=std::min(size_t(size),m.bytes.size()-m.pos);
        memcpy(buf,m.bytes.data()+m.pos,n);m.pos+=n;return int(n);
    }
};
struct Decoder {
    AVIOContext* io=nullptr;
    AVFormatContext* format=nullptr;
    AVCodecContext* codec=nullptr;
    AVPacket* packet=nullptr;
    AVFrame* frame=nullptr;
    SwrContext* swr=nullptr;
    ~Decoder(){
        swr_free(&swr);av_frame_free(&frame);av_packet_free(&packet);
        avcodec_free_context(&codec);avformat_close_input(&format);
        if(io){av_freep(&io->buffer);avio_context_free(&io);}
    }
};
inline bool decode_mp3(const std::vector<uint8_t>& bytes,TtsPcm& out){
    if(bytes.size()<100||!(memcmp(bytes.data(),"ID3",3)==0||
        (bytes[0]==255&&(bytes[1]&224)==224)))return false;
    Memory m{bytes};
    Decoder d;
    auto* buffer=static_cast<uint8_t*>(av_malloc(4096));
    if(!buffer)return false;
    d.io=avio_alloc_context(buffer,4096,0,&m,Memory::read,nullptr,nullptr);
    if(!d.io){av_free(buffer);return false;}
    d.format=avformat_alloc_context();
    if(!d.format)return false;
    d.format->pb=d.io;d.format->flags|=AVFMT_FLAG_CUSTOM_IO;
    const auto* mp3=av_find_input_format("mp3");
    if(!mp3||avformat_open_input(&d.format,nullptr,mp3,nullptr)<0||
       avformat_find_stream_info(d.format,nullptr)<0)return false;
    const int stream=av_find_best_stream(d.format,AVMEDIA_TYPE_AUDIO,-1,-1,nullptr,0);
    if(stream<0)return false;
    const auto* params=d.format->streams[stream]->codecpar;
    if(params->codec_id!=AV_CODEC_ID_MP3)return false;
    const auto* decoder=avcodec_find_decoder(AV_CODEC_ID_MP3);
    if(!decoder)return false;
    d.codec=avcodec_alloc_context3(decoder);
    if(!d.codec||avcodec_parameters_to_context(d.codec,params)<0||
       avcodec_open2(d.codec,decoder,nullptr)<0)return false;
    d.packet=av_packet_alloc();d.frame=av_frame_alloc();
    if(!d.packet||!d.frame)return false;
    TtsPcm pcm;pcm.rate=24000;
    constexpr size_t max_samples=24000*90;
    int input_rate=0,input_format=-1,channels=0;
    AVChannelLayout mono=AV_CHANNEL_LAYOUT_MONO;
    auto convert=[&](const uint8_t** source,int count)->bool{
        const int capacity=swr_get_out_samples(d.swr,count);
        if(capacity<0||size_t(capacity)>max_samples-pcm.samples.size())return false;
        const auto start=pcm.samples.size();
        pcm.samples.resize(start+capacity);
        uint8_t* dest=reinterpret_cast<uint8_t*>(pcm.samples.data()+start);
        const int n=swr_convert(d.swr,&dest,capacity,source,count);
        if(n<0)return false;
        pcm.samples.resize(start+n);return true;
    };
    auto drain=[&]()->bool{
        for(;;){
            const int rc=avcodec_receive_frame(d.codec,d.frame);
            if(rc==AVERROR(EAGAIN)||rc==AVERROR_EOF)return true;
            if(rc<0)return false;
            auto* f=d.frame;
            if(f->sample_rate<8000||f->sample_rate>48000||f->ch_layout.nb_channels<1||
               f->ch_layout.nb_channels>2||f->nb_samples<=0)return false;
            if(!d.swr){
                input_rate=f->sample_rate;input_format=f->format;channels=f->ch_layout.nb_channels;
                if(swr_alloc_set_opts2(&d.swr,&mono,AV_SAMPLE_FMT_FLT,24000,
                    &f->ch_layout,static_cast<AVSampleFormat>(f->format),f->sample_rate,0,nullptr)<0||
                    swr_init(d.swr)<0)return false;
            }
            if(input_rate!=f->sample_rate||input_format!=f->format||channels!=f->ch_layout.nb_channels)return false;
            const uint8_t* source[2]={f->extended_data[0],channels==2?f->extended_data[1]:nullptr};
            if(!convert(source,f->nb_samples))return false;
            av_frame_unref(f);
        }
    };
    int rc;
    while((rc=av_read_frame(d.format,d.packet))>=0){
        if(d.packet->stream_index==stream&&
            (avcodec_send_packet(d.codec,d.packet)<0||!drain()))return false;
        av_packet_unref(d.packet);
    }
    if(rc!=AVERROR_EOF||avcodec_send_packet(d.codec,nullptr)<0||!drain()||!d.swr)return false;
    if(!convert(nullptr,0))return false;
    bool audible=false;
    for(auto& v:pcm.samples){if(!std::isfinite(v))return false;v=std::clamp(v,-1.f,1.f);audible|=std::abs(v)>0.0001f;}
    if(!audible)return false;
    out=std::move(pcm);return true;
}
inline bool field_type(obs_data_t* obj,const char* field,obs_data_type expected){
    auto* item=obs_data_item_byname(obj,field);
    if(!item)return false;
    const bool ok=obs_data_item_gettype(item)==expected;
    obs_data_item_release(&item);return ok;
}
}
inline bool decode_tiktok_response(const std::string& json,TtsPcm& out){
    out={};
    if(json.empty()||json.size()>TikTokMaxResponse||json.find('\0')!=std::string::npos)return false;
    auto* obj=obs_data_create_from_json(json.c_str());
    if(!obj)return false;
    const bool ok=tiktok_detail::field_type(obj,"success",OBS_DATA_BOOLEAN)&&
        obs_data_get_bool(obj,"success")&&tiktok_detail::field_type(obj,"data",OBS_DATA_STRING);
    std::string encoded=ok?obs_data_get_string(obj,"data"):"";
    obs_data_release(obj);
    if(encoded.empty()||encoded.size()%4)return false;
    size_t padding=0;
    if(encoded.back()=='=')++padding;
    if(encoded.size()>1&&encoded[encoded.size()-2]=='=')++padding;
    for(size_t i=0;i<encoded.size();++i){
        const char c=encoded[i];
        if(i>=encoded.size()-padding){if(c!='=')return false;}
        else if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='+'||c=='/'))return false;
    }
    std::vector<uint8_t> bytes(encoded.size()/4*3);
    const int n=av_base64_decode(bytes.data(),encoded.c_str(),int(bytes.size()));
    if(n<0||size_t(n)!=bytes.size()-padding)return false;
    bytes.resize(n);
    return tiktok_detail::decode_mp3(bytes,out);
}
}
