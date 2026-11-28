#!/usr/bin/env bash
# Isolated OBS registration, stereo transport and optional REAL HTDemucs model test.
# No OBS profile edits, plugin installation, model downloads or process interruption.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
run_real=false
combined=false
tts=false
stt=false
effects="stem_live"
count=2
case "${1:-}" in
  "") ;;
  --with-model) run_real=true ;;
  --combined) run_real=true; combined=true; effects="stem_live;yamnet"; count=3 ;;
  --combined-tts) run_real=true; combined=true; tts=true; effects="stem_live;yamnet;tts"; count=4 ;;
  --combined-stt) run_real=true; combined=true; tts=true; stt=true; effects="stem_live;yamnet;tts;stt"; count=5 ;;
  *) echo "Usage: $0 [--with-model|--combined|--combined-tts|--combined-stt]" >&2; exit 2 ;;
esac
work="$(mktemp -d /tmp/lll-sound-stem-live-XXXXXX)"
socket_dir=""
worker_pid=""
cleanup(){
  local status=$?
  if ((status)); then
    for log in configure build registry yamnet-48000 yamnet-44100 offline model real-audio tts-piper tts-kokoro tts-chatterbox tts-tiktok tts-unified tts-retention tts-long tts-options tts-options-workers stt-48000 stt-44100 parakeet-48000 parakeet-44100 parakeet-native nemotron-native nemotron-48000 nemotron-44100 sttts signal-interop; do
      if [[ -f "$work/$log.log" ]]; then
        printf '\n=== %s ===\n' "$log" >&2
        grep -E 'SOUND_STT_(LIFECYCLE|REAL|PARAKEET_SWITCH|NEMOTRON_LIVE|NEMOTRON_SWITCH)|SOUND_STTTS_REAL|SOUND_SIGNAL_REAL' "$work/$log.log" >&2 || true
        tail -40 "$work/$log.log" >&2
      fi
    done
  fi
  if [[ -n "$worker_pid" ]]; then
    kill "$worker_pid" 2>/dev/null || true
    wait "$worker_pid" 2>/dev/null || true
  fi
  if [[ -n "$socket_dir" ]]; then rm -rf -- "$socket_dir"; fi
  rm -rf -- "$work"
}
trap cleanup EXIT
inc="$(pkg-config --variable=includedir libobs)/obs"
read -r -a libs <<< "$(pkg-config --libs libobs)"
cxx="${CXX:-c++}"
"$cxx" -std=c++17 -O2 -pthread -I"$root/src" \
    "$root/tests/verify-stem-live-pipeline.cpp" -o "$work/pipeline"
"$work/pipeline" | tee "$work/pipeline.log"
"$cxx" -std=c++17 -O2 -pthread -I"$root/src" \
    "$root/tests/verify-stem-recovery.cpp" -o "$work/recovery"
"$work/recovery"
"$cxx" -std=c++17 -O2 -pthread -I"$root/src" \
    "$root/tests/verify-stem-mix.cpp" -o "$work/mix"
"$work/mix"
"$cxx" -std=c++17 -O2 -pthread -I"$root/src" \
    "$root/tests/verify-stem-overlap.cpp" -o "$work/overlap"
"$work/overlap"
"$cxx" -std=c++17 -O2 -pthread -I"$root/src" \
    "$root/tests/verify-stem-dispatch.cpp" -o "$work/dispatch"
"$work/dispatch"
"$cxx" -std=c++17 -O2 -pthread -I"$root/src" \
    "$root/tests/verify-stem-live-client.cpp" -o "$work/client"
# The fake protocol server is self-contained and does not use the GPU.
: "${XDG_RUNTIME_DIR:?XDG_RUNTIME_DIR is needed for private socket tests}"
socket_dir="$(mktemp -d "$XDG_RUNTIME_DIR/lll-stem-test-XXXXXX")"
chmod 700 "$socket_dir"
"$work/client" "$socket_dir/fake.sock" | tee "$work/client.log"
"$cxx" -std=c++17 -O2 -pthread -I"$root/src" \
    "$root/tests/verify-stem-v2-client.cpp" -o "$work/client-v2"
