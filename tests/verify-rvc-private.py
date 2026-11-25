"""Real private resident C ABI RVC worker; no listener, no microphone."""
import array, math, os, pathlib, signal, socket, struct, subprocess, sys, time, wave

helper,lib,model,fixture=map(pathlib.Path,sys.argv[1:])
with wave.open(str(fixture),'rb') as w:
    assert w.getframerate()==16000 and w.getnchannels()==1 and w.getsampwidth()==2
    src=array.array('h',w.readframes(16000))
assert len(src)==16000
pcm=array.array('f',(x/32768 for x in src))
MAGIC=0x52564333
def receive(s,n):
    b=bytearray()
    while len(b)<n:
        x=s.recv(n-len(b))
        if not x:raise EOFError('RVC worker closed')
        b.extend(x)
    return b
def run(device):
    parent,child=socket.socketpair()
    parent.settimeout(45)
    voice=os.environ.get("SOUND_RVC_VOICE",""); index=os.environ.get("SOUND_RVC_INDEX","")
    p=subprocess.Popen([str(helper),str(lib),str(model),device,voice,index,os.environ.get("SOUND_RVC_PACKAGED","default"),"0"],stdin=child,stdout=subprocess.DEVNULL)
    child.close()
    try:
        def req(seq,shift,rate=48000,valid=True,blend=0.,rms=.25,protect=.33,median=1):
            start=time.monotonic()
            parent.sendall(struct.pack('<IIIIifffI',MAGIC,seq,16000,rate,shift,blend,rms,protect,median)+pcm.tobytes())
            magic,reply,status,count=struct.unpack('<IIII',receive(parent,16))
            assert magic==MAGIC and seq==reply
            if not valid:
                assert status==1 and count==0
                return
            assert status==0 and rate*.88<=count<=rate
            out=array.array('f');out.frombytes(receive(parent,count*4))
            assert all(math.isfinite(v) and abs(v)<=1 for v in out)
            assert max(map(abs,out))>.001
            print('RVC_PRIVATE',device,seq,rate,count,round((time.monotonic()-start)*1000,1),flush=True)
            return out
        a=req(1,0)
        req(2,25,valid=False)
        b=req(3,4)
        assert a!=b
        req(4,0,44100)
        req(5,0,valid=False,blend=float('nan'))
        if index or (not voice and os.environ.get("SOUND_RVC_PACKAGED_RETRIEVAL")):
            c=req(6,0,blend=.3)
            assert a!=c
            req(7,0,blend=.6)
        elif voice:
            req(6,0,valid=False,blend=.3)
        req(8,0,valid=False,rms=float('nan'))
        req(9,0,valid=False,protect=-.1)
        req(10,0,valid=False,median=2)
        req(11,0,rms=1.,protect=.5,median=0)
        req(12,0,rms=0.,protect=0.,median=1)
        assert p.poll() is None
        print('RVC_PRIVATE_PASS='+device,flush=True)
    finally:
        parent.close()
        try:p.wait(timeout=5)
        except subprocess.TimeoutExpired:p.kill();p.wait()
run(os.environ.get('SOUND_RVC_TEST_DEVICE','cuda'))
