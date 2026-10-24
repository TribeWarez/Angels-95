#!/usr/bin/env python3
"""Collapse a multi-skin glTF/GLB into ONE skin.

FBX2glTF exports each mesh part of a rigged FBX as its own skin even when every
part is bound to the SAME armature. raylib supports a single skin per model and
its multi-skin handling is the crash class `flatten_glb.py` exists to avoid, so
for an ANIMATED weapon we collapse the duplicate skins instead of stripping the
rig:

  * every skinned node is re-pointed at skin 0
  * the duplicate skin records are dropped

That is only sound when the skins are equivalent: this tool requires every skin
to list the SAME joint nodes in the SAME order (which is what FBX2glTF emits for
one armature) and refuses otherwise rather than guessing a remap. JOINTS_0 data
therefore needs no rewriting.

Usage: python merge_glb_skins.py <model.glb>
Edits the file in place.
"""
import json
import struct
import sys

GLB_MAGIC = 0x46546C67
JSON_CHUNK = 0x4E4F534A
BIN_CHUNK = 0x004E4942


def read_glb(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, ver, length = struct.unpack_from("<III", data, 0)
    if magic != GLB_MAGIC:
        raise ValueError(f"{path}: not a GLB")
    off = 12
    js = None
    bin_data = b""
    while off < length:
        clen, ctype = struct.unpack_from("<II", data, off)
        off += 8
        chunk = data[off:off + clen]
        off += clen
        if ctype == JSON_CHUNK:
            js = json.loads(chunk.decode("utf-8"))
        elif ctype == BIN_CHUNK:
            bin_data = chunk
    return js, bin_data


def write_glb(path, js, bin_data):
    js_bytes = json.dumps(js, separators=(",", ":")).encode("utf-8")
    js_bytes += b" " * ((4 - (len(js_bytes) % 4)) % 4)
    bin_data = bytes(bin_data)
    bin_data += b"\x00" * ((4 - (len(bin_data) % 4)) % 4)
    total = 12 + 8 + len(js_bytes) + (8 + len(bin_data) if bin_data else 0)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", GLB_MAGIC, 2, total))
        f.write(struct.pack("<II", len(js_bytes), JSON_CHUNK))
        f.write(js_bytes)
        if bin_data:
            f.write(struct.pack("<II", len(bin_data), BIN_CHUNK))
            f.write(bin_data)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 1
    path = sys.argv[1]
    model, bin_data = read_glb(path)
    skins = model.get("skins", [])
    if len(skins) <= 1:
        print(f"{path}: already single-skin ({len(skins)}) - nothing to do")
        return 0

    base = skins[0].get("joints", [])
    for i, skin in enumerate(skins[1:], start=1):
        if skin.get("joints", []) != base:
            print(f"{path}: skin {i} has a DIFFERENT joint list; refusing to merge.")
            print("  Re-export the model so every part is bound to one armature.")
            return 2

    by_skin = {}
    for node in model.get("nodes", []):
        if "skin" in node:
            by_skin[node["skin"]] = by_skin.get(node["skin"], 0) + 1
            node["skin"] = 0

    model["skins"] = [skins[0]]
    # Animations are unaffected: they target joint NODES, not skin records.
    write_glb(path, model, bin_data)
    print(f"{path}: merged {len(skins)} skins -> 1 ({len(base)} joints); "
          f"re-pointed nodes per skin {by_skin}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