"$work/client-v2" "$socket_dir/fake-v2.sock" --reject
venv="${STEM_SPLITTER_PYTHON:?Set STEM_SPLITTER_PYTHON to the installed stem worker Python}"
test -x "$venv"
"$venv" -m unittest discover -s "$root/tests" -p "test_live_stem*.py" -v \
    >"$work/python.log" 2>&1
grep -E 'Ran [0-9]+ tests|^OK$' "$work/python.log"
stt_args=()
if [[ "$stt" == true ]]; then
  : "${SOUND_STT_WHISPER_INCLUDE:?Specify the matched Whisper header directory}"
  : "${SOUND_STT_RUNTIME_DIR:?Specify the qualified Whisper CPU runtime directory}"
  : "${SOUND_STT_MODEL:?Specify local English Whisper model for JFK acceptance}"
  : "${SOUND_STT_FIXTURE:?Specify the whisper.cpp JFK sample WAV}"
  : "${SOUND_STT_PARAKEET_MODEL:?Specify the pinned Parakeet LiteRT-LM model}"
  : "${SOUND_STT_NEMOTRON_MODEL:?Specify the pinned Nemotron streaming GGUF model}"
  : "${SOUND_STT_NEMOTRON_RUNTIME_DIR:?Specify the isolated CPU streaming runtime and licenses}"
  test -s "$SOUND_STT_NEMOTRON_MODEL" && test -s "$SOUND_STT_NEMOTRON_RUNTIME_DIR/libsound-nemotron-runtime.so"

  test -s "$SOUND_STT_PARAKEET_MODEL"
  test -s "$SOUND_STT_MODEL" && test -s "$SOUND_STT_FIXTURE" && test -s "$SOUND_STT_RUNTIME_DIR/LICENSE"
  stt_args=(-DSOUND_STT_WHISPER_INCLUDE="$SOUND_STT_WHISPER_INCLUDE" -DSOUND_STT_RUNTIME_DIR="$SOUND_STT_RUNTIME_DIR" -DSOUND_STT_NEMOTRON_RUNTIME_DIR="$SOUND_STT_NEMOTRON_RUNTIME_DIR")
fi
cmake -S "$root" -B "$work/build" -G Ninja "${stt_args[@]}" \
    -DFETCH_OBS=OFF -DSOUND_EFFECTS="$effects" -DSOUND_EXPERIMENTAL=ON \
    -DSOUND_TOOLS_MENU=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DOBS_PLUGIN_DESTINATION=bin/64bit -DOBS_DATA_DESTINATION=data \
    -DCMAKE_INSTALL_PREFIX="$work/stage" >"$work/configure.log" 2>&1
cmake --build "$work/build" --parallel 4 >"$work/build.log" 2>&1
cmake --install "$work/build" >"$work/install.log" 2>&1
module="$work/stage/bin/64bit/live-latent-lab-sound.so"
test -s "$module"
if env -u LD_LIBRARY_PATH ldd "$module" | grep -Ei 'not found|libonnxruntime|libtorch|libpython|libwhisper|libggml|libQt6'; then
    echo "Unexpected dependency in stem OBS module" >&2;exit 1
fi
nonce="$(cat "$work/build/sound-stem-build-nonce.txt")"
[[ "$nonce" =~ ^[a-f0-9]{16}$ ]]
"$cxx" -std=gnu++17 -O2 -I"$inc" "$root/tests/verify-module-load.cpp" \
    -Wl,--no-as-needed -lobs-frontend-api "${libs[@]}" -Wl,--as-needed \
    -o "$work/registry"
QT_QPA_PLATFORM=offscreen xvfb-run -a -s '-screen 0 1280x720x24' \
    timeout 20 "$work/registry" "$module" "$work/stage/data" "$effects" \
    >"$work/registry.log" 2>&1
