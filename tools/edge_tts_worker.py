#!/usr/bin/env python3
"""Explicit cloud-only Edge speech worker, same-user socket, bounded WAV output."""
import asyncio
import io
import json
import os
from pathlib import Path
import subprocess
import wave
import edge_tts
from tts_worker import Handler, Server, MAX_SECONDS, speech_options

VOICES = frozenset(v["id"] for v in json.loads(
    Path(__file__).with_name("edge-voices.json").read_text()))
RATE = 24000
MAX_MP3 = 1024 * 1024

async def generate(voice, text, options):
    result = bytearray()
    client = edge_tts.Communicate(text, voice, connect_timeout=10, receive_timeout=20,
                                  rate=f"{options['rate']:+d}%", pitch=f"{options['pitch']:+d}Hz",
                                  volume=f"{options['volume']:+d}%")
    async for part in client.stream():
        if part["type"] == "audio":
            chunk = part["data"]
            if len(result) + len(chunk) > MAX_MP3:
                raise ValueError("Speech response too large")
            result.extend(chunk)
    if not result:
        raise ValueError("Empty speech response")
    return bytes(result)

class EdgeEngine:
    def synthesize(self, model, voice, text, options=None):
        if model != "edge" or not isinstance(voice, str) or voice not in VOICES:
            raise ValueError("Unknown Edge voice or backend")
        options = speech_options(model, {} if options is None else options)
        mp3 = asyncio.run(asyncio.wait_for(generate(voice, text, options), timeout=30))
        # Raw output avoids a streaming WAV header with unknown chunk sizes.
        raw = subprocess.run(
            ["/usr/bin/ffmpeg", "-nostdin", "-v", "error", "-f", "mp3",
             "-i", "pipe:0", "-t", str(MAX_SECONDS+1), "-ac", "1", "-ar", str(RATE),
             "-f", "s16le", "pipe:1"],
            input=mp3, capture_output=True, timeout=8, check=True).stdout
        if not raw or len(raw) % 2 or len(raw) > RATE * MAX_SECONDS * 2 or not any(raw):
            raise ValueError("Invalid decoded speech")
        buf = io.BytesIO()
        with wave.open(buf, "wb") as out:
            out.setnchannels(1)
            out.setsampwidth(2)
            out.setframerate(RATE)
            out.writeframes(raw)
        return buf.getvalue()

def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", type=Path, required=True)
    args = parser.parse_args()
    os.umask(0o077)
    parent = args.socket.parent
    if not parent.is_dir() or parent.is_symlink() or parent.stat().st_uid != os.getuid() or parent.stat().st_mode & 0o077:
        raise SystemExit("Private owner-only runtime directory required")
    if args.socket.exists() or args.socket.is_symlink():
        raise SystemExit("Socket already exists; refusing overwrite")
    with Server(str(args.socket), Handler) as server:
        server.engine = EdgeEngine()
        os.chmod(args.socket, 0o600)
        print("SOUND_EDGE_WORKER_READY", flush=True)
        try:
            server.serve_forever()
        finally:
            args.socket.unlink(missing_ok=True)

if __name__ == "__main__":
    main()
