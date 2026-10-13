"""
Repair GameData/Audio/Sounds/Guns/machgf3b.wav.

The shipped file has trailing junk bytes after the audio `data` chunk: the RIFF
size header accounts for 65510 bytes but the `data` chunk only declares 65466.
raylib's frame-count converter rejects the inconsistent header.

This script walks the RIFF chunks, copies each chunk verbatim into a fresh
container, and drops anything after the final chunk. The repaired file has a
consistent RIFF size and decodes cleanly.

Run from repo root:
    py tools/repair_wav_truncate.py GameData/Audio/Sounds/Guns/machgf3b.wav
"""

import struct
import sys
from pathlib import Path


def repair(path: Path) -> None:
    raw = path.read_bytes()
    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise SystemExit(f"{path}: not a RIFF/WAVE file")

    out = bytearray(b"RIFF")
    # RIFF size = (file size - 8); we will set it to match the rebuilt content
    out += b"\x00\x00\x00\x00"
    out += b"WAVE"

    p = 12
    while p + 8 <= len(raw):
        chunk_id = raw[p : p + 4]
        chunk_size = struct.unpack_from("<I", raw, p + 4)[0]
        end = p + 8 + chunk_size
        if end > len(raw):
            # Truncated last chunk: stop here, do not copy the broken tail.
            break
        out += raw[p:end]
        p = end
        # pad byte if chunk size is odd
        if chunk_size % 2 == 1 and p < len(raw):
            out += raw[p : p + 1]
            p += 1

    # RIFF size = total written - 8
    struct.pack_into("<I", out, 4, len(out) - 8)

    path.write_bytes(bytes(out))
    print(f"{path}: rewrote {len(raw)} -> {len(out)} bytes")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: repair_wav_truncate.py <file.wav>")
    repair(Path(sys.argv[1]))
