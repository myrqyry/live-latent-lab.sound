#include "engines/stt_nemotron.hpp"
#include <fstream>
#include <iostream>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <vector>
int main(int argc,char**argv){
 if(argc!=4&&argc!=5)return 2;
 const std::string device=argc==5?argv[4]:"auto";
 std::ifstream file(argv[3],std::ios::binary|std::ios::ate);
 auto bytes=file.tellg();if(bytes<=0||bytes%4)return 3;
 std::vector<float> pcm(size_t(bytes)/4);file.seekg(0);file.read(reinterpret_cast<char*>(pcm.data()),bytes);
 stts::SttNemotron engine;
 if(engine.open(argv[1],"/nonexistent.gguf",4,"en-US",3))return 4;
 if(!engine.open(argv[1],argv[2],4,"en-US",3,device))return 5;
 std::cout<<"NEMOTRON_SELECTED_BACKEND="<<engine.device_backend()<<std::endl;
 std::string actual=engine.device_backend();
 std::transform(actual.begin(),actual.end(),actual.begin(),[](unsigned char c){return std::tolower(c);});
 if(actual=="unknown"||(device!="auto"&&actual.find(device)==std::string::npos))return 13;
 for(int trial=0;trial<2;++trial){
  if(!engine.begin())return 6;
  std::string text,last;int updates=0;size_t first=0;
  auto started=std::chrono::steady_clock::now();
  for(size_t i=0;i<pcm.size();i+=8960){
   auto count=std::min<size_t>(8960,pcm.size()-i);
   if(!engine.feed(pcm.data()+i,count,text))return 7;
   if(!text.empty()&&text!=last){++updates;if(!first)first=i+count;
    std::cout<<"NEMOTRON_PARTIAL sample="<<i+count<<" "<<text<<std::endl;last=text;}
  }
  if(!engine.finish(text))return 8;
  auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();
  std::cout<<"NEMOTRON_FINAL trial="<<trial<<" first_ms="<<first/16<<" updates="<<updates<<" compute_ms="<<ms<<" "<<text<<std::endl;
  std::transform(text.begin(),text.end(),text.begin(),[](unsigned char c){return std::tolower(c);});
  if(updates<2||first>=pcm.size()||text.find("country")==std::string::npos||text.find("ask")==std::string::npos)return 9;
 }
 engine.close();
 for(int right:{0,6,13}){
   if(!engine.open(argv[1],argv[2],4,"en-US",right,device)||!engine.begin())return 10;
   std::string out;int updates=0;
   for(size_t i=0;i<pcm.size();i+=8960){
      auto old=out;
      if(!engine.feed(pcm.data()+i,std::min<size_t>(8960,pcm.size()-i),out))return 11;
      if(!out.empty()&&out!=old)++updates;
   }
   if(!engine.finish(out)||out.find("country")==std::string::npos||updates<2)return 12;
   std::cout<<"NEMOTRON_CONTEXT right="<<right<<" updates="<<updates<<" pass=1"<<std::endl;
   engine.reset();engine.close();
 }
 puts("SOUND_STT_NEMOTRON_NATIVE_PASS=1");
}