grep -F "SOUND_REGISTRY_MATCH=1 EXPECTED=[$effects] ACTUAL_COUNT=$count" "$work/registry.log"
grep -F "SOUND_STEM_LIVE_CANDIDATE_$nonce" "$work/registry.log"
echo "SOUND_STEM_ISOLATED_PASS=1"
if [[ "$combined" == true ]]; then
    lib="${SOUND_MEDIAPIPE_LIB:?Set SOUND_MEDIAPIPE_LIB}"
    model="${SOUND_YAMNET_MODEL:?Set SOUND_YAMNET_MODEL}"
    test -s "$lib" && test -s "$model"
    "$cxx" -std=gnu++17 -O2 -pthread -I"$inc" \
      "$root/tests/verify-yamnet-obs-audio.cpp" "${libs[@]}" -o "$work/yamnet-audio"
    for rate in 48000 44100; do
        SOUND_YAMNET_TEST_RATE="$rate" QT_QPA_PLATFORM=offscreen \
          xvfb-run -a -s '-screen 0 1280x720x24' timeout 28 \
          "$work/yamnet-audio" "$module" "$work/stage/data" "$lib" "$model" \
          >"$work/yamnet-$rate.log" 2>&1 || { tail -40 "$work/yamnet-$rate.log"; exit 1; }
        grep -F 'SOUND_YAMNET_CATEGORY_SIGNAL catalog=1 count=521 scores=1' "$work/yamnet-$rate.log"
        grep -F 'SOUND_YAMNET_REAL_OBS_AUDIO_PASS=1' "$work/yamnet-$rate.log"
        grep -F 'SOUND_YAMNET_REAL_OBS_BAD_MODEL fail_closed=1' "$work/yamnet-$rate.log"
        grep -F 'SOUND_YAMNET_REAL_OBS_STALE safely_expired=1' "$work/yamnet-$rate.log"
        grep -F "SOUND_YAMNET_REAL_OBS_DRY_STEREO rate=$rate" "$work/yamnet-$rate.log" | grep -F 'pass=1'
    done
    echo "SOUND_COMBINED_YAMNET_PASS=1"
fi

if [[ "$run_real" != true ]]; then
  echo "Actual GPU-to-OBS inference not checked. Run with --with-model."
  exit 0
fi
precision="${STEM_VERIFY_PRECISION:-fp32}"
[[ "$precision" == fp32 || "$precision" == autocast ]] || { echo "Unsupported precision" >&2;exit 2; }
source_wav="${STEM_VERIFY_SOURCE_WAV:?Set a licensed 44.1 kHz stereo WAV fixture}"
test -f "$source_wav"
STEM_PRIVATE_SOURCE="$source_wav" STEM_PRIVATE_TARGET="$work/input-f32-stereo.bin" "$venv" - <<'PY'
import os
from pathlib import Path
import numpy as np,soundfile as sf
from scipy.signal import resample_poly
audio,rate=sf.read(os.environ['STEM_PRIVATE_SOURCE'],dtype='float32',always_2d=True)
assert rate==44100 and audio.shape[1]==2 and len(audio)>=rate*16, 'Need at least 16 s of 44.1kHz stereo audio'
data=resample_poly(audio[:16*rate],160,147,axis=0).astype('<f4')
fixture=np.tile(data,(2,1))[:24*48000]
assert fixture.shape==(24*48000,2)
fixture.tofile(os.environ['STEM_PRIVATE_TARGET'])
PY
probe_flags=()
if [[ "${SOUND_RT_PROBE:-0}" == 1 ]];then probe_flags=(-DSOUND_RT_PROBE -rdynamic -ldl);fi
"$cxx" -std=gnu++17 -O2 -pthread -I"$inc" \
    "$root/tests/verify-stem-live-obs-audio.cpp" "${libs[@]}" "${probe_flags[@]}" -o "$work/obs-audio"
# First, an unavailable backend must mute instead of silently passing music.
QT_QPA_PLATFORM=offscreen xvfb-run -a -s '-screen 0 1280x720x24' \
    timeout 22 "$work/obs-audio" "$module" "$work/stage/data" \
    "$work/input-f32-stereo.bin" "$socket_dir/missing.sock" --expect-silence \
    >"$work/offline.log" 2>&1
