#!/usr/bin/env python3
"""Opt-in direct CUDA probe of the published Kijai Mel-Band FP16 asset.
Not an OBS worker or a music-quality benchmark. No downloads or installation.
"""
import argparse
import hashlib
import importlib.metadata
import importlib.util
import os
from pathlib import Path
import sys
import time

os.environ["HF_HUB_OFFLINE"] = "1"
os.environ["TRANSFORMERS_OFFLINE"] = "1"

SHA256 = "6119aef379a6c7264e0b37db65ae1e6488b8ca4a00baf56d6d244737b8488226"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True,
                        help="Local 48 kHz interleaved stereo float32, >=8 seconds")
    parser.add_argument("--safetensors-package", type=Path,
                        help="Optional explicitly trusted existing safetensors package directory, if absent from this venv")
    args = parser.parse_args()
    with args.model.open("rb") as stream:
        if hashlib.file_digest(stream, "sha256").hexdigest() != SHA256:
            raise ValueError("Unexpected model checkpoint")
    if importlib.metadata.version("audio-separator") != "0.47.0":
        raise RuntimeError("Probe pinned to audio-separator 0.47.0")
    import numpy as np
    from scipy.signal import resample_poly
    import torch
    if not torch.cuda.is_available():
        raise RuntimeError("Explicit CUDA unavailable")
    if args.safetensors_package is not None:
        package = args.safetensors_package.resolve(strict=True)
        spec = importlib.util.spec_from_file_location(
            "safetensors", package / "__init__.py",
            submodule_search_locations=[str(package)])
        if spec is None or spec.loader is None:
            raise RuntimeError("Invalid explicit safetensors package path")
        mod = importlib.util.module_from_spec(spec)
        sys.modules["safetensors"] = mod
        spec.loader.exec_module(mod)
    from safetensors.torch import load_file
    from audio_separator.separator.uvr_lib_v5.roformer.mel_band_roformer import MelBandRoformer

    raw = np.fromfile(args.fixture, dtype="<f4").reshape(-1, 2).T
    if raw.shape[1] < 8 * 48000 or not np.isfinite(raw).all():
        raise ValueError("Invalid fixture")
    pcm = resample_poly(raw[:, :8 * 48000], 147, 160, axis=1).astype("float32")
    started = time.monotonic()
    # Published Kijai loader geometry; current installed adapter's default
    # zero-DC behavior. Strict state loading, FP32 execution, no autocast.
    model = MelBandRoformer(dim=384, depth=6, stereo=True, num_stems=1,
        time_transformer_depth=1, freq_transformer_depth=1, num_bands=60,
        dim_head=64, heads=8, attn_dropout=0., ff_dropout=0., flash_attn=True,
        dim_freqs_in=1025, sample_rate=44100, stft_n_fft=2048,
        stft_hop_length=441, stft_win_length=2048, stft_normalized=False,
        mask_estimator_depth=2, match_input_audio_length=True).eval()
    state = load_file(str(args.model), device="cpu")
    model.load_state_dict(state, strict=True)
    del state
    model = model.to("cuda")
    torch.cuda.synchronize()
    print(f"MELBAND_STRICT_LOAD_PASS device={next(model.parameters()).device} "
          f"dtype={next(model.parameters()).dtype} elapsed={time.monotonic()-started:.4f}", flush=True)
    with torch.inference_mode():
        for seconds in (1.4, 2.8, 8.0):
            frames = round(44100 * seconds)
            x = torch.from_numpy(np.ascontiguousarray(pcm[:, :frames])).to("cuda").unsqueeze(0)
            times = []
            for repeat in range(3):
                torch.cuda.synchronize()
                torch.cuda.reset_peak_memory_stats()
                start = time.monotonic()
                y = model(x)
                torch.cuda.synchronize()
                elapsed = time.monotonic() - start
                if y.shape != x.shape or not torch.isfinite(y).all():
                    raise ValueError("Invalid vocal output")
                other = x - y
                vocal_rms = y.square().mean().sqrt().item()
                instrumental_rms = other.square().mean().sqrt().item()
                stereo_diff = (y[:, 0] - y[:, 1]).square().mean().sqrt().item()
                if min(vocal_rms, instrumental_rms, stereo_diff) <= 0:
                    raise ValueError("Degenerate probe output")
                times.append(elapsed)
                print(f"MELBAND_WINDOW seconds={seconds} repeat={repeat} time={elapsed:.4f} "
                      f"rtf={elapsed/seconds:.4f} peak_mib={torch.cuda.max_memory_allocated()/1048576:.1f} "
                      f"vocal_rms={vocal_rms:.6g} instrumental_rms={instrumental_rms:.6g} "
                      f"stereo_diff={stereo_diff:.6g}", flush=True)
                del y, other
            print(f"MELBAND_SUMMARY seconds={seconds} warm_median_s={np.median(times[1:]):.4f}", flush=True)
    print("MELBAND_PROBE_DONE", flush=True)


if __name__ == "__main__":
    main()
