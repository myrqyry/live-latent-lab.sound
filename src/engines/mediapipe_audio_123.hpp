#pragma once

// Minimal ABI declarations for the *pinned* MediaPipe Tasks 1.2.3 C audio
// classifier. These are structural declarations, not a copy of the SDK.
// tests/verify-mediapipe-audio-native.sh checks their layout against the
// unmodified official v1.2.3 C headers and exercises a real YAMNet model.
//
// No inference or model loading may occur on an OBS audio callback thread.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <dlfcn.h>
#include <string>
#include <utility>
#include <vector>

namespace stts::mp_audio_123 {

struct BaseOptions {
    const char* model_asset_buffer = nullptr;
    unsigned int model_asset_buffer_count = 0;
    const char* model_asset_path = nullptr;
    int file_descriptor = 0;
    int delegate = 0;
    int host_environment = 0;
    int host_system = 1; // Linux
    const char* host_version = nullptr;
    const char* ca_bundle_path = nullptr;
    const char* app_id = nullptr;
    const char* app_version = nullptr;
    const char* npu_dispatch_library_dir = nullptr;
};
struct ClassifierOptions {
    const char* display_names_locale = nullptr;
    int max_results = 5;
    float score_threshold = 0.f;
    const char** category_allowlist = nullptr;
    uint32_t category_allowlist_count = 0;
    const char** category_denylist = nullptr;
    uint32_t category_denylist_count = 0;
};
struct Category {
    int index;
    float score;
    char* category_name;
    char* display_name;
};
struct Categories {
    Category* categories;
    uint32_t categories_count;
    int head_index;
    char* head_name;
};
struct ClassificationResult {
    Categories* classifications;
    uint32_t classifications_count;
    int64_t timestamp_ms;
    bool has_timestamp_ms;
};
struct AudioClassifierResult {
    ClassificationResult* results;
    int results_count;
};
struct AudioData {
    int num_channels;
    double sample_rate;
    float* audio_data;
    size_t audio_data_size;
};
struct Options {
    BaseOptions base_options;
    ClassifierOptions classifier_options;
    int running_mode = 1; // kMpAudioRunningModeAudioClips
    void (*result_callback)(int, AudioClassifierResult*) = nullptr;
};
struct Prediction {
    std::string category;
    int index = -1;
    float score = 0.f;
};

class Classifier {
    using Create = int (*)(Options*, void**, char**);
    using Classify = int (*)(void*, const AudioData*, AudioClassifierResult*, char**);
    using CloseResult = void (*)(AudioClassifierResult*);
    using Close = int (*)(void*, char**);
    using FreeError = void (*)(char*);

public:
    Classifier() = default;
    Classifier(const Classifier&) = delete;
    Classifier& operator=(const Classifier&) = delete;
    ~Classifier() { close(); }

