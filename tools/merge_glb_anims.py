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

A clip may be built from SEVERAL source files played in sequence by joining them
with '+'. That is how the shotgun's shell-by-shell reload ships: the pack authors
ReloadStep and ReloadEnd as separate takes, and the engine plays one "Reload"
clip, so they are concatenated:

  python merge_glb_anims.py Shotgun_01.glb Fire=Fire.glb Reload=Step.glb+End.glb

Each source's keyframe times are shifted by the running duration and channels
that animate the same node+property are combined into ONE channel, because glTF
does not allow two channels to target the same node+path. A 1/60 s gap is left
between segments so timestamps stay strictly increasing.

A clip whose name the model already carries is skipped, so re-running the
converter over an already-merged model cannot duplicate Fire/Reload.

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
    accessor = {
        "bufferView": bv, "componentType": 5126,
        "count": len(floats) // comps, "type": gltype,
    }
    if len(floats) >= comps:
        # glTF REQUIRES min/max on an animation sampler's input accessor, and
        # readers that trust them get a zero-length clip without. Cheap to add
        # and it makes the merged files validator-clean.
        accessor["min"] = [min(floats[i::comps]) for i in range(comps)]
        accessor["max"] = [max(floats[i::comps]) for i in range(comps)]
    model["accessors"].append(accessor)
    return ac


def read_accessor(js, bin_data, accessor_index):
    """Read a float accessor as (list of tuples, component count).

    Only float accessors are read - FBX2glTF and Blender both write animation
    times/values as floats, and re-encoding them is what lets a concatenated clip
    carry its own shifted timeline.
    """
    ac = js["accessors"][accessor_index]
    comps = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}.get(ac["type"])
    if comps is None or ac.get("componentType", 5126) != 5126:
        raise ValueError(f"accessor {accessor_index} is {ac.get('type')}/"
                         f"{ac.get('componentType')}, not a float animation accessor")
    bv = js["bufferViews"][ac["bufferView"]]
    off = bv.get("byteOffset", 0) + ac.get("byteOffset", 0)
    count = ac["count"] * comps
    flat = struct.unpack_from("<%df" % count, bin_data, off)
    return [tuple(flat[i * comps:(i + 1) * comps]) for i in range(ac["count"])], comps


# A concatenated clip must not share a timestamp between two segments: the last
# sample of part A sits exactly at part A's duration, so part B starts one frame
# later instead of on top of it.
SEAM_GAP = 1.0 / 60.0


