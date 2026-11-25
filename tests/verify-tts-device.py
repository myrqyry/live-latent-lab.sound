#!/usr/bin/env python3
"""Test hardware-aware Piper selection without requiring external model downloads."""
from pathlib import Path
from unittest.mock import patch
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from tts_worker import load_piper_voice

class FakeOrt:
    def __init__(self, providers): self.providers=providers
    def get_available_providers(self): return self.providers

class Session:
    def __init__(self, providers): self.providers=providers
    def get_providers(self): return self.providers

class Voice:
    def __init__(self, providers): self.session=Session(providers)

class FakePiper:
    def __init__(self, runtime_providers=None, cuda_error=None):
        self.calls=[]
        self.runtime_providers=runtime_providers or ["CUDAExecutionProvider", "CPUExecutionProvider"]
        self.cuda_error=cuda_error
    def load(self, path, use_cuda=False):
        self.calls.append(use_cuda)
        if use_cuda and self.cuda_error:
            raise RuntimeError("GPU backend failed")
        return Voice(self.runtime_providers if use_cuda else ["CPUExecutionProvider"])

def exercise(providers, expected_calls, expected, *, cuda_error=False, runtime_providers=None):
    ort=FakeOrt(providers)
    piper=FakePiper(runtime_providers, cuda_error)
    with patch.dict(sys.modules, {"onnxruntime":ort}):
        voice=load_piper_voice(piper, "model.onnx")
    assert piper.calls==expected_calls, piper.calls
    assert expected in voice.session.get_providers(), voice.session.get_providers()

exercise(["CPUExecutionProvider"],[False],"CPUExecutionProvider")
exercise(["CUDAExecutionProvider","CPUExecutionProvider"],[True],"CUDAExecutionProvider")
exercise(["CUDAExecutionProvider","CPUExecutionProvider"],[True,False],"CPUExecutionProvider",cuda_error=True)
exercise(["CUDAExecutionProvider","CPUExecutionProvider"],[True,False],"CPUExecutionProvider",runtime_providers=["CPUExecutionProvider"])
print("SOUND_TTS_DEVICE_POLICY_PASS=1")

with patch.dict(sys.modules, {"onnxruntime":FakeOrt(["CUDAExecutionProvider","CPUExecutionProvider"])}):
    p=FakePiper()
    assert load_piper_voice(p,"model.onnx","cpu").session.get_providers()==["CPUExecutionProvider"]
    assert p.calls==[False]
with patch.dict(sys.modules, {"onnxruntime":FakeOrt(["CPUExecutionProvider"])}):
    p=FakePiper()
    try:load_piper_voice(p,"model.onnx","cuda")
    except RuntimeError:pass
    else:raise AssertionError("Explicit CUDA must not fall back")
    assert not p.calls
print("SOUND_TTS_EXPLICIT_DEVICE_PASS=1")

from tts_worker import load_kokoro_voice
class FakeKokoro:
    @staticmethod
    def from_session(session, voices):return Voice(session.get_providers())
class FakeKokoroOrt(FakeOrt):
    def __init__(self,providers,fail=False):
        super().__init__(providers);self.calls=[];self.fail=fail
    def InferenceSession(self,path,providers):
        self.calls.append(providers)
        if self.fail and "CUDAExecutionProvider" in providers:raise RuntimeError("GPU backend failed")
        return Session(providers)
for available,device,fail,expected in [
    (["CPUExecutionProvider"],"auto",False,[["CPUExecutionProvider"]]),
    (["CUDAExecutionProvider","CPUExecutionProvider"],"cpu",False,[["CPUExecutionProvider"]]),
    (["CUDAExecutionProvider","CPUExecutionProvider"],"auto",False,[["CUDAExecutionProvider","CPUExecutionProvider"]]),
    (["CUDAExecutionProvider","CPUExecutionProvider"],"auto",True,[["CUDAExecutionProvider","CPUExecutionProvider"],["CPUExecutionProvider"]]),
]:
    ort=FakeKokoroOrt(available,fail)
    with patch.dict(sys.modules,{"onnxruntime":ort}):
        voice=load_kokoro_voice(FakeKokoro,"m.onnx","v.bin",device)
    assert ort.calls==expected and voice.session.get_providers()==expected[-1]
with patch.dict(sys.modules,{"onnxruntime":FakeKokoroOrt(["CPUExecutionProvider"])}):
    try:load_kokoro_voice(FakeKokoro,"m.onnx","v.bin","cuda")
    except RuntimeError:pass
    else:raise AssertionError("Explicit CUDA must not fall back")
print("SOUND_KOKORO_DEVICE_PASS=1")