grep -F 'expect_silence=1' "$work/offline.log" | grep -F 'pass=1'
# Then run the *real cached* model through the plugin's actual OBS callback.
quality=(--backend "${STEM_VERIFY_BACKEND:-file}" --device "${STEM_VERIFY_DEVICE:-auto}" --shifts "${STEM_VERIFY_SHIFTS:-1}" --overlap "${STEM_VERIFY_OVERLAP:-0.25}")
if [[ -n "${STEM_VERIFY_SEGMENT:-}" ]]; then quality+=(--segment-seconds "$STEM_VERIFY_SEGMENT"); fi
HF_HUB_OFFLINE=1 TRANSFORMERS_OFFLINE=1 "$venv" "$root/tools/live_stem_sidecar.py" \
    "${quality[@]}" --precision "$precision" --socket "$socket_dir/model.sock" >"$work/model.log" 2>&1 &
worker_pid=$!
for ((i=0;i<180;i++)); do
    [[ -S "$socket_dir/model.sock" ]] && break
    kill -0 "$worker_pid" || { tail -25 "$work/model.log" >&2;exit 1; }
    sleep .2
done
test -S "$socket_dir/model.sock"
QT_QPA_PLATFORM=offscreen xvfb-run -a -s '-screen 0 1280x720x24' \
    timeout 42 "$work/obs-audio" "$module" "$work/stage/data" \
    "$work/input-f32-stereo.bin" "$socket_dir/model.sock" \
    >"$work/real-audio.log" 2>&1
grep -F 'expect_silence=0' "$work/real-audio.log" | grep -F 'pass=1'
grep -F 'STEM_QUALITY' "$work/model.log"
grep -F 'STEM_WARMUP' "$work/model.log"
grep -F 'STEM_WINDOW seq=0' "$work/model.log"
grep -F 'STEM_WINDOW seq=1' "$work/model.log"
for mode in MIX AUTO_RECOVER RECOVER;do
    env "SOUND_STEM_$mode"=1 QT_QPA_PLATFORM=offscreen xvfb-run -a timeout 80 \
        "$work/obs-audio" "$module" "$work/stage/data" "$work/input-f32-stereo.bin" "$socket_dir/model.sock" \
        >"$work/stem-$mode.log" 2>&1 || { tail -45 "$work/stem-$mode.log";exit 1; }
    grep "STEM_LIVE_$mode.*pass=1" "$work/stem-$mode.log"
    grep '^RT_CALLBACK ' "$work/stem-$mode.log" || true
done
"$cxx" -std=gnu++17 -O2 -pthread -I"$inc" \
    "$root/tests/verify-stem-output-obs.cpp" "${libs[@]}" "${probe_flags[@]}" -o "$work/obs-outputs"
for smooth in 0 1;do
    for recover in 0 1;do
        env_args=()
        if [[ "$smooth" == 1 ]];then env_args+=(SOUND_STEM_SMOOTH=1);fi
        if [[ "$recover" == 1 ]];then env_args+=(SOUND_STEM_OUTPUT_RECOVER=1);fi
        log="$work/fanout-$smooth-$recover.log"
        env "${env_args[@]}" QT_QPA_PLATFORM=offscreen xvfb-run -a timeout 80 \
            "$work/obs-outputs" "$module" "$work/stage/data" "$work/input-f32-stereo.bin" "$socket_dir/model.sock" \
            >"$log" 2>&1 || { tail -60 "$log";exit 1; }
        grep -E '^STEM_OUTPUT |^STEM_FANOUT_OBS |^RT_CALLBACK ' "$log"
    done
