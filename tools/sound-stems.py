#!/usr/bin/env python3
"""Explicit local stem profiles; no automatic downloads, service installation or GPU fallback."""
import argparse
import array
import json
import math
import os
from pathlib import Path
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import time

from live_stem_sidecar import socket_path, read_exact

HERE=Path(__file__).resolve().parent
CONFIG=Path(os.getenv("XDG_CONFIG_HOME",str(Path.home()/".config")))/"live-latent-lab-sound/stems.json"
FIELDS={"python","demucs_models","melband_model","safetensors_package"}
PROFILES={"demucs":("lll-sound-stems.sock",1,4,8),
          "melband":("lll-sound-melband.sock",2,2,2.8)}

def validate(data):
    if not isinstance(data,dict) or not data or "python" not in data or any(
        k not in FIELDS or not isinstance(v,str) or not v or not Path(v).is_absolute()
        for k,v in data.items()):
        raise ValueError("Profile requires absolute paths and a Python interpreter; unknown fields rejected")
    return data

def read_config(path):
    info=path.lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_uid!=os.getuid() or info.st_mode&0o077 or info.st_size>16384:
        raise ValueError("Config must be a private regular user-owned file")
    return validate(json.loads(path.read_text()))

def write_config(path,data):
    validate(data)
    if path.is_symlink():raise ValueError("Refusing config symlink")
    path.parent.mkdir(mode=0o700,parents=True,exist_ok=True)
    fd,temp=tempfile.mkstemp(prefix=".stems-",dir=path.parent)
    try:
        with os.fdopen(fd,"w") as f:
            json.dump(data,f,indent=2,sort_keys=True);f.write("\n");f.flush();os.fsync(f.fileno())
        os.link(temp,path) # atomic create, never overwrite an existing profile
    finally:os.unlink(temp)

def endpoint(profile):
    if profile not in PROFILES:raise ValueError("Unknown profile")
    runtime=os.getenv("XDG_RUNTIME_DIR")
    if not runtime:raise ValueError("XDG_RUNTIME_DIR required")
    p=Path(runtime)/PROFILES[profile][0]
    return p

def command(cfg,profile):
    validate(cfg)
    path=socket_path(str(endpoint(profile)))
    python=cfg["python"]
    if not os.access(python,os.X_OK) or not Path(python).is_file():
        raise ValueError("Python interpreter missing or not executable")
    if profile=="demucs":
        if not Path(cfg.get("demucs_models","/nonexistent")).is_dir():
            raise ValueError("Configure an existing Demucs model directory")
        return [python,str(HERE/"live_stem_sidecar.py"),"--model-dir",cfg["demucs_models"],
                "--backend","resident","--device","cuda","--precision","fp32",
                "--shifts","0","--overlap","0.1","--segment-seconds","4","--socket",str(path)]
    if not Path(cfg.get("melband_model","/nonexistent")).is_file():
        raise ValueError("Configure the pinned Mel-Band model")
    args=[python,str(HERE/"live_stem_v2.py"),"--model",cfg["melband_model"],"--socket",str(path)]
    if cfg.get("safetensors_package"):args+=["--safetensors-package",cfg["safetensors_package"]]
    return args

def worker_env():
    env=os.environ.copy()
    env.pop("PYTHONPATH",None);env.pop("PYTHONHOME",None);env.pop("PYTHONOPTIMIZE",None)
    env.update(HF_HUB_OFFLINE="1",TRANSFORMERS_OFFLINE="1",PYTHONDONTWRITEBYTECODE="1",PYTHONNOUSERSITE="1")
    return env

