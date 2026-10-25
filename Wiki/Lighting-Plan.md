# Lighting Plan — dynamic shadows and "Compiling Lighting"

Status: **design only. Nothing in this document is implemented.** Written after a
full audit of `Source/Renderer/LitLightning.*`, `Source/Renderer/rlights/`,
`GameData/Shaders/Lights/LitFog.fs`, `GameData/Shaders/Surface.fs`, and the client
and editor render paths. This file exists so the next person does not have to
rediscover that `castShadow` has no consumer.

---

## 0. Where the engine actually is

Fixes from the audit have already landed, and are listed here so the plan starts
from the truth rather than from history:

* `castShadow` is stored (`.ozls` stat) and **read by nothing**. There is no shadow
  map, no depth texture, no `rlEnableShader` anywhere in the tree.
* `SURF_SHADOW_HI` / `SURF_SHADOW_LO` / `SURF_DIRTY_SHADOWS` have no GLSL and no
  CPU path. They are now **hidden** from the Surface Properties dialog rather than
  offered as dead checkboxes.
* `is_static`'s comment says "baked into lightmap". No lightmap exists; the field's
  only effect is to skip flicker. Phase B below makes the name true.
* `LightNode::zoneId` is written by `PawnSystem::AssignLightZones()` on every world
  load and every light-property apply, and read by nothing.
* `DrawLightFlares` (`Core.hpp`) is a camera-facing radial sprite at the light's
  own position — a bulb glow. There is no screen-space lens flare: no ghosts, no
  occlusion test, no view-dependent term.
* Light cones now work (`innerCone`/`outerCone` are uploaded), `effect` reaches the
  shader (so torch/fire/lamp/watery finally have their colour tints), `intensity`
  is applied once rather than squared, and the flicker clock rides on the light
  rather than on its slot index.

Still open and **not** in scope for this plan: the `MAX_LIGHTS` mismatch (C++ says
64, both GLSL programs say 32, costing ~1088 guaranteed-miss `glGetUniformLocation`
calls per frame), and the 2x cull radius that can evict the sun from a 32-light
level. Both are cheap and should be done before shadows, because a shadow pass will
make the light budget matter more, not less.

---

## Phase A — Dynamic shadows

### A.1 Why not a shadow map per light

A level has up to 32 lights and almost all of them are point lights. A cube shadow
map per light is 6 faces × 32 lights, which is unaffordable on the target and
pointless for a PS1-styled game where the shadows that read are large and soft.

So: **one shadow-casting sun, and analytic occlusion for everything else.**

### A.2 The two mechanisms

**(1) Directional: one cascade, orthographic, 2048².**

The engine's light budget effectively means one sun per level at most. A single
cascade is therefore not a simplification, it is the whole requirement. Draw the
scene depth from the sun's point of view into a `RenderTexture2D` with a depth
attachment, then sample it in `LitFog.fs` and `Surface.fs`.

Sketch of the data flow:

```
LitShadow_BuildPass(lights, camera)
  -> pick the brightest active DIRECTIONAL light (or none)
  -> fit an ortho frustum around the camera's near region
  -> bind a depth-only shader, render all CSG-visible opaque geometry
  -> upload the light view-projection matrix to both live programs
```

Uniforms both programs gain: `uShadowMap` (sampler2DShadow), `uShadowMat`
(mat4), `uShadowTexel` (float), `uShadowEnabled` (int).

Fragment side, after `NdotL` is computed and before it is accumulated:

```glsl
float shadow = 1.0;
if (uShadowEnabled == 1 && lights[i].castShadow == 1 && NdotL > 0.0) {
    vec4 lp = uShadowMat * vec4(fragWorldPos, 1.0);
    vec3 proj = lp.xyz / lp.w * 0.5 + 0.5;
    // Outside the cascade: assume lit. Clamping to the border is what produces
    // the classic "shadow smear along the edge of the map" band.
    if (any(lessThan(proj.xy, vec2(0.0))) || any(greaterThan(proj.xy, vec2(1.0))))
        shadow = 1.0;
    else {
        float bias = max(0.0025 * (1.0 - NdotL), 0.0006);
        float sum = 0.0;
        // 4-tap rotated Poisson. Period-correct dither is unnecessary; the
        // surface shader already aliases hard.
        for (int t = 0; t < 4; t++)
            sum += texture(uShadowMap, vec3(proj.xy + kShadowTaps[t] * uShadowTexel, proj.z - bias));
        shadow = sum * 0.25;
    }
}
lightAccum += effColor * NdotL * attenuation * spotFactor * shadow;
```

