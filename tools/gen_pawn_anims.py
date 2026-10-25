#!/usr/bin/env python3
"""Generate vertex-keyframe (.ozanim) clips for the shipped UNRIGGED pawn models.

The four shipped enemies fall into two camps:

  * Brute   -> Watcher_of_the_Hollow_Eye.glb is rigged and carries one clip
               ("all"), so it uses oz::SkeletalMesh directly.
  * Walker / Skaarj / Floater -> the GLBs have no skin and no clip at all. The
               engine's only animation path for them is `animfile=` + .ozanim
               (oz::AnimatedMesh), and nothing had ever been authored, so they
               were limited to a 6cm walk bob.

This script derives a small, deliberately conservative clip set from each model's
own geometry (no randomness, no hand animation) and writes one .ozanim per def:

    Idle   - slow height-weighted breathing sway
    Walk   - alternating low-half "leg" swing + body counter-sway + bob
    Run    - the walk cycle, quicker and wider
    Death  - non-looping collapse toward the ground
    Fly    - wing flap (models whose nodes are named Wing*) and body bob

IMPORTANT - vertex ordering and space match raylib EXACTLY:
  * raylib walks the glTF *node array* in file order and loads each node's mesh
    primitives (rmodels.c), so mesh[] order is NOT the vertex order. A generator
    that walked `meshes[]` put every offset on the wrong vertex for two of these
    three models (spider counts are [337,1261], wasp [918,864,34,34]).
  * raylib bakes the node transform chain into mesh.vertices (these files use a
    scale of 100 plus a -90deg X rotation), so the offsets must be in that same
    transformed space - which is why the matrix composition below exists.

Run from the repo root:  python tools/gen_pawn_anims.py
"""

import json
import math
import os
import struct
import sys

# (model, output, profile) - profile picks the clip set/params below.
MODELS = [
    ("GameData/Pawn/Pawns/Huntsman_spider.glb", "GameData/Pawn/Pawns/Anims/Walker.ozanim", "walker"),
    ("GameData/Pawn/Pawns/Predator.gltf", "GameData/Pawn/Pawns/Anims/Skaarj.ozanim", "biped"),
    ("GameData/Pawn/Pawns/XenoWasp.glb", "GameData/Pawn/Pawns/Anims/Floater.ozanim", "flyer"),
]

COMP = {5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2), 5123: ("H", 2), 5125: ("I", 4), 5126: ("f", 4)}
NUM = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


# ---------------------------------------------------------------------------
# glTF/GLB reading
# ---------------------------------------------------------------------------
def load(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] == b"glTF":
        jlen, _ = struct.unpack_from("<II", data, 12)
        doc = json.loads(data[20:20 + jlen])
        off = 20 + jlen
        blen, _ = struct.unpack_from("<II", data, off)
        return doc, data[off + 8: off + 8 + blen]
    # .gltf: external buffer
    base = os.path.dirname(path)
    with open(os.path.join(base, doc_buffer_uri(path)), "rb") as f:
        return json.loads(data), f.read()


def doc_buffer_uri(path):
    with open(path, "rb") as f:
        doc = json.loads(f.read())
    return doc["buffers"][0]["uri"]


def read_vec3(doc, blob, acc_idx):
    a = doc["accessors"][acc_idx]
    bv = doc["bufferViews"][a["bufferView"]]
    fmt, size = COMP[a["componentType"]]
    stride = bv.get("byteStride") or size * 3
    base = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
    return [struct.unpack_from("<" + fmt * 3, blob, base + i * stride) for i in range(a["count"])]


# ---------------------------------------------------------------------------
# Node transforms (column-major, glTF convention: parent * translate * rot * scale)
# ---------------------------------------------------------------------------
def quat_matrix(q):
    x, y, z, w = q
    return [
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 0.0],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), 0.0],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), 0.0],
        [0.0, 0.0, 0.0, 1.0],
    ]


def mat_mul(a, b):
    return [[sum(a[r][k] * b[k][c] for k in range(4)) for c in range(4)] for r in range(4)]


def node_local(n):
    if "matrix" in n:
        m = n["matrix"]  # column-major -> rows
        return [[m[c * 4 + r] for c in range(4)] for r in range(4)]
    t = n.get("translation", [0, 0, 0])
    r = quat_matrix(n.get("rotation", [0, 0, 0, 1]))
    s = n.get("scale", [1, 1, 1])
    tr = [[1, 0, 0, t[0]], [0, 1, 0, t[1]], [0, 0, 1, t[2]], [0, 0, 0, 1]]
    sc = [[s[0], 0, 0, 0], [0, s[1], 0, 0], [0, 0, s[2], 0], [0, 0, 0, 1]]
    return mat_mul(mat_mul(tr, r), sc)


def apply(m, v):
    x, y, z = v
    return (
        m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3],
        m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3],
        m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3],
    )


def world_vertices(path):
    """[(x,y,z,'group')] in raylib's load order (flat node array, node.mesh)."""
    doc, blob = load(path)
    nodes = doc.get("nodes", [])
    parent = {}
    for i, n in enumerate(nodes):
        for c in n.get("children", []):
            parent[c] = i

    def world(i):
        m = node_local(nodes[i])
        p = parent.get(i)
        return mat_mul(world(p), m) if p is not None else m

    verts = []
    for i, n in enumerate(nodes):
        if n.get("mesh") is None:
            continue
        m = world(i)
        group = n.get("name", "")
        for prim in doc["meshes"][n["mesh"]].get("primitives", []):
            attr = prim["attributes"].get("POSITION")
            if attr is None:
                continue
            for v in read_vec3(doc, blob, attr):
                verts.append((*apply(m, v), group))
    return verts


