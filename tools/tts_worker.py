#!/usr/bin/env python3
"""Same-user Unix socket TTS worker; persistent, explicitly selected local engines."""
import argparse
import gc
import math
import ctypes
import time
import io
import json
import os
import socketserver
import wave
from http.server import BaseHTTPRequestHandler
from pathlib import Path

MAX_TEXT = 600  # UTF-8 bytes, bound local inference and audio duration
MAX_SECONDS = 90

# Per-engine native controls. Ranges are qualified application bounds,
# not a claim that every extreme value produces pleasing speech.
OPTIONS = {
    'kokoro': {
        'speed': (1.0, .5, 2.0), 'language': ('auto', 'en-us', 'en-gb'),
        'is_phonemes': False, 'trim': True, 'sentence_pause': (.25, 0., 2.),
        'clause_pause': (.1, 0., 2.),
    },
    'piper': {
        'length_scale': (1., .5, 2.), 'noise_scale': (.667, 0., 2.),
        'noise_w_scale': (.8, 0., 2.), 'normalize_audio': True,
        'volume': (1., .05, 2.),
    },
    'chatterbox': {
        'temperature': (.8, .1, 1.5), 'top_k': (1000, 0, 6563),
        'top_p': (.95, .05, 1.), 'repetition_penalty': (1.2, .5, 2.),
        'audio_prompt_path': '', 'norm_loudness': True,
    },
    'edge': {'rate': (0, -50, 100), 'pitch': (0, -50, 50), 'volume': (0, -90, 100)},
    'tiktok': {},
}

def speech_options(model, raw):
    if model not in OPTIONS or not isinstance(raw, dict):
        raise ValueError('Invalid speech options')
    schema = OPTIONS[model]
    if raw.keys() - schema.keys():
        raise ValueError('Unsupported synthesis option')
    options = {}
    for k, spec in schema.items():
        default = spec[0] if isinstance(spec, tuple) else spec
        v = raw.get(k, default)
        if isinstance(default, bool):
            valid = type(v) is bool
        elif isinstance(default, str):
            valid = isinstance(v, str) and (v in spec if isinstance(spec, tuple) else len(v.encode()) <= 4096 and '\x00' not in v)
        else:
            valid = type(v) in (int, float) and math.isfinite(v) and spec[1] <= v <= spec[2]
            if isinstance(default, int):
                valid = valid and type(v) is int
        if not valid:
            raise ValueError('Invalid synthesis option: ' + k)
        options[k] = v
    return options

class Retention:
    """One lease per source and model. Only the serial server mutates this."""
    def __init__(self, clock=time.monotonic):
        self.clock = clock
        self.records = {}
        self.anonymous = {}

    @staticmethod
    def deadline(now, ttl):
        return float("inf") if ttl == -1 else now + ttl

    def acquire(self, model, client, ttl):
        rows = self.records.setdefault(model, {})
        self.evictable(model)  # prune expired records before applying the limit
        if client not in rows and len(rows) >= 256:
            raise ValueError("Too many model leases")
        rows[client] = (True, self.clock() + 120, ttl)
        self.anonymous.pop(model, None)

    def release(self, model, client):
        rows = self.records.get(model, {})
        previous = rows.get(client)
        if previous and previous[0]:
            rows[client] = (False, self.deadline(self.clock(), previous[2]), previous[2])

    def used(self, model, ttl):
        # Unmanaged/old clients get a bounded idle cache. Managed requests
        # are additionally protected by their source's independent lease.
        self.evictable(model)
        if not self.records.get(model):
            self.anonymous[model] = self.deadline(self.clock(), ttl)

    def evictable(self, model):
        now = self.clock()
        rows = self.records.get(model, {})
        for client, (active, deadline, ttl) in list(rows.items()):
            if active and now >= deadline:
                # Crashed client: never leave an eternal orphan lease.
                rows[client] = (False, self.deadline(deadline, max(0, ttl)), ttl)
            if not rows[client][0] and now >= rows[client][1]:
                del rows[client]
        if self.anonymous.get(model, 0) <= now:
            self.anonymous.pop(model, None)
        return not rows and model not in self.anonymous

    def status(self, models):
        names = sorted(set(models) | set(self.records))
        return {"loaded": sorted(models), "active": {
            name: sum(row[0] for row in self.records.get(name, {}).values())
            for name in names}}

