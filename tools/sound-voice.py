#!/usr/bin/env python3
"""Sound voice runtime control. Real checks; no model download or OBS mutation."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
CONFIG_DEFAULT = Path(os.getenv("XDG_CONFIG_HOME", str(Path.home() / ".config"))) / "live-latent-lab-sound" / "voice.json"
FIELDS = ("voice_dir", "audio_root", "rvc_gguf", "cuda_cli", "rvc_test_audio")
BACKENDS = ("rvc-cuda-offline", "meanvc2-live")


def path_for(value: str, relative_to: Path) -> str:
    p = Path(value).expanduser()
    return str((relative_to / p).resolve() if not p.is_absolute() else p.resolve())


def read_profile(filename: Path) -> dict:
    if not filename.exists():
        return {}
    if filename.is_symlink() or filename.stat().st_size > 16384:
        raise ValueError("profile symlinks and oversized configurations are not accepted")
    data = json.loads(filename.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or any(key not in FIELDS or not isinstance(value, str) or not value
                                           for key, value in data.items()):
        raise ValueError("Sound profile must be an object of nonempty strings: " + ", ".join(FIELDS))
    return {key: path_for(value, filename.parent) for key, value in data.items()}


def write_profile(filename: Path, values: dict, overwrite: bool) -> None:
    if filename.is_symlink():
        raise ValueError("refusing to write a profile symlink")
    if filename.exists() and not overwrite:
        raise FileExistsError(f"Existing profile not overwritten: {filename}")
    if filename.exists() and overwrite:
        import datetime
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        backup = filename.with_name(f"{filename.name}.bak-{stamp}")
        if backup.exists():
            raise FileExistsError(f"Backup already exists: {backup}")
        os.link(filename, backup)
    filename.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    import tempfile
    fd, temp = tempfile.mkstemp(prefix=".sound-", suffix=".json", dir=filename.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            json.dump(values, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temp, 0o600)
        os.replace(temp, filename)
    finally:
        if os.path.exists(temp):
            os.unlink(temp)


def gpu_info() -> dict:
    smi = shutil.which("nvidia-smi")
    if not smi:
        return {"available": False, "reason": "nvidia-smi missing"}
    try:
        result = subprocess.run(
            [smi, "--query-gpu=name,memory.total,memory.free,compute_cap", "--format=csv,noheader"],
            capture_output=True, text=True, check=True, timeout=6)
        devices = [s.strip() for s in result.stdout.splitlines() if s.strip()]
        return {"available": bool(devices), "devices": devices}
    except (OSError, subprocess.SubprocessError) as error:
        return {"available": False, "reason": str(error)}


def inspect(cfg: dict, profile: Path) -> dict:
    audio_root = Path(cfg.get("audio_root", str(Path.home() / "loam/audio.cpp")))
    voice = Path(cfg["voice_dir"]) if "voice_dir" in cfg else None
    model = Path(cfg.get("rvc_gguf", str(audio_root / "models/RVC-GGUF/rvc-f16.gguf")))
    cli = Path(cfg.get("cuda_cli", "/tmp/audio-cpp-rvc-cuda-sm75/bin/audiocpp_cli"))
    files = {
        "rvc_checkpoint": voice / "model.pth" if voice else None,
        "rvc_index": voice / "model.index" if voice else None,
        "rvc_base_gguf": model,
        "native_cuda_cli": cli,
        "rvc_test_audio": Path(cfg.get("rvc_test_audio", str(Path.home() / "Music/IBM-7094-RVC-Sound-2026-10-14/sola-independent/source-a.wav"))),
        "meanvc2_gguf": audio_root / "models/MeanVC2-GGUF/meanvc2-120ms-40ms-q4_k.gguf",
        "meanvc2_c_api": Path(os.getenv("AUDIOCPP_BUILD", "/tmp/audio-cpp-eval-20261013")) / "bin/libaudiocpp.so",
    }
    assets = {key: {"exists": bool(p and p.is_file()), "size": p.stat().st_size if p and p.is_file() else None}
              for key, p in files.items()}
    gpu = gpu_info()
    native_ready = gpu["available"] and os.access(cli, os.X_OK) and all(
        assets[key]["exists"] for key in ("rvc_checkpoint", "rvc_index", "rvc_base_gguf", "native_cuda_cli", "rvc_test_audio"))
    bridge_ready = all(assets[key]["exists"] for key in ("meanvc2_gguf", "meanvc2_c_api"))
    bridge_ready = bridge_ready and all(shutil.which(x) for x in ("ffmpeg", "xvfb-run", "cmake", "ninja", "cc", "c++", "pkg-config"))
    disk = shutil.disk_usage(ROOT)
    return {
        "profile": str(profile),
        "configured": profile.is_file(),
        "voice_selected": voice is not None,
        "gpu": gpu,
        "assets": assets,
        "disk_free_gib": round(disk.free / 1024 ** 3, 2),
        "low_disk_space": disk.free < 12 * 1024 ** 3,
        "backends": {
            "rvc-cuda-offline": {"status": "testable" if native_ready else "missing_requirements",
                                 "streaming": False, "note": "real native offline RVC, not a live service"},
            "meanvc2-live": {"status": "testable" if bridge_ready else "missing_requirements",
                              "streaming": True, "note": "experimental real two-process OBS path, not auto-installed"},
            "rvc-onnx-streaming": {"status": "not_integrated", "streaming": False,
                                   "note": "requires IBM ONNX export, RMVPE/index fidelity and latency verification"},
        },
    }


def verify(cfg: dict, profile: Path, backend: str, report_dir: str | None) -> int:
    status = inspect(cfg, profile)
    if status["backends"][backend]["status"] != "testable":
        print(f"SOUND_VOICE_NOT_READY={backend}; run doctor", file=sys.stderr)
        return 2
    env = os.environ.copy()
    audio_root = cfg.get("audio_root", str(Path.home() / "loam/audio.cpp"))
    env["AUDIOCPP_ROOT"] = audio_root
    if backend == "rvc-cuda-offline":
        env.update({
            "RVC_AUDIOCPP_ROOT": audio_root,
            "RVC_RESIDENT_CLI": cfg.get("cuda_cli", "/tmp/audio-cpp-rvc-cuda-sm75/bin/audiocpp_cli"),
            "RVC_RESIDENT_GGUF": cfg.get("rvc_gguf", str(Path(audio_root) / "models/RVC-GGUF/rvc-f16.gguf")),
            "RVC_RESIDENT_VOICE_DIR": cfg["voice_dir"],
            "RVC_RESIDENT_INPUT": cfg.get("rvc_test_audio", str(Path.home() / "Music/IBM-7094-RVC-Sound-2026-10-14/sola-independent/source-a.wav")),
        })
        if report_dir:
            env["RVC_RESIDENT_REPORT_DIR"] = path_for(report_dir, Path.cwd())
        test = ROOT / "tests/verify-rvc-resident-cuda.sh"
    else:
        if report_dir:
            raise ValueError("meanvc2-live test does not accept --report-dir")
        test = ROOT / "tests/verify-meanvc2-sidecar-obs.sh"
    print(f"SOUND_VOICE_VERIFY_RUNNING={backend}", flush=True)
    return subprocess.call([str(test)], cwd=ROOT, env=env)


def main() -> int:
    p = argparse.ArgumentParser(description="Sound voice setup and real readiness tests; no automatic install.")
    p.add_argument("--config", type=Path, default=CONFIG_DEFAULT)
    sub = p.add_subparsers(dest="command", required=True)
    s = sub.add_parser("configure", help="Store paths for an existing model without altering the model.")
    s.add_argument("--voice-dir", required=True)
    s.add_argument("--audio-root")
    s.add_argument("--rvc-gguf")
    s.add_argument("--cuda-cli")
    s.add_argument("--rvc-test-audio")
    s.add_argument("--overwrite", action="store_true")
    s = sub.add_parser("doctor", help="Inspect hardware, files and what really works.")
    s.add_argument("--json", action="store_true")
    s = sub.add_parser("verify", help="Run real expensive model/OBS integration checks.")
    s.add_argument("--backend", required=True, choices=BACKENDS)
    s.add_argument("--report-dir")
    args = p.parse_args()
    filename = args.config.expanduser().absolute()
    if args.command == "configure":
        voice = Path(args.voice_dir).expanduser().resolve()
        if not ((voice / "model.pth").is_file() and (voice / "model.index").is_file()):
            raise ValueError("voice directory requires both model.pth and model.index")
        values = {"voice_dir": str(voice)}
        for field in ("audio_root", "rvc_gguf", "cuda_cli", "rvc_test_audio"):
            if getattr(args, field):
                values[field] = path_for(getattr(args, field), Path.cwd())
        write_profile(filename, values, args.overwrite)
        print(f"SOUND_VOICE_PROFILE_SAVED={filename}")
        return 0
    cfg = read_profile(filename)
    if args.command == "verify":
        return verify(cfg, filename, args.backend, args.report_dir)
    data = inspect(cfg, filename)
    if args.json:
        print(json.dumps(data, indent=2, sort_keys=True))
    else:
        print(f"Sound profile: {'configured' if data['configured'] else 'not configured'}")
        print(f"Voice selected: {data['voice_selected']}")
        print(f"GPU: {', '.join(data['gpu'].get('devices', [])) or 'unavailable'}")
        print(f"Disk free: {data['disk_free_gib']} GiB; low space: {data['low_disk_space']}")
        for name, info in data["backends"].items():
            print(f"{name}: {info['status']} — {info['note']}")
        print("Missing: " + ", ".join(k for k, v in data["assets"].items() if not v["exists"]))
        print("No installation or streaming RVC is implied by a passing doctor.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"SOUND_VOICE_ERROR={error}", file=sys.stderr)
        sys.exit(2)
