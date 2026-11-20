#pragma once
// Linux-only local worker transport. Run ONLY on the inference thread.
// The Python sidecar owns HTDemucs; OBS must never call Python or the socket
// from its audio callback.
#include <atomic>
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
    bool exchange(const std::string& path,uint32_t rate,uint32_t seq,
                  const float* in_l,const float* in_r,size_t frames,
                  int stem,float* out_l,float* out_r){
        if(stopping_.load() || (rate!=44100 && rate!=48000)
           || frames!=size_t(rate)*8 || stem<0 || stem>3
           || !in_l || !in_r || !out_l || !out_r ||
           path.empty() || path.size()>=sizeof(sockaddr_un{}.sun_path))
            return false;
        // The socket is owned by this user and is not a world-exposed server.
        struct stat statbuf{};
        if(::lstat(path.c_str(),&statbuf) || !S_ISSOCK(statbuf.st_mode)
           || statbuf.st_uid!=::geteuid() || (statbuf.st_mode & 077))
            return false;
        int fd=::socket(AF_UNIX,SOCK_STREAM,0);
        if(fd<0)return false;
        {
            std::lock_guard<std::mutex> lock(socket_mutex_);
            if(stopping_.load()){::close(fd);return false;}
            socket_=fd;
        }
        const bool success=exchange_fd(fd,path,rate,seq,in_l,in_r,frames,stem,out_l,out_r);
        {
            std::lock_guard<std::mutex> lock(socket_mutex_);
            if(socket_==fd)socket_=-1;
            ::close(fd);
        }
        return success && !stopping_.load();
    }
private:
    bool exchange_fd(int fd,const std::string& path,uint32_t rate,uint32_t seq,
                     const float* in_l,const float* in_r,size_t frames,
                     int stem,float* out_l,float* out_r){
        timeval timeout{};timeout.tv_sec=12;
        if(::setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)) ||
           ::setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout)))return false;
        sockaddr_un address{};address.sun_family=AF_UNIX;
        std::memcpy(address.sun_path,path.c_str(),path.size()+1);
        if(::connect(fd,reinterpret_cast<const sockaddr*>(&address),sizeof(address)))return false;
        unsigned char request[36]{};
        std::memcpy(request,"LLSTEM01",8);
        put_le32(request+8,1);put_le32(request+12,rate);
        put_le32(request+16,uint32_t(frames));
        put_le32(request+20,2);put_le32(request+24,seq);
        if(!write_fully(fd,request,sizeof request)
           || !write_fully(fd,in_l,frames*sizeof(float))
           || !write_fully(fd,in_r,frames*sizeof(float)))return false;
        unsigned char response[36]{};
        if(!read_fully(fd,response,sizeof response)
           || std::memcmp(response,"LLSTEM01",8)
           || get_le32(response+8)!=1 || get_le32(response+12)!=rate
           || get_le32(response+16)!=frames || get_le32(response+20)!=2
           || get_le32(response+24)!=seq || get_le32(response+28)!=4
           || get_le32(response+32)!=0)return false;
        std::vector<float> data(size_t(4)*2*frames);
        if(!read_fully(fd,data.data(),data.size()*sizeof(float)))return false;
        for(float value:data)if(!std::isfinite(value))return false;
        const size_t offset=size_t(stem)*2*frames;
        std::memcpy(out_l,data.data()+offset,frames*sizeof(float));
        std::memcpy(out_r,data.data()+offset+frames,frames*sizeof(float));
        return true;
    }
    std::atomic<bool>& stopping_;
    std::mutex socket_mutex_;
    int socket_{-1};
};
} // namespace stts
