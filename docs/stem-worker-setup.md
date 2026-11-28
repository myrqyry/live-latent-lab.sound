# Local stem worker setup

The experimental stem_live filters use external user-owned workers.
The lightweight `tools/sound-stems` launcher stores explicit local paths
once and starts either qualified model with the correct private endpoint,
CUDA device, precision and quality options. No downloads, package installs,
automatic CPU fallback, background service registration or scene edits.
Both workers can be invoked separately; concurrent GPU use is not yet
qualified. Prefer starting only the model you need.

## Configure once

Run from the checkout:

```sh
tools/sound-stems configure \
  --python /path/to/venv/bin/python \
  --demucs-models /path/to/verified/htdemucs-model-directory \
  --melband-model /path/to/MelBandRoformer_fp16.safetensors
tools/sound-stems doctor
```

The launcher uses the Python interpreter's existing audio-separator
0.47.0, CUDA torch, numpy and scipy. Mel-Band requires safetensors.
If that interpreter lacks safetensors but a compatible trusted existing
package is available, explicitly pass
`--safetensors-package /path/to/site-packages/safetensors`.
Only that package is imported from the supplied directory, not the
whole foreign virtual environment. A dedicated environment with its
own dependencies remains preferable for distribution. Both loaders
check the pinned model weights during real startup; doctor checks
paths and actual imports/CUDA availability, not hashes or inference.
Configuration is a user-owned mode-0600 JSON file at
`$XDG_CONFIG_HOME/live-latent-lab-sound/stems.json` (default
`~/.config/live-latent-lab-sound/stems.json`). Configure refuses to
overwrite an existing file; edit the existing paths deliberately or
use --config for another profile. It never persists credentials.

## Start and check

```sh
tools/sound-stems run demucs
# Or, in a separate foreground session:
tools/sound-stems run melband
```

| Profile | OBS producer | Socket filename under XDG_RUNTIME_DIR |
| --- | --- | --- |
| demucs | Buffered stem (experimental) | lll-sound-stems.sock |
| melband | RoFormer stems (experimental) | lll-sound-melband.sock |

Demucs uses resident FP32 CUDA, shifts 0, model overlap .1, 4 s
segment. Mel-Band uses the pinned CUDA FP32 model. The two worker
protocols remain incompatible; the launcher does not negotiate or
fall back. The matching OBS filter defaults already use these paths.
The workers refuse occupied paths, including stale sockets, and
never remove an endpoint they did not create. Ctrl-C or SIGTERM
normally removes the worker's own socket; a hard crash may leave a
stale endpoint. Inspect the owner and ensure no server is listening
before manually removing a stale endpoint. No automatic process kill.

In another terminal, while the worker is idle:

```sh
tools/sound-stems probe melband
# or: tools/sound-stems probe demucs
```

Probe uses the actual socket protocol with synthetic silent stereo
PCM and verifies the response identity, geometry and finite samples.
It consumes one inference request, so do not run it during a
latency-sensitive production stream. It is not a music quality or
end-to-end latency test. Doctor distinguishes a testable local
configuration from a live endpoint; an existing socket is not proof
of a ready worker.

For optional convenience, symlink tools/sound-stems into a directory
on PATH (the wrapper resolves its checkout path). No global or
system-wide installation is needed. See [v1](stem-api.md) and
[v2](stem-api-v2.md) for the filter and transport contracts.
