"""Private resident CUDA Kokoro helper using the pinned audio.cpp C ABI.

No HTTP listener or cloud fallback. Only the reader owns this child.
"""
import ctypes as C
import hashlib
import io
import json
import math
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import wave
from array import array

VOICES = frozenset(("af_alloy af_aoede af_bella af_heart af_jessica af_kore af_nicole "
                    "af_nova af_river af_sarah af_sky am_adam am_echo am_eric am_fenrir "
                    "am_liam am_michael am_onyx am_puck am_santa bf_alice bf_emma bf_isabella "
                    "bf_lily bm_daniel bm_fable bm_george bm_lewis").split())
MODEL_SHA = '9a4ca6176460cdc8c7845b804c0bbe85a4791ee3ffff568a3fdadbb708c63f47'
MAX_WAV = 24000 * 90 * 2 + 44

def receive(sock, n):
    data = bytearray()
    while len(data) < n:
        chunk = sock.recv(n-len(data))
        if not chunk:
            raise EOFError('Native worker closed')
        data.extend(chunk)
    return bytes(data)

def packet(sock, limit):
    n, = struct.unpack('<I', receive(sock, 4))
    if not 0 < n <= limit:
        raise RuntimeError('Invalid native worker packet')
    return receive(sock, n)

def send(sock, data):
    sock.sendall(struct.pack('<I', len(data)) + data)

def validate(voice, text, speed):
    if not isinstance(voice, str) or voice not in VOICES or not isinstance(text, str) or not text.strip() or '\x00' in text or len(text.encode()) > 600:
        raise ValueError('Invalid native English speech request')
    if type(speed) not in (float, int) or not math.isfinite(speed) or not .5 <= speed <= 2:
        raise ValueError('Invalid speed')