done
echo "SOUND_STEM_LIVE_REAL_GPU_OBS_PASS=1"
if [[ "$tts" == true ]]; then
    "$root/tests/verify-tiktok-native.sh"
    # Release this test's GPU stem worker before speech acceptance. The
    # module is unchanged; this is not a concurrent inference stress test.
    kill "$worker_pid"; wait "$worker_pid" || true; worker_pid=""
    "$cxx" -std=c++17 -O2 -pthread -I"$inc" \
        "$root/tests/verify-tts-obs.cpp" "${libs[@]}" -o "$work/tts-audio"
    for spec in 'piper en_US-amy-low' 'kokoro af_heart' 'chatterbox default' 'tiktok en_us_stitch'; do
        read -r engine voice <<< "$spec"
        QT_QPA_PLATFORM=offscreen xvfb-run -a -s '-screen 0 1280x720x24' \
            timeout 55 "$work/tts-audio" "$module" "$work/stage/data" "$engine" "$voice" \
            >"$work/tts-$engine.log" 2>&1
        grep -F "SOUND_TTS_REAL_OBS backend=$engine " "$work/tts-$engine.log" | grep -F 'pass=1'
        grep -F "SOUND_TTS_CLOCKED_20261118_T721 backend=$engine " "$work/tts-$engine.log"
    done
    grep -F "SOUND_TIKTOK_NATIVE_HTTP_PASS" "$work/tts-tiktok.log"
    QT_QPA_PLATFORM=offscreen xvfb-run -a -s '-screen 0 1280x720x24' \
        timeout 120 "$work/tts-audio" "$module" "$work/stage/data" unified unused \
        >"$work/tts-unified.log" 2>&1
    grep -F 'SOUND_TTS_UNIFIED_20261118_U481' "$work/tts-unified.log"
    grep -F 'SOUND_TTS_UNIFIED_PASS=1' "$work/tts-unified.log"
    grep -F 'SOUND_TTS_EDGE_20261118_E521' "$work/tts-unified.log"
    grep -F 'SOUND_TTS_UNIFIED_SWITCH backend=edge ' "$work/tts-unified.log" | grep -F 'pass=1'
    echo "SOUND_COMBINED_EDGE_PASS=1"
    python3 "$root/tests/verify-tts-retention.py"
    python3 "$root/tests/verify-tts-retention-workers.py"
    "$cxx" -std=c++17 -O2 -pthread -I"$inc" \
        "$root/tests/verify-tts-retention-obs.cpp" "${libs[@]}" $(pkg-config --libs libcurl) -o "$work/tts-retention"
    QT_QPA_PLATFORM=offscreen xvfb-run -a -s '-screen 0 1280x720x24' \
        timeout 100 "$work/tts-retention" "$module" "$work/stage/data" \
        >"$work/tts-retention.log" 2>&1
    grep -F 'SOUND_TTS_RETENTION_20261118_R621' "$work/tts-retention.log"
    grep -F 'SOUND_TTS_RETENTION_OBS_PASS=1' "$work/tts-retention.log"
    echo "SOUND_COMBINED_RETENTION_PASS=1"
    "$cxx" -std=c++17 -O2 -I"$root/src" "$root/tests/verify-tts-text.cpp" -o "$work/tts-text"
    "$work/tts-text"
    QT_QPA_PLATFORM=offscreen xvfb-run -a -s '-screen 0 1280x720x24' \
        timeout 120 "$work/tts-audio" "$module" "$work/stage/data" long unused \
        >"$work/tts-long.log" 2>&1
    grep -F 'SOUND_TTS_LONG_INPUT_20261118_L673' "$work/tts-long.log"
    grep -F 'SOUND_TTS_LONG_OBS bytes=673 started=1 completed=1 ' "$work/tts-long.log" | grep -F 'pass=1'
    echo "SOUND_COMBINED_LONG_SPEECH_PASS=1"
    python3 "$root/tests/verify-tts-options.py"
    python3 "$root/tests/verify-tts-options-workers.py" "$work/options-reference.wav" >"$work/tts-options-workers.log" 2>&1
    grep -F 'SOUND_TTS_OPTIONS_WORKERS_PASS=1' "$work/tts-options-workers.log"
    SOUND_TTS_TEST_REFERENCE="$work/options-reference.wav" QT_QPA_PLATFORM=offscreen xvfb-run -a -s '-screen 0 1280x720x24' \
        timeout 150 "$work/tts-audio" "$module" "$work/stage/data" options unused \
        >"$work/tts-options.log" 2>&1
    grep -F 'SOUND_TTS_NATIVE_OPTIONS_20261118_O536' "$work/tts-options.log"
    grep -F 'SOUND_TTS_OPTIONS_REFERENCE_PASS=1' "$work/tts-options.log"
    grep -F 'SOUND_TTS_OPTIONS_OBS_PASS=1' "$work/tts-options.log"
    echo "SOUND_COMBINED_NATIVE_OPTIONS_PASS=1"


    echo "SOUND_COMBINED_TTS_PASS=1"
