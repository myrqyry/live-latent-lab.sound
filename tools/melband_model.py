"""Pinned local-only Mel-Band RoFormer model loader."""
import hashlib
import importlib.metadata
import os
import time

SHA256 = "6119aef379a6c7264e0b37db65ae1e6488b8ca4a00baf56d6d244737b8488226"


def load_model(model_path):
    os.environ["HF_HUB_OFFLINE"]="1"
    os.environ["TRANSFORMERS_OFFLINE"]="1"
    with model_path.open("rb") as stream:
        if hashlib.file_digest(stream, "sha256").hexdigest() != SHA256:
            raise ValueError("Unexpected model checkpoint")
    if importlib.metadata.version("audio-separator") != "0.47.0":
        raise RuntimeError("Probe pinned to audio-separator 0.47.0")
    import torch
    if not torch.cuda.is_available():
        raise RuntimeError("Explicit CUDA unavailable")
    from safetensors.torch import load_file
    from audio_separator.separator.uvr_lib_v5.roformer.mel_band_roformer import MelBandRoformer

    started = time.monotonic()
    # Published Kijai loader geometry; current installed adapter's default
    # zero-DC behavior. Strict state loading, FP32 execution, no autocast.
    model = MelBandRoformer(dim=384, depth=6, stereo=True, num_stems=1,
        time_transformer_depth=1, freq_transformer_depth=1, num_bands=60,
        dim_head=64, heads=8, attn_dropout=0., ff_dropout=0., flash_attn=True,
        dim_freqs_in=1025, sample_rate=44100, stft_n_fft=2048,
        stft_hop_length=441, stft_win_length=2048, stft_normalized=False,
        mask_estimator_depth=2, match_input_audio_length=True).eval()
    state = load_file(str(model_path), device="cpu")
    model.load_state_dict(state, strict=True)
    del state
    model = model.to("cuda")
    torch.cuda.synchronize()
    print(f"MELBAND_STRICT_LOAD_PASS device={next(model.parameters()).device} "
          f"dtype={next(model.parameters()).dtype} elapsed={time.monotonic()-started:.4f}", flush=True)
    return model

