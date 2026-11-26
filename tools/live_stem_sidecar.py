#!/usr/bin/env python3
"""Opt-in local 8-second stereo HTDemucs worker for OBS; no model downloads.

Run with the EXISTING stem-splitter venv, not OBS's Python or a cloud service.
The socket protocol is fixed-size/versioned and rejects incomplete or invalid stem responses.
"""
from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import re
import signal
import socket
import socketserver
import stat
import struct
import tempfile
import time

MAGIC=b"LLSTEM01"
VERSION=1
WINDOW_SECONDS=8
REQUEST=struct.Struct("<8sIIIIIII")
STEMS=("Drums","Bass","Other","Vocals")
STEM_PATTERN=re.compile(r"\((Drums|Bass|Other|Vocals)\)")
MAX_BYTES=WINDOW_SECONDS*48000*2*4

# These are execution modes, not model quantization formats. INT8/INT4 and
# native FP16 Demucs must stay unavailable until compatibility and quality tests.
def precision_options(mode: str) -> dict[str, bool]:
    if mode not in ('fp32','autocast'):
        raise ValueError('Unsupported/unqualified Demucs precision: '+str(mode))
    return {'use_autocast':mode=='autocast','use_native_fp16':False}


def demucs_options(shifts=1, overlap=.25, segment=None):
    # The live worker retains its 8 s request geometry. Limit quality
    # options to bounded candidates; HTDemucs has a training segment limit.
    if type(shifts) is not int or not 0 <= shifts <= 2:
        raise ValueError("Live Demucs shifts must be 0–2")
    if type(overlap) not in (int,float) or not math.isfinite(overlap) or not .05 <= overlap <= .5:
        raise ValueError("Live Demucs overlap must be 0.05–0.5")
    if segment is not None and (type(segment) is not int or not 1 <= segment <= 7):
        raise ValueError("Live Demucs segment must be 1–7 seconds or default")
    return {"segment_size":"Default" if segment is None else str(segment),
            "shifts":shifts,"overlap":overlap,"segments_enabled":True}


def require_device(requested, actual):
    if requested not in ("auto","cpu","cuda"):
        raise ValueError("Unsupported compute device")
    if requested!="auto" and str(actual).split(":")[0]!=requested:
        raise RuntimeError(f"Demucs requested {requested} but runtime selected {actual}")


def require_effective_precision(requested: str, actual: str, device: object) -> None:
    if actual!=requested:
        raise RuntimeError(f'Demucs requested {requested} but runtime selected {actual}; refusing silent fallback')
    if requested=='autocast' and str(device).split(':')[0]!='cuda':
        raise RuntimeError('Demucs autocast is opt-in for verified CUDA only')



def parse_request(raw:bytes)->tuple[int,int,int,int]:
    if len(raw)!=REQUEST.size:raise ValueError("Invalid Sound stem header length")
    magic,version,rate,frames,channels,seq,stems,status=REQUEST.unpack(raw)
    if (magic!=MAGIC or version!=VERSION or rate not in (44100,48000)
        or frames!=rate*WINDOW_SECONDS or channels!=2 or stems or status):
        raise ValueError("Unsupported Sound stem protocol or audio shape")
    return version,rate,frames,seq


def read_exact(stream:socket.socket,n:int)->bytes:
    if n<0 or n>MAX_BYTES*4:raise ValueError("Oversized Sound stem transfer")
    chunks=[]
    remaining=n
    while remaining:
        chunk=stream.recv(min(262144,remaining))
        if not chunk:raise EOFError("Incomplete Sound stem transfer")
        chunks.append(chunk)
        remaining-=len(chunk)
    return b"".join(chunks)


