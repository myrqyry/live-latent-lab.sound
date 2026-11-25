#include "framework/same_overlap.hpp"
#include <cassert>
#include <cstdio>
#include <cmath>
int main(){
    std::vector<float> prev,out;
    same_overlap::blend({0,1,2,3,10,11,12,13},prev,out,2);
    assert((out==std::vector<float>{0,1,10,11}));
    same_overlap::blend({2,3,4,5,12,13,14,15},prev,out,2);
    assert((out==std::vector<float>{2,3,12,13}));
    prev={1,1,1,1,2,2,2,2};
    same_overlap::blend({3,3,3,3,0,0,0,0,4,4,4,4,0,0,0,0},prev,out,4);
    for(int ch=0;ch<2;++ch)for(int i=0;i<4;++i)
        assert(std::abs(out[ch*4+i]-(1+ch+2.f*i/3))<1e-6f);
    bool rejected=false;try{same_overlap::blend({},prev,out,4);}catch(...){rejected=true;}assert(rejected);
    puts("SAME_OVERLAP_POLICY=1");
}