def load_piper_voice(piper, model_path, device="auto"):
    """Prefer CUDA when ONNX Runtime actually provides it; retain CPU fallback.

    ONNX Runtime can silently replace an unavailable CUDA EP with CPU. Inspect
    the resulting session instead of treating use_cuda=True as proof of GPU.
    """
    import onnxruntime as ort
    if device not in ("auto", "cpu", "cuda"):
        raise ValueError("Unknown reader device")
    cuda_available = "CUDAExecutionProvider" in ort.get_available_providers()
    if device == "cuda" and not cuda_available:
        raise RuntimeError("CUDA execution provider unavailable")
    if cuda_available and device != "cpu":
        try:
            voice = piper.load(model_path, use_cuda=True)
            if "CUDAExecutionProvider" in voice.session.get_providers():
                print("SOUND_TTS_PROVIDER model=piper selected=cuda", flush=True)
                return voice
            if device == "cuda":
                raise RuntimeError("CUDA execution provider not selected")
            print("SOUND_TTS_PROVIDER_FALLBACK model=piper cause=cuda_not_selected", flush=True)
        except Exception as exc:
            if device == "cuda":
                raise
            print("SOUND_TTS_PROVIDER_FALLBACK model=piper cause=" + type(exc).__name__, flush=True)
    voice = piper.load(model_path, use_cuda=False)
    print("SOUND_TTS_PROVIDER model=piper selected=cpu", flush=True)
    return voice


def load_kokoro_voice(kokoro, model_path, voices_path, device="auto"):
    import onnxruntime as ort
    if device not in ("auto", "cpu", "cuda"):
        raise ValueError("Unknown reader device")
    available = ort.get_available_providers()
    cuda = "CUDAExecutionProvider" in available and device != "cpu"
    if device == "cuda" and not cuda:
        raise RuntimeError("CUDA execution provider unavailable")
    if cuda:
        try:
            session = ort.InferenceSession(str(model_path), providers=["CUDAExecutionProvider", "CPUExecutionProvider"])
            if "CUDAExecutionProvider" in session.get_providers():
                voice = kokoro.from_session(session, str(voices_path))
                print("SOUND_TTS_PROVIDER model=kokoro registered=" + ",".join(session.get_providers()), flush=True)
                return voice
            if device == "cuda":
                raise RuntimeError("CUDA execution provider not selected")
            print("SOUND_TTS_PROVIDER_FALLBACK model=kokoro cause=cuda_not_selected", flush=True)
        except Exception as exc:
            if device == "cuda":
                raise
            print("SOUND_TTS_PROVIDER_FALLBACK model=kokoro cause=" + type(exc).__name__, flush=True)
    session = ort.InferenceSession(str(model_path), providers=["CPUExecutionProvider"])
    voice = kokoro.from_session(session, str(voices_path))
    print("SOUND_TTS_PROVIDER model=kokoro registered=" + ",".join(session.get_providers()), flush=True)
    return voice


