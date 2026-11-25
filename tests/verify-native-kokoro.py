"""Contract test, plus real CUDA worker when runtime/model supplied."""
import sys, pathlib, io, wave, time
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'tools'))
from tts_worker import Engine,speech_options
assert speech_options('kokoro_native',{})=={'speed':1.0}
for raw in ({'is_phonemes':True},{'speed':3},{'trim':True}):
    try:speech_options('kokoro_native',raw)
    except ValueError:pass
    else:raise AssertionError(raw)
e=Engine('reader',pathlib.Path('/nonexistent'))
assert not e.supports('kokoro_native')
try:e.synthesize('kokoro_native','af_heart','Hello.')
except ValueError:pass
else:raise AssertionError('Unconfigured native backend accepted')
assert not e.models
# Invalid input must not launch an inference process.
e=Engine('reader',pathlib.Path('/nonexistent'),native_library=pathlib.Path('/missing-library'),native_model=pathlib.Path('/missing-model'))
try:e.synthesize('kokoro_native','not-a-voice','Hello.')
except ValueError:pass
else:raise AssertionError('Invalid voice accepted')
assert not e.models
try:e.synthesize('kokoro_native','af_heart','Hello.')
except RuntimeError:pass
else:raise AssertionError('Missing runtime/model accepted')
assert not e.models
if len(sys.argv)==3:
    e=Engine('reader',pathlib.Path('/nonexistent'),native_library=pathlib.Path(sys.argv[1]),native_model=pathlib.Path(sys.argv[2]))
    assert e.supports('kokoro_native')
    def synth(voice='af_heart',speed=1):
        t=time.monotonic()
        b=e.synthesize('kokoro_native',voice,'Hello from Live Latent Lab.',{'speed':speed})
        with wave.open(io.BytesIO(b)) as f:
            assert (f.getframerate(),f.getnchannels(),f.getsampwidth())==(24000,1,2)
            assert any(f.readframes(f.getnframes()))
            duration=f.getnframes()/24000
        print('NATIVE_KOKORO',voice,speed,duration,round(1000*(time.monotonic()-t),1))
        return duration
    try:
        a=synth();p=e.models['kokoro_native'].process.pid
        assert synth(speed=1.5)<a
        synth('am_michael')
        assert e.models['kokoro_native'].process.pid==p
        e.models['kokoro_native'].process.kill()
        e.models['kokoro_native'].process.wait()
        try:synth()
        except RuntimeError:pass
        else:raise AssertionError('Dead child accepted')
        assert 'kokoro_native' not in e.models
        synth()
        assert e.models['kokoro_native'].process.pid!=p
    finally:
        e.sweep()
    assert not e.models
print('NATIVE_KOKORO_PASS=1')