**(2) Point and spot: analytic ray-sphere occlusion, in the fragment shader.**

For a point light, the shadow test is "is this fragment inside the light's sphere
of influence, from the light's point of view, given occluders". The cheap and
period-appropriate version is a **shadow sphere test against the CSG volumes**: the
collision world is already AABB-only (`CsgProcessor`), so for each light, each
volume is tested analytically and the union is uploaded as a small list of spheres
or boxes.

```glsl
// uOccCount == 0 in the common case, so this is one branch per light.
float occlude = 0.0;
for (int o = 0; o < uOccCount; o++)
    occlude = max(occlude, sphereOcclusion(fragWorldPos, lights[i].position, uOccRadius[o]));
lightAccum *= (1.0 - occlude * 0.75f);
```

Budget it: 8 occluders per light, 4 shadow-casting point lights per frame, chosen
by the same distance sort. Everything else gets no shadow. That is a deliberate
"only the lights that matter cast" rule, and it should be visible in the editor (see
A.5).

### A.3 What becomes live, and what gets deleted

| Field | Today | After |
|---|---|---|
| `LightNode::castShadow` | written, never read | read by the fragment path above; `.ozls` stat becomes real |
| `rlights::Light` | no shadow field | gains `castShadow` + location |
| `SURF_SHADOW_HI` / `_LO` | hidden from the dialog | **deleted from the header**, or kept and given meaning (see below) |
| `SURF_DIRTY_SHADOWS` | hidden | deleted, or repurposed as "ignore cascades on this surface" |
| `kLightStats` "Shadow" group | UI for a nonexistent feature | real; `is_static` moves to the lighting group |

`SURF_SHADOW_HI` / `SURF_SHADOW_LO` are the one genuinely ambiguous case. In UT99
they select shadow resolution for a surface. With one cascade there is nothing to
select. **Recommendation: delete both bits and leave a hole in the bit allocation**
rather than invent a meaning — the bits are already absent from every shipped
`.ozone`, so deletion is free, and a hole documents that the space was considered.
`SURF_DIRTY_SHADOWS` maps cleanly onto "this surface ignores the sun's shadow", which
is genuinely useful for painted backdrops, so keep that one.

### A.4 Order of work

1. `uShadowEnabled = 0` everywhere, plus a `LitShadow_BuildPass` that early-returns.
   This lands the plumbing with zero visual change and proves the frame cost is nil.
2. The depth pass for the sun, no sampling yet. Verify with a debug view.
3. Sampling in `LitFog.fs` only. `Surface.fs` gets it in the same commit or the two
   programs disagree in the same frame — which is exactly the class of bug that
   produced the ambient leak documented in AGENTS.md.
4. Per-surface opt-out (`SURF_DIRTY_SHADOWS`).
5. Analytic occluders for point lights.
6. Editor visualisation.

**Rule for the whole phase: `LitFog.fs` and `Surface.fs` must gain shadow code in
the same commit.** They already duplicate five things they should not (see
`Wiki/Lighting-Plan` follow-ups in AGENTS.md), and a shadow pass that lands in one
and not the other produces exactly one class of bug: a surface-flagged brush that
does not receive the shadow of the room it stands in.

### A.5 Editor presentation

The editor must show what the player gets, or shadows become another thing the
author has to guess about. Concretely:

* A shadow debug view: `OTEditor.ViewMode` gains a `SHADOWS` mode that renders
  `uShadowMap`'s depth. This is a new entry in the existing Lit/Unlit/Wire cycle
  (`Main.cpp:1585` cycles `li < 3`, which is why `LightingMode::DYNAMIC` is
  unreachable today).
* Occluder spheres drawn per shadow-casting light in the existing gizmo block
  (`OzPawnSystem.cpp:1105-1183`), using the same radius convention as the shader.
