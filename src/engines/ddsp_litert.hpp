#pragma once
// LiteRT 2.4.0 public C API. Keep the optional runtime local to the worker.
#include <litert/c/litert_compiled_model.h>
#include <litert/c/litert_environment.h>
#include <litert/c/litert_model.h>
#include <litert/c/litert_options.h>
#include <litert/c/litert_tensor_buffer.h>
#include <array>
#include <vector>
#include <string>
#include <stdexcept>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <dlfcn.h>
#include "../framework/ddsp_synthesis.hpp"

namespace ddsp {
inline void check(LiteRtStatus s){if(s!=kLiteRtStatusOk)throw std::runtime_error("LiteRT status "+std::to_string(int(s)));}
#define DDSP_API(X) \
 X(LiteRtCreateEnvironment) X(LiteRtDestroyEnvironment) X(LiteRtCreateModelFromFile) X(LiteRtDestroyModel) \
 X(LiteRtCreateOptions) X(LiteRtDestroyOptions) X(LiteRtSetOptionsHardwareAccelerators) X(LiteRtCreateCompiledModel) X(LiteRtDestroyCompiledModel) \
 X(LiteRtGetModelSignature) X(LiteRtGetNumSignatureInputs) X(LiteRtGetNumSignatureOutputs) X(LiteRtGetSignatureInputName) X(LiteRtGetSignatureOutputName) \
 X(LiteRtGetSignatureInputTensorByIndex) X(LiteRtGetSignatureOutputTensorByIndex) X(LiteRtGetRankedTensorType) \
 X(LiteRtGetCompiledModelInputBufferRequirements) X(LiteRtGetCompiledModelOutputBufferRequirements) X(LiteRtCreateManagedTensorBufferFromRequirements) \
 X(LiteRtDestroyTensorBuffer) X(LiteRtGetTensorBufferPackedSize) X(LiteRtLockTensorBuffer) X(LiteRtUnlockTensorBuffer) X(LiteRtRunCompiledModel)
class Runtime {
    void* library=nullptr;
public:
#define DECLARE(name) decltype(&::name) name=nullptr;
    DDSP_API(DECLARE)
#undef DECLARE
    LiteRtEnvironment env=nullptr;
    explicit Runtime(const std::string& path){
        library=dlopen(path.c_str(),RTLD_NOW|RTLD_LOCAL|RTLD_NODELETE);
        if(!library)throw std::runtime_error("Cannot open LiteRT");
        try {
#define RESOLVE(name) name=reinterpret_cast<decltype(name)>(dlsym(library,#name));if(!name)throw std::runtime_error("Missing " #name);
            DDSP_API(RESOLVE)
#undef RESOLVE
            check(LiteRtCreateEnvironment(0,nullptr,&env));
        }catch(...){close();throw;}
    }
    ~Runtime(){close();}
    Runtime(const Runtime&)=delete;
    Runtime& operator=(const Runtime&)=delete;
private:
    void close(){if(env) LiteRtDestroyEnvironment(env);if(library)dlclose(library);}
};
#undef DDSP_API

class Model {
    Runtime& r;
    LiteRtModel model=nullptr;
    LiteRtCompiledModel compiled=nullptr;
    LiteRtOptions options=nullptr;
    std::vector<LiteRtTensorBuffer> inputs,outputs;
    std::vector<std::string> in_names,out_names;
    std::vector<size_t> in_sizes,out_sizes;
    void clear(){
        for(auto b:inputs)if(b)r.LiteRtDestroyTensorBuffer(b);
        for(auto b:outputs)if(b)r.LiteRtDestroyTensorBuffer(b);
        if(compiled)r.LiteRtDestroyCompiledModel(compiled);
        if(options)r.LiteRtDestroyOptions(options);
        if(model)r.LiteRtDestroyModel(model);
    }
    static size_t find(const std::vector<std::string>& names,const char* key){
        auto p=std::find(names.begin(),names.end(),key);
        if(p==names.end())throw std::runtime_error(std::string("Missing DDSP tensor ")+key);
        return size_t(p-names.begin());
    }
public:
    Model(Runtime& rt,const std::string& path,size_t n_in,size_t n_out):r(rt){
        try{
            check(r.LiteRtCreateModelFromFile(r.env,path.c_str(),&model));
            check(r.LiteRtCreateOptions(&options));
            check(r.LiteRtSetOptionsHardwareAccelerators(options,kLiteRtHwAcceleratorCpu));
            check(r.LiteRtCreateCompiledModel(r.env,model,options,&compiled));
            LiteRtSignature sig=nullptr;
            check(r.LiteRtGetModelSignature(model,0,&sig));
            for(bool input:{true,false}){
                LiteRtParamIndex count=0;
                check(input?r.LiteRtGetNumSignatureInputs(sig,&count):r.LiteRtGetNumSignatureOutputs(sig,&count));
                if(count!=(input?n_in:n_out))throw std::runtime_error("DDSP tensor count");
                auto& buffers=input?inputs:outputs;
                auto& names=input?in_names:out_names;
                auto& sizes=input?in_sizes:out_sizes;
                buffers.resize(count);
                for(LiteRtParamIndex i=0;i<count;++i){
                    const char* name=nullptr;LiteRtTensor tensor=nullptr;LiteRtRankedTensorType type{};
                    check(input?r.LiteRtGetSignatureInputName(sig,i,&name):r.LiteRtGetSignatureOutputName(sig,i,&name));
                    check(input?r.LiteRtGetSignatureInputTensorByIndex(sig,i,&tensor):r.LiteRtGetSignatureOutputTensorByIndex(sig,i,&tensor));
                    check(r.LiteRtGetRankedTensorType(tensor,&type));
                    if(!name||type.element_type!=kLiteRtElementTypeFloat32||type.layout.rank!=1||type.layout.dimensions[0]<=0||type.layout.dimensions[0]>1024)throw std::runtime_error("DDSP tensor shape");
                    size_t n=type.layout.dimensions[0];
                    LiteRtTensorBufferRequirements req=nullptr;
                    check(input?r.LiteRtGetCompiledModelInputBufferRequirements(compiled,0,i,&req):r.LiteRtGetCompiledModelOutputBufferRequirements(compiled,0,i,&req));
                    check(r.LiteRtCreateManagedTensorBufferFromRequirements(r.env,&type,req,&buffers[i]));
                    size_t bytes=0;check(r.LiteRtGetTensorBufferPackedSize(buffers[i],&bytes));
                    if(bytes!=n*sizeof(float))throw std::runtime_error("DDSP buffer size");
                    names.emplace_back(name);sizes.push_back(n);
                }
            }
        }catch(...){clear();throw;}
    }
    ~Model(){clear();}
    Model(const Model&)=delete;Model& operator=(const Model&)=delete;
    void put(const char* key,const float* values,size_t n){
        size_t i=find(in_names,key);if(n!=in_sizes[i])throw std::runtime_error("DDSP input shape");
        void* dst=nullptr;check(r.LiteRtLockTensorBuffer(inputs[i],&dst,kLiteRtTensorBufferLockModeWrite));
        std::memcpy(dst,values,n*sizeof(float));check(r.LiteRtUnlockTensorBuffer(inputs[i]));
    }
    void get(const char* key,float* values,size_t n){
        size_t i=find(out_names,key);if(n!=out_sizes[i])throw std::runtime_error("DDSP output shape");
        void* src=nullptr;check(r.LiteRtLockTensorBuffer(outputs[i],&src,kLiteRtTensorBufferLockModeRead));
        std::memcpy(values,src,n*sizeof(float));check(r.LiteRtUnlockTensorBuffer(outputs[i]));
        for(size_t j=0;j<n;++j)if(!std::isfinite(values[j]))throw std::runtime_error("Nonfinite DDSP output");
    }
    void invoke(){check(r.LiteRtRunCompiledModel(compiled,0,inputs.size(),inputs.data(),outputs.size(),outputs.data()));}
};
struct Feature {float hz=0,scaled=0,db=-80,power=0;};
class Features {
    Model model;
public:
    Features(Runtime& r,const std::string& path):model(r,path,1,4){}
    Feature run(const std::array<float,1024>& pcm){
        Feature f;model.put("audio",pcm.data(),pcm.size());model.invoke();
        model.get("output_0",&f.hz,1);model.get("output_1",&f.scaled,1);model.get("output_2",&f.db,1);model.get("output_3",&f.power,1);
        return f;
    }
};
class Decoder {
    Model model;std::array<float,512> state{};
public:
    Decoder(Runtime& r,const std::string& path):model(r,path,3,4){}
    void reset(){state.fill(0);}
    Controls run(const Feature& f){
        Controls c;model.put("f0_scaled",&f.scaled,1);model.put("pw_scaled",&f.power,1);model.put("state",state.data(),state.size());model.invoke();
        model.get("output_0",&c.amplitude,1);model.get("output_1",c.harmonics.data(),60);model.get("output_2",c.noise.data(),65);model.get("output_3",state.data(),512);
        return c;
    }
};
}
