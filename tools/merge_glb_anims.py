#!/usr/bin/env python3
"""Merge glTF animations from separate .glb files into a model .glb.

Called by `tools/convert_fbx.ps1` on its animated path, AFTER
`merge_glb_skins.py` has collapsed FBX2glTF's one-skin-per-part output into a
single skin (raylib supports one skin per model).

FBX2glTF exports each FBX independently, so a rig's animations end up in their
own .glb files with no skin. raylib's LoadModelAnimations() validates the
animation against the model's skeleton, so the clips must live in the model GLB.
This copies the animation channels (remapping node indices by name) plus their
bufferViews/accessors/buffer bytes into the target model GLB and renames each
clip.

Usage:
  python merge_glb_anims.py <model.glb> <clipName=anim.glb> [<clipName=anim.glb> ...]

Writes the merged model back to <model.glb>.
"""
import json
import struct
import sys
import os

GLB_MAGIC = 0x46546C67
JSON_CHUNK = 0x4E4F534A
BIN_CHUNK = 0x004E4942


def read_glb(path):
    with open(path, "rb") as f:
        magic, ver, length = struct.unpack("<III", f.read(12))
        if magic != GLB_MAGIC:
            raise ValueError(f"{path}: not a GLB")
        chunks = {}
        while f.tell() < length:
            clen, ctype = struct.unpack("<II", f.read(8))
            data = f.read(clen)
            chunks[ctype] = data
    js = json.loads(chunks[JSON_CHUNK].decode("utf-8"))
    bin_data = chunks.get(BIN_CHUNK, b"")
    return js, bytearray(bin_data)


def write_glb(path, js, bin_data):
    js_bytes = json.dumps(js, separators=(",", ":")).encode("utf-8")
    js_pad = (4 - (len(js_bytes) % 4)) % 4
    js_bytes += b" " * js_pad
    bin_data = bytes(bin_data)
    bin_pad = (4 - (len(bin_data) % 4)) % 4
    bin_padded = bin_data + b"\x00" * bin_pad
    total = 12 + 8 + len(js_bytes) + (8 + len(bin_padded) if bin_padded else 0)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", GLB_MAGIC, 2, total))
        f.write(struct.pack("<II", len(js_bytes), JSON_CHUNK))
        f.write(js_bytes)
        if bin_padded:
            f.write(struct.pack("<II", len(bin_padded), BIN_CHUNK))
            f.write(bin_padded)


def node_name_index(model_js):
    m = {}
    for i, n in enumerate(model_js.get("nodes", [])):
        name = n.get("name")
        if name is not None and name not in m:
            m[name] = i
    return m


def add_accessor(model, mbin, floats, comps, gltype):
    """Append float data to the model buffer and register a bufferView+accessor."""
    data = struct.pack("<%df" % len(floats), *floats)
    while len(mbin) % 4:
        mbin.append(0)
    off = len(mbin)
    mbin.extend(data)
    bv = len(model["bufferViews"])
    model["bufferViews"].append({"buffer": 0, "byteOffset": off, "byteLength": len(data)})
    ac = len(model["accessors"])
    model["accessors"].append({
        "bufferView": bv, "componentType": 5126,
        "count": len(floats) // comps, "type": gltype,
    })
    return ac


def synth_idle(model, mbin, node_indices):
    """Add an 'Idle' clip holding the rest TRS of every animated bone.

    FBX2glTF exports each animation separately with no idle/rest clip, so the
    view-model has nothing to return to after a one-shot Fire/Reload. This
    synthesizes a static Idle from the nodes' bind transforms.
    """
    if not node_indices:
        return
    input_ac = add_accessor(model, mbin, [0.0, 1.0], 1, "SCALAR")
    samplers = []
    channels = []
    for ni in sorted(node_indices):
        node = model["nodes"][ni]
        t = list(node.get("translation", [0.0, 0.0, 0.0]))
        r = list(node.get("rotation", [0.0, 0.0, 0.0, 1.0]))
        s = list(node.get("scale", [1.0, 1.0, 1.0]))
        outs = {
            "translation": add_accessor(model, mbin, t + t, 3, "VEC3"),
            "rotation": add_accessor(model, mbin, r + r, 4, "VEC4"),
            "scale": add_accessor(model, mbin, s + s, 3, "VEC3"),
        }
        for path, out in outs.items():
            si = len(samplers)
            samplers.append({"input": input_ac, "output": out, "interpolation": "LINEAR"})
            channels.append({"sampler": si, "target": {"node": ni, "path": path}})
    model["animations"].append({"name": "Idle", "samplers": samplers, "channels": channels})
    print(f"  + synthesized 'Idle' clip ({len(channels)} channels)")


def merge(model_path, clips):
    model, mbin = read_glb(model_path)
    model.setdefault("bufferViews", [])
    model.setdefault("accessors", [])
    model.setdefault("animations", [])
    if not model.get("buffers"):
        model["buffers"] = [{"byteLength": len(mbin)}]
    name2node = node_name_index(model)
    animated_nodes = set()

    for clip_name, anim_path in clips:
        anim, abin = read_glb(anim_path)
        if not anim.get("animations"):
            print(f"  (no animations in {os.path.basename(anim_path)})")
            continue
        base_len = len(mbin)
        bv_off = len(model["bufferViews"])
        ac_off = len(model["accessors"])

        for bv in anim.get("bufferViews", []):
            nbv = dict(bv)
            nbv["buffer"] = 0
            nbv["byteOffset"] = bv.get("byteOffset", 0) + base_len
            model["bufferViews"].append(nbv)
        for ac in anim.get("accessors", []):
            nac = dict(ac)
            if "bufferView" in nac:
                nac["bufferView"] = nac["bufferView"] + bv_off
            if "sparse" in nac:  # unsupported; drop sparse (skip winding)
                nac.pop("sparse", None)
            model["accessors"].append(nac)

        anim_nodes = anim.get("nodes", [])
        mapped = 0
        skipped = 0
        for a in anim["animations"]:
            src_name = a.get("name", "Scene")
            for s in a.get("samplers", []):
                s["input"] += ac_off
                s["output"] += ac_off
            new_channels = []
            for ch in a.get("channels", []):
                tgt = ch.get("target", {})
                ni = tgt.get("node")
                if ni is not None and ni < len(anim_nodes):
                    nm = anim_nodes[ni].get("name")
                    if nm in name2node:
                        tgt["node"] = name2node[nm]
                        animated_nodes.add(name2node[nm])
                        new_channels.append(ch)
                        mapped += 1
                        continue
                skipped += 1
            if not new_channels:
                continue
            model["animations"].append({
                "name": clip_name,
                "samplers": a.get("samplers", []),
                "channels": new_channels,
            })
        mbin.extend(abin)
        print(f"  + clip '{clip_name}' from {os.path.basename(anim_path)} "
              f"({mapped} channels mapped, {skipped} skipped)")

    # Ensure an Idle clip exists (from the bind pose) for one-shot returns.
    if animated_nodes and not any(a.get("name") == "Idle" for a in model.get("animations", [])):
        synth_idle(model, mbin, animated_nodes)

    model["buffers"][0]["byteLength"] = len(mbin)
    write_glb(model_path, model, mbin)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    model_path = sys.argv[1]
    clips = []
    for arg in sys.argv[2:]:
        if "=" in arg:
            name, path = arg.split("=", 1)
        else:
            name, path = os.path.splitext(os.path.basename(arg))[0], arg
        clips.append((name, path))
    print(f"Merging into {model_path}")
    merge(model_path, clips)
    return 0


if __name__ == "__main__":
    sys.exit(main())
