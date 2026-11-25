"""Validate built-in-voice native Turbo and optionally run actual inference."""
import io, pathlib, sys, time, wave
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'tools'))
from tts_worker import Engine,speech_options
from native_turbo import Codec, Worker
from native_kokoro import Codec as KokoroCodec, Worker as KokoroWorker
assert Codec.device == Worker.device == 'cpu'
assert KokoroCodec.device == KokoroWorker.device == 'cuda'

assert speech_options('chatterbox_native',{})==dict(temperature=.8,top_k=1000,top_p=.95,repetition_penalty=1.2,seed=0,max_tokens=1000,text_chunk_size=128)
for raw in ({'audio_prompt_path':'x.wav'},{'norm_loudness':True},{'top_k':6564},{'temperature':False}):
    try:speech_options('chatterbox_native',raw)
    except ValueError:pass
    else:raise AssertionError(raw)
e=Engine('reader',pathlib.Path('/nonexistent'))
assert not e.supports('chatterbox_native')
from native_turbo import validate
try:validate('default','x'*129,{})
except ValueError:pass
else:raise AssertionError('Oversized CPU request accepted')

if len(sys.argv)==3:
    e=Engine('reader',pathlib.Path('/nonexistent'),turbo_library=pathlib.Path(sys.argv[1]),turbo_model=pathlib.Path(sys.argv[2]))
    assert e.supports('chatterbox_native')
    def synth(text='Hello from Live Latent Lab.',options=None):
        start=time.monotonic()
        b=e.synthesize('chatterbox_native','default',text,options)
        with wave.open(io.BytesIO(b)) as f:
            assert (f.getframerate(),f.getnchannels(),f.getsampwidth())==(24000,1,2)
            assert any(f.readframes(f.getnframes()))
            print('NATIVE_TURBO',round(f.getnframes()/24000,3),round(1000*(time.monotonic()-start),1),flush=True)
        return b
    try:
        synth()
        p=e.models['chatterbox_native'].process.pid
        synth('[laugh] That was unexpected. [sigh] Let us try again.',{'temperature':.65,'top_k':50,'top_p':.9,'repetition_penalty':1.3,'seed':42,'max_tokens':512,'text_chunk_size':64})
        assert e.models['chatterbox_native'].process.pid==p
        try:e.synthesize('chatterbox_native','other','Hello.')
        except ValueError:pass
        else:raise AssertionError('Invalid voice accepted')
        assert e.models['chatterbox_native'].process.pid==p
        e.models['chatterbox_native'].process.kill();e.models['chatterbox_native'].process.wait()
        try:synth()
        except RuntimeError:pass
        else:raise AssertionError('Dead worker accepted')
        assert 'chatterbox_native' not in e.models
        synth()
    finally:e.sweep()
    assert not e.models
print('NATIVE_TURBO_PASS=1')
