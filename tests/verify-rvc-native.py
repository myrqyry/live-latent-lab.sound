#!/usr/bin/env python3
"""Isolated native RVC qualification. No OBS, downloads, or installed service changes.

Supply a pinned audio.cpp server executable, RVC GGUF, synthetic/authorized
speech WAV (mono PCM16 at 16kHz, >= 2s), output dir and compiled SOLA harness.
Uses a temporary loopback-only server, not a production transport.
"""
import argparse
import base64
from array import array
import hashlib
import io
import json
import math
import os
from pathlib import Path
import socket
import signal
import statistics
import subprocess
import time
import urllib.request
import wave

MODEL_SHA = '09bd8f233e7ae82697826513c94d754c3904e58e30ad503b83437defccf99a72'
SERVER_SHA = '03194177a8a7814add8ca011adb3cca94f8ed65644d046160780d8473aea3b05'
RATE = 48000
HOP = RATE//2

def digest(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()

def wav_bytes(samples,rate):
    buf=io.BytesIO()
    with wave.open(buf,'wb') as f:
        f.setnchannels(1);f.setsampwidth(2);f.setframerate(rate)
        f.writeframes(samples.tobytes())
    return buf.getvalue()

def pcm(b,rate):
    with wave.open(io.BytesIO(b)) as f:
        if (f.getnchannels(),f.getsampwidth(),f.getframerate())!=(1,2,rate):
            raise ValueError('Unexpected PCM geometry')
        x=array('h',f.readframes(f.getnframes()))
        if not x:raise ValueError('Empty PCM')
        return x

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for key in ('server','model','fixture','stitch','output'):
        p.add_argument(key,type=Path)
    p.add_argument('--backend',choices=('cpu','cuda'),default='cuda')
    p.add_argument('--hops',type=int,default=24,choices=(0,12,24,60))
    args=p.parse_args()
    assert digest(args.model)==MODEL_SHA and args.model.stat().st_size==1260187648
    assert digest(args.server)==SERVER_SHA
    if args.backend=='cuda':
        free=int(subprocess.check_output(['nvidia-smi','--query-gpu=memory.free','--format=csv,noheader,nounits'],text=True).splitlines()[0])
        if free<4000:raise RuntimeError(f'Insufficient CUDA headroom: {free} MiB')
    source=pcm(args.fixture.read_bytes(),16000)
    assert len(source)>=32000 and max(map(abs,source))>500
    w=args.output;w.mkdir(parents=True,exist_ok=True)
    with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
    cfg=dict(host='127.0.0.1',port=port,ui=False,ui_management=False,backend=args.backend,device=0,threads=6,lazy_load=True,max_loaded_models=1,idle_unload_ms=120000,min_free_memory_mb=1536,models=[dict(id='rvc',family='rvc',path=str(args.model.resolve()),task='vc',mode='offline',session_options={'rvc.weight_type':'f32','rvc.voice_cache_slots':1})])
    config=w/'server.json';config.write_text(json.dumps(cfg))
    env=os.environ.copy()
    if args.backend=='cpu':env['CUDA_VISIBLE_DEVICES']=''
    base='http://127.0.0.1:'+str(port)
    with (w/'server.log').open('w') as log:
        server=subprocess.Popen([str(args.server.resolve()),'--config',str(config.resolve()),'--no-ui'],cwd=args.server.resolve().parent,env=env,stdout=log,stderr=log)
        stitch=None
        try:
            for _ in range(100):
                if server.poll() is not None:raise RuntimeError('Server exited')
                try:
                    with urllib.request.urlopen(base+'/health',timeout=1) as r:r.read()
                    break
                except OSError:time.sleep(.1)
            else:raise RuntimeError('Server startup timeout')
            def run(samples,shift=0):
                req=dict(model='rvc',request=dict(audio_base64=base64.b64encode(wav_bytes(samples,16000)).decode(),options=dict(voice_id='default',semitone_shift=shift,retrieval_blend=0.,output_sample_rate=RATE)))
                request=urllib.request.Request(base+'/v1/tasks/run',data=json.dumps(req).encode(),headers={'Content-Type':'application/json'})
                t=time.monotonic()
                with urllib.request.urlopen(request,timeout=75) as r:j=json.loads(r.read(2_000_000))
                output=pcm(base64.b64decode(j['audio'],validate=True),RATE)
                ms=(time.monotonic()-t)*1000
                assert abs(len(output)/RATE-len(samples)/16000)<.12
                return output,ms
            stats=[]
            for i,(secs,shift) in enumerate(((1,0),(.5,0),(1,0),(2,0),(1,4))):
                output,ms=run(source[:int(secs*16000)],shift)
                rms=math.sqrt(sum(x*x for x in output)/len(output))/32768
                assert rms>.0001
                (w/f'clip-{i}.wav').write_bytes(wav_bytes(output,RATE))
                print('RVC_CLIP',args.backend,secs,shift,round(ms,1),len(output),round(rms,5),flush=True)
            if args.hops:
                if args.backend!='cuda':raise ValueError('Paced test requires CUDA; use --hops 0 for CPU')
                stitch=subprocess.Popen([str(args.stitch.resolve())],stdin=subprocess.PIPE,stdout=subprocess.PIPE)
                result=array('h')
                start=time.monotonic()
                misses=0;boundaries=[];previous=None
                for j in range(args.hops):
                    # Window covers t-250ms through t+750ms. Initial left context is silence.
                    # Each input hop is exactly 8000 frames, each output hop exactly 24000.
                    t0=j*8000-4000
                    window=array('h',(source[k%len(source)] if k>=0 else 0 for k in range(t0,t0+16000)))
                    # Pace captured windows at the actual half-second hop interval.
                    time.sleep(max(0,start+j*.5-time.monotonic()))
                    output,ms=run(window)
                    start_frame=RATE*24//100
                    count=HOP+RATE//25+RATE//50
                    assert len(output)>=start_frame+count
                    x=array('f',(v/32768 for v in output[start_frame:start_frame+count]))
                    stitch.stdin.write(x.tobytes());stitch.stdin.flush()
                    b=stitch.stdout.read(HOP*4)
                    assert len(b)==HOP*4
                    y=array('f');y.frombytes(b)
                    assert all(math.isfinite(v) and abs(v)<=1.001 for v in y)
                    if previous is not None:boundaries.append(abs(y[0]-previous))
                    previous=y[-1];result.extend(int(max(-1,min(1,v))*32767) for v in y)
                    elapsed=(time.monotonic()-(start+j*.5))*1000
                    if elapsed>500:misses+=1
                    stats.append(elapsed)
                    print('RVC_HOP',j,'request_ms',round(ms,1),'complete_ms',round(elapsed,1),flush=True)
                stitch.stdin.close()
                assert stitch.wait(timeout=5)==0
                assert len(result)==args.hops*HOP
                (w/'stitched.wav').write_bytes(wav_bytes(result,RATE))
                print('RVC_STREAM', 'hops',args.hops,'frames',len(result),'median_ms',round(statistics.median(stats),1),'max_ms',round(max(stats),1),'deadline_misses',misses,'max_boundary_step',round(max(boundaries),5),flush=True)
                assert misses==0
            print('RVC_NATIVE_PASS='+args.backend,flush=True)
        finally:
            if stitch is not None and stitch.poll() is None:
                stitch.kill();stitch.wait()
            server.terminate()
            try:server.wait(timeout=4)
            except subprocess.TimeoutExpired:server.kill();server.wait()

if __name__=='__main__':
    def terminate(_signal, _frame):
        raise SystemExit(143)
    signal.signal(signal.SIGTERM, terminate)
    main()