def separate_window(sep, audio,rate:int, temporary:Path):
    import numpy as np
    import soundfile as sf
    from scipy.signal import resample_poly
    if audio.shape!=(2,rate*WINDOW_SECONDS) or not np.isfinite(audio).all():
        raise ValueError("Invalid stereo PCM window")
    model_audio=audio
    if rate==48000:
        model_audio=resample_poly(audio,147,160,axis=1).astype("float32")
    if model_audio.shape!=(2,44100*WINDOW_SECONDS):
        raise ValueError("Invalid 44.1 kHz resampling")
    input_path=temporary/"window.wav"
    sf.write(input_path,model_audio.T,44100,format="WAV",subtype="PCM_16")
    # The loaded audio-separator model snapshots output_dir at load time.
    # Update both its live model and the facade for each ephemeral window.
    sep.output_dir=str(temporary)
    if getattr(sep,"model_instance",None) is not None:
        sep.model_instance.output_dir=str(temporary)
    files=sep.separate(str(input_path))
    if len(files)!=4:raise ValueError("Incomplete Demucs separation")
    by_name={}
    for name in files:
        basename=Path(name).name
        match=STEM_PATTERN.search(basename)
        path=(temporary/basename).resolve()
        if (not match or match.group(1) in by_name or
            path.parent!=temporary.resolve() or not path.is_file()):
            raise ValueError("Untrusted or mismatched stem output")
        by_name[match.group(1)]=path
    if set(by_name)!=set(STEMS):
        raise ValueError("Missing expected separate stems")
    output=[]
    for stem in STEMS:
        y,sr=sf.read(by_name[stem],dtype="float32",always_2d=True)
        if sr!=44100 or y.shape!=(44100*WINDOW_SECONDS,2) or not np.isfinite(y).all():
            raise ValueError("Invalid Demucs stem content")
        y=y.T
        if rate==48000:
            y=resample_poly(y,160,147,axis=1).astype("float32")
        if y.shape!=(2,rate*WINDOW_SECONDS) or not np.isfinite(y).all():
            raise ValueError("Invalid separated audio timing")
        output.append(y)
    return np.asarray(output,dtype="<f4")


def warmup(separator, temporary: Path) -> float:
    # Exercise the real separation before declaring the socket ready. This
    # primes runtime/kernel and filesystem caches, not model residency:
    # the installed audio-separator Demucs adapter reloads weights per call.
    import numpy as np
    rate=48000
    t=np.arange(rate*WINDOW_SECONDS,dtype=np.float32)/rate
    pcm=np.stack((.03*np.sin(2*np.pi*220*t),.025*np.sin(2*np.pi*330*t)))
    started=time.monotonic()
    with tempfile.TemporaryDirectory(prefix="stem-warmup-",dir=temporary) as folder:
        separate_window(separator,pcm,rate,Path(folder))
    return time.monotonic()-started


def serve_client(conn:socket.socket,separator,temporary:Path)->None:
    version,rate,frames,seq=VERSION,0,0,0
    conn.settimeout(30.0)
    try:
        version,rate,frames,seq=parse_request(read_exact(conn,REQUEST.size))
        import numpy as np
        raw=read_exact(conn,frames*2*4)
        audio=np.frombuffer(raw,dtype="<f4").reshape(2,frames)
        if not np.isfinite(audio).all():raise ValueError("Non-finite input audio")
        with tempfile.TemporaryDirectory(prefix="stem-window-",dir=temporary) as folder:
            started=time.monotonic()
            stems=separate_window(separator,audio,rate,Path(folder))
            elapsed=time.monotonic()-started
            print(f"STEM_WINDOW seq={seq} duration={WINDOW_SECONDS}s processing={elapsed:.3f}s stems=4",flush=True)
        header=REQUEST.pack(MAGIC,VERSION,rate,frames,2,seq,4,0)
        conn.sendall(header)
        conn.sendall(stems.tobytes(order="C"))
    except (ValueError,EOFError,OSError,RuntimeError) as exc:
        print(f"STEM_REJECTED seq={seq} reason={type(exc).__name__}: {str(exc)[:150]}",flush=True)
        try:conn.sendall(REQUEST.pack(MAGIC,VERSION,rate,frames,2,seq,0,1))
        except OSError:pass


def socket_path(path:str|None)->Path:
    runtime=os.getenv("XDG_RUNTIME_DIR")
    if not path:
        if not runtime:raise ValueError("Set XDG_RUNTIME_DIR or pass --socket")
        path=str(Path(runtime)/"lll-sound-stems.sock")
    dest=Path(path).absolute()
    parent=dest.parent
    if not parent.is_dir() or parent.is_symlink() or parent.stat().st_uid!=os.getuid():
        raise ValueError("Socket directory must be owned by the current user")
    if parent.stat().st_mode & 0o022:
        raise ValueError("Socket directory must not be group/world writable")
    if dest.exists() or dest.is_symlink():
        raise FileExistsError("Stem sidecar socket path already exists; refusing overwrite")
    return dest


