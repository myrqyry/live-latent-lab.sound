# Local stem worker setup

## Normal use: inside OBS

**Native-enabled RoFormer build:** Native CUDA (GGUF; no Python) is
selected by default. The compiled helper ships with its own matched
audio.cpp runtime, isolated from the OBS process and the RVC runtime.
A blank GGUF model override resolves to
`mel-band-roformer-f16.gguf` in the same family folder used by Browse.
Without a custom library this is
`<OBS plugin config>/models/Mel-Band-RoFormer-GGUF/`.
A configured shared library or explicit family directory can override it.
Select another qualified GGUF (such as Q8_0) directly in the picker.
No Python environment, saved stem profile or command is needed
for this backend. If the native model/runtime is missing, the filter
mutes and reports an error rather than silently falling back.
Uncheck Native CUDA to explicitly use the previous Python
compatibility path. Demucs still uses the managed Python backend.

Add the Buffered stem or RoFormer stems filter to an audio source.
The default **Automatic (OBS manages worker)** mode starts its own
private local CUDA worker when audio arrives, using the saved local
profile if present. No terminal, running service or socket setup is
required. The worker uses the installed script resources, not a
source checkout. Cold startup is silent; the fixed sample-count
prefill begins only after model loading and warmup finish.
The filter releases its own child on removal, disable, configuration
change, shutdown or 30 seconds of no audio callbacks. It never
terminates an external worker. One managed filter per family is
currently allowed; multiple ordinary Stem Output sources share that
producer's inference. Concurrent CUDA families are not qualified.

If a profile is absent, select an existing compatible Python
interpreter and the appropriate model file/directory in the filter
properties. Optional safetensors package override is available for
Mel-Band. Override fields are not necessary when the saved local
profile already identifies those assets. Model pickers start from
Sound's Model Library preference. The UI reports worker status
when opened or refreshed; Retry worker resets the retry budget.
The Linux runtime and model assets must already be available.
No downloads, package installs, CPU fallback or automatic service
registration. No inference is performed on the OBS audio thread.

An explicitly selected **External socket (advanced)** mode retains
the user-owned worker workflow below. Automatic mode honors older
nondefault socket selections; choose Managed by OBS to override
them. The named default endpoints no longer need to be running in
managed mode.

## Optional saved profile and diagnostic CLI

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

## Advanced external worker: start and check

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
fall back. Select External socket (advanced) to use these paths instead of the managed private endpoint.
The workers refuse occupied paths, including stale sockets, and
never remove an endpoint they did not create. Ctrl-C or SIGTERM
normally removes the worker's own socket; a hard crash may leave a
stale endpoint. Inspect the owner and ensure no server is listening
before manually removing a stale endpoint. External endpoints are never killed or deleted by OBS. Managed workers use unique mode-0700 temporary directories; normal stop cleans the owned directory. If OBS crashes, the Linux parent-death signal kills its managed child, but a private stale temporary directory can remain.

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
