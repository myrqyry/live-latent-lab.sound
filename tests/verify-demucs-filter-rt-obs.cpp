// Actual filter callback against real OBS packet types, using synthetic
// separated output only to qualify transport, not neural inference.
#include "../src/demucs-filter.cpp"
#include <cassert>
#include <cstdio>
int main(){
    constexpr size_t N=32;
    demucs_filter_data filter{};
    filter.capture=std::make_unique<stts::DemucsCapture>(128);
    filter.output=std::make_unique<stts::DemucsFilterQueue>(N);
    std::array<float,N> input{},bass{};
    for(size_t i=0;i<N;++i){input[i]=float(i+1)/N;bass[i]=-input[i];}
    std::array<std::array<float,N>,MAX_AUDIO_CHANNELS> planes{};
    obs_audio_data packet{};
    packet.frames=N;
    for(size_t ch=0;ch<planes.size();++ch)packet.data[ch]=reinterpret_cast<uint8_t*>(planes[ch].data());
    const float* mono[]={input.data()};
    const float* multi[]={input.data(),bass.data(),input.data(),bass.data()};
    filter.tag=8;
    assert(filter.output->publish(8,mono,1,N));
    planes[0]=bass;
    assert(demucs_filter_audio(&filter,&packet)==&packet);
    for(auto& ch:planes)assert(ch==input);
    std::array<float,N> captured{};
    uint64_t epoch=0;
    assert(filter.capture->read_chunk(captured.data(),N,epoch) && captured==bass);
    assert(filter.output->publish(8,mono,1,N));
    filter.tag=21; // change to multichannel: old mono block cannot leak
    assert(filter.output->publish(21,multi,4,N));
    assert(demucs_filter_audio(&filter,&packet)==&packet);
    for(size_t ch=0;ch<planes.size();++ch)assert(planes[ch]==((ch/2)%2?bass:input));
    filter.tag=31; // mode 7, without any separated output
    assert(demucs_filter_audio(&filter,&packet)==&packet);
    for(auto& ch:planes)assert((ch==std::array<float,N>{}));
    puts("DEMUCS_FILTER_RT_OBS_CALLBACK_PASS=1");
}
