#include <obs.h>
#include <fstream>
#include <iterator>
#include <cstdio>
#include <cmath>
#include "framework/tiktok_native.hpp"
int main(int argc,char** argv){
    if(argc!=2||!obs_startup("en-US",nullptr,nullptr))return 2;
    std::ifstream in(argv[1],std::ios::binary);
    std::string json((std::istreambuf_iterator<char>(in)),{});
    stts::TtsPcm pcm;
    const bool decoded=stts::decode_tiktok_response(json,pcm);
    size_t audible=0;
    bool finite=true;
    for(float sample:pcm.samples){finite&=std::isfinite(sample)&&std::abs(sample)<=1.01f;audible+=std::abs(sample)>0.0001f;}
    const bool valid=decoded&&pcm.rate==24000&&pcm.samples.size()>2400&&audible>1000&&finite;
    bool rejected=true;
    for(const auto* bad:{"{}", "{\"success\":false,\"data\":\"AAAA\"}", "{\"success\":\"true\",\"data\":\"AAAA\"}", "{\"success\":true,\"data\":\"!!!?\"}", "{\"success\":true,\"data\":\"SUQzAAAA\"}"}){
        rejected&=!stts::decode_tiktok_response(bad,pcm)&&pcm.samples.empty();
    }
    // Same valid MPEG payload, but an explicit rejected/non-boolean status.
    auto* original=obs_data_create_from_json(json.c_str());
    auto* response=obs_data_create();
    obs_data_set_string(response,"data",obs_data_get_string(original,"data"));
    obs_data_set_bool(response,"success",false);
    rejected&=!stts::decode_tiktok_response(obs_data_get_json(response),pcm)&&pcm.samples.empty();
    obs_data_set_string(response,"success","true");
    rejected&=!stts::decode_tiktok_response(obs_data_get_json(response),pcm)&&pcm.samples.empty();
    obs_data_release(response);obs_data_release(original);
    rejected&=!stts::decode_tiktok_response(std::string(stts::TikTokMaxResponse+1,'x'),pcm)&&pcm.samples.empty();
    auto parts=stts::split_speech(std::string(279,'a')+"🌱"+std::string(300,'b'),280);
    std::string reconstructed;
    bool bounded=!parts.empty();
    for(auto& part:parts){reconstructed+=part;bounded&=part.size()<=280&&stts::valid_utf8(part);}
    bounded&=reconstructed==std::string(279,'a')+"🌱"+std::string(300,'b');
    printf("TIKTOK_NATIVE_DECODE valid=%d audible=%zu reject=%d split=%d\n",valid,audible,rejected,bounded);
    obs_shutdown();
    return valid&&rejected&&bounded?0:1;
}