* The point-light influence sphere is currently drawn at `radius * 0.25`
  (`OzPawnSystem.cpp:1134`) while the shader cuts off at `radius * 1.0`. Fix that
  first — it is a one-line change and it is the reason a designer cannot judge a
  point light's reach today.

---

## Phase B — "Compiling Lighting": a static lighting bake

### B.1 Goal

Move every light that can be known at build time out of the runtime 32-light
budget, and out of the fragment shader's inner loop. A level with 40 torches should
cost the same per fragment as one with 4.

### B.2 Vertex lighting first, not lightmaps

The obvious approach is lightmap atlases. It is the wrong first step here:

* the engine has **no UV2 and no unwrapper** — `OzoneParser` generates per-primitive
  UVs, and the six face sub-meshes in `BuildFaceMeshes` *alias one texcoord array*,
  so per-face UV is not even representable today;
* an unwrapper is a project in itself (seams, chart packing, padding, dilation);
* lightmaps need a second texture bind and their own resolution policy.

**Recommendation: bake to per-vertex colour first.** It needs no unwrapper, it rides
in the mesh's existing `colors` stream, and it is a strict subset of the atlas
solution — the bake math is identical, only the output differs.

`Mesh.colors` is already a raylib stream that `BuildFaceMeshes` carries through, and
`LitFog.fs` already samples `texture0` and multiplies by `colDiffuse`; adding
`vertexColor` to the lit path is a one-line change per shader.

```glsl
uniform int uBakedLighting;   // 0 = dynamic only, 1 = vertex colour holds baked light
...
vec3 baked = (uBakedLighting == 1) ? fragColor.rgb : vec3(0.0);
litColor = baseColor.rgb * ((colDiffuse.rgb + specAccum) * lightAccum + baked);
```

Note the subtlety: baked colour is **added**, not multiplied, because it already
contains the light's colour. Getting this backwards is silent — everything just
looks slightly wrong.

### B.3 The tool

`tools/lightbake.cpp`, a `make lightbake` target, plus an AngelEd menu item
(`Tools > Bake Lighting`) so a designer does not have to leave the editor.

```
lightbake <world-dir>
  load the world exactly as the client does (same parser, same CSG rebuild)
  collect static lights
  for each renderable:
    for each UNIQUE vertex index:
      accumulate( position, normal, every static light )
    write colours back
  emit <world-dir>/World.bake
```

### B.4 Non-negotiables

These are the things that make a bake trustworthy or worthless.

1. **Share the attenuation and NdotL maths with the runtime.** Extract one function
   used by both `lightbake` and the GLSL, or baked and dynamic lights will disagree
   at every transition — which looks like "the bake is wrong" every time. This is
   the single most important rule in this document.
2. **Bake per unique vertex index, not per triangle corner.** Primitive meshes share
   indices; baking per corner produces seams along every shared edge.
3. **Remove baked lights from the runtime set.** This is the entire payoff and it is
   what finally makes `is_static` mean what its comment already claims. A light
   marked static must not be submitted to the shader at all when a valid bake exists.
4. **Deterministic output.** Same input → byte-identical `World.bake`, so the file
   diffs cleanly in git and a re-bake with no changes shows up as no change.
5. **Stale detection.** `World.bake` records a hash of (light block, world geometry
   version). A mismatch is a warning at load, not a silent stale bake. This is the
   failure mode that makes people stop trusting bakes.
6. **Fallback.** No `World.bake`, or a stale one → everything dynamic, exactly as
   today. Never a partial bake.

### B.5 What needs baking

* Generated brush geometry (`box`/`cyl`/`sph`/`pyr`/`pln`) — these are the CSG
  surfaces and they are cheap: a box has 8 vertices regardless of its world size.
* `Mesh.Static` props.
* **Not** `Mesh.Skeletal` or anything with vertex-keyframe animation
  (`ozanim`, `AnimatedMesh`): those upload positions per frame, so a baked colour
  would ride along but a baked *position-derived* term would not. Baked lighting
  only if the animation is a rigid transform.

### B.6 On "CSG surfaces"

The collision world is AABB-only and irrelevant to baking — CSG decides *whether* a
brush is solid, not how it is shaded. What needs baking is the **render mesh**, and
the phrase should be read that way. Worth writing down, because the two systems
share a name and nothing else.

### B.7 Storage

