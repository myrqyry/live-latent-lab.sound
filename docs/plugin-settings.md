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
  Stable Audio and LoRA model selectors.
- Runtime library: initial browsing location for supported native library
  and RVC helper selectors.
- Backend-specific links for the effects compiled into the module.
  Setup guides explain required runtime/model provenance; model
  downloads and license decisions remain user-initiated.

Choose an existing folder, or Clear to use the normal system default.
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
