#include "framework/same_texture.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
using namespace same_texture;
int main(){
    const std::vector<float> a{0,10,2,12,4,14,6,16,8,18};
    auto eq=[](const auto& x,const auto& y){assert(x==y);};
    eq(process(a,2,5,Mode::Freeze,0,.5f,2,2),a);
    eq(process(a,2,5,Mode::Freeze,1,.5f,2,2),std::vector<float>{4,14,4,14,4,14,4,14,4,14});
    eq(process(a,2,5,Mode::Freeze,.5f,0,2,2),std::vector<float>{0,10,1,11,2,12,3,13,4,14});
    eq(process(a,2,5,Mode::Smear,1,0,1,2),std::vector<float>{1,11,2,12,4,14,6,16,7,17});
    eq(process(a,2,5,Mode::Smear,0,0,2,2),a);
    eq(process(a,2,5,Mode::Grain,1,.5f,1,2),std::vector<float>{4,14,6,16,4,14,6,16,4,14});
    eq(process(a,2,5,Mode::Grain,1,1,1,2),std::vector<float>{6,16,8,18,6,16,8,18,6,16});
    bool rejected=false;try{process(a,2,4,Mode::Freeze,1,0,1,2);}catch(...){rejected=true;}assert(rejected);
    rejected=false;try{process(a,2,5,Mode::Freeze,NAN,0,1,2);}catch(...){rejected=true;}assert(rejected);
    puts("SAME_TEXTURE_POLICY=1");
}
