#include "c-api.h"
#include <fstream>
#include <vector>
#include <string>
#include <cstdio>
#include <algorithm>
#include <chrono>
int main(int argc,char** argv){
 if(argc!=3)return 2;
 std::string dir=argv[1],e=dir+"/encoder-epoch-99-avg-1.int8.onnx",d=dir+"/decoder-epoch-99-avg-1.int8.onnx",j=dir+"/joiner-epoch-99-avg-1.int8.onnx",t=dir+"/tokens.txt";
 SherpaOnnxOnlineRecognizerConfig c{};c.feat_config={16000,80};c.model_config.transducer={e.c_str(),d.c_str(),j.c_str()};c.model_config.tokens=t.c_str();c.model_config.provider="cpu";c.model_config.num_threads=2;c.decoding_method="greedy_search";c.max_active_paths=4;
 auto begin=std::chrono::steady_clock::now();auto* r=SherpaOnnxCreateOnlineRecognizer(&c);
 if(!r)return 3;
 printf("LOAD_MS=%lld version=%s\n",(long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-begin).count(),SherpaOnnxGetVersionStr());
 auto* s=SherpaOnnxCreateOnlineStream(r);if(!s)return 4;
 std::ifstream in(argv[2],std::ios::binary|std::ios::ate);if(!in||in.tellg()<=0)return 5;
 std::vector<float> pcm(size_t(in.tellg())/4);in.seekg(0);in.read(reinterpret_cast<char*>(pcm.data()),pcm.size()*4);
 std::string last;int partials=0;begin=std::chrono::steady_clock::now();
 for(size_t i=0;i<pcm.size();i+=1600){
  SherpaOnnxOnlineStreamAcceptWaveform(s,16000,pcm.data()+i,std::min<size_t>(1600,pcm.size()-i));
  while(SherpaOnnxIsOnlineStreamReady(r,s))SherpaOnnxDecodeOnlineStream(r,s);
  auto* a=SherpaOnnxGetOnlineStreamResult(r,s);
  if(a&&a->text&&last!=a->text){last=a->text;printf("PARTIAL %.2fs %s\n",(i+1600)/16000.,last.c_str());++partials;}
  if(a)SherpaOnnxDestroyOnlineRecognizerResult(a);
 }
 SherpaOnnxOnlineStreamInputFinished(s);while(SherpaOnnxIsOnlineStreamReady(r,s))SherpaOnnxDecodeOnlineStream(r,s);
 auto* a=SherpaOnnxGetOnlineStreamResult(r,s);
 std::string final=a&&a->text?a->text:"";
 printf("FINAL %s\nDECODE_MS=%lld partials=%d\n",final.c_str(),(long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-begin).count(),partials);
 if(a)SherpaOnnxDestroyOnlineRecognizerResult(a);SherpaOnnxDestroyOnlineStream(s);SherpaOnnxDestroyOnlineRecognizer(r);
 std::transform(final.begin(),final.end(),final.begin(),[](unsigned char c){return std::tolower(c);});
 bool ok=partials>=2&&final.find("country")!=std::string::npos&&final.find("ask")!=std::string::npos;printf("ZIPFORMER_NATIVE_PASS=%d\n",ok);return ok?0:1;
}
