#include "engines/mic_dsp_engine.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>

constexpr unsigned SR=48000;
constexpr size_t N=24000;
static std::vector<float> tone(float hz,float amplitude=.4f,unsigned rate=SR) {
    std::vector<float> out(N);
    for(size_t i=0;i<N;++i) out[i]=amplitude*std::sin(6.283185307179586*hz*double(i)/rate);
    return out;
}
static double rms(const std::vector<float>& v) {
    double sum=0;
    for(size_t i=4096;i<v.size();++i) sum+=double(v[i])*v[i];
    return std::sqrt(sum/(v.size()-4096));
}
static std::vector<float> run(int type,float hz,unsigned rate=SR) {
    stts::MicDspEngine engine;
    engine.configure(static_cast<stts::MicDspEngine::Profile>(type),rate);
    auto samples=tone(hz,.4f,rate);
    engine.process(samples.data(),samples.size());
    for(float x:samples) assert(std::isfinite(x));
    return samples;
}
int main(){
    auto high=tone(9000), low=tone(100), mid=tone(700);
    stts::MicDspEngine off;
    off.configure(stts::MicDspEngine::BYPASS,SR);
    auto dry=high;off.process(dry.data(),dry.size());
    assert(dry==high);

    const double condenser=rms(run(2,9000))/rms(high);
    const double ribbon=rms(run(3,9000))/rms(high);
    const double telephone=rms(run(4,9000))/rms(high);
    const double dyn_low=rms(run(1,100))/rms(low);
    const double radio_mid=rms(run(5,700))/rms(mid);
    stts::MicDspEngine tape;
    tape.configure(stts::MicDspEngine::TAPE,SR);
    auto tape_data=tone(550,.8f), orig=tape_data;
    tape.process(tape_data.data(),tape_data.size());
    double tape_delta=0;
    for(size_t i=4096;i<tape_data.size();++i)
        tape_delta+=std::abs(tape_data[i]-orig[i]);
    tape_delta/=(tape_data.size()-4096);

    auto clamped=run(3,9000,32000);
    assert(rms(clamped)>.0001);

    // Profile change must reset both previous filter state AND coefficients.
    stts::MicDspEngine reconfig;
    reconfig.configure(stts::MicDspEngine::TAPE,SR);
    reconfig.set_parameters(0.f,2.0f);
    auto warmed=tone(700);
    reconfig.process(warmed.data(),warmed.size());
    reconfig.configure(stts::MicDspEngine::RIBBON,SR);
    auto after=tone(9000), fresh=run(3,9000);
    reconfig.process(after.data(),after.size());
    double reconfig_error=0;
    for(size_t i=4096;i<after.size();++i)
        reconfig_error+=std::abs(after[i]-fresh[i]);
    reconfig_error/=(after.size()-4096);

    const bool pass=condenser>1.10 && ribbon<.9 && telephone<.4 &&
        dyn_low<.8 && radio_mid>.1 && tape_delta>.05 &&
        reconfig_error<1e-5;
    std::printf("MIC_DSP_CORE condenser=%.3f ribbon=%.3f telephone=%.3f dynamic_low=%.3f radio_mid=%.3f tape_delta=%.3f reconfig_error=%.7f\n",
        condenser,ribbon,telephone,dyn_low,radio_mid,tape_delta,reconfig_error);
    std::printf("MIC_DSP_CORE_PASS=%d\n",pass);
    return pass?0:1;
}
