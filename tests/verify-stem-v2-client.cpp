#include "framework/live-stem-client.hpp"
#include <cassert>
#include <cstdio>
#include <vector>
#include <cmath>
#include <thread>
#include <algorithm>
int main(int argc,char** argv){
    if(argc!=2&&argc!=3)return 2;
    std::atomic<bool> stopping{false};
    stts::LiveStemClient c(stopping);
    if(argc==3){
        // Test-only peer: correct geometry but wrong profile, wrong stem
        // count, then nonfinite audio. None may invoke the publisher.
        std::string path=argv[1];assert(access(path.c_str(),F_OK)!=0);
        int server=socket(AF_UNIX,SOCK_STREAM,0);assert(server>=0);
        sockaddr_un addr{};addr.sun_family=AF_UNIX;
        assert(path.size()<sizeof(addr.sun_path));
        std::strcpy(addr.sun_path,path.c_str());
        assert(bind(server,(sockaddr*)&addr,sizeof(addr))==0);
        assert(chmod(path.c_str(),0600)==0&&listen(server,1)==0);
        constexpr size_t n=123480;
        std::thread peer([&]{
            for(int i=0;i<3;++i){
                int fd=accept(server,nullptr,nullptr);assert(fd>=0);
                unsigned char request[40]{};
                assert(stts::read_fully(fd,request,40));
                const unsigned char golden[]={
                  0x4c,0x4c,0x53,0x54,0x45,0x4d,0x30,0x32,
                  2,0,0,0, 0x44,0xac,0,0, 0x58,0xe2,1,0, 2,0,0,0,
                  45,0,0,0, 0,0,0,0, 0,0,0,0, 1,0,0,0};
                static_assert(sizeof golden==40);
                assert(std::memcmp(request,golden,40)==0);
                std::vector<float> pcm(2*n);
                assert(stts::read_fully(fd,pcm.data(),pcm.size()*4));
                stts::put_le32(request+28,i==1?4:2);
                stts::put_le32(request+36,i==0?99:1);
                assert(stts::write_fully(fd,request,40));
                if(i==2){
                    std::vector<float> y(4*n,0);
                    y.back()=NAN;
                    assert(stts::write_fully(fd,y.data(),y.size()*4));
                }
                close(fd);
            }
        });
        std::vector<float> l(n),r(n);
        for(int i=0;i<3;++i)
            assert(!c.exchange_melband(path,44100,45,l.data(),r.data(),n,[]{return true;},[](const float*,size_t){assert(false);return false;}));
        peer.join();close(server);unlink(path.c_str());
        puts("STEM_V2_REJECTION_PASS=1");
        return 0;
    }
    for(unsigned rate:{44100u,48000u}){
        size_t n=rate*28/10;
        std::vector<float> l(n),r(n);
        for(size_t i=0;i<n;++i){l[i]=.15f*std::sin(i*.05);r[i]=.09f*std::sin(i*.037);}
        double energies[2]={};
        assert(c.exchange_melband(argv[1],rate,45,l.data(),r.data(),n,[]{return true;},[&](const float* out,size_t count){
            assert(count==4*n);
            for(size_t i=0;i<n;++i){
                assert(std::abs(out[i]+out[2*n+i]-l[i])<1e-6f);
                assert(std::abs(out[n+i]+out[3*n+i]-r[i])<1e-6f);
                for(size_t s=0;s<2;++s)energies[s]+=double(out[2*s*n+i])*out[2*s*n+i]+double(out[(2*s+1)*n+i])*out[(2*s+1)*n+i];
            }
            return energies[0]>0&&energies[1]>0;
        }));
        printf("STEM_V2_CLIENT rate=%u vocals_energy=%g instrumental_energy=%g pass=1\n",rate,energies[0],energies[1]);
    }
}