def model_path(value:str|None)->Path:
    if value:return Path(value).expanduser().resolve(strict=True)
    settings=Path.home()/".config/stem-splitter/settings.json"
    saved=json.loads(settings.read_text(encoding="utf-8"))
    return Path(saved["model_dir"]).resolve(strict=True)


class StreamServer(socketserver.UnixStreamServer):
    allow_reuse_address=False
    def __init__(self,address,handler,separator,temporary):
        self.separator=separator
        self.temporary=temporary
        super().__init__(address,handler)


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        serve_client(self.request,self.server.separator,self.server.temporary)


def main()->int:
    parser=argparse.ArgumentParser(description="Local-only buffered live HTDemucs worker")
    parser.add_argument("--model-dir",default=None)
    parser.add_argument("--socket",default=None)
    parser.add_argument('--precision',choices=('fp32','autocast'),default='fp32',
                        help='FP32 reference (default), or measured experimental CUDA mixed FP16 autocast')
    parser.add_argument("--device",choices=("auto","cuda","cpu"),default="auto",
                        help="Require a device or let the installed runtime choose")
    parser.add_argument("--shifts",type=int,default=1,help="Random shift averaging, 0–2")
    parser.add_argument("--overlap",type=float,default=.25,help="Model overlap, 0.05–0.5")
    parser.add_argument("--segment-seconds",type=int,default=None,help="1–7 seconds, omit for model default")
    args=parser.parse_args()
    try:
        quality=demucs_options(args.shifts,args.overlap,args.segment_seconds)
        if args.device=="cpu" and args.precision=="autocast":
            raise ValueError("CUDA autocast cannot use CPU")
    except ValueError as error:
        parser.error(str(error))
    if args.device=="cpu":
        os.environ["CUDA_VISIBLE_DEVICES"]=""
    models=model_path(args.model_dir)
    if not (models/"htdemucs.yaml").is_file() or not any(models.glob("*.th")):
        raise FileNotFoundError("Verified HTDemucs model is not cached; no downloads allowed")
    endpoint=socket_path(args.socket)
    os.environ["HF_HUB_OFFLINE"]="1"
    os.environ["TRANSFORMERS_OFFLINE"]="1"
    from audio_separator.separator import Separator
    print("STEM_MODEL_LOADING existing HTDemucs model only",flush=True)
    sep=Separator(model_file_dir=str(models),output_format="WAV",use_soundfile=True,
                  **precision_options(args.precision),
                  demucs_params=quality)
    require_device(args.device,sep.torch_device)
    sep.load_model("htdemucs.yaml")
    require_effective_precision(args.precision,sep.effective_precision,sep.torch_device)
    actual=sep.model_instance
    if actual.shifts!=quality["shifts"] or actual.overlap!=quality["overlap"] or not actual.segments_enabled:
        raise RuntimeError("Demucs runtime did not retain requested quality settings")
    if args.segment_seconds is not None and str(actual.segment_size)!=str(args.segment_seconds):
        raise RuntimeError("Demucs runtime did not retain requested segment setting")
    print(f"STEM_QUALITY device={sep.torch_device} shifts={actual.shifts} overlap={actual.overlap} segment={actual.segment_size}",flush=True)
    print(f'STEM_PRECISION requested={args.precision} effective={sep.effective_precision} device={sep.torch_device}',flush=True)
    with tempfile.TemporaryDirectory(prefix="lll-sound-stem-sidecar-") as work:
        elapsed=warmup(sep,Path(work))
        print(f"STEM_WARMUP processing={elapsed:.3f}s",flush=True)
        old_umask=os.umask(0o077)
        try:
            with StreamServer(str(endpoint),Handler,sep,Path(work)) as server:
                os.chmod(endpoint,0o600)
                print(f"STEM_SIDECAR_READY socket={endpoint} model=htdemucs window=8s precision={sep.effective_precision}",flush=True)
                try:server.serve_forever(poll_interval=.2)
                except KeyboardInterrupt:pass
        finally:
            os.umask(old_umask)
            if endpoint.exists() and endpoint.is_socket():
                endpoint.unlink()
    return 0

if __name__=="__main__":
    raise SystemExit(main())
