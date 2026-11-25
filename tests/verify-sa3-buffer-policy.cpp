#include "framework/sa3_buffer_policy.hpp"
#include <cassert>
#include <cstdio>
int main(){
    sa3buffer::Policy p;
    assert(p.target(6000)==2);
    p.observe(1200);assert(p.target(6000)==2);
    p.observe(5500);assert(p.target(6000)==3);
    p.observe(12500);assert(p.target(6000)==3);
    assert(p.target(56000)==2);
    for(int i=0;i<12;i++)p.observe(1000);
    assert(p.target(6000)==2);
    p.observe(1000000);assert(p.target(6000)==3);
    assert(p.target(0)==3);
    printf("SA3_BUFFER_POLICY_PASS=1\n");
}
