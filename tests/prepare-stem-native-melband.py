#!/usr/bin/env python3
"""Prepare a pinned local Mel-Band checkpoint for audio.cpp comparison.
No download or model installation. Writes a new candidate directory only.
The output is not a qualified native model until inference is verified.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from melband_model import SHA256,import_safetensors

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model",type=Path,required=True)
    parser.add_argument("--output-dir",type=Path,required=True)
    parser.add_argument("--safetensors-package",type=Path)
    args=parser.parse_args()
    with args.model.open("rb") as stream:
        if hashlib.file_digest(stream,"sha256").hexdigest()!=SHA256:
            raise ValueError("Unexpected checkpoint")
    load_file=import_safetensors(args.safetensors_package)
    from safetensors.torch import save_file
    state=load_file(str(args.model),device="cpu")
    result={}
    split=removed=0
    # Match audio.cpp tests/roformer/convert_reference_ckpt.py. The
    # native model derives RoPE frequencies and uses separate Q/K/V.
    for key,tensor in state.items():
        if key.endswith(".rotary_embed.freqs"):
            removed+=1
        elif key.endswith(".to_qkv.weight"):
            if tuple(tensor.shape)!=(1536,384):
                raise ValueError("Unexpected QKV shape")
            for name,weight in zip(("q","k","v"),tensor.chunk(3,dim=0)):
                result[key[:-len(".to_qkv.weight")]+".to_"+name+".weight"]=weight.clone()
            split+=1
        else:
            result[key]=tensor.contiguous()
    if len(state)!=684 or len(result)!=696 or split!=12 or removed!=12:
        raise ValueError("Unexpected pinned tensor inventory")
    config=dict(model_type="mel_band_roformer",sample_rate=44100,stereo=True,
        chunk_size=123480,num_overlap=1,normalize=False,dim=384,depth=6,
        num_bands=60,num_stems=1,time_transformer_depth=1,freq_transformer_depth=1,
        dim_head=64,heads=8,n_fft=2048,hop_length=441,win_length=2048,
        stft_normalized=False,mask_estimator_depth=2,mlp_expansion_factor=4,
        has_final_norm=False)
    args.output_dir.mkdir(mode=0o700)
    save_file(result,str(args.output_dir/"model.safetensors"))
    (args.output_dir/"config.json").write_text(json.dumps(config,indent=2)+"\n")
    print(f"NATIVE_MELBAND_PREPARED tensors={len(result)} split_qkv={split} removed_rope={removed}")

if __name__=="__main__":main()