Binary sidecar `World.bake`, one per world, next to `World.ozone`. Not embedded in
the OZONE text: it is binary, it is regenerable, and it should be gitignored or
committed by policy but never hand-edited.

Header: magic + version + light-count + geometry hash. Then per renderable: vertex
count + RGB float triples. If vertex interpolation is wanted later (which vertex
colours cannot do well on a low-poly box), that is the migration point to an atlas
and the reason to keep the bake in its own file.

### B.8 Order relative to Phase A

**B first.** Baked lighting and shadows compose badly at first: a baked vertex
colour has no shadow term, so a shadow map landing on top of a bake has to be
applied to the baked term as well, which means the fragment path needs a branch for
"this geometry is baked" that shadows otherwise skip. Doing B first means Phase A's
fragment work only ever runs on the dynamic subset, which is small.

---

## Follow-ups this plan does not cover

### A.1 Measured light capacity (benchmark, not theory)

Measured on the real client with synthetic worlds (`tools`-style generator, closed
box, lights on a lattice so the distance sort has no favourites). Remove the probe
before shipping anything based on this table.

| world point lights | reported `submitted` (pre-fix) | GLSL slots | reached the shader |
|---|---|---|---|
| 8 | 8 | 32 | 8 |
| 24 | 24 | 32 | 24 |
| 32 | 32 | 32 | 32 |
| 48 | 48 | 32 | **32** |
| 64 | 64 | 32 | **32** |

**This is now fixed.** `MAX_LIGHTS` is 32 in `rlights.h` and 32 in both GLSL
programs, and `tests/Surface.test.cpp` parses all three files and fails if they
disagree. Before the fix, C++ said 64 and the shaders said 32.

**Frame time is not the constraint.** At 1280x720 *and* 2560x1440, wall-clock frame
time was pinned at exactly 16.67 ms (60 Hz vsync) for every count from 8 to 64. The
per-fragment light loop is not what limits this engine today. Do not spend the
budget on culling before it is spent on correctness.

**The sun used to be silently deleted. This was proven and is now fixed.** A world
with 40 point lights plus one directional sun rendered **byte-identically** to the
same world with the sun line deleted (identical SHA-256). Three bugs compounded:

1. `MAX_LIGHTS` was 64 in C++ and 32 in GLSL, so the submission loop wrote 32 slots
   the driver does not have. *(fixed — one value, plus a test)*
2. `LitLightning_SortByDistance` ordered purely by `(active, not-culled, distance)`,
   and a directional light never gets a `radius`, so it defaulted to 50, sorted as
   *culled*, and landed last. *(fixed — see below)*
3. A directional light's contribution is `NdotL` only — no attenuation, no radius —
   so it is the cheapest light in the scene and the first one that can be traded
   away for point lights without anyone noticing.

The re-verification after the fix: the 40-point-plus-sun capture now differs from
the 40-point-only capture, while the 40-point-only capture is **unchanged** from
before the fix — so the fix restored the sun and changed nothing else.

**The ordering fix.** `LightSubmitTier()` ranks a light by how much it should want a
slot, and the sort uses that instead of raw distance:

| tier | kind | why |
|---|---|---|
| 0 | `DIRECTIONAL` | global, no falloff, never distance-culled — distance is meaningless for it |
| 1 | point/spot within its radius | the camera is inside its influence |
| 2 | point/spot beyond its radius | the shader already returns 0.0, so this is the correct light to lose |

Tier 2 also removed the old `radius * 2` slack: a light between 1x and 2x radius used
to occupy a precious slot while already being invisible to every fragment.

`LitLightning_Update` now **returns** the submitted count, because "is this scene
lit" cannot be answered by `lights.size()` — the camera fill light was keyed off
"does any LightNode have `active` set", which stays true even when every one of them
was culled or evicted. That suppressed the fill light in exactly the case it exists
to prevent, leaving a scene lit only by ambient (0.1/10 == 0.01, near-black).

**Recommended hard limits until a shadow pass exists:**

