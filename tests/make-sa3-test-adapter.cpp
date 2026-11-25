// Test-only nontrained rank-2 adapter against one actual Small-Music tensor.
// Never ship this as a creative model or as trained weights.
#include <gguf.h>
#include <ggml.h>
#include <cmath>
#include <cstdio>
int main(int argc,char** argv){
    if(argc!=3)return 2;
    ggml_context* metadata=nullptr;
    auto* base=gguf_init_from_file(argv[1],{true,&metadata});
    if(!base)return 3;
    const auto* target=ggml_get_tensor(metadata,"dit.0.self.qkv.weight");
    if(!target||ggml_n_dims(target)!=2||target->ne[0]!=1024||target->ne[1]!=3072)return 4;
    gguf_free(base);ggml_free(metadata);
    auto* ctx=ggml_init({1024*1024,nullptr,false});
    auto* g=gguf_init_empty();if(!ctx||!g)return 5;
    gguf_set_val_str(g,"general.architecture","sa3-lora");
    gguf_set_val_str(g,"lora.target","dit");
    gguf_set_val_str(g,"lora.adapter_type","lora");
    gguf_set_val_str(g,"lora.base_model","small-music");
    gguf_set_val_u32(g,"lora.rank",2);gguf_set_val_f32(g,"lora.alpha",2.f);
    auto* a=ggml_new_tensor_2d(ctx,GGML_TYPE_F32,1024,2);
    auto* b=ggml_new_tensor_2d(ctx,GGML_TYPE_F32,2,3072);
    ggml_set_name(a,"dit.0.self.qkv.lora_A");ggml_set_name(b,"dit.0.self.qkv.lora_B");
    auto* av=static_cast<float*>(a->data);auto* bv=static_cast<float*>(b->data);
    for(int i=0;i<2048;++i)av[i]=.025f*std::sin(float(i)*.127f);
    for(int i=0;i<6144;++i)bv[i]=.025f*std::cos(float(i)*.071f);
    gguf_add_tensor(g,a);gguf_add_tensor(g,b);
    const bool ok=gguf_write_to_file(g,argv[2],false);
    gguf_free(g);ggml_free(ctx);
    return ok?0:1;
}