def merge_sequence(model, mbin, clip_name, part_paths):
    """Append ONE clip built from part_paths played back to back."""
    name2node = node_name_index(model)
    merged = {}      # (node index, property) -> {"times": [...], "values": [...], comps}
    offset = 0.0
    for path in part_paths:
        anim, abin = read_glb(path)
        anim_nodes = anim.get("nodes", [])
        part_end = 0.0
        part_channels = 0
        for a in anim.get("animations", []):
            for ch in a.get("channels", []):
                target = ch.get("target", {})
                ni = target.get("node")
                prop = target.get("path")
                if ni is None or prop not in ("translation", "rotation", "scale"):
                    continue
                if ni >= len(anim_nodes):
                    continue
                name = anim_nodes[ni].get("name")
                if name not in name2node:
                    continue
                sampler = a["samplers"][ch["sampler"]]
                if sampler.get("interpolation", "LINEAR") != "LINEAR":
                    raise ValueError(f"{os.path.basename(path)}: {name}.{prop} uses "
                                     f"'{sampler.get('interpolation')}' interpolation; "
                                     f"only LINEAR segments can be concatenated")
                times, _ = read_accessor(anim, abin, sampler["input"])
                values, comps = read_accessor(anim, abin, sampler["output"])
                t = [x[0] for x in times]
                if t:
                    part_end = max(part_end, t[-1])
                slot = merged.setdefault((name2node[name], prop),
                                         {"times": [], "values": [], "comps": comps})
                if slot["comps"] != comps:
                    raise ValueError(f"{name}.{prop}: '{prop}' has {comps} components "
                                     f"in one segment and {slot['comps']} in another")
                slot["times"].extend(offset + x for x in t)
                slot["values"].extend(values)
                part_channels += 1
        offset += part_end + SEAM_GAP
        print(f"  + segment {os.path.basename(path)} ({part_channels} channels, "
              f"{part_end:.3f}s)")

    if not merged:
        print(f"  (no usable channels for '{clip_name}')")
        return set()

    # Every channel must reach the clip's end time. raylib resamples a clip onto
    # a uniform 60 fps grid and its GetPoseAtTimeGLTF leaves `keyframe` at 0 when
    # no sample pair brackets the requested time, so a channel that stops early
    # does not HOLD its last pose - it snaps back to its SECOND keyframe for the
    # rest of the clip. A target that appears in only one segment would therefore
    # pop mid-reload, so each one gets a duplicate of its final sample at the clip
    # end. (This is padding, not interpolation: the value is unchanged.)
    clip_end = max(slot["times"][-1] for slot in merged.values() if slot["times"])
    for slot in merged.values():
        if slot["times"] and slot["times"][-1] < clip_end:
            slot["times"].append(clip_end)
            slot["values"].append(slot["values"][-1])

    samplers = []
    channels = []
    for (ni, prop), slot in sorted(merged.items()):
        times_ac = add_accessor(model, mbin, slot["times"], 1, "SCALAR")
        flat = [v for sample in slot["values"] for v in sample]
        values_ac = add_accessor(model, mbin, flat, slot["comps"], "VEC%d" % slot["comps"])
        si = len(samplers)
        samplers.append({"input": times_ac, "output": values_ac,
                         "interpolation": "LINEAR"})
        channels.append({"sampler": si, "target": {"node": ni, "path": prop}})
    model["animations"].append({"name": clip_name, "samplers": samplers,
                                "channels": channels})
    print(f"  + clip '{clip_name}' ({len(part_paths)} segment(s), "
          f"{len(channels)} channels, {clip_end:.3f}s)")
    return {ni for (ni, _prop) in merged}


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


def merge_single(model, mbin, clip_name, anim_path, name2node, animated_nodes):
    """Append ONE clip copied straight out of its own .glb (remapped by name).

    This is the original path and is deliberately left alone for single-file
    clips: it copies the source's accessors verbatim instead of re-encoding them,
    so the shipped Pistol_01/Rifle_01 bytes stay exactly what they always were.
    """
    anim, abin = read_glb(anim_path)
    if not anim.get("animations"):
        print(f"  (no animations in {os.path.basename(anim_path)})")
        return
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


def merge(model_path, clips):
    model, mbin = read_glb(model_path)
    model.setdefault("bufferViews", [])
    model.setdefault("accessors", [])
    model.setdefault("animations", [])
    if not model.get("buffers"):
        model["buffers"] = [{"byteLength": len(mbin)}]
    name2node = node_name_index(model)
    animated_nodes = set()

    for clip_name, part_paths in clips:
        if any(a.get("name") == clip_name for a in model["animations"]):
            # Idempotent re-run: the model already carries this clip (Blender
            # exports include their actions in the base GLB, and convert_fbx.ps1
            # can be re-run over an already-merged model). Appending it again
            # would leave two clips with one name and a second copy of the data.
            print(f"  = clip '{clip_name}' already present, skipping")
            continue
        if len(part_paths) == 1:
            merge_single(model, mbin, clip_name, part_paths[0], name2node, animated_nodes)
        else:
            animated_nodes.update(merge_sequence(model, mbin, clip_name, part_paths))

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
            name, paths = arg.split("=", 1)
        else:
            name, paths = os.path.splitext(os.path.basename(arg))[0], arg
        clips.append((name, [p for p in paths.split("+") if p]))
    print(f"Merging into {model_path}")
    merge(model_path, clips)
    return 0


if __name__ == "__main__":
    sys.exit(main())
