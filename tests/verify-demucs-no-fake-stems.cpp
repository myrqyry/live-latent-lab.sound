// No OBS dependency: source worker may only publish complete, finite stems.
#include "framework/stem_output_guard.hpp"
#include <cassert>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>
#include <iostream>

int main() {
 using Batch=std::vector<std::vector<float>>;
 std::size_t writes=0;
 auto collect=[&writes](std::size_t, const float*,std::size_t){ ++writes; };
 assert(!stts::stem_batch_is_valid(Batch{},4,480));
 assert(!stts::publish_stems_if_valid(Batch{},4,480,collect));
 assert(writes==0);
 assert(!stts::publish_stems_if_valid(Batch(4),4,480,collect));
 assert(writes==0);
 assert(!stts::publish_stems_if_valid(Batch(3,std::vector<float>(480,0.2f)),4,480,collect));
 assert(writes==0);
 Batch short_stem(4,std::vector<float>(480,.2f));
 short_stem[2].resize(479);
 assert(!stts::publish_stems_if_valid(short_stem,4,480,collect));
 assert(writes==0); // Nothing is written before the fourth stem validates.
 Batch nan_stem(4,std::vector<float>(480,.2f));
 nan_stem[2][23]=std::numeric_limits<float>::quiet_NaN();
 assert(!stts::publish_stems_if_valid(nan_stem,4,480,collect));
 assert(writes==0);
 Batch real_stems(4,std::vector<float>(480,.2f));
 real_stems[3][44]=-.1f;
 assert(stts::stem_batch_is_valid(real_stems,4,480));
 assert(stts::publish_stems_if_valid(real_stems,4,480,collect));
 assert(writes==4);
 std::cout<<"DEMUCS_NO_FAKE_STEMS_PASS=1\n";
}
