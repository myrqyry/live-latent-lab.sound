#!/usr/bin/env python3
"""Generate three matching private stereo contexts and CUDA reference PCM.
No downloads. Output directory must not exist. Not a quality ground truth.
"""
import argparse
from pathlib import Path
import sys
import time
import numpy as np
import soundfile as sf
import torch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from melband_model import load_model

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--model",type=Path,required=True)
    p.add_argument("--fixture",type=Path,required=True,help="Local 44.1kHz stereo WAV >=10.8 seconds")
    p.add_argument("--output-dir",type=Path,required=True)
    p.add_argument("--safetensors-package",type=Path)
    args=p.parse_args()
    pcm,rate=sf.read(args.fixture,dtype="float32",always_2d=True)
    if rate!=44100 or pcm.shape[1]!=2 or len(pcm)<476280 or not np.isfinite(pcm).all():
        raise ValueError("Expected finite 44.1kHz stereo fixture of at least 10.8s")
    windows=np.stack([pcm[i*44100:i*44100+123480] for i in (0,4,8)])
    model=load_model(args.model,args.safetensors_package)
    outputs=[]
    with torch.inference_mode():
        for i,x in enumerate(windows):
            z=torch.from_numpy(np.ascontiguousarray(x.T)).unsqueeze(0).to("cuda")
            torch.cuda.synchronize();start=time.monotonic()
            y=model(z);torch.cuda.synchronize()
            print(f"REFERENCE context={i} elapsed_s={time.monotonic()-start:.4f}",flush=True)
            y=y[0].cpu().numpy().T
            if y.shape!=x.shape or not np.isfinite(y).all():
                raise ValueError("Invalid model output")
            outputs.append(y)
    args.output_dir.mkdir(mode=0o700)
    windows.astype("<f4").tofile(args.output_dir/"input.raw")
    np.stack(outputs).astype("<f4").tofile(args.output_dir/"reference.raw")
    print("NATIVE_REFERENCE_PREPARED=1")
if __name__=="__main__":main()
