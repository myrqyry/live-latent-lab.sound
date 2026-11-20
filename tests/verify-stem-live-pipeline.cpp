#include "framework/live-stem-pipeline.hpp"
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>

static constexpr unsigned RATE=48000;
static constexpr size_t PACKET=480;
static constexpr size_t FRAMES=RATE*8;

int main(){
  stts::LiveStemPipeline flow(RATE);
  assert(flow.ok());
  assert(flow.window_frames()==FRAMES);
  std::vector<float> left(PACKET,0.21f),right(PACKET,-0.13f);
  // Initial eight seconds must return silence but retain two independent input channels.
  for(size_t i=0;i<FRAMES/PACKET;++i){
    flow.process(left.data(),right.data(),PACKET);
    for(size_t n=0;n<PACKET;++n){
      assert(left[n]==0.f&&right[n]==0.f);
    }
    std::fill(left.begin(),left.end(),.21f);
    std::fill(right.begin(),right.end(),-.13f);
  }
  std::vector<float> in_l(FRAMES),in_r(FRAMES);
  assert(flow.read_input_window(in_l.data(),in_r.data()));
  assert(in_l[123]==.21f&&in_r[123]==-.13f);
  assert(in_l.back()==.21f&&in_r.back()==-.13f);
  // Worker returns distinct stereo stem, not a copy of the dry mix.
  std::vector<float> out_l(FRAMES,.045f),out_r(FRAMES,-.075f);
  assert(flow.push_output_window(out_l.data(),out_r.data()));
  for(size_t i=0;i<(RATE*4)/PACKET;++i){
    flow.process(left.data(),right.data(),PACKET);
    for(size_t n=0;n<PACKET;++n)assert(left[n]==0.f&&right[n]==0.f);
    std::fill(left.begin(),left.end(),.21f);
    std::fill(right.begin(),right.end(),-.13f);
  }
  // After exactly 12 seconds the separated audio plays, preserving L/R.
  for(size_t i=0;i<FRAMES/PACKET;++i){
    flow.process(left.data(),right.data(),PACKET);
    for(size_t n=0;n<PACKET;++n)assert(left[n]==.045f&&right[n]==-.075f);
    std::fill(left.begin(),left.end(),.21f);
    std::fill(right.begin(),right.end(),-.13f);
  }
  // No stale/fabricated output: if next window misses its deadline, output is
  // silent and cannot resume at an incorrect position later.
  flow.process(left.data(),right.data(),PACKET);
  assert(!flow.ok());
  for(size_t n=0;n<PACKET;++n)assert(left[n]==0.f&&right[n]==0.f);
  assert(!flow.push_output_window(out_l.data(),out_r.data()));
  puts("SOUND_STEM_LIVE_PIPELINE_PASS=1");

  stts::LiveStemPipeline bad(RATE);
  std::vector<float> huge(33*RATE,.2f);
  assert(!bad.push_input(huge.data(),huge.data(),huge.size()));
  assert(!bad.ok());
  assert(!bad.read_input_window(in_l.data(),in_r.data()));
  // OBS may operate at a different rate; reject unknown shapes.
  stts::LiveStemPipeline unsupported(32000);
  assert(!unsupported.ok());
  return 0;
}
