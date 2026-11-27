"""Source-level guard for the legacy OBS capture/lifetime boundary.

OBS holds its audio callback mutex during dispatch AND removal. The
callback must never acquire the control mutex or signal a locking os_event.
This guard complements (not replaces) integration/stress testing.
"""
from pathlib import Path
import re

source = (Path(__file__).resolve().parents[1] / "src/whisper-source.cpp").read_text()

def body(name):
    start = re.search(r"\b" + name + r"\s*\([^;{}]*\)\s*\{", source).end()
    depth = 1
    end = start
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end-1]

capture = re.sub(r"//[^\n]*", "", body("whisper_audio_callback"))
assert not re.search(r"lock_guard|unique_lock|os_event_signal|\bnew\b|std::vector", capture), "callback must not lock or allocate"
destroy = body("whisper_destroy")
assert "audio_lock" not in destroy, "destroy must not retain a guard over delete"
assert destroy.index("whisper_detach_capture") < destroy.index("worker_thread.join()") < destroy.index("delete data")
assert "obs_source_remove_audio_capture_callback" in body("whisper_detach_capture")
assert "whisper_detach_capture" in body("whisper_deactivate")
print("LEGACY_WHISPER_RT_GUARD_PASS=1")