class Worker:
    entrypoint = Path(__file__)
    model_id = "kokoro_native"
    device = "cuda"

    def __init__(self, library, model):
        self.sock, child = socket.socketpair()
        self.sock.settimeout(40)
        self.process = None
        self.reported = False
        env = os.environ.copy()
        if self.device == "cpu":
            env["CUDA_VISIBLE_DEVICES"] = ""
        try:
            self.process = subprocess.Popen([sys.executable, str(self.entrypoint.resolve()),
                str(child.fileno()), str(library), str(model)], pass_fds=(child.fileno(),),
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
        except BaseException:
            self.sock.close()
            raise
        finally:
            child.close()

    def close(self):
        self.sock.close()
        if self.process is not None:
            if self.process.poll() is None:
                self.process.terminate()
                try:
                    self.process.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    self.process.kill()
            self.process.wait()

    def synthesize(self, voice, text, speed):
        validate(voice, text, speed)
        return self.request([voice, text, speed])

    def request(self, args):
        try:
            send(self.sock, json.dumps(args, ensure_ascii=False).encode())
            result = packet(self.sock, MAX_WAV)
            if not result.startswith(b'RIFF'):
                raise RuntimeError('Native synthesis failed')
            if not self.reported:
                print('SOUND_TTS_PROVIDER model='+self.model_id+' selected='+self.device, flush=True)
                self.reported = True
            return result
        except (OSError, EOFError, RuntimeError) as exc:
            self.close()
            raise RuntimeError('Native speech worker failed') from exc

class Backend(C.Structure):
    _fields_ = [('backend', C.c_char_p), ('device', C.c_int), ('threads', C.c_int)]

class Codec:
    device = "cuda"
    threads = 8
    model_bytes = 189550976
    model_sha = MODEL_SHA

    def __init__(self, library, model):
        model = Path(model)
        if model.stat().st_size != self.model_bytes:
            raise RuntimeError('Unqualified native speech model size')
        with model.open('rb') as f:
            if hashlib.file_digest(f, 'sha256').hexdigest() != self.model_sha:
                raise RuntimeError('Unqualified native speech model checksum')
        self.lib = C.CDLL(str(Path(library).resolve(strict=True)), mode=C.RTLD_LOCAL)
        ptr, string, integer = C.c_void_p, C.c_char_p, C.c_int
        pp = C.POINTER(ptr)
        def bind(name, args, restype=integer):
            f = getattr(self.lib, 'audiocpp_'+name)
            f.argtypes, f.restype = args, restype
            setattr(self, name, f)
        bind('abi_version', [], C.c_uint32)
        if self.abi_version() >> 8 != 2:
            raise RuntimeError('Unsupported native ABI')
        bind('registry_create', [string, pp])
        bind('model_load', [ptr, string, ptr, ptr, pp])
        bind('session_create', [ptr, string, string, C.POINTER(Backend), ptr, pp])
        bind('request_create', [], ptr)
        bind('request_set_text', [ptr, string, string])
        bind('request_set_voice_id', [ptr, string])
        bind('request_set_option', [ptr, string, string])
        bind('session_run', [ptr, ptr, pp])
        bind('result_audio', [ptr, C.POINTER(C.POINTER(C.c_float)), C.POINTER(C.c_size_t), C.POINTER(integer), C.POINTER(integer)])
        for name in ('registry_free','model_free','session_free','request_free','result_free'):
            bind(name, [ptr], None)
        self.registry, self.model, self.session = ptr(), ptr(), ptr()
        try:
            self.check(self.registry_create(None, C.byref(self.registry)))
            self.check(self.model_load(self.registry, os.fsencode(model), None, None, C.byref(self.model)))
            backend = Backend(self.device.encode('ascii'), 0, self.threads)
            self.check(self.session_create(self.model, b'tts', b'offline', C.byref(backend), None, C.byref(self.session)))
        except BaseException:
            self.close()
            raise

    @staticmethod
    def check(status):
        if status:
            raise RuntimeError('Native audio.cpp status '+str(status))

    def close(self):
        for name in ('session','model','registry'):
            p = getattr(self, name)
            if p:
                getattr(self, name+'_free')(p)
                p.value = None

    def synthesize(self, voice, text, speed):
        validate(voice, text, speed)
        language = b'en-gb' if voice.startswith('b') else b'en-us'
        return self.run(voice, text, language, {'speed':speed})

    def run(self, voice, text, language, options):
        request, result = self.request_create(), C.c_void_p()
        if not request:
            raise RuntimeError('Native request allocation failed')
        try:
            self.check(self.request_set_text(request, text.encode(), language))
            if voice is not None:
                self.check(self.request_set_voice_id(request, voice.encode()))
            for key, value in options.items():
                self.check(self.request_set_option(request, key.encode(), str(value).encode()))
            self.check(self.session_run(self.session, request, C.byref(result)))
            samples, frames, rate, ch = C.POINTER(C.c_float)(), C.c_size_t(), C.c_int(), C.c_int()
            self.check(self.result_audio(result, C.byref(samples), C.byref(frames), C.byref(rate), C.byref(ch)))
            if not samples or rate.value != 24000 or ch.value != 1 or not 0 < frames.value <= 24000*90:
                raise RuntimeError('Invalid native audio geometry')
            pcm = array('h')
            peak = 0.
            for i in range(frames.value):
                v = samples[i]
                if not math.isfinite(v):
                    raise RuntimeError('Nonfinite native audio')
                peak = max(peak, abs(v))
                pcm.append(int(max(-1., min(1., v))*32767))
            if peak <= .001:
                raise RuntimeError('Silent native audio')
            if sys.byteorder != 'little':
                pcm.byteswap()
            buf = io.BytesIO()
            with wave.open(buf, 'wb') as out:
                out.setnchannels(1); out.setsampwidth(2); out.setframerate(24000)
                out.writeframes(pcm.tobytes())
            return buf.getvalue()
        finally:
            if result:
                self.result_free(result)
            self.request_free(request)

def serve(fd, library, model, codec_type=Codec):
    with socket.socket(fileno=fd) as sock:
        codec = codec_type(library, model)
        try:
            while True:
                try:
                    req = packet(sock, 4096)
                except EOFError:
                    return
                send(sock, codec.synthesize(*json.loads(req)))
        finally:
            codec.close()

if __name__ == '__main__':
    serve(int(sys.argv[1]), sys.argv[2], sys.argv[3])
