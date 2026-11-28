# Native plugin preferences

OBS already provides a Plugin Manager for discovering, enabling and
updating plugins. Sound does not replace it or modify its state.

The optional Qt settings bridge attaches a single **Plugins** category to
the existing OBS Settings list and page stack. Sound contributes its
own **LLL.sound** tab; other plugins can independently use the same
versioned widget convention in `src/ui/plugin-settings-page.hpp`.
No dependency on Aftermarket, no cross-module C++ object exchange,
and no changes to OBS's native page definitions. This approach is based
on Aftermarket's existing settings injection. Unsupported or mismatched
settings-dialog layouts are left alone. Only the owned tab is removed
on unload; the category disappears when its last contributor is removed.

## Sound settings

- Model library: initial browsing location for YAMNet, STT, DDSP, RVC,
  Stable Audio and LoRA model selectors, including custom RVC checkpoints/indexes. The Audio Diffusion dock uses the saved source directory when valid, otherwise the model library.
- Runtime library: initial browsing location for supported native library
  and RVC helper selectors.
- Backend-specific links for the effects compiled into the module.
  Setup guides explain required runtime/model provenance; model
  downloads and license decisions remain user-initiated.

Choose an existing folder, or Clear to use the automatic plugin models folder (for runtime library, the normal system default). Qt-free builds also read the same plugin preferences.ini ([General], models and runtimes); only editing via the embedded settings page requires Qt. Missing model directories fall back to the plugin-owned shared models folder. Qt-enabled builds also recognize legacy QSettings-escaped paths and save them in the common format on Apply; Qt-free users with an older escaped path can re-save from the settings-enabled build. The preference is a starting location, not automatic model selection.
Apply or OK saves the preferences in the plugin's own config directory.
Cancel discards unsaved changes. A native validation rejection must not
commit the draft. This does not set or migrate a source's selected model,
change a running inference engine or configure external workers.
Per-source devices and voices stay per-source; no global device override
is offered because backend capabilities differ.

The default lightweight public-linux preset remains Qt-free.
To build its optional settings-enabled variant:

```sh
cmake --preset public-linux -B build-public-settings -DSOUND_SETTINGS_PAGE=ON
cmake --build build-public-settings --parallel 2
cmake --install build-public-settings --prefix "$PWD/stage-settings"
```

For other opt-in effects, add the same CMake option to the corresponding
build. Qt6 Widgets development/runtime dependencies are required.
The OBS Settings integration relies on private Qt widget structure,
not an official OBS extension API. It is verified on Linux OBS 32.2.0;
other OBS versions are not qualified. Aftermarket's existing separate
page has not been migrated. Other plugins have not yet adopted the bridge.


### Unified model browsing folder

On a fresh install, no preferences file or external model library is
required. Sound lazily creates one writable `models/` directory under
its OBS plugin configuration directory. On Linux this is normally
`~/.config/obs-studio/plugin_config/live-latent-lab-sound/models/`.
All model Browse controls start here and retain their own file-type
filters. Models needing multiple files may be organized in subfolders;
no model-family directory tree is imposed.

A custom Model Library preference remains supported. For compatibility,
existing matching family subdirectories within that library and
existing absolute `[ModelDirectories]` overrides in preferences.ini
retain precedence. If a custom library has no matching family folder,
Browse opens the configured root. If no valid library is configured,
the plugin-owned shared models folder is used. If it cannot be created,
the normal system chooser is the fallback. No existing model file or
selected source setting is moved or changed. Runtime and reference
audio selectors are unchanged.
