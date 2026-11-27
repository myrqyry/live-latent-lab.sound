// Exercise actual legacy source callbacks and OBS mix structures. No model
// initialization: synthetic separated samples test only transport/render.
#include "../src/demucs-source.cpp"
#include <cassert>
#include <cstdio>

int main(){
    demucs_source_data parent{};
    parent.capture=std::make_unique<stts::DemucsCapture>(4096);
    for(auto& s:parent.stem_buffers)s=std::make_shared<stts::DemucsStemOutput>(4096);
    register_parent("test-parent",&parent);
    stem_source_data child{};
    child.output=bind_stem("test-parent",0);
    assert(child.output && !bind_stem("test-parent",0) && !bind_stem("test-parent",4));
    std::array<float,AUDIO_OUTPUT_FRAMES> a{},b{},c{},d{},input{};
    for(size_t i=0;i<input.size();++i)input[i]=float(i)/input.size();
    struct audio_data packet{};
    packet.data[0]=reinterpret_cast<uint8_t*>(input.data());packet.frames=input.size();
    demucs_audio_capture(&parent,nullptr,&packet,false);
    uint64_t epoch=0;
    assert(parent.capture->read_chunk(a.data(),a.size(),epoch) && a==input);
    assert(child.output->publish(input.data(),input.size()));
    obs_source_audio_mix mix{};
    mix.output[0].data[0]=a.data();mix.output[0].data[1]=b.data();
    mix.output[1].data[0]=c.data();mix.output[1].data[1]=d.data();
    uint64_t ts=0;
    assert(stem_source_audio_render(&child,&ts,&mix,3,2,48000));
    assert(a==input && b==input && c==input && d==input);
    assert(stem_source_audio_render(&child,&ts,&mix,3,2,48000));
    const std::array<float,AUDIO_OUTPUT_FRAMES> zero{};
    assert(a==zero && b==zero && c==zero && d==zero);
    assert(child.output->publish(input.data(),input.size()));
    unregister_parent("test-parent");
    for(auto& s:parent.stem_buffers){s->close();s.reset();}
    assert(stem_source_audio_render(&child,&ts,&mix,3,2,48000));
    assert(a==zero && b==zero && c==zero && d==zero);
    a=input;b=input;c=input;d=input;
    assert(demucs_source_audio_render(&parent,&ts,&mix,3,2,48000));
    assert(a==zero && b==zero && c==zero && d==zero);
    child.output->release();
    puts("DEMUCS_SOURCE_RT_OBS_CALLBACK_PASS=1");
}
