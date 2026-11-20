#include "engines/stt_nemotron.hpp"
#include <fstream>
#include <iostream>
#include <vector>
#include <algorithm>
int main(int argc,char** argv){
 if(argc!=4)return 2;
 std::ifstream f(argv[3],std::ios::binary|std::ios::ate);auto n=f.tellg();if(n<=0||n%4)return 3;
 std::vector<float> pcm(size_t(n)/4);f.seekg(0);f.read(reinterpret_cast<char*>(pcm.data()),n);
 for(auto lang:{"es-ES","auto"}){
  stts::SttNemotron e;if(!e.open(argv[1],argv[2],4,lang,3)||!e.begin())return 4;
  std::string text;int updates=0;
  for(size_t i=0;i<pcm.size();i+=8960){
   auto old=text;
   if(!e.feed(pcm.data()+i,std::min<size_t>(8960,pcm.size()-i),text))return 5;
   if(text!=old&&!text.empty())++updates;
  }
  if(!e.finish(text)||text.find("gente")==std::string::npos||text.find("funeral")==std::string::npos||updates<2)return 6;
  std::cout<<"NEMOTRON_LOCALE "<<lang<<" updates="<<updates<<" text="<<text<<" pass=1"<<std::endl;
 }
}