class Engine:
    def __init__(self, mode, assets, device="auto"):
        self.mode, self.assets = mode, assets
        self.device = device
        self.models = {}
        self.retention = Retention()

    def supports(self, model):
        return model in (('kokoro','piper') if self.mode == 'reader' else ('chatterbox',))

    def sweep(self):
        # Called only between HTTP requests, never during inference.
        for model in list(set(self.models) | set(self.retention.records)):
            if self.retention.evictable(model) and model in self.models:
                del self.models[model]
                gc.collect()
                if model == 'chatterbox':
                    try:
                        import torch
                        torch.cuda.empty_cache()
                    except Exception as exc:
                        print('SOUND_TTS_CUDA_CLEANUP_ERROR', type(exc).__name__, flush=True)
                # glibc may retain large freed model-loading allocations.
                # Best effort only; imported runtimes and CUDA context stay.
                try:
                    ctypes.CDLL(None).malloc_trim(0)
                except (AttributeError, OSError):
                    pass
                print('SOUND_TTS_MODEL_UNLOADED', model, flush=True)


    def synthesize(self, model, voice, text, options=None):
        import numpy as np
        options = speech_options(model, {} if options is None else options)
        if model == 'kokoro' and self.mode == 'reader':
            from kokoro_onnx import Kokoro
            if model not in self.models:
                self.models[model] = load_kokoro_voice(Kokoro, self.assets/'kokoro/kokoro-v1.0.fp16.onnx',
                                                       self.assets/'kokoro/voices-v1.0.bin', self.device)
            engine = self.models[model]
            if voice not in engine.get_voices() or voice[:2] not in ('af','am','bf','bm'):
                raise ValueError('Select a supported English Kokoro voice')
            lang = options.pop('language')
            if lang == 'auto':
                lang = 'en-gb' if voice.startswith('b') else 'en-us'
            samples, rate = engine.create(text, voice=voice, lang=lang, **options)
        elif model == 'piper' and self.mode == 'reader':
            from piper import PiperVoice
            if voice != 'en_US-amy-low':
                raise ValueError('Unknown local Piper voice')
            if model not in self.models:
                self.models[model] = load_piper_voice(PiperVoice, self.assets/'piper/en_US-amy-low.onnx', self.device)
            from piper.config import SynthesisConfig
            chunks = list(self.models[model].synthesize(text, SynthesisConfig(**options)))
            if not chunks:
                raise ValueError('Empty Piper result')
            rate = chunks[0].sample_rate
            if any(c.sample_rate != rate or c.sample_channels != 1 or c.sample_width != 2 for c in chunks):
                raise ValueError('Unexpected Piper output format')
            samples = np.frombuffer(b''.join(c.audio_int16_bytes for c in chunks), dtype='<i2').astype(np.float32)/32768
        elif model == 'chatterbox' and self.mode == 'chatterbox':
            from chatterbox.tts_turbo import ChatterboxTurboTTS
            if voice != 'default':
                raise ValueError('Only the cached built-in voice is qualified')
            if model not in self.models:
                self.models[model] = ChatterboxTurboTTS.from_local(self.assets, device='cuda', nano=True)
            engine = self.models[model]
            # Reference conditioning mutates this cached engine. Restore the
            # built-in conditioning even if the request fails, so a different
            # OBS source never inherits the preceding source's reference.
            prompt = options.pop('audio_prompt_path')
            if prompt:
                import soundfile as sf
                path = Path(prompt)
                if (path.suffix.lower() not in ('.wav', '.flac') or not path.is_file()
                        or path.stat().st_size > 32 * 1024 * 1024):
                    raise ValueError('Reference must be a local WAV or FLAC, at most 32 MiB')
                info = sf.info(str(path))
                if not 5.0 < info.duration <= 60.0:
                    raise ValueError('Reference must be longer than 5 and at most 60 seconds')
            default_conds = engine.conds
            try:
                samples, rate = engine.generate(text, audio_prompt_path=prompt or None, **options).detach().cpu().numpy(), engine.sr
            finally:
                engine.conds = default_conds
        else:
            raise ValueError('Unsupported backend')
        samples = np.asarray(samples, dtype=np.float32).reshape(-1)
        if not (8000 <= rate <= 48000 and 0 < samples.size <= rate*MAX_SECONDS
                and np.isfinite(samples).all() and np.abs(samples).max() > .001):
            raise ValueError('Invalid or silent speech result')
        buf = io.BytesIO()
        with wave.open(buf, 'wb') as out:
            out.setnchannels(1)
            out.setsampwidth(2)
            out.setframerate(rate)
            out.writeframes((np.clip(samples, -1, 1)*32767).astype('<i2').tobytes())
        return buf.getvalue()

