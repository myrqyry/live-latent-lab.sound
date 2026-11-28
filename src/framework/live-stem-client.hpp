#pragma once
// Linux-only local worker transport. Run ONLY on the inference thread.
// The Python sidecar owns HTDemucs; OBS must never call Python or the socket
// from its audio callback.
#include <atomic>
#include <chrono>
#include <poll.h>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace stts {
inline uint32_t get_le32(const unsigned char* p){
    return uint32_t(p[0]) | (uint32_t(p[1])<<8)
         | (uint32_t(p[2])<<16) | (uint32_t(p[3])<<24);
}
inline void put_le32(unsigned char* p,uint32_t value){
    p[0]=uint8_t(value);p[1]=uint8_t(value>>8);
    p[2]=uint8_t(value>>16);p[3]=uint8_t(value>>24);
}
inline bool read_fully(int fd,void* dst,size_t bytes){
    auto* ptr=static_cast<unsigned char*>(dst);
    while(bytes){
        const ssize_t count=::recv(fd,ptr,bytes,0);
        if(count<0 && errno==EINTR)continue;
        if(count<=0)return false;
        ptr+=count;bytes-=size_t(count);
    }
    return true;
}
inline bool write_fully(int fd,const void* src,size_t bytes){
    auto* ptr=static_cast<const unsigned char*>(src);
    while(bytes){
        const ssize_t count=::send(fd,ptr,bytes,MSG_NOSIGNAL);
        if(count<0 && errno==EINTR)continue;
        if(count<=0)return false;
        ptr+=count;bytes-=size_t(count);
    }
    return true;
}

class LiveStemClient {
public:
    explicit LiveStemClient(std::atomic<bool>& stopping):stopping_(stopping){}
    LiveStemClient(const LiveStemClient&)=delete;
    LiveStemClient& operator=(const LiveStemClient&)=delete;
    ~LiveStemClient(){cancel();}
    void cancel(){
        std::lock_guard<std::mutex> lock(socket_mutex_);
        if(socket_>=0)::shutdown(socket_,SHUT_RDWR);
    }
    // Worker-only. The response is borrowed until the next request; the
    // consumer must copy/publish it synchronously, never retain the pointer.
    template<class Current,class Publish>
    bool exchange_batch(const std::string& path,uint32_t rate,uint32_t seq,
                  const float* in_l,const float* in_r,size_t frames,
                  Current&& current,Publish&& publish){
        if(stopping_.load() || !current() || (rate!=44100 && rate!=48000)
           || frames!=size_t(rate)*8
           || !in_l || !in_r ||
           path.empty() || path.size()>=sizeof(sockaddr_un{}.sun_path))
            return false;
        // The socket is owned by this user and is not a world-exposed server.
        struct stat statbuf{};
        if(::lstat(path.c_str(),&statbuf) || !S_ISSOCK(statbuf.st_mode)
           || statbuf.st_uid!=::geteuid() || (statbuf.st_mode & 077))
            return false;
        response_.resize(size_t(4)*2*frames);
        int fd=::socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
        if(fd<0)return false;
        {
            std::lock_guard<std::mutex> lock(socket_mutex_);
            if(stopping_.load()){::close(fd);return false;}
            socket_=fd;
        }
        const bool success=exchange_fd(fd,path,rate,seq,in_l,in_r,frames,current);
        {
            std::lock_guard<std::mutex> lock(socket_mutex_);
            if(socket_==fd)socket_=-1;
            ::close(fd);
        }
        return success && !stopping_.load() && current() &&
            publish(response_.data(),response_.size()) && current();
    }
    template<class Current>
    bool exchange(const std::string& path,uint32_t rate,uint32_t seq,
                  const float* in_l,const float* in_r,size_t frames,
                  int stem,float* out_l,float* out_r,Current&& current){
        if(stem<0||stem>3||!out_l||!out_r)return false;
        return exchange_batch(path,rate,seq,in_l,in_r,frames,current,
            [&](const float* data,size_t){
                const size_t offset=size_t(stem)*2*frames;
                std::memcpy(out_l,data+offset,frames*sizeof(float));
                std::memcpy(out_r,data+offset+frames,frames*sizeof(float));
                return true;
            });
    }
    bool exchange(const std::string& path,uint32_t rate,uint32_t seq,
                  const float* in_l,const float* in_r,size_t frames,
                  int stem,float* out_l,float* out_r){
        return exchange(path,rate,seq,in_l,in_r,frames,stem,out_l,out_r,[]{return true;});
    }
private:
    template<class Current>
    bool exchange_fd(int fd,const std::string& path,uint32_t rate,uint32_t seq,
                     const float* in_l,const float* in_r,size_t frames,
                     Current& current){
        // One absolute request deadline, not a fresh timeout for each
        // partial transfer. Check generation and shutdown every poll.
        using Clock=std::chrono::steady_clock;
        auto deadline=Clock::now()+std::chrono::seconds(12);
        auto transfer=[&](void* raw,size_t size,bool sending){
            auto* p=static_cast<unsigned char*>(raw);
            while(size&&!stopping_.load()&&current()&&Clock::now()<deadline){
                pollfd event{fd,short(sending?POLLOUT:POLLIN),0};
                int ready=::poll(&event,1,20);
                if(ready<0&&errno==EINTR)continue;
                if(ready<0)return false;
                if(!ready)continue;
                if(!(event.revents&(sending?POLLOUT:POLLIN)))return false;
                auto n=sending?::send(fd,p,size,MSG_NOSIGNAL) : ::recv(fd,p,size,0);
                if(n<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK))continue;
                if(n<=0)return false;
                p+=n;size-=size_t(n);
            }
            return size==0&&!stopping_.load()&&current();
        };
        sockaddr_un address{};address.sun_family=AF_UNIX;
        std::memcpy(address.sun_path,path.c_str(),path.size()+1);
        if(::connect(fd,reinterpret_cast<const sockaddr*>(&address),sizeof(address)))return false;
        unsigned char request[36]{};
        std::memcpy(request,"LLSTEM01",8);
        put_le32(request+8,1);put_le32(request+12,rate);
        put_le32(request+16,uint32_t(frames));
        put_le32(request+20,2);put_le32(request+24,seq);
        if(!transfer(request,sizeof request,true)
           || !transfer(const_cast<float*>(in_l),frames*sizeof(float),true)
           || !transfer(const_cast<float*>(in_r),frames*sizeof(float),true))return false;
        unsigned char response[36]{};
        if(!transfer(response,sizeof response,false)
           || std::memcmp(response,"LLSTEM01",8)
           || get_le32(response+8)!=1 || get_le32(response+12)!=rate
           || get_le32(response+16)!=frames || get_le32(response+20)!=2
           || get_le32(response+24)!=seq || get_le32(response+28)!=4
           || get_le32(response+32)!=0)return false;
        auto& data=response_;
        if(!transfer(data.data(),data.size()*sizeof(float),false))return false;
        for(float value:data)if(!std::isfinite(value))return false;
        return true;
    }
    std::atomic<bool>& stopping_;
    std::mutex socket_mutex_;
    int socket_{-1};
    std::vector<float> response_; // worker-owned reusable response buffer
};
} // namespace stts