fi

if [[ "$stt" == true ]]; then
    "$cxx" -std=c++17 -O2 -I"$inc" "$root/tests/verify-stt-unified-ui.cpp" -Wl,--no-as-needed -lobs-frontend-api "${libs[@]}" -Wl,--as-needed -o "$work/stt-ui"
    QT_QPA_PLATFORM=offscreen xvfb-run -a timeout 30 "$work/stt-ui" "$module" "$work/stage/data"
    "$cxx" -std=c++17 -O2 -I"$root/src" "$root/tests/verify-stt-caption-history.cpp" -o "$work/stt-history"
    "$work/stt-history"
    "$cxx" -std=c++17 -O2 -I"$root/src" "$root/tests/verify-stt-config.cpp" -o "$work/stt-config"
    "$work/stt-config"
    "$cxx" -std=c++17 -O2 -pthread -I"$root/src" "$root/tests/verify-stt-wake.cpp" -o "$work/stt-wake"
    "$work/stt-wake"
    "$cxx" -std=c++17 -O2 -pthread -I"$inc" "$root/tests/verify-stt-obs.cpp" "${libs[@]}" -o "$work/stt-test"
    for rate in 48000 44100; do
        ffmpeg -nostdin -v error -y -i "$SOUND_STT_FIXTURE" -ar "$rate" -ac 1 -f f32le "$work/stt-$rate.raw"
        QT_QPA_PLATFORM=offscreen xvfb-run -a -s '-screen 0 1280x720x24' \
            timeout 90 "$work/stt-test" "$module" "$work/stage/data" "$SOUND_STT_MODEL" "$work/stt-$rate.raw" "$rate" \
            >"$work/stt-$rate.log" 2>&1
        grep -F "SOUND_STT_REAL rate=$rate " "$work/stt-$rate.log" | grep -F 'pass=1'
        grep -F 'SOUND_STT_LIFECYCLE off=1 missing=1 recovered=1' "$work/stt-$rate.log"
    done
    "$cxx" -std=c++17 -O2 -pthread -I"$root/src" "$root/tests/verify-stt-capture.cpp" -o "$work/stt-capture"
    "$work/stt-capture"
    "$cxx" -std=c++17 -O2 -I"$root/src" "$root/tests/verify-stt-parakeet-native.cpp" -ldl -o "$work/parakeet-native"
    ffmpeg -nostdin -v error -y -i "$SOUND_STT_FIXTURE" -ar 16000 -ac 1 -f f32le "$work/jfk16.raw"
    timeout 60 "$work/parakeet-native" "$lib" "$SOUND_STT_PARAKEET_MODEL" "$work/jfk16.raw" >"$work/parakeet-native.log" 2>&1
    grep -F 'SOUND_STT_PARAKEET_NATIVE_PASS=1' "$work/parakeet-native.log"
    for rate in 48000 44100; do
        QT_QPA_PLATFORM=offscreen xvfb-run -a -s '-screen 0 1280x720x24' \
            timeout 105 "$work/stt-test" "$module" "$work/stage/data" "$SOUND_STT_PARAKEET_MODEL" "$work/stt-$rate.raw" "$rate" "$lib" "$SOUND_STT_MODEL" \
            >"$work/parakeet-$rate.log" 2>&1
        grep -F 'SOUND_STT_PARAKEET_PICKER=1' "$work/parakeet-$rate.log"
        grep -F 'SOUND_STT_PARAKEET_SWITCH invalidated=1 whisper=1 saved=1 returned=1' "$work/parakeet-$rate.log"
        grep -F "SOUND_STT_REAL rate=$rate " "$work/parakeet-$rate.log" | grep -F 'pass=1'
    done
    echo "SOUND_COMBINED_PARAKEET_PASS=1"
    nemotron_lib="$work/stage/bin/64bit/stt-runtime/libsound-nemotron-runtime.so"
    if nm -D --defined-only "$nemotron_lib" | grep -E ' [TW] (ggml_|transcribe_)'; then
        echo 'Nemotron runtime exports conflicting native engine symbols' >&2; exit 1
    fi
    if env -u LD_LIBRARY_PATH ldd "$nemotron_lib" | grep -Ei 'not found|libggml|libtranscribe'; then
        echo 'Nemotron runtime must isolate its GGML implementation' >&2; exit 1
    fi
    "$cxx" -std=c++17 -O2 -I"$root/src" "$root/tests/verify-stt-nemotron-native.cpp" -ldl -o "$work/nemotron-native"
    timeout 60 "$work/nemotron-native" "$nemotron_lib" "$SOUND_STT_NEMOTRON_MODEL" "$work/jfk16.raw" >"$work/nemotron-native.log" 2>&1
    grep -F 'SOUND_STT_NEMOTRON_NATIVE_PASS=1' "$work/nemotron-native.log"
    "$cxx" -std=c++17 -O2 -pthread -I"$inc" "$root/tests/verify-stt-nemotron-obs.cpp" "${libs[@]}" -o "$work/nemotron-obs"
    for rate in 48000 44100; do
        QT_QPA_PLATFORM=offscreen xvfb-run -a -s '-screen 0 1280x720x24' \
            timeout 120 "$work/nemotron-obs" "$module" "$work/stage/data" "$SOUND_STT_NEMOTRON_MODEL" "$work/stt-$rate.raw" "$rate" - "$SOUND_STT_MODEL" \
            >"$work/nemotron-$rate.log" 2>&1
        grep -F "SOUND_STT_NEMOTRON_LIVE rate=$rate " "$work/nemotron-$rate.log" | grep -F 'pass=1'
        if [[ "$rate" == 48000 ]]; then
            grep -F 'SOUND_STT_NEMOTRON_SWITCH whisper=1 saved=1 returned=1 invalidated=1' "$work/nemotron-$rate.log"
        fi
        grep -F 'SOUND_STT_LIFECYCLE off=1 missing=1 recovered=1' "$work/nemotron-$rate.log"
        grep -F "SOUND_STT_REAL rate=$rate " "$work/nemotron-$rate.log" | grep -F 'pass=1'
    done
    echo "SOUND_COMBINED_NEMOTRON_PASS=1"
    "$cxx" -std=c++17 -O2 -pthread -I"$inc" "$root/tests/verify-sttts-obs.cpp" "${libs[@]}" -o "$work/sttts"
    QT_QPA_PLATFORM=offscreen xvfb-run -a -s '-screen 0 1280x720x24' \
        timeout 120 "$work/sttts" "$module" "$work/stage/data" "$SOUND_STT_NEMOTRON_MODEL" "$work/stt-48000.raw" >"$work/sttts.log" 2>&1
    grep -F 'SOUND_STTTS_REAL ' "$work/sttts.log" | grep -F 'pass=1'
    echo "SOUND_COMBINED_STTTS_PASS=1"
    "$cxx" -std=c++17 -O2 -fPIC -shared -I"$root/src" -I"$inc" \
        "$root/tests/sound-signal-client-module.cpp" "${libs[@]}" -o "$work/independent-client.so"
    "$cxx" -std=c++17 -O2 -pthread -I"$inc" "$root/tests/verify-sound-signals-obs.cpp" "${libs[@]}" -o "$work/signal-interop"
    QT_QPA_PLATFORM=offscreen xvfb-run -a -s '-screen 0 1280x720x24' \
        timeout 90 "$work/signal-interop" "$module" "$work/stage/data" "$SOUND_STT_NEMOTRON_MODEL" "$work/stt-48000.raw" "$lib" "${SOUND_YAMNET_MODEL:?Set SOUND_YAMNET_MODEL}" "$work/independent-client.so" >"$work/signal-interop.log" 2>&1
    grep -F 'SOUND_SIGNAL_REAL ' "$work/signal-interop.log" | grep -F 'pass=1'
    echo "SOUND_COMBINED_SIGNAL_INTEROP_PASS=1"



    echo "SOUND_COMBINED_STT_PASS=1"
fi

# Public qualification is verification-only; staging is done via the build preset.
echo "SOUND_PUBLIC_ACCEPTANCE_PASS=1"
