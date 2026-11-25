#!/usr/bin/env python3
"""Real native worker, binary planar f32 request/reply, no fake backend."""
import array, math, os, pathlib, socket, struct, subprocess, sys, time, wave
helper, model, source = sys.argv[1:4]
device = sys.argv[4] if len(sys.argv)>4 else 'gpu:CUDA0'
def read_pcm(path):
    with wave.open(path,'rb') as f:
        assert (f.getnchannels(),f.getsampwidth(),f.getframerate())==(2,2,44100)
        n=f.getnframes()
        a=array.array('h',f.readframes(n))
        return n,array.array('f',(a[i]/32768 for ch in range(2) for i in range(ch,2*n,2)))
def recv_exact(sock,n):
    b=bytearray()
    while len(b)<n:
        chunk=sock.recv(n-len(b))
        if not chunk:raise RuntimeError('worker closed before full response')
        b.extend(chunk)
    return b
n,pcm=read_pcm(source)
parent,child=socket.socketpair()
parent.settimeout(45)
p=subprocess.Popen([helper,'--serve',model,device],stdin=child,stdout=subprocess.DEVNULL)
child.close()
def request(seq,mode,amount,frames,data):
    start=time.monotonic()
    parent.sendall(struct.pack('<IIIIffII',0x53414d45,seq,frames,mode,amount,.4,3,4)+data.tobytes())
    magic,returned,status,size,builds,ms=struct.unpack('<IIIIII',recv_exact(parent,24))
    assert magic==0x53414d45 and returned==seq and status==0 and size==frames,(returned,status,size)
    out=array.array('f');out.frombytes(recv_exact(parent,size*8))
    assert len(out)==size*2 and all(math.isfinite(v) and abs(v)<=.751 for v in out)
    assert max(map(abs,out))>.005
    elapsed=round((time.monotonic()-start)*1000)
    print(f'SAME_RESIDENT_REQUEST seq={seq} frames={frames} graph_builds={builds} processing_ms={ms} wall_ms={elapsed}',flush=True)
    return out,builds
try:
    a,b=request(1,2,.8,n,pcm)
    c,d=request(2,2,.8,n,pcm)
    assert b==1 and d==1 and a==c
    # Different PCM input, identical graph shape, must not replay stale tensors.
    inverse=array.array('f',(-v for v in pcm))
    e,f=request(3,2,.8,n,inverse)
    assert f==1 and e!=a
    # Change geometry, then return to original shape; only one graph pair cached.
    shorter=n//2
    half=array.array('f',list(pcm[:shorter])+list(pcm[n:n+shorter]))
    _,g=request(4,3,.8,shorter,half)
    h,j=request(5,2,.8,n,pcm)
    assert g==2 and j==3 and h==a
    # Invalid mode must reject without emitting audio or corrupting framing.
    parent.sendall(struct.pack('<IIIIffII',0x53414d45,6,n,99,.8,.4,3,4)+pcm.tobytes())
    magic,seq,status,size,builds,ms=struct.unpack('<IIIIII',recv_exact(parent,24))
    assert (magic,seq,status,size,builds)==(0x53414d45,6,1,0,3)
    i,k=request(7,2,.8,n,pcm)
    assert k==3 and i==a
    if os.getenv('SAME_RESIDENT_SOAK')=='1':
        # Sequential short blocks with actual changing audio. This measures
        # compute/transport, not overlap-add seams or OBS end-to-end latency.
        short=16384
        times=[]
        for step in range(60):
            offset=(step*1024)%(n-short)
            block=array.array('f',list(pcm[offset:offset+short])+list(pcm[n+offset:n+offset+short]))
            start=time.monotonic()
            _,count=request(100+step,1+step%3,.5,short,block)
            assert count==4
            times.append((time.monotonic()-start)*1000)
        times.sort()
        print(f'SAME_RESIDENT_SHORT_BLOCKS count=60 frames={short} p50_ms={times[29]:.1f} p95_ms={times[56]:.1f} max_ms={times[-1]:.1f}')
        assert times[56]<short/44100*1000
    invalid=array.array('f',pcm);invalid[0]=float('nan')
    parent.sendall(struct.pack('<IIIIffII',0x53414d45,999,n,2,.8,.4,3,4)+invalid.tobytes())
    magic,seq,status,size,builds,ms=struct.unpack('<IIIIII',recv_exact(parent,24))
    assert (magic,seq,status,size)==(0x53414d45,999,1,0)
    # Test both EOF shutdown and no child left behind.
    parent.shutdown(socket.SHUT_WR)
    assert p.wait(timeout=5)==0
    print('SAME_RESIDENT_PASS=1')
finally:
    parent.close()
    if p.poll() is None:p.kill();p.wait(timeout=5)