def doctor(cfg):
    result={}
    for profile in PROFILES:
        try:
            p=endpoint(profile)
            key="demucs_models" if profile=="demucs" else "melband_model"
            model=Path(cfg[key])
            if profile=="demucs":
                present=(model/"htdemucs.yaml").is_file() and (model/"955717e8-8726e21a.th").is_file()
            else:present=model.is_file()
            # Import actual runtime, but do not load weights or start a service.
            code="import importlib.metadata as m, torch; assert m.version('audio-separator')=='0.47.0'; assert torch.cuda.is_available(); import numpy, scipy"
            if profile=="melband":
                code+="; from melband_model import import_safetensors; import_safetensors("+repr(cfg.get("safetensors_package"))+")"
            check=subprocess.run([cfg["python"],"-c",code],cwd=HERE,env=worker_env(),capture_output=True,text=True,timeout=30)
            result[profile]={"model_present":present,"runtime_check":check.returncode==0,
                             "runtime_error":check.stderr[-800:] if check.returncode else None,
                             "endpoint":str(p),"endpoint_present":p.exists() or p.is_symlink(),
                             "status":"testable" if present and check.returncode==0 else "missing_requirements"}
        except (KeyError,OSError,ValueError,subprocess.SubprocessError) as e:
            result[profile]={"status":"missing_requirements","reason":str(e)}
    return result

def probe(profile):
    p=endpoint(profile);info=p.lstat()
    if not stat.S_ISSOCK(info.st_mode) or info.st_uid!=os.geteuid() or info.st_mode&0o077:
        raise ValueError("Not a private same-user socket")
    _,version,stems,seconds=PROFILES[profile]
    header=struct.Struct("<8sIIIIIII" if version==1 else "<8s8I")
    magic=b"LLSTEM01" if version==1 else b"LLSTEM02"
    rate=48000;n=384000 if version==1 else 134400;seq=0x534f554e
    base=(magic,version,rate,n,2,seq)
    request=base+(0,0)+(() if version==1 else (1,))
    expected=base+(stems,0)+(() if version==1 else (1,))
    # Silent synthetic PCM: proves protocol/model roundtrip, not separation quality.
    started=time.monotonic()
    with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as s:
        s.settimeout(12)
        s.connect(str(p));s.sendall(header.pack(*request));s.sendall(bytes(2*n*4))
        if header.unpack(read_exact(s,header.size))!=expected:
            raise RuntimeError("Incorrect or failed response")
        y=array.array("f");y.frombytes(read_exact(s,stems*2*n*4))
        if sys.byteorder!="little":y.byteswap()
        if not all(math.isfinite(v) for v in y):raise RuntimeError("Nonfinite response")
    return {"profile":profile,"protocol":version,"stems":stems,"rate":rate,"frames":n,
            "elapsed_ms":round((time.monotonic()-started)*1000,1),"result":"real_silent_roundtrip"}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--config",type=Path,default=CONFIG)
    sub=p.add_subparsers(dest="action",required=True)
    c=sub.add_parser("configure",help="Create a private profile; never overwrite")
    for key in sorted(FIELDS):c.add_argument("--"+key.replace("_","-"),required=key=="python")
    sub.add_parser("doctor",help="Check paths and imports, not model inference")
    for action in ("run","probe"):
        cmd=sub.add_parser(action,help="Run one worker in the foreground" if action=="run" else "Real silent socket roundtrip")
        cmd.add_argument("profile",choices=PROFILES)
    args=p.parse_args()
    try:
        if args.action=="configure":
            cfg={k:str(Path(getattr(args,k)).expanduser().absolute()) for k in FIELDS if getattr(args,k)}
            write_config(args.config,cfg);print(f"SOUND_STEMS_CONFIG={args.config}")
        elif args.action=="probe":print(json.dumps(probe(args.profile),indent=2))
        else:
            cfg=read_config(args.config)
            if args.action=="doctor":
                report=doctor(cfg);print(json.dumps(report,indent=2))
                return 0 if all(x["status"]=="testable" for x in report.values()) else 2
            argv=command(cfg,args.profile)
            print(f"SOUND_STEMS_START profile={args.profile} endpoint={endpoint(args.profile)}",flush=True)
            os.execve(argv[0],argv,worker_env())
    except (OSError,ValueError,RuntimeError,EOFError) as e:
        print(f"SOUND_STEMS_ERROR: {e}",file=sys.stderr);return 2
    return 0

if __name__=="__main__":raise SystemExit(main())
