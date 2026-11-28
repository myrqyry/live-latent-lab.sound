#!/usr/bin/env python3
"""Experimental local two-stem worker. LLSTEM02 profile 1 only.
Returns full context, not a streaming playback schedule.
No downloads, explicit CUDA FP32, pinned Kijai weights.
"""
import argparse
import os
from pathlib import Path
import socketserver
import signal
import struct
import time

from live_stem_sidecar import read_exact, socket_path

MAGIC=b"LLSTEM02"
VERSION=2
PROFILE=1
HEADER=struct.Struct("<8s8I")
STEMS=("Vocals","Instrumental")

def parse_request(raw):
    if len(raw)!=HEADER.size:raise ValueError("Invalid v2 header size")
    magic,version,rate,n,channels,seq,stems,status,profile=HEADER.unpack(raw)
    if (magic!=MAGIC or version!=VERSION or profile!=PROFILE or rate not in (44100,48000)
        or n!=rate*28//10 or channels!=2 or stems or status):
        raise ValueError("Unsupported v2 profile or geometry")
    return rate,n,seq

def serve_client(conn,model):
    import numpy as np
    rate=n=seq=0
    conn.settimeout(30)
    try:
        rate,n,seq=parse_request(read_exact(conn,HEADER.size))
        pcm=np.frombuffer(read_exact(conn,2*n*4),dtype="<f4").reshape(2,n)
        if not np.isfinite(pcm).all():raise ValueError("Nonfinite input")
        started=time.monotonic()
        vocal=np.asarray(model(pcm,rate),dtype="<f4")
        if vocal.shape!=pcm.shape or not np.isfinite(vocal).all():raise ValueError("Invalid vocal output")
        result=np.stack((vocal,pcm-vocal)).astype("<f4",copy=False)
        if not np.isfinite(result).all():raise ValueError("Invalid residual")
        print(f"STEM_V2_WINDOW seq={seq} rate={rate} seconds=2.8 processing={time.monotonic()-started:.4f}",flush=True)
        conn.sendall(HEADER.pack(MAGIC,VERSION,rate,n,2,seq,2,0,PROFILE))
        conn.sendall(result.tobytes(order="C"))
    except (ValueError,EOFError,OSError,RuntimeError) as exc:
        print(f"STEM_V2_REJECTED seq={seq} reason={type(exc).__name__}: {str(exc)[:120]}",flush=True)
        try:conn.sendall(HEADER.pack(MAGIC,VERSION,rate,n,2,seq,0,1,PROFILE))
        except OSError:pass

class ResidentMelBand:
    def __init__(self,path,safetensors_package=None):
        from melband_model import load_model
        self.model=load_model(path,safetensors_package)
    def __call__(self,pcm,rate):
        import numpy as np
        import torch
        from scipy.signal import resample_poly
        x=resample_poly(pcm,147,160,axis=1) if rate==48000 else pcm
        # copy: incoming buffer is read-only.
        x=torch.from_numpy(np.array(x,dtype="float32",order="C",copy=True)).unsqueeze(0).to("cuda")
        with torch.inference_mode():y=self.model(x)
        if y.shape!=x.shape:raise ValueError("Incorrect model output geometry")
        vocal=y[0].cpu().numpy()
        if rate==48000:vocal=resample_poly(vocal,160,147,axis=1)
        if vocal.shape!=pcm.shape:raise ValueError("Incorrect resampled geometry")
        return vocal

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--model",type=Path,required=True)
    p.add_argument("--socket",required=True,help="Separate private v2 endpoint")
    p.add_argument("--safetensors-package",type=Path,help="Explicit trusted existing safetensors package directory")
    args=p.parse_args()
    endpoint=socket_path(args.socket)
    import numpy as np
    model=ResidentMelBand(args.model,args.safetensors_package)
    model(np.zeros((2,123480),dtype="<f4"),44100)
    class Handler(socketserver.BaseRequestHandler):
        def handle(self):serve_client(self.request,model)
    old=os.umask(0o077)
    try:
        with socketserver.UnixStreamServer(str(endpoint),Handler) as server:
            os.chmod(endpoint,0o600)
            inode=endpoint.stat().st_ino
            print(f"STEM_V2_READY profile=1 names=Vocals,Instrumental socket={endpoint}",flush=True)
            def stop(signum,frame):raise KeyboardInterrupt
            signal.signal(signal.SIGTERM,stop)
            try:server.serve_forever(poll_interval=.2)
            except KeyboardInterrupt:pass
            finally:
                if endpoint.is_socket() and endpoint.stat().st_ino==inode:endpoint.unlink()
    finally:os.umask(old)
    return 0

if __name__=="__main__":raise SystemExit(main())
