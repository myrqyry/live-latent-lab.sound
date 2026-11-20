#include "engines/stt_parakeet.hpp"
#include <atomic>
#include <thread>
#include <fstream>
#include <iostream>
int main(int argc,char** argv){
 if(argc!=5)return 2;
 std::ifstream f(argv[3],std::ios::binary|std::ios::ate);
 auto n=f.tellg();if(n<=0||n%4)return 3;
 std::vector<float> pcm(size_t(n)/4);f.seekg(0);f.read(reinterpret_cast<char*>(pcm.data()),n);
 stts::SttParakeet p;
 if(!p.open(argv[1],argv[2]))return 4;
 std::atomic<int> classified=0;
 std::atomic<bool> fail=false,stop=false;
 std::thread other([&](){
  stts::mp_audio_123::Classifier c;std::string error;
  if(!c.open(argv[1],argv[4],error,5)){fail=true;return;}
  std::vector<float> sine(16000);
  for(size_t i=0;i<sine.size();++i)sine[i]=.3f*std::sin(2*3.141592653589793*440*i/16000);
  while(!stop){
    std::vector<stts::mp_audio_123::Prediction> r;
    if(!c.classify(sine.data(),sine.size(),16000,r,error)||r.empty()||r.front().category!="Sine wave"){fail=true;break;}
    ++classified;std::this_thread::sleep_for(std::chrono::milliseconds(40));
  }
 });
 std::string t;bool ok=p.transcribe(pcm,t)&&t.find("country")!=std::string::npos;
 p.close();
 ok=ok&&p.open(argv[1],argv[2])&&p.transcribe(pcm,t)&&t.find("country")!=std::string::npos;
 stop=true;other.join();
 std::cout<<"PARAKEET_SHARED_RUNTIME text="<<ok<<" YAMNet_windows="<<classified<<" failed="<<fail<<std::endl;
 return !(ok&&!fail&&classified>1);
}
