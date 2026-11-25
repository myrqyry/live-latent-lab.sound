#!/usr/bin/env python3
"""Qualify actual Magenta streaming DDSP features and recurrent control models.

Usage: python verify-ddsp-models.py MODEL_DIRECTORY (LiteRT 2.4.0).
No model downloads; no GPU; no synthetic decoder.
"""
from pathlib import Path
from importlib.metadata import version
import sys
import time
import numpy as np
from ai_edge_litert.interpreter import Interpreter

root = Path(sys.argv[1])
print("DDSP_RUNTIME", version("ai-edge-litert"))
names = ("Bassoon", "Clarinet", "Flute", "Melodica", "Saxophone", "Sitar",
         "Trombone", "Trumpet", "Tuba", "Violin", "Vowels")

def open_model(name, inputs, outputs):
    model = Interpreter(model_path=str(root / (name + ".tflite")), num_threads=1)
    model.allocate_tensors()
    def check(details, expected):
        actual = {d["name"]: tuple(d["shape"]) for d in details}
        assert actual == expected, (name, actual, expected)
        assert all(d["dtype"] == np.float32 for d in details)
        return {d["name"]: d["index"] for d in details}
    return (model, check(model.get_input_details(), inputs),
            check(model.get_output_details(), outputs))

f, fi, fo = open_model("extract_features_micro", {"call_audio:0": (1024,)},
                      {f"StatefulPartitionedCall:{j}": (1,) for j in range(4)})
def features(pcm):
    t = time.perf_counter_ns()
    f.set_tensor(fi["call_audio:0"], pcm)
    f.invoke()
    vals = [float(f.get_tensor(fo[f"StatefulPartitionedCall:{j}"])[0]) for j in range(4)]
    assert np.isfinite(vals).all()
    return vals, (time.perf_counter_ns() - t) / 1e6

ts = np.arange(1024) / 16000
for hz in (110, 220, 440, 660, 880):
    (pitch, pitch_scaled, power_db, power_scaled), _ = features((.3*np.sin(2*np.pi*hz*ts)).astype("f"))
    assert abs(pitch-hz)/hz < .015, (pitch, hz)
    assert -17 < power_db < -10
    print("DDSP_PITCH", hz, round(pitch, 3), round(power_db, 3))
silence, _ = features(np.zeros(1024, dtype="f"))
assert silence[2] < -70  # raw pitch on silence is not reliable
frames = []
feature_ms = []
for j in range(150):
    # 20 ms hops, smoothly changing monophonic pitch and dynamics.
    t = (j*320+np.arange(1024))/16000
    audio = ((.16+.10*np.sin(2*np.pi*.9*t))*np.sin(2*np.pi*(220*t+18*t*t))).astype("f")
    v, ms = features(audio)
    frames.append(v)
    feature_ms.append(ms)

def stat(ms):
    return [round(float(v), 3) for v in np.percentile(ms, [50, 95, 100])]

print("DDSP_FEATURE_MS_P50_P95_MAX", stat(feature_ms))
all_harmonics = {}
for name in names:
    model, ins, outs = open_model(name, {"call_state:0": (512,), "call_f0_scaled:0": (1,),
        "call_pw_scaled:0": (1,)}, {f"StatefulPartitionedCall:{j}": shape
        for j, shape in enumerate(((1,), (60,), (65,), (512,)))})
    state = np.zeros(512, dtype="f")
    ms = []
    total = []
    old_state = state.copy()
    for _, pitch, _, power in frames:
        start = time.perf_counter_ns()
        for key, value in (("call_state:0", state), ("call_f0_scaled:0", np.array([pitch], dtype="f")),
                           ("call_pw_scaled:0", np.array([power], dtype="f"))):
            model.set_tensor(ins[key], value)
        model.invoke()
        a, h, noise, state = [model.get_tensor(outs[f"StatefulPartitionedCall:{j}"]) for j in range(4)]
        ms.append((time.perf_counter_ns()-start)/1e6)
        assert all(np.isfinite(x).all() for x in (a,h,noise,state))
        assert a[0] >= 0 and h.min() >= 0 and noise.min() >= 0
        assert abs(float(h.sum())-1) < .001
        total.append(h*float(a[0]))
    assert not np.array_equal(state, old_state)
    total = np.array(total)
    assert float(total.max()) > .001
    all_harmonics[name] = total
    print("DDSP_DECODER", name, "MS_P50_P95_MAX", stat(ms),
          "MAX_HARMONIC", round(float(total.max()), 5))
    assert np.percentile(ms, 95) < 20, "Model fails 20ms hop budget on this host"
assert np.linalg.norm(all_harmonics["Violin"]-all_harmonics["Flute"]) > .01
print("DDSP_MODEL_QUALIFICATION_PASS=1")