class Handler(BaseHTTPRequestHandler):
    def setup(self):
        self.request.settimeout(10)
        super().setup()

    def log_message(self, *_):
        pass # no user text in logs

    def reply(self, code, body, mime='application/json'):
        self.send_response(code)
        self.send_header('Content-Type', mime)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Sound-TTS-Options', '1')
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == '/healthz':
            self.reply(200, b'{"ok":true}')
        elif self.path == '/v1/models/status' and hasattr(self.server.engine, 'retention'):
            engine = self.server.engine
            engine.sweep()
            self.reply(200, json.dumps(engine.retention.status(engine.models)).encode())
        else:
            self.reply(404, b'{}')

    def do_POST(self):
        try:
            n = int(self.headers.get('Content-Length', '0'))
            if self.path not in ('/v1/audio/speech','/v1/models/lease') or not 0 < n <= 8192:
                raise ValueError('Invalid request')
            obj = json.loads(self.rfile.read(n))
            engine = self.server.engine
            if self.path == '/v1/models/lease':
                if not hasattr(engine, 'retention'):
                    raise ValueError('No local model leases')
                model, client, action = obj['model'], obj['client'], obj['action']
                ttl = obj.get('ttl', 0)
                if (not engine.supports(model) or not isinstance(client, str) or
                    not 1 <= len(client) <= 128 or type(ttl) is not int or
                    not -1 <= ttl <= 3600 or action not in ('acquire','release')):
                    raise ValueError('Invalid lease')
                if action == 'acquire':
                    engine.retention.acquire(model, client, ttl)
                else:
                    engine.retention.release(model, client)
                engine.sweep()
                self.reply(200, b'{"ok":true}')
                return
            text = obj['input']
            if not isinstance(text, str) or not text.strip() or len(text.encode()) > MAX_TEXT:
                raise ValueError('Text must be 1–600 UTF-8 bytes')
            if obj.get('response_format') != 'wav':
                raise ValueError('WAV required')
            ttl = obj.get('retention_ttl', 30)
            if type(ttl) is not int or not -1 <= ttl <= 3600:
                raise ValueError('Invalid TTL')
            options = speech_options(obj['model'], obj.get('options', {}))
            try:
                audio = engine.synthesize(obj['model'], obj['voice'], text, options)
            finally:
                if hasattr(engine, 'retention') and engine.supports(obj['model']):
                    engine.retention.used(obj['model'], ttl)
            self.reply(200, audio, 'audio/wav')
        except (BrokenPipeError, ConnectionResetError):
            return
        except Exception as exc:
            # No plaintext input or paths in error responses.
            print('SOUND_TTS_ERROR', type(exc).__name__, flush=True)
            try:
                self.reply(400 if isinstance(exc, (ValueError, KeyError, TypeError)) else 503,
                           b'{"error":"Speech generation failed"}')
            except OSError:
                pass

class Server(socketserver.UnixStreamServer):
    request_queue_size = 16
    def service_actions(self):
        if hasattr(self.engine, 'sweep'):
            self.engine.sweep()

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--mode', choices=['reader','chatterbox'], required=True)
    parser.add_argument('--assets', type=Path, required=True)
    parser.add_argument('--socket', type=Path, required=True)
    parser.add_argument('--device', choices=['auto','cpu','cuda'], default='auto', help='Reader ONNX provider policy')
    args = parser.parse_args()
    os.environ['HF_HUB_OFFLINE'] = '1'
    os.environ['TRANSFORMERS_OFFLINE'] = '1'
    os.umask(0o077)
    parent = args.socket.parent
    if not parent.is_dir() or parent.is_symlink() or parent.stat().st_uid != os.getuid() or parent.stat().st_mode & 0o077:
        raise SystemExit('Private owner-only runtime directory required')
    if args.socket.exists() or args.socket.is_symlink():
        raise SystemExit('Socket already exists; refusing overwrite')
    args.assets.resolve(strict=True)
    with Server(str(args.socket), Handler) as server:
        server.engine = Engine(args.mode, args.assets, args.device)
        os.chmod(args.socket, 0o600)
        print('SOUND_TTS_WORKER_READY', args.mode, flush=True)
        try:
            server.serve_forever()
        finally:
            args.socket.unlink(missing_ok=True)

if __name__ == '__main__':
    main()
