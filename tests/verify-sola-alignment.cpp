// Isolated, deterministic SOLA check. This tests real waveform stitching,
// not RVC inference, ONNX readiness, microphone capture, or OBS installation.
#include "framework/sola_stitcher.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Audio = std::vector<float>;
constexpr size_t kHop = 3200;      // 80 ms at native RVC 40 kHz
constexpr size_t kOverlap = 1600;  // 40 ms overlap
constexpr size_t kSearch = 480;    // 12 ms SOLA search at 40 kHz
constexpr int kSampleRate = 40000;
using stts::SolaStitcher;
using stts::SolaSettings;
struct Result {
    Audio aligned, unaligned;
    size_t alignment_misses = 0;
    size_t offset_checks = 0;
    size_t silent_fallbacks = 0;
    size_t hops = 0;
    double align_rms = 0;
    double noalign_rms = 0;
    double align_boundary_delta = 0;
    double noalign_boundary_delta = 0;
};
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
uint16_t u16(const char *p) {
    return uint16_t(uint8_t(p[0])) | uint16_t(uint8_t(p[1])) << 8;
}
uint32_t u32(const char *p) {
    return uint32_t(uint8_t(p[0])) | uint32_t(uint8_t(p[1])) << 8 |
           uint32_t(uint8_t(p[2])) << 16 | uint32_t(uint8_t(p[3])) << 24;
}
void w16(std::ofstream &f,uint16_t v) {
    char b[2]={char(v),char(v>>8)};f.write(b,2);
}
void w32(std::ofstream &f,uint32_t v) {
    char b[4]={char(v),char(v>>8),char(v>>16),char(v>>24)};f.write(b,4);
}
Audio read_wav_pcm16_mono(const std::string& filename) {
    std::ifstream f(filename,std::ios::binary|std::ios::ate);
    require(bool(f),"Could not read WAV file");
    const auto size=f.tellg();
    require(size>44 && size<(32<<20),"Invalid or oversized test WAV");
    std::vector<char> bytes(size_t(size),'\0');
    f.seekg(0);f.read(bytes.data(),size);
    require(bool(f),"Truncated WAV");
    require(std::memcmp(bytes.data(),"RIFF",4)==0 &&
            std::memcmp(bytes.data()+8,"WAVE",4)==0,"Not a RIFF WAVE");
    size_t at=12;bool format_found=false;
    size_t data_start=0,data_size=0;
    while(at+8<=bytes.size()) {
        uint32_t len=u32(bytes.data()+at+4);
        at+=8;
        require(len<=bytes.size()-at,"Invalid WAV chunk length");
        if (std::memcmp(bytes.data()+at-8,"fmt ",4)==0) {
            require(len>=16 && u16(bytes.data()+at)==1 &&
                    u16(bytes.data()+at+2)==1 &&
                    u32(bytes.data()+at+4)==kSampleRate &&
                    u16(bytes.data()+at+14)==16,
                    "Expected 40-kHz mono PCM16 WAV");
            format_found=true;
        } else if(std::memcmp(bytes.data()+at-8,"data",4)==0) {
            data_start=at;data_size=len;
        }
        at+=len+(len%2);
    }
    require(format_found && data_size && !(data_size%2),
            "Missing WAV fmt or mono PCM samples");
    Audio pcm(data_size/2);
    for(size_t i=0;i<pcm.size();++i) {
        uint16_t sample=u16(bytes.data()+data_start+2*i);
        pcm[i]=float(int16_t(sample))/32768.0f;
    }
    return pcm;
}
void write_wav_pcm16_mono(const std::string& filename, const Audio& pcm) {
    require(pcm.size()<(1u<<23),"Output WAV too large");
    std::ofstream f(filename,std::ios::binary);
    require(bool(f),"Cannot create output WAV");
    f.write("RIFF",4);w32(f,uint32_t(36+2*pcm.size()));
    f.write("WAVEfmt ",8);w32(f,16);w16(f,1);w16(f,1);
    w32(f,kSampleRate);w32(f,kSampleRate*2);w16(f,2);w16(f,16);
    f.write("data",4);w32(f,uint32_t(2*pcm.size()));
    for(float x:pcm) {
        const float bounded=std::max(-1.0f,std::min(1.0f,x));
        const int32_t integer=int32_t(std::round(double(bounded)*32767.0));
        w16(f,uint16_t(int16_t(integer)));
    }
    require(bool(f),"Output WAV write failed");
}
Audio synthetic() {
    Audio output(kSampleRate*6);
    constexpr double tau=6.2831853071795864769;
    uint32_t rng=0x1badb002U;
    for(size_t i=0;i<output.size();++i) {
        rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;
        double noise=(double(rng)/4294967295.0-0.5);
        double seconds=double(i)/kSampleRate;
        // Voiced-like harmonics + reproducible broad-band content.
        output[i]=float(
            0.23*std::sin(tau*(170*seconds+11.3*seconds*seconds))+
            0.11*std::sin(tau*(403*seconds-2.2*seconds*seconds))+
            0.07*std::sin(tau*883*seconds)+0.07*noise);
    }
    return output;
}
double rms_difference(const Audio& a,const Audio& b) {
    require(a.size()==b.size(),"Audio length changed unexpectedly");
    double energy=0;
    for(size_t i=0;i<a.size();++i) {
        double diff=double(a[i])-b[i];energy+=diff*diff;
    }
    return std::sqrt(energy/a.size());
}
double boundary_derivative_error(const Audio& actual,const Audio& truth) {
    double sum=0;size_t count=0;
    for(size_t i=kHop;i<actual.size();i+=kHop) {
        const double delta_actual=double(actual[i])-actual[i-1];
        const double delta_expected=double(truth[i])-truth[i-1];
        sum+=std::abs(delta_actual-delta_expected);++count;
    }
    return count ? sum/count : 0;
}
Result run(const Audio& signal,bool jitter) {
    SolaStitcher aligned({kHop,kOverlap,kSearch,0.2f,1.0f});
    SolaStitcher baseline({kHop,kOverlap,0,0.2f,1.0f});
    const std::array<size_t,11> offsets={0,111,480,319,46,425,208,377,87,466,253};
    const size_t hops=(signal.size()+kHop-1)/kHop;
    Result m;m.hops=hops;
    m.aligned.reserve(hops*kHop);m.unaligned.reserve(hops*kHop);
    for(size_t tick=0;tick<hops;++tick) {
        const size_t wanted=(jitter && tick)?offsets[tick%offsets.size()]:0;
        Audio candidate(kHop+kOverlap+kSearch);
        // Independent model windows with deterministic timing shifts,
        // modeled using *real* or synthetic voice PCM. Not new inference.
        for(size_t i=0;i<candidate.size();++i) {
            const int64_t index=int64_t(tick*kHop)+int64_t(i)-int64_t(wanted);
            if(index>=0 && size_t(index)<signal.size())
                candidate[i]=signal[size_t(index)];
        }
        auto a=aligned.push(candidate.data(),candidate.size());
        auto b=baseline.push(candidate.data(),candidate.size());
        require(a.pcm.size()==kHop && b.pcm.size()==kHop,
                "SOLA failed to produce one full hop");
        m.aligned.insert(m.aligned.end(),a.pcm.begin(),a.pcm.end());
        m.unaligned.insert(m.unaligned.end(),b.pcm.begin(),b.pcm.end());
        if(tick) {
            if(a.alignment_used) {
                ++m.offset_checks;
                if(a.offset_frames!=wanted)++m.alignment_misses;
            } else {
                ++m.silent_fallbacks;
            }
        } else require(a.offset_frames==0,"First chunk must use offset zero");
    }
    require(m.aligned.size()==hops*kHop &&
            m.unaligned.size()==hops*kHop,"Output hop count drifted");
    m.aligned.resize(signal.size());
    m.unaligned.resize(signal.size());
    m.align_rms=rms_difference(m.aligned,signal);
    m.noalign_rms=rms_difference(m.unaligned,signal);
    m.align_boundary_delta=boundary_derivative_error(m.aligned,signal);
    m.noalign_boundary_delta=boundary_derivative_error(m.unaligned,signal);
    return m;
}
void negative_tests() {
    bool threw=false;
    try {SolaStitcher invalid({0,kOverlap,kSearch});}catch(const std::invalid_argument&){threw=true;}
    require(threw,"Zero hop accepted");
    threw=false;
    try {SolaStitcher invalid({kHop,kOverlap,kSearch,1.0f,0.2f});}catch(const std::invalid_argument&){threw=true;}
    require(threw,"Invalid fade interval accepted");
    SolaStitcher align({kHop,kOverlap,kSearch});
    Audio silent(kHop+kOverlap+kSearch,0.0f);
    auto first=align.push(silent.data(),silent.size());
    auto second=align.push(silent.data(),silent.size());
    require(first.pcm==second.pcm && second.offset_frames==0 &&
            !second.alignment_used,"Silence must not trigger phase matching");
    threw=false;
    try {align.push(silent.data(),kHop);}catch(const std::invalid_argument&){threw=true;}
    require(threw,"Incomplete lookahead was accepted");
    Audio invalid=silent;invalid[10]=std::numeric_limits<float>::quiet_NaN();
    threw=false;
    try {align.push(invalid.data(),invalid.size());}catch(const std::invalid_argument&){threw=true;}
    require(threw,"Nonfinite input was accepted");
    align.reset();require(!align.primed(),"Reset failed");
    auto after_reset=align.push(silent.data(),silent.size());
    require(first.pcm==after_reset.pcm,"Reset changed the initial output");
}
void show(const char* label,const Result& m) {
    std::cout<<label<<" hops="<<m.hops
      <<" output_frames="<<m.aligned.size()
      <<" match_checks="<<m.offset_checks
      <<" offset_misses="<<m.alignment_misses
      <<" silence_fallbacks="<<m.silent_fallbacks
      <<" noalign_rms="<<m.noalign_rms
      <<" sola_rms="<<m.align_rms
      <<" noalign_boundary_error="<<m.noalign_boundary_delta
      <<" sola_boundary_error="<<m.align_boundary_delta<<"\n";
}

