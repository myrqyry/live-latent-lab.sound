"""Exercise non-default options using actual local and Edge speech workers."""
import http.client
import io
import json
import os
from pathlib import Path
import socket
import sys
import wave

class UnixConnection(http.client.HTTPConnection):
    def __init__(self, name):
        super().__init__('localhost', timeout=45)
        self.path = str(Path(os.environ['XDG_RUNTIME_DIR']) / name / 'speech.sock')
    def connect(self):
        self.sock=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect(self.path)

SERVICES = {'kokoro':'lll-sound-tts','piper':'lll-sound-tts','edge':'lll-sound-edge',
            'chatterbox':'lll-sound-chatterbox'}
VOICES = {'kokoro':'af_heart','piper':'en_US-amy-low','edge':'en-US-AriaNeural','chatterbox':'default'}

def request(model, text, options, expected=200):
    c=UnixConnection(SERVICES[model])
    try:
        c.request('POST','/v1/audio/speech',json.dumps(dict(model=model,voice=VOICES[model],
                  input=text,response_format='wav',retention_ttl=0,options=options)),
                  {'Content-Type':'application/json'})
        r=c.getresponse()
        body=r.read()
        assert r.status==expected,(model,r.status,body[:100])
        if expected!=200:return
        with wave.open(io.BytesIO(body)) as w:
            assert w.getnchannels()==1 and w.getsampwidth()==2 and w.getnframes()>10000
            assert any(w.readframes(w.getnframes()))
            print('SOUND_TTS_REAL_OPTIONS_WORKER',model,'frames',w.getnframes(),'rate',w.getframerate(),flush=True)
        return body
    finally:
        c.close()

# Use only our own neutral synthetic reference, never a user's recording.
ref=Path(sys.argv[1])
ref.write_bytes(request('piper','This is a synthetic reference voice for the Live Latent Lab speech test. We are checking that the local speech model can use its own generated reference audio.',{'length_scale':1.2,'noise_scale':.55,'noise_w_scale':.65,'normalize_audio':False,'volume':.8}))
with wave.open(str(ref)) as w:
    assert 5 < w.getnframes()/w.getframerate() <=60
request('kokoro','həlˈoʊ wˈɜːld',{'is_phonemes':True,'speed':1.1,'trim':False,'sentence_pause':.4,'clause_pause':.2})
request('kokoro','Hello from Live Latent Lab.',{'language':'en-gb','speed':1.1,'trim':True})
request('kokoro','Hello.',{'continuous':True},400)
request('piper','Hello.',{'speaker_id':5},400)
request('chatterbox','Hello.',{'exaggeration':.5},400)
request('edge','Hello.',{'rate':101},400)
print('SOUND_TTS_OPTIONS_WORKERS_PASS=1',flush=True)
