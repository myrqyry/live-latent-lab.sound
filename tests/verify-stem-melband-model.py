#!/usr/bin/env python3
"""Opt-in direct CUDA probe of the published Kijai Mel-Band FP16 asset.
Not an OBS worker or a music-quality benchmark. No downloads or installation.
"""
import argparse
import os
from pathlib import Path
import sys
import time

os.environ["HF_HUB_OFFLINE"] = "1"
os.environ["TRANSFORMERS_OFFLINE"] = "1"

def load_model(model_path, safetensors_package=None):
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
    from melband_model import load_model as load_pinned
    return load_pinned(model_path,safetensors_package)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True,
                        help="Local 48 kHz interleaved stereo float32, >=8 seconds")
    parser.add_argument("--safetensors-package", type=Path,
                        help="Optional explicitly trusted existing safetensors package directory, if absent from this venv")
    args = parser.parse_args()
    import numpy as np
    from scipy.signal import resample_poly
    import torch
    model = load_model(args.model, args.safetensors_package)
    raw = np.fromfile(args.fixture, dtype="<f4").reshape(-1, 2).T
    if raw.shape[1] < 8 * 48000 or not np.isfinite(raw).all():
        raise ValueError("Invalid fixture")
    pcm = resample_poly(raw[:, :8 * 48000], 147, 160, axis=1).astype("float32")
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