void pair_independently_converted(const char *path_a, const char *path_b,
                                  const char *path_full, const char *path_out) {
    // Independent native audio.cpp inference of 0.00-3.60s and
    // 2.50s-end, joined at 3.20s. The clips are independently
    // generated by the IBM 7094 checkpoint (no sample fabrication).
    const Audio a=read_wav_pcm16_mono(path_a);
    const Audio b=read_wav_pcm16_mono(path_b);
    const Audio reference=read_wav_pcm16_mono(path_full);
    constexpr size_t join=128000;    // 3.20 s at 40 kHz
    constexpr size_t b_nominal=28000;// 3.20s - 2.50s
    constexpr size_t pre=kSearch/2;  // allows +/-6 ms relative shift
    require(join+kOverlap+kSearch<=a.size(),
            "First independent output lacks future overlap");
    require(b_nominal+kHop+kOverlap+kSearch<=b.size(),
            "Second independent output lacks future overlap");

    SolaStitcher stitch({kHop,kOverlap,kSearch,0.2f,1.0f});
    SolaStitcher noalign({kHop,kOverlap,0,0.2f,1.0f});
    const float *warm=a.data()+join-kHop;
    const size_t warm_len=kHop+kOverlap+kSearch;
    (void)stitch.push(warm,warm_len);
    (void)noalign.push(warm,warm_len);
    const float* candidate=b.data()+b_nominal-pre;
    const auto aligned=stitch.push(candidate,kHop+kOverlap+kSearch);
    const auto unaligned=noalign.push(b.data()+b_nominal,
                                    kHop+kOverlap+kSearch);
    require(aligned.pcm.size()==kHop && unaligned.pcm.size()==kHop,
            "Stitch must emit an exact 80ms hop");

    double aligned_overlap_error=0, unaligned_overlap_error=0;
    for(size_t i=0;i<kOverlap;++i) {
        const double p=a[join+i];
        const double x=b[b_nominal-pre+aligned.offset_frames+i];
        const double y=b[b_nominal+i];
        aligned_overlap_error+=(p-x)*(p-x);
        unaligned_overlap_error+=(p-y)*(p-y);
    }
    aligned_overlap_error=std::sqrt(aligned_overlap_error/kOverlap);
    unaligned_overlap_error=std::sqrt(unaligned_overlap_error/kOverlap);

    auto stitch_audio=[&](const Audio& hop,size_t offset,
                          bool aligned_flag)->Audio {
        Audio output(a.begin(),a.begin()+join);
        output.insert(output.end(),hop.begin(),hop.end());
        const size_t tail_begin=b_nominal+(aligned_flag ?
                                               offset-pre : 0)+kHop;
        require(tail_begin<b.size(),"Missing tail in second RVC window");
        output.insert(output.end(),b.begin()+tail_begin,b.end());
        return output;
    };
    Audio aligned_audio=stitch_audio(aligned.pcm,aligned.offset_frames,true);
    Audio unaligned_audio=stitch_audio(unaligned.pcm,0,false);
    Audio hardsplice(a.begin(),a.begin()+join);
    hardsplice.insert(hardsplice.end(),b.begin()+b_nominal,b.end());

    const auto local_ref_error=[&](const Audio& output)->double {
        const size_t last=std::min({output.size(),reference.size(),join+kHop});
        double accum=0;
        for(size_t i=join;i<last;++i) {
            const double delta=output[i]-reference[i];
            accum+=delta*delta;
        }
        return std::sqrt(accum/double(last-join));
    };
    const auto derivative_at_seam=[&](const Audio& output)->double {
        return std::abs(double(output[join])-output[join-1]);
    };
    const double reference_rms_aligned=local_ref_error(aligned_audio);
    const double reference_rms_unaligned=local_ref_error(unaligned_audio);
    const double seam_jump_hard=derivative_at_seam(hardsplice);
    const double seam_jump_unaligned=derivative_at_seam(unaligned_audio);
    const double seam_jump_aligned=derivative_at_seam(aligned_audio);

    // A single best-offset selection changes the total length by up
    // to the +/-6ms search range. Correcting only the *audition tail*
    // makes the comparisons equal-duration; it is NOT proof of stable
    // A/V synchronization in a genuine streaming RVC processor.
    const int64_t length_adjustment=int64_t(reference.size())-
                                    int64_t(aligned_audio.size());
    aligned_audio.resize(reference.size(),0.0f);
    unaligned_audio.resize(reference.size(),0.0f);
    hardsplice.resize(reference.size(),0.0f);
    const std::filesystem::path dest(path_out);
    require(std::filesystem::is_directory(dest),"Missing RVC pair output folder");
    write_wav_pcm16_mono((dest/"rvc-independent-hard-splice.wav").string(),
                         hardsplice);
    write_wav_pcm16_mono((dest/"rvc-independent-crossfade-noalign.wav").string(),
                         unaligned_audio);
    write_wav_pcm16_mono((dest/"rvc-independent-sola.wav").string(),
                         aligned_audio);
    std::cout<<"SOLA_RVC_INDEPENDENT_PAIR"
             <<" offset_frames="<<aligned.offset_frames
             <<" nominal_offset="<<pre
             <<" alignment_used="<<aligned.alignment_used
             <<" overlap_noalign_rms="<<unaligned_overlap_error
             <<" overlap_sola_rms="<<aligned_overlap_error
             <<" seam_hard_jump="<<seam_jump_hard
             <<" seam_noalign_jump="<<seam_jump_unaligned
             <<" seam_sola_jump="<<seam_jump_aligned
             <<" reference_noalign_rms="<<reference_rms_unaligned
             <<" reference_sola_rms="<<reference_rms_aligned
             <<" audition_length_adjust_frames="<<length_adjustment
             <<" output_frames="<<aligned_audio.size()<<"\n";
    std::cout<<"SOLA_RVC_INDEPENDENT_PAIR_PASS=1 (comparison only; no audio quality assertion)\n";
}

} // namespace

