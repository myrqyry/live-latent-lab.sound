#include "framework/rvc_window.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
int main(){
 for(size_t rate:{44100,48000}){
  rvc_native::Window w(rate);
  assert(w.needed()==rate*3/4);
  std::vector<float> a(w.needed(),.25f),b(w.needed(),.5f);
  auto x=w.push(a.data(),b.data(),a.size());
  assert(w.needed()==rate/2&&x.size()==16000);
  assert(std::abs(x[8000]-.375f)<.001f&&std::abs(x[1000])<.001f);
  a.resize(w.needed(),.25f);b.resize(w.needed(),.5f);
  x=w.push(a.data(),b.data(),a.size());
  assert(std::abs(x[1000]-.375f)<.001f);
  rvc_native::Pipeline p(rate);
  std::vector<float> l(rate/10,.2f),r(rate/10,-.1f),wet(rate/2,.5f);
  for(int i=0;i<20;++i){
   std::fill(l.begin(),l.end(),.2f);std::fill(r.begin(),r.end(),-.1f);
   if(i==10)assert(p.write(wet.data()));
   if(i==15)assert(p.write(wet.data()));
   p.process(l.data(),r.data(),l.size(),0);
   assert(std::abs(l[0]-(i<15?0:.2f))<1e-6);
   assert(std::abs(r[0]-(i<15?0:-.1f))<1e-6);
  }
  p.fail();std::fill(l.begin(),l.end(),.9f);std::fill(r.begin(),r.end(),.8f);
  p.process(l.data(),r.data(),l.size(),1);
  assert(std::abs(l[0]-.2f)<1e-6&&std::abs(r[0]+.1f)<1e-6);
 }
 puts("RVC_WINDOW_PASS=1");
}