    bool open(const std::string& library_path, const std::string& model_path,
              std::string& error, int max_results=5,const std::string& device="cpu") {
        close();
        if(device!="auto"&&device!="gpu"&&device!="cpu"){
            error="Unknown YAMNet compute delegate";return false;
        }
        if(max_results<1||max_results>1024){error="Invalid maximum category count";return false;}
        max_results_=max_results;
        if(library_path.empty()||model_path.empty()) {
            error="Both native library and YAMNet model paths are required";
            return false;
        }
        // MediaPipe/XNNPACK can retain worker-thread TLS destructors past
        // classifier close. dlclose-unmapping the .so before those threads
        // finish crashes OBS during shutdown. Pin native code until process
        // exit on Linux; still close each classifier/result on its worker.
        int flags=RTLD_NOW|RTLD_LOCAL;
#if defined(__linux__) && defined(RTLD_NODELETE)
        flags|=RTLD_NODELETE;
#endif
        library_=dlopen(library_path.c_str(),flags);
        if(!library_) {
            const char* cause=dlerror();
            error=cause?cause:"Unable to open MediaPipe library";
            return false;
        }
        create_=reinterpret_cast<Create>(dlsym(library_,"MpAudioClassifierCreate"));
        classify_=reinterpret_cast<Classify>(dlsym(library_,"MpAudioClassifierClassify"));
        close_result_=reinterpret_cast<CloseResult>(dlsym(library_,"MpAudioClassifierCloseResult"));
        close_=reinterpret_cast<Close>(dlsym(library_,"MpAudioClassifierClose"));
        free_error_=reinterpret_cast<FreeError>(dlsym(library_,"MpErrorFree"));
        if(!create_ || !classify_ || !close_result_ || !close_ || !free_error_) {
            error="MediaPipe 1.2.3 Audio Classifier C symbols not found";
            close();return false;
        }
        auto try_delegate=[&](int requested,const char* name){
            Options config{};
            config.base_options.model_asset_path=model_path.c_str();
            config.base_options.delegate=requested;
            config.base_options.host_system=1;
            config.classifier_options.max_results=max_results_;
            char* message=nullptr;
            const int status=create_(&config,&classifier_,&message);
            const bool ok=status==0&&classifier_;
            if(!ok){
                error=message?message:"MediaPipe classifier creation failed (status "+
                    std::to_string(status)+")";
            }
            if(message)free_error_(message);
            if(ok){delegate_=name;error.clear();return true;}
            if(classifier_){
                char* close_message=nullptr;
                close_(classifier_,&close_message);
                if(close_message)free_error_(close_message);
                classifier_=nullptr;
            }
            return false;
        };
        if(device!="cpu"&&try_delegate(2,"gpu"))return true;
        if(device=="gpu"){close();return false;}
        if(try_delegate(1,"cpu"))return true;
        close();return false;
    }

    bool classify(const float* samples, size_t count, double sample_rate,
                  std::vector<Prediction>& output, std::string& error) {
        output.clear();
        if(!classifier_ || !samples || count==0 ||
           !std::isfinite(sample_rate) || sample_rate<8000.0 ||
           sample_rate>192000.0) {
            error="Classifier not ready or invalid mono PCM buffer";
            return false;
        }
        // C API takes ownership of neither the caller's PCM nor result object.
        AudioData input{1,sample_rate,const_cast<float*>(samples),count};
        AudioClassifierResult result{};
        char* message=nullptr;
        const int status=classify_(classifier_,&input,&result,&message);
        if(status!=0) {
            error=message?message:("MediaPipe classification status "+
                                    std::to_string(status));
            if(message)free_error_(message);
            close_result_(&result);
            return false;
        }
        if(message)free_error_(message);
        if(result.results && result.results_count>0) {
            const auto& first=result.results[0];
            if(first.classifications && first.classifications_count>0) {
                const auto& head=first.classifications[0];
                for(uint32_t i=0;i<head.categories_count && i<uint32_t(max_results_);++i) {
                    const auto& p=head.categories[i];
                    if(!std::isfinite(p.score)||p.score<0.f||p.score>1.f)
                        continue;
                    const char* label=p.category_name ? p.category_name :
                                        p.display_name ? p.display_name : "";
                    output.push_back(Prediction{label,p.index,p.score});
                }
            }
        }
        close_result_(&result);
        if(output.empty()) {
            error="MediaPipe returned no valid audio categories";
            return false;
        }
        return true;
    }
    bool ready() const {return classifier_!=nullptr;}
    const std::string& delegate()const{return delegate_;}
    void close() {
        if(classifier_ && close_) {
            char* message=nullptr;
            close_(classifier_,&message);
            if(message && free_error_)free_error_(message);
        }
        classifier_=nullptr;delegate_.clear();
        if(library_)dlclose(library_);
        library_=nullptr;
        create_=nullptr;classify_=nullptr;close_result_=nullptr;
        close_=nullptr;free_error_=nullptr;
    }
private:
    std::string delegate_;
    int max_results_=5;
    void* library_=nullptr;
    void* classifier_=nullptr;
    Create create_=nullptr;
    Classify classify_=nullptr;
    CloseResult close_result_=nullptr;
    Close close_=nullptr;
    FreeError free_error_=nullptr;
};

} // namespace stts::mp_audio_123