int main(int argc,char **argv) {
    try {
        if(argc==6 && std::string(argv[1])=="--independent") {
            pair_independently_converted(argv[2],argv[3],argv[4],argv[5]);
            return 0;
        }
        if(argc!=1 && argc!=3) {
            std::cerr<<"Usage: verify-sola-alignment [real-40k-mono-pcm16.wav output_directory]\n";
            return 2;
        }
        negative_tests();
        const Audio sample=synthetic();
        const auto unshifted=run(sample,false);
        const auto shifted=run(sample,true);
        show("SOLA_NO_SHIFT",unshifted);
        show("SOLA_SYNTHETIC_SHIFT",shifted);
        require(unshifted.align_rms<1e-6 && unshifted.noalign_rms<1e-6,
                "An unshifted stream should not change");
        require(shifted.alignment_misses==0 && shifted.silent_fallbacks==0,
                "SOLA failed to recover known synthetic offsets");
        require(shifted.align_rms<1e-6,"SOLA did not reconstruct synthetic stream");
        require(shifted.noalign_rms>shifted.align_rms*1000,
                "Baseline no-alignment control was not meaningfully worse");
        require(shifted.align_boundary_delta<shifted.noalign_boundary_delta*0.01,
                "SOLA failed to improve boundary error");
        if(argc==3) {
            const Audio actual=read_wav_pcm16_mono(argv[1]);
            const auto real_unchanged=run(actual,false);
            const auto real_shifted=run(actual,true);
            show("SOLA_RVC_NO_SHIFT",real_unchanged);
            show("SOLA_RVC_SHIFT",real_shifted);
            require(real_unchanged.align_rms<1e-6,
                    "SOLA unnecessarily changed a native RVC waveform");
            require(real_shifted.align_rms<real_shifted.noalign_rms,
                    "SOLA failed to improve the real RVC waveform");
            require(real_shifted.align_boundary_delta<real_shifted.noalign_boundary_delta,
                    "SOLA failed to improve real RVC seam errors");
            const std::filesystem::path dest(argv[2]);
            require(std::filesystem::exists(dest) &&
                    std::filesystem::is_directory(dest),
                    "Audition folder does not exist");
            write_wav_pcm16_mono((dest/"sola-controlled-unaligned.wav").string(),
                                  real_shifted.unaligned);
            write_wav_pcm16_mono((dest/"sola-controlled-realigned.wav").string(),
                                  real_shifted.aligned);
            std::cout<<"SOLA_AUDITIONS="<<dest.string()<<"\n";
        }
        std::cout<<"SOLA_DETERMINISTIC_PASS=1\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr<<"SOLA_TEST_FAILED="<<e.what()<<"\n";
        return 1;
    }
}
