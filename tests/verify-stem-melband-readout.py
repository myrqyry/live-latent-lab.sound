#!/usr/bin/env python3
"""Opt-in readout/context experiment on a private 44.1 kHz stereo music WAV.
Reference is the same model with full context, not ground-truth stems.
No audio files written or uploaded; no downloads. Reports model-time only.
"""
import argparse
import importlib.util
from pathlib import Path
import time

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--safetensors-package", type=Path)
    args = parser.parse_args()
    import numpy as np
    import soundfile as sf
    import torch
    import os
    os.environ["HF_HUB_OFFLINE"]="1"
    os.environ["TRANSFORMERS_OFFLINE"]="1"
    spec=importlib.util.spec_from_file_location("melband_probe",Path(__file__).with_name("verify-stem-melband-model.py"))
    probe=importlib.util.module_from_spec(spec)
    spec.loader.exec_module(probe)
    pcm,sr=sf.read(args.fixture,dtype="float32",always_2d=True)
    if sr!=44100 or pcm.shape[1]!=2 or len(pcm)<16*sr or not np.isfinite(pcm).all():
        raise ValueError("Need >=16s of finite stereo 44.1 kHz PCM")
    pcm=np.ascontiguousarray(pcm[:16*sr].T)
    model=probe.load_model(args.model,args.safetensors_package)
    def infer(start,frames):
        data=pcm[:,start:start+frames]
        if data.shape!=(2,frames):raise ValueError("Invalid context bounds")
        x=torch.from_numpy(np.ascontiguousarray(data)).unsqueeze(0).to("cuda")
        torch.cuda.synchronize()
        before=time.monotonic()
        with torch.inference_mode(): y=model(x)
        torch.cuda.synchronize()
        elapsed=time.monotonic()-before
        if y.shape!=x.shape or not torch.isfinite(y).all():raise ValueError("Invalid vocal estimate")
        return y[0].cpu().numpy(),elapsed
    # Reference 16 seconds; score exactly the same interior 7 seconds for every configuration.
    reference,elapsed=infer(0,16*sr)
    print(f"READOUT_REFERENCE time={elapsed:.4f}s",flush=True)
    hop=round(.7*sr); begin=8*hop; count=10
    ref=reference[:,begin:begin+count*hop]
    mix=pcm[:,begin:begin+count*hop]
    def rms(y): return float(np.sqrt(np.mean(np.square(y,dtype=np.float64))))
    if rms(ref)<=1e-8:raise ValueError("Degenerate reference")
    for context_hops,lookahead_hops in ((2,.5),(4,.5),(4,1.5),(8,.5),(8,1.5)):
        context=context_hops*hop;lookahead=round(lookahead_hops*hop)
        history=context-hop-lookahead
        chunks=[];blended=[];times=[];previous_future=None
        blend_frames=round(.14*sr)
        weight=(.5-.5*np.cos(np.linspace(0,np.pi,blend_frames))).astype("float32")
        for index in range(count):
            read=begin+index*hop
            y,t=infer(read-history,context)
            chunk=y[:,history:history+hop]
            chunks.append(chunk);times.append(t)
            # Reuse the preceding estimate of the first 140 ms of this hop.
            # Both predictions already exist; no extra model pass or lookahead.
            mixed=chunk.copy()
            if previous_future is not None:
                mixed[:,:blend_frames]=previous_future*(1-weight)+mixed[:,:blend_frames]*weight
            blended.append(mixed)
            previous_future=y[:,history+hop:history+hop+blend_frames].copy()
            assert previous_future.shape==(2,blend_frames)
        edges=np.arange(hop,count*hop,hop)
        for method,parts in (("direct",chunks),("blend140ms",blended)):
            out=np.concatenate(parts,axis=1)
            err=rms(out-ref)/rms(ref)
            # First-difference error exactly at hop boundaries relative to the reference.
            seam=rms((out[:,edges]-out[:,edges-1])-(ref[:,edges]-ref[:,edges-1]))
            recon=float(np.max(np.abs((out+(mix-out))-mix)))
            print(f"READOUT method={method} context={context/sr:.2f}s hop={hop/sr:.2f}s future={lookahead/sr:.2f}s "
                  f"causal_delay={(hop+lookahead)/sr:.2f}s "
                  f"median={np.median(times):.4f}s max={max(times):.4f}s "
                  f"rtf={sum(times)/(count*hop/sr):.4f} ref_rel_rms={err:.5f} seam_rms={seam:.7f} "
                  f"residual_reconstruction_max={recon:.8g}",flush=True)
    print("MELBAND_READOUT_PROBE_PASS=1",flush=True)
if __name__=="__main__":
    main()
