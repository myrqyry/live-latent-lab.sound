// Opt-in disposable OBS audio source. Does not load a model in the OBS process.
// Captures another OBS source and communicates with an independently warmed
// local MeanVC2 sidecar. The original source remains unmodified on errors.
#include <obs-module.h>
#include <obs.h>
#include <util/platform.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "framework/ring-buffer.hpp"
#include "meanvc2-protocol.h"

struct meanvc2_bridge {
    obs_source_t *self{};
    obs_source_t *upstream{};
    std::string input_name, socket_path;
    std::unique_ptr<stts::RingBuffer> capture{new stts::RingBuffer(16000*2)};
    std::atomic<bool> stop{false};
    std::thread worker;
    std::atomic<size_t> dropped{0};
    int phase=0;
    float accum=0;
    bool supported_rate=false;
};

static int send_all(int fd,const void*buf,size_t bytes) {
    auto *p=(const char*)buf;
    while(bytes) {
        ssize_t n=send(fd,p,bytes,MSG_NOSIGNAL);
        if(n>0) {p+=n;bytes-=(size_t)n;continue;}
        if(n<0 && errno==EINTR)continue;
        return -1;
    }
    return 0;
}
static int recv_all(int fd,void*buf,size_t bytes) {
    auto *p=(char*)buf;
    while(bytes) {
        ssize_t n=recv(fd,p,bytes,0);
        if(n>0) {p+=n;bytes-=(size_t)n;continue;}
        if(n<0 && errno==EINTR)continue;
        return -1;
    }
    return 0;
}

static void captured(void *ctx,obs_source_t *source,const struct audio_data *audio,bool muted) {
    auto &b=*static_cast<meanvc2_bridge*>(ctx);
    if(b.stop.load(std::memory_order_acquire)||muted||!b.supported_rate||
       source==b.self||!audio||!audio->data[0])return;
    const auto *left=(const float*)audio->data[0];
    const auto *right=audio->data[1]?(const float*)audio->data[1]:left;
    std::array<float,1024> temp{};
    size_t count=0;
    for(size_t i=0;i<audio->frames;++i) {
        b.accum += (left[i]+right[i])*0.5f;
        if(++b.phase==3) {
            temp[count++]=b.accum/3.0f;
            b.phase=0;b.accum=0;
            if(count==temp.size()) {
                b.dropped.fetch_add(count-b.capture->write(temp.data(),count));
                count=0;
            }
        }
    }
    if(count)b.dropped.fetch_add(count-b.capture->write(temp.data(),count));
}

static void bridge_worker(meanvc2_bridge *b) {
    std::array<float,MVC2_FRAMES> in{};
    std::array<float,MVC2_MAX_OUTPUT> converted{};
    std::vector<float> l(MVC2_MAX_OUTPUT*3),r(MVC2_MAX_OUTPUT*3);
    while(!b->stop.load()) {
        int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
        if(fd<0)break;
        sockaddr_un addr{};
        addr.sun_family=AF_UNIX;
        if(b->socket_path.empty() || b->socket_path.size()>=sizeof(addr.sun_path)) {
            close(fd);break;
        }
        std::memcpy(addr.sun_path,b->socket_path.c_str(),b->socket_path.size()+1);
        if(connect(fd,(sockaddr*)&addr,sizeof(addr))!=0) {
            close(fd);
            // Avoid replaying stale microphone buffers when service returns.
            std::array<float,2048> discard{};
            while(b->capture->available()>0)
                if(b->capture->read(discard.data(),discard.size())==0)break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        // Verify that the socket is owned by this login, not an untrusted
        // local service masquerading as the native inference sidecar.
        struct ucred peer{};
        socklen_t peer_len=sizeof(peer);
        if(getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&peer_len)!=0 ||
           peer_len!=sizeof(peer) || peer.uid!=getuid()) {
            close(fd);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        timeval tmo{1,0};
        setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tmo,sizeof(tmo));
        setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tmo,sizeof(tmo));
        bool good=true;
        while(good && !b->stop.load()) {
            if(b->capture->available()<MVC2_FRAMES) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));continue;
            }
            if(b->capture->available()>16000) {
                std::array<float,2048> discard{};
                while(b->capture->available()>MVC2_FRAMES)
                    if(b->capture->read(discard.data(),std::min<size_t>(
                            discard.size(),b->capture->available()-MVC2_FRAMES))==0)break;
            }
            if(b->capture->read(in.data(),MVC2_FRAMES)!=MVC2_FRAMES)break;
            mvc2_header header{MVC2_MAGIC,MVC2_FRAMES};
            good=send_all(fd,&header,sizeof(header))==0 &&
                 send_all(fd,in.data(),sizeof(in))==0;
            mvc2_header reply{};
            if(good)good=recv_all(fd,&reply,sizeof(reply))==0;
            if(good && (reply.magic!=MVC2_MAGIC || reply.frames>MVC2_MAX_OUTPUT))good=false;
            if(good && reply.frames)
                good=recv_all(fd,converted.data(),reply.frames*sizeof(float))==0;
            if(!good)break;
            if(reply.frames==0)continue;
            for(size_t i=0;i<reply.frames;++i) {
                const float value=std::isfinite(converted[i])?converted[i]:0.0f;
                for(size_t j=0;j<3;++j)l[3*i+j]=r[3*i+j]=value;
            }
            obs_source_audio out{};
            out.data[0]=(uint8_t*)l.data();
            out.data[1]=(uint8_t*)r.data();
            out.frames=reply.frames*3;
            out.speakers=SPEAKERS_STEREO;
            out.format=AUDIO_FORMAT_FLOAT_PLANAR;
            out.samples_per_sec=48000;
            out.timestamp=os_gettime_ns();
            obs_source_output_audio(b->self,&out);
        }
        close(fd);
        // On any disconnect this source falls silent; upstream remains intact.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