# ---------------------------------------------------------------------------
# Clip fields. `v` is (x,y,z,group); `c` is the model's bounds/centre context.
# ---------------------------------------------------------------------------
class Ctx:
    def __init__(self, verts):
        xs = [v[0] for v in verts]
        ys = [v[1] for v in verts]
        zs = [v[2] for v in verts]
        self.miny, self.maxy = min(ys), max(ys)
        self.h = max(self.maxy - self.miny, 1e-6)
        self.cx = (min(xs) + max(xs)) * 0.5
        self.cz = (min(zs) + max(zs)) * 0.5
        self.ex = max((max(xs) - min(xs)) * 0.5, 1e-6)

    def hnorm(self, v):
        return min(1.0, max(0.0, (v[1] - self.miny) / self.h))

    def side(self, v):
        return 1.0 if v[0] >= self.cx else -1.0


def field_idle(c, v, u):
    h = c.hnorm(v)
    w = 2.0 * math.pi * u
    dx = 0.010 * c.h * h * math.sin(w)
    dy = 0.012 * c.h * h * math.sin(w + 1.2)
    dz = 0.006 * c.h * h * math.cos(w)
    return dx, dy, dz


def field_walk(c, v, u, amp=1.0):
    h = c.hnorm(v)
    w = 2.0 * math.pi * u
    side = c.side(v)
    legw = max(0.0, 1.0 - h / 0.55)          # lower half of the body = "legs"
    swing = math.sin(w + (0.0 if side > 0 else math.pi))
    dx = amp * (0.015 * c.h * legw * side * math.cos(w) + 0.010 * c.h * h * math.sin(w))
    dy = amp * (0.030 * c.h * legw * max(0.0, math.sin(2.0 * w + (0.0 if side > 0 else math.pi)))
                + 0.015 * c.h * h * math.sin(2.0 * w))
    dz = amp * 0.12 * c.h * legw * swing
    return dx, dy, dz


def field_death(c, v, u):
    e = u ** 1.6
    h = c.hnorm(v)
    drop = v[1] - c.miny
    dx = 0.10 * c.h * c.side(v) * max(0.0, 1.0 - h / 0.55) * e
    dy = -drop * 0.85 * e - 0.02 * c.h * e
    dz = drop * 0.55 * e
    return dx, dy, dz


def field_fly(c, v, u, cycles=2.0, amp=1.0):
    h = c.hnorm(v)
    w = 2.0 * math.pi * u
    flap = math.sin(w * cycles)
    wing = 1.0 if v[3].lower().startswith("wing") else 0.0
    if wing:
        return (amp * 0.06 * c.h * c.side(v) * flap,
                amp * 0.35 * c.h * flap,
                amp * 0.05 * c.h * flap)
    return (0.0, amp * 0.030 * c.h * math.sin(w) * (0.25 + 0.75 * h), 0.0)


# ---------------------------------------------------------------------------
# Clip definitions: (name, duration, loop, segments, field)
# ---------------------------------------------------------------------------
def clips_for(profile):
    if profile == "flyer":
        return [
            ("Idle", 0.60, 1, 6, lambda c, v, u: field_fly(c, v, u, 2.0, 1.0)),
            ("Fly", 0.45, 1, 6, lambda c, v, u: field_fly(c, v, u, 3.0, 1.15)),
            ("Death", 1.10, 0, 5, field_death),
        ]
    if profile == "walker":
        # A spider scuttles: quicker, smaller steps.
        return [
            ("Idle", 2.40, 1, 6, field_idle),
            ("Walk", 0.70, 1, 6, lambda c, v, u: field_walk(c, v, u, 0.8)),
            ("Run", 0.45, 1, 6, lambda c, v, u: field_walk(c, v, u, 1.4)),
            ("Death", 1.10, 0, 5, field_death),
        ]
    return [
        ("Idle", 2.40, 1, 6, field_idle),
        ("Walk", 0.90, 1, 6, lambda c, v, u: field_walk(c, v, u, 1.0)),
        ("Run", 0.55, 1, 6, lambda c, v, u: field_walk(c, v, u, 1.8)),
        ("Death", 1.10, 0, 5, field_death),
    ]


def fmt(x):
    s = f"{x:.6g}"
    return "0" if s in ("-0", "0", "") else s


def generate(model, out, profile):
    verts = world_vertices(model)
    c = Ctx(verts)
    lines = ["ozanim 1",
             f"# Auto-generated by tools/gen_pawn_anims.py from {os.path.basename(model)}",
             f"# {len(verts)} vertices, model height {c.h:.3f} world units. Do not hand-edit;",
             "# re-run the generator after changing the model."]
    total_keys = 0
    for name, dur, loop, segs, field in clips_for(profile):
        lines.append(f'clip "{name}" fps 30 loop {loop}')
        # Include the closing key (identical to key 0 for looping clips) so the
        # fmod wrap in Clip::SampleOffsets interpolates instead of snapping.
        for k in range(segs + 1):
            u = k / float(segs)
            t = dur * u
            lines.append(f"key {t:.4f}")
            total_keys += 1
            for idx, v in enumerate(verts):
                dx, dy, dz = field(c, v, u)
                if abs(dx) < 1e-5 and abs(dy) < 1e-5 and abs(dz) < 1e-5:
                    continue
                lines.append(f"  v {idx} {fmt(dx)} {fmt(dy)} {fmt(dz)}")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    text = "\n".join(lines) + "\n"
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print(f"{out}: {len(verts)} verts, {total_keys} keys, {len(text) / 1024.0:.0f} KiB")


def main():
    for model, out, profile in MODELS:
        generate(model, out, profile)
    return 0


if __name__ == "__main__":
    sys.exit(main())
