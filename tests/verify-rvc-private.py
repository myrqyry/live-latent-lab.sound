"""Real private resident C ABI RVC worker; no listener, no microphone."""
import array, math, os, pathlib, signal, socket, struct, subprocess, sys, time, wave

helper,lib,model,fixture=map(pathlib.Path,sys.argv[1:])
with wave.open(str(fixture),'rb') as w:
    assert w.getframerate()==16000 and w.getnchannels()==1 and w.getsampwidth()==2
    src=array.array('h',w.readframes(16000))
assert len(src)==16000
pcm=array.array('f',(x/32768 for x in src))
MAGIC=0x52564331
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
    p=subprocess.Popen([str(helper),str(lib),str(model),device],stdin=child,stdout=subprocess.DEVNULL)
    child.close()
    try:
        def req(seq,shift,rate=48000,valid=True):
            start=time.monotonic()
            parent.sendall(struct.pack('<IIIIi',MAGIC,seq,16000,rate,shift)+pcm.tobytes())
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
        assert p.poll() is None
        print('RVC_PRIVATE_PASS='+device,flush=True)
    finally:
        parent.close()
        try:p.wait(timeout=5)
        except subprocess.TimeoutExpired:p.kill();p.wait()
run(os.environ.get('SOUND_RVC_TEST_DEVICE','cuda'))