static void stop_bridge(meanvc2_bridge *b) {
    // Stop the capture producer before touching SPSC queue ownership.
    if(b->upstream) {
        obs_source_remove_audio_capture_callback(b->upstream,captured,b);
        obs_source_release(b->upstream);
        b->upstream=nullptr;
    }
    b->stop=true;
    if(b->worker.joinable())b->worker.join();
}
static void start_bridge(meanvc2_bridge *b,obs_data_t *settings) {
    b->input_name=obs_data_get_string(settings,"input_name");
    b->socket_path=obs_data_get_string(settings,"socket_path");
    obs_audio_info info{};
    b->supported_rate=obs_get_audio_info(&info) && info.samples_per_sec==48000;
    if(!b->supported_rate) {
        blog(LOG_WARNING,"MeanVC2 test source requires OBS audio at 48 kHz; disabled");
        return;
    }
    b->capture->reset();
    b->phase=0;b->accum=0;
    b->stop=false;
    if(!b->input_name.empty())b->upstream=obs_get_source_by_name(b->input_name.c_str());
    if(b->upstream==b->self) {
        obs_source_release(b->upstream);b->upstream=nullptr;
    }
    if(!b->upstream)return;
    obs_source_add_audio_capture_callback(b->upstream,captured,b);
    b->worker=std::thread(bridge_worker,b);
}
static void* bridge_create(obs_data_t *settings,obs_source_t *source) {
    auto *b=new meanvc2_bridge();
    b->self=source;
    obs_source_set_async_unbuffered(source,true);
    start_bridge(b,settings);
    return b;
}
static void bridge_destroy(void *data) {
    auto *b=(meanvc2_bridge*)data;
    stop_bridge(b);
    blog(LOG_INFO,"MeanVC2 bridge discarded %zu input samples during this instance",
         b->dropped.load());
    delete b;
}
static void bridge_update(void*data,obs_data_t*settings) {
    auto *b=(meanvc2_bridge*)data;
    const std::string input=obs_data_get_string(settings,"input_name");
    const std::string sock=obs_data_get_string(settings,"socket_path");
    if(input==b->input_name && sock==b->socket_path)return;
    stop_bridge(b);
    start_bridge(b,settings);
}
static void bridge_defaults(obs_data_t *settings) {
    obs_data_set_default_string(settings,"socket_path","");
}
static bool list_source(void *ctx,obs_source_t *source) {
    auto *p=(obs_property_t*)ctx;
    if(obs_source_get_output_flags(source)&OBS_SOURCE_AUDIO)
        obs_property_list_add_string(p,obs_source_get_name(source),obs_source_get_name(source));
    return true;
}
static obs_properties_t *bridge_properties(void*) {
    auto *props=obs_properties_create();
    auto *list=obs_properties_add_list(props,"input_name","Input audio source (leave original unchanged)",
        OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(list,"(not selected)","");
    obs_enum_sources(list_source,list);
    obs_properties_add_text(props,"socket_path","Private Unix socket (local only)",OBS_TEXT_DEFAULT);
    return props;
}
static const char* bridge_name(void*) {return "MeanVC2 bridge (experimental, external sidecar)";}
struct obs_source_info meanvc2_bridge_source=[]{
    obs_source_info i{};
    i.id="meanvc2_bridge_source";
    i.type=OBS_SOURCE_TYPE_INPUT;
    i.output_flags=OBS_SOURCE_AUDIO;
    i.get_name=bridge_name;
    i.create=bridge_create;
    i.destroy=bridge_destroy;
    i.update=bridge_update;
    i.get_defaults=bridge_defaults;
    i.get_properties=bridge_properties;
    return i;
}();
