#!/usr/bin/env python3
"""Opt-in real local HTDemucs A/B check. No network, fixture or weights in Git."""
import argparse
import gc
import importlib.util
import os
from pathlib import Path
import tempfile
import time
import numpy as np
from scipy.signal import resample_poly

os.environ["HF_HUB_OFFLINE"]="1"
os.environ["TRANSFORMERS_OFFLINE"]="1"
spec=importlib.util.spec_from_file_location("live_stem_sidecar",Path(__file__).resolve().parents[1]/"tools/live_stem_sidecar.py")
sidecar=importlib.util.module_from_spec(spec)
spec.loader.exec_module(sidecar)

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--model-dir",required=True,type=Path)
    parser.add_argument("--fixture",required=True,type=Path,help="Local interleaved stereo f32, 48 kHz, >=16s")
    args=parser.parse_args()
    import torch
    from audio_separator.separator import Separator
    quality=sidecar.demucs_options(0,.1,4)
    pcm=np.fromfile(args.fixture,dtype="<f4").reshape(-1,2).T
    assert pcm.shape[1]>=16*48000
    windows=[(48000,pcm[:,:8*48000]),(48000,pcm[:,8*48000:16*48000]),
             (44100,resample_poly(pcm[:,:8*48000],147,160,axis=1).astype("float32")),
             (44100,np.zeros((2,44100*8),np.float32))]
    with tempfile.TemporaryDirectory(prefix="stem-ab-") as temp:
        ref=Separator(model_file_dir=str(args.model_dir),output_format="WAV",use_soundfile=True,
            demucs_params=quality,**sidecar.precision_options("fp32"))
        sidecar.require_device("cuda",ref.torch_device)
        ref.load_model("htdemucs.yaml")
        sidecar.require_effective_precision("fp32",ref.effective_precision,ref.torch_device)
        sidecar.warmup(ref,Path(temp))
        reference=[];legacy_times=[]
        for rate,audio in windows:
            start=time.monotonic()
            with tempfile.TemporaryDirectory(dir=temp) as folder:
                reference.append(sidecar.separate_window(ref,audio,rate,Path(folder)))
            legacy_times.append(time.monotonic()-start)
        del ref;gc.collect();torch.cuda.empty_cache()
        start=time.monotonic()
        candidate=sidecar.ResidentDemucs(args.model_dir,"cuda",quality,"fp32")
        load=time.monotonic()-start
        warmup=sidecar.warmup(candidate,Path(temp))
        torch.cuda.reset_peak_memory_stats()
        candidate_times=[]
        model=candidate.model
        first_param=next(model.parameters())
        address=first_param.data_ptr()
        for i,((rate,audio),expected) in enumerate(zip(windows,reference)):
            start=time.monotonic()
            actual=sidecar.separate_window(candidate,audio,rate,None)
            candidate_times.append(time.monotonic()-start)
            assert candidate.model is model and next(model.parameters()).data_ptr()==address
            assert next(model.parameters()).device.type=="cuda"
            diff=actual.astype(np.float64)-expected
            rms=np.sqrt(np.mean(diff**2,axis=(1,2)))
            ref_rms=np.sqrt(np.mean(expected.astype(np.float64)**2,axis=(1,2)))
            # PCM16-equivalent numerical tolerance, separately per named stem.
            limit=np.maximum(2/32768,.005*ref_rms)
            passed=bool(np.all(rms<=limit) and np.max(np.abs(diff))<=.005)
            print(f"STEM_RESIDENT_AB case={i} rate={rate} legacy_s={legacy_times[-len(windows)+i]:.3f} resident_s={candidate_times[-1]:.3f} rms={rms.tolist()} reference_rms={ref_rms.tolist()} max_error={np.max(np.abs(diff)):.7f} pass={int(passed)}",flush=True)
            assert passed
        print(f"STEM_RESIDENT_AB_PASS model_loads={candidate.loads} windows={candidate.windows} load_s={load:.3f} warmup_s={warmup:.3f} legacy_median_s={np.median(legacy_times):.3f} resident_median_s={np.median(candidate_times):.3f} cuda_peak_mib={torch.cuda.max_memory_allocated()/1048576:.1f}",flush=True)
if __name__=="__main__":main()
