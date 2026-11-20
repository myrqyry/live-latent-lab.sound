# Verified mic-only Sound release — November 12, 2026

Only the procedural Microphone DSP is qualified for the current release candidate. Its seven settings are bypass, Dynamic, Condenser, Ribbon, Telephone, Vintage Radio and Tape, with sweep and resonance controls. It does not do neural microphone modeling.

## Proved against actual libobs on yuzu-mint

- tests/verify-quarantine.sh: default quarantined module exposes ZERO effects.
- tests/verify-mic-dsp.sh: exactly one registered mic filter, independent spectral tests and real post-filter stereo capture. Ribbon RMS changed left 0.282843 to 0.212818 and right 0.113137 to 0.0851272; legacy saved custom-ONNX type 7 dry-bypasses.
- tests/verify-mic-candidate.sh: new standalone candidate with a random 16-hex build ID, exact-binary module load, proper registry, real stereo audio, no unwanted ONNX/Whisper/Qt dependencies, OBS-running install refusal, and successful install + preserved backup in a fake disposable HOME.

## Prepare, then install separately (OBS MUST be closed for installation)

1. Run: ./tools/stage-verified-mic-dsp.sh
2. The printed CANDIDATE_DIR lives under ~/.local/share/live-latent-lab-sound/candidates/. The verified-mic-dsp.txt receipt records the exact binary SHA256 and unique OBS loader marker.
3. After closing OBS, dry-run: ./tools/install-verified-mic-dsp.sh CANDIDATE_DIR
4. Only when intentional: ./tools/install-verified-mic-dsp.sh CANDIDATE_DIR --apply

Installation verifies the receipt and binary, refuses to operate while OBS runs, backs up the WHOLE existing user plugin directory in ~/.config/obs-studio/plugin-backups/sound-before-mic-*, and copies only the candidate binary/locale. No other plugin copies, model files, OBS scenes or profiles are removed. It does not modify global /usr installations.

After restarting OBS, its NEWEST log must contain SOUND_MIC_DSP_CANDIDATE_ followed by the EXACT nonce from the receipt. Verify the filter under the microphone source's Filters panel. An old marker indicates the wrong binary is being loaded; investigate duplicate paths rather than repeatedly installing.

## Update boundary

OBS running: 32.2.0. Local pkg-config headers: libobs 32.0.1. Latest official OBS: 33.0.1, released October 29, 2026. OBS 33 changes plugin loading paths; legacy search remains supported until OBS 34. The 31.1.1 template dependency pins and macOS/Windows CI are NOT verified for 33 and are not casually bumped. No OS upgrade, live OBS reload or OBS installation was performed.

All other AI features stay quarantined. In particular the original goal of synchronized LIVE stem separation is still open; offline separation is not a substitute. An isolated audio capture pass does not prove final OBS recording/stream output or physical mic quality. Source remains safe and reversible; nothing has been pushed.