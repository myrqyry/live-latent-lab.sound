#!/usr/bin/env python3
"""TEST ONLY. Deterministic protocol peer, not an audio separator.

Never launch this in OBS or ship it as a model runtime. The distinct
constant outputs establish exact channel/selection/transport behavior.
"""
import array
import os
import socket
import struct
import sys

HEADER = struct.Struct("<8sIIIIIII")
path = sys.argv[1]
assert not os.path.lexists(path)
def read_exact(peer, n):
    buf = bytearray(n)
    view = memoryview(buf)
    while view:
        count = peer.recv_into(view)
        if not count:
            raise EOFError()
        view = view[count:]
    return buf

with socket.socket(socket.AF_UNIX) as server:
    server.bind(path)
    os.chmod(path, 0o600)
    server.listen(2)
    try:
        while True:
            peer, _ = server.accept()
            with peer:
                peer.settimeout(4)
                try:
                    header = HEADER.unpack(read_exact(peer, HEADER.size))
                    magic, version, rate, frames, channels, seq, stems, status = header
                    assert (magic, version, channels, stems, status) == (b"LLSTEM01", 1, 2, 0, 0)
                    assert rate in (44100, 48000) and frames == 8 * rate
                    read_exact(peer, frames * 8)
                    peer.sendall(HEADER.pack(magic, version, rate, frames, channels, seq, 4, 0))
                    for stem in range(4):
                        for value in ((stem + 1) * .025, -(stem + 1) * .0375):
                            samples = array.array("f", [value]) * frames
                            if sys.byteorder != "little":
                                samples.byteswap()
                            peer.sendall(samples)
                    print(f"TEST_FIXTURE seq={seq} frames={frames}", flush=True)
                except (OSError, EOFError):
                    pass
    finally:
        os.unlink(path)
