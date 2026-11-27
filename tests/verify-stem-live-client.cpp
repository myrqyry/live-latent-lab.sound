#include "framework/live-stem-client.hpp"
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <string>
#include <cstring>

int main(int argc,char**argv){
 if(argc!=2)return 2;
 std::string path=argv[1];assert(access(path.c_str(),F_OK)!=0);
 int server=socket(AF_UNIX,SOCK_STREAM,0);assert(server>=0);
 sockaddr_un addr{};addr.sun_family=AF_UNIX;std::strncpy(addr.sun_path,path.c_str(),sizeof(addr.sun_path)-1);
 assert(bind(server,(sockaddr*)&addr,sizeof(addr))==0);
 assert(chmod(path.c_str(),0600)==0);
 assert(listen(server,1)==0);
 std::atomic<bool> worker_ok{false};
 std::thread worker([&]{
  int s=accept(server,nullptr,nullptr);assert(s>=0);
  unsigned char hdr[36]{};
  assert(stts::read_fully(s,hdr,sizeof hdr));
  assert(std::memcmp(hdr,"LLSTEM01",8)==0);
  assert(stts::get_le32(hdr+8)==1);
  assert(stts::get_le32(hdr+12)==48000);
  assert(stts::get_le32(hdr+16)==384000);
  assert(stts::get_le32(hdr+20)==2);
  assert(stts::get_le32(hdr+24)==45);
  std::vector<float> sent(2*384000);
  assert(stts::read_fully(s,sent.data(),sent.size()*sizeof(float)));
  assert(sent[0]==.2f&&sent[384000]==-.12f);
  unsigned char reply[36]{};
  std::memcpy(reply,"LLSTEM01",8);
  stts::put_le32(reply+8,1);stts::put_le32(reply+12,48000);
  stts::put_le32(reply+16,384000);stts::put_le32(reply+20,2);
  stts::put_le32(reply+24,45);stts::put_le32(reply+28,4);
  stts::put_le32(reply+32,0);
  assert(stts::write_fully(s,reply,36));
  std::vector<float> result(4*2*384000);
  for(size_t stem=0;stem<4;++stem){
    for(size_t i=0;i<384000;++i){
      result[(stem*2)*384000+i]=float(stem+1)*.1f;
      result[(stem*2+1)*384000+i]=-float(stem+1)*.07f;
    }
  }
  assert(stts::write_fully(s,result.data(),result.size()*sizeof(float)));
  worker_ok=true;
  close(s);
 });
 std::vector<float> l(384000,.2f),r(384000,-.12f),outl(384000),outr(384000);
 std::atomic<bool> stop{false};
 stts::LiveStemClient client(stop);
 assert(client.exchange(path,48000,45,l.data(),r.data(),384000,3,outl.data(),outr.data()));
 assert(std::abs(outl[200]-.4f)<1e-5 && std::abs(outr[200]+.28f)<1e-5);
 worker.join();
 // Reject untrusted world-readable sockets before any IO or connection.
 assert(chmod(path.c_str(),0666)==0);
 assert(!client.exchange(path,48000,46,l.data(),r.data(),384000,3,outl.data(),outr.data()));
 assert(chmod(path.c_str(),0600)==0);
 // A superseded generation cancels an outstanding request without waiting
 // for a hung sidecar or publishing its response.
 std::atomic<bool> current{true},received{false},finished{false};
 std::thread stalled([&]{
    int s=accept(server,nullptr,nullptr);assert(s>=0);
    unsigned char hdr[36];std::vector<float> input(2*384000);
    assert(stts::read_fully(s,hdr,sizeof hdr));
    assert(stts::read_fully(s,input.data(),input.size()*sizeof(float)));
    received=true;char byte;
    assert(recv(s,&byte,1,0)==0);close(s);
 });
 std::fill(outl.begin(),outl.end(),.99f);
 std::thread request([&]{
    assert(!client.exchange(path,48000,47,l.data(),r.data(),384000,3,outl.data(),outr.data(),[&]{return current.load();}));
    finished=true;
 });
 auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(3);
 while(!received&&std::chrono::steady_clock::now()<limit)std::this_thread::yield();
 assert(received);auto cancelled=std::chrono::steady_clock::now();current=false;
 limit=cancelled+std::chrono::milliseconds(500);
 while(!finished&&std::chrono::steady_clock::now()<limit)std::this_thread::yield();
 assert(finished);request.join();stalled.join();
 assert(outl[0]==.99f);
 puts("SOUND_STEM_CANCEL_PASS=1");
 close(server);unlink(path.c_str());
 assert(worker_ok);
 puts("SOUND_LIVE_STEM_SOCKET_CONTRACT_PASS=1");
}
