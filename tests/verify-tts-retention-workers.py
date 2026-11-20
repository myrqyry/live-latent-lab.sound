"""Real local worker acceptance, no mock speech, no external network."""
import http.client
import io
import json
import os
import socket
import time
import wave

class UnixConnection(http.client.HTTPConnection):
    def __init__(self):
        super().__init__('localhost', timeout=45)
    def connect(self):
        self.sock=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect(os.environ['XDG_RUNTIME_DIR']+'/lll-sound-tts/speech.sock')
def request(path, body=None):
    c=UnixConnection()
    try:
        c.request('GET' if body is None else 'POST',path,
                  None if body is None else json.dumps(body),
                  {'Content-Type':'application/json'})
        r=c.getresponse()
        data=r.read()
        assert r.status==200,(path,r.status,data[:100])
        return data
    finally:
        c.close()
def status():
    return json.loads(request('/v1/models/status'))
def lease(model, action, ttl=0):
    request('/v1/models/lease',dict(model=model,client='worker-acceptance-'+str(os.getpid()),action=action,ttl=ttl))
def speech(model,voice):
    body=request('/v1/audio/speech',dict(model=model,voice=voice,input='Hello from Live Latent Lab.',response_format='wav',retention_ttl=0))
    with wave.open(io.BytesIO(body)) as w:
        assert w.getnchannels()==1 and w.getsampwidth()==2 and w.getnframes()>10000
        assert any(w.readframes(w.getnframes()))
def wait(predicate,timeout=8):
    until=time.monotonic()+timeout
    while time.monotonic()<until:
        if predicate(status()):return
        time.sleep(.1)
    raise AssertionError(status())
try:
    lease('kokoro','acquire')
    lease('piper','acquire')
    speech('kokoro','af_heart')
    speech('piper','en_US-amy-low')
    wait(lambda s: set(s['loaded'])=={'kokoro','piper'})
    lease('kokoro','release')
    wait(lambda s: set(s['loaded'])=={'piper'})
    lease('piper','release')
    wait(lambda s: not s['loaded'])
    lease('kokoro','acquire')
    speech('kokoro','af_heart')
    assert 'kokoro' in status()['loaded']
finally:
    for model in ('kokoro','piper'):lease(model,'release')
wait(lambda s: not s['loaded'])
print('SOUND_TTS_RETENTION_CPU_PASS=1')