| kind | per-region limit | hard total | notes |
|---|---|---|---|
| directional | 1 | 1 | one sun; never let a second exist |
| spot | 8 | 12 | each costs a `smoothstep` + `normalize` |
| point | 24 | 32 | at the GPU limit; tier 2 lights are dropped first, not the sun |
| transient (muzzle flash etc.) | 8 | 8 | submitted **first**, so they evict world lights |
| **effective world budget** | | **24** | 32 slots minus 8 transients, whenever a transient is live |

**24 world lights is still the number to design to**, because `CombatFX` transients
cannot be evicted: a level authored to 32 loses 8 of its own lights the instant a
weapon fires, and *which* 8 changes as transients expire. Dust_Ravine already ships
32 and is therefore still over budget on any frame with a live transient.

### A.2 Other findings this plan does not cover

Recorded because they were found in the same audit and would otherwise be lost:

1. **FIXED.** `LitFog.fs` ran a *second* full 32-iteration light loop per fragment
   purely to tint fog, and `Surface.fs` had no equivalent, so a fogged surface brush
   disagreed with a fogged wall next to it. The in-scatter is now accumulated inside
   the existing shading loop (`scatterAccum`) in **both** programs, with identical
   body and constants, and `mix(lit, fogColor + scatterAccum, fogFactor)` is the same
   expression in each.
2. **FIXED.** `DetailTexture` in `LitFog.fs` had no CPU uploader, so it sampled unit 0 -
   the diffuse map - and multiplied albedo by the diffuse texture resampled at
   `fragWorldPos.xz * 16`, on every fragment of every mesh, particle, pickup,
   projectile and the view-model. It is now gated on `uHasDetail` (default 0); nothing
   uploads a detail map, so the block is skipped. Bind a map **and** set the flag
   together or detail silently stays off.
3. **FIXED.** `Core.hpp` put `DrawLights()` in `if (Debug)` and the whole
   `DrawAll`/`DrawEntities`/`DrawLightFlares`/view-model chain in the `else`, so
   `debug=1` removed NPCs, props, particles, wind, flares, the view-model and all
   simulation - and disabled the *good* light gizmos, which live in `DrawEntities`.
   The debug overlay is now additive: the game block always runs.
4. `restore_ambient` sets `{1,1,1,1}` under a comment claiming it matches the
   default, which is `{0.1,0.1,0.1,1}` - a script leaves the world 10x brighter.
5. `GameData/Pawn/Lights/Light.Point.ozls` and `Light.Spot.ozls` are cited by name
   in `Placement.cpp` and `OzOzoneLoader.cpp` and **do not exist**; only `Light.ozls`
   ships. `Dust_Ravine` has `name=Light.Point` on two lights, which resolves to
   nothing, silently. **No spot light exists anywhere in the shipped worlds**, so the
   cone fix could not be validated against shipped content.
6. `LightNode::zoneId` is written by `AssignLightZones()` on every world load and every
   light-property apply, and read by nothing.
7. **Fog was effectively off in every shipped world.** `fog_density` is authored as
   0.0012..0.003 in all five skyzones, and the shader multiplied a *linear* ramp by it
   - about 0.2% haze at 100 units. It is now an exponential rate
   (`1 - exp(-d * density)`) in both live shaders, the densities were re-authored to
   real values, and `fogStart`/`fogEnd` both stay live (`fogEnd` is a ceiling on the
   exponential, not a multiplier - multiplying halved the haze over exactly the
   mid-field range that matters).
8. **`set_fog` / `restore_fog` never published to `OzoneLoader`**, so
   `SurfaceMaterial` - and therefore every painted backdrop and every
   surface-flagged brush - kept the *startup* fog while the lit geometry around it
   used the level's. Measured: `LitFog` carried density 0.0022 while
   `SurfaceMaterial` still carried 1.0. Both opcodes now publish, and
   `restore_fog` restores to a recorded `g_levelFog` instead of the dead
   post-process `FogTint` state it used to read.
9. **The skybox is not fogged**, so there is a hard horizontal seam where fogged
   world geometry meets sky. This is the remaining reason distant geometry reads as
   a flat band rather than atmosphere. Fixing it means blending the skybox faces
   toward `fogColor` near the horizon.
10. The shipped backdrop *textures* are tiling stone walls
    (`bb_wall_13.dds` at `texScaleU=3 texScaleV=2`), not painted panoramas. No amount
    of fog makes a tiled wall read as distant scenery - the art has to change.