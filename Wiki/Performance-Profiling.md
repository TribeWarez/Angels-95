# Performance profiling and benchmarking

This page documents the diagnostics added after `Sektor_01_SilentPines` was
reported at ~2 FPS. Everything here is header-only under `Source/Diagnostics/`,
so it ships in the client and the editor without touching either object list.

## What runs all the time

| Piece | File | Notes |
|---|---|---|
| Frame profiler | `Source/Diagnostics/FrameProfiler.hpp` | scopes + counters + 240-frame ring + p50/p95/p99 + stutter capture |
| Overlay | `Source/Diagnostics/ProfilerOverlay.hpp` | compact and expanded, 2D only |
| Ablation switches | `Source/Diagnostics/PerfToggles.hpp` | non-persisted, diagnostic only |
| Benchmark session | `Source/Diagnostics/BenchMode.hpp` | `--bench` state, shared by both targets |

Collection is **on by default**. Disabled scope/counter calls cost one bool test;
the enabled path is one `steady_clock` read and an array index per scope.

The overlay is visible by default and is toggled from:

- **Client:** `Perf > Toggle Perf Overlay` in the native menu bar, or `F9`.
  `Shift+F9` switches compact/expanded. `/perf` is the platform-independent
  console entry point. Visibility is persisted in `[AngelTech.Debug] perf_overlay`.
- **Editor:** the `Perf:On/Off` chip at the end of toolbar row 2, plus
  `PerfX:On/Off` for the expanded view. `Panes:Fast/Full` controls whether
  secondary viewports draw props/NPCs or level geometry only. Persisted in
  `[Options]` in `System/AngelEd.ini`.

Ablation switches are **never** persisted: `--bench-disable` and `/perf isolate`
exist to bisect a frame, not to become a player setting.

## Counters worth reading first

| Counter | What it means |
|---|---|
| `brush.total` | renderables that survived the frustum/occluder cull |
| `brush.fast` | brushes on the one-`DrawModelEx` path |
| `brush.perface` | brushes drawn as up-to-six face sub-meshes |
| `draw.calls` / `draw.faces` | mesh submissions |
| `tex.stat` | filesystem `IsPathFile` calls (should be ~0 in steady state) |
| `uniform.upload` | `SetShaderValue` calls (each one calls `glUseProgram`) |
| `light.submitted` | light records uploaded across every program |
| `mesh.bind` | lit mesh draws that went through `Mesh::BindMaterial` |
| `particle.drawn` / `particle.live` | particle draw/list sizes |
| `props.culled` / `props.drawn` | per-pane `Mesh.*` culling |
| `sim.steps` | fixed 60 Hz simulation steps this frame |

## Client

```bash
Angels95.exe --world Sektor_01_SilentPines --bench 300 \
  --bench-warmup 60 --bench-csv build/perf/silentpines.csv \
  --bench-cam "-80,0,18,90,0" --bench-phase all
```

| Flag | Meaning |
|---|---|
| `--bench [frames]` / `--bench-frames N` | measured frames (default 600) |
| `--bench-warmup N` | discarded frames; history resets at the boundary (default 60) |
| `--bench-csv PATH` | per-frame CSV (default `System/perf_frames.csv`) |
| `--bench-cam "x,y,z,yaw[,pitch]"` | fixed camera, OZONE Z-up, same parser as `--shot-cam` |
| `--bench-phase all\|render` | `render` freezes simulation/particles/scripts |
| `--bench-res WxH` | default is pinned to 1280x720 so runs are comparable |
| `--bench-disable a,b,c` | ablation: `lights,particles,props,pawns,fx,skinning,sky,portals,glow,postfx,overlays,grid,hud` |

Bench mode skips the splash and menu, disables audio, vsync, MSAA, post-FX
toggles that vary, and the overlay, then exits after writing the CSV and logging
a summary. `--bench` and `--shot` are mutually exclusive; bench wins with a
warning.

### Reading the result

The CSV starts with `#` metadata (build, world, frames, resolution, camera) and
one row per measured frame:

```
frame,ms,fps,scope.frame.world,scope.world.geometry,...,counter.draw.calls,...
```

Tools:

```powershell
.\tools\perf_report.ps1 -Csv build\perf\silentpines.csv            # p50/p95/p99 + top scopes/counters
.\tools\perf_compare.ps1 -Baseline before.csv -Candidate after.csv  # exits 1 on >10% p95 regression
.\tools\perf_matrix.ps1 -Frames 180 -Phase render                   # every world
```

The summary in `System/angels95.log` prints p50/p95/p99 and the top scopes by
peak. `perf_report` is the friendlier view; `perf_compare` is the regression gate.

## Editor

```bash
AngelEd.exe GameData/Worlds/Sektor_01_SilentPines/World.ozone \
  --bench 300 --bench-views 3 --bench-csv build/perf/editor_4view.csv
```

| Flag | Meaning |
|---|---|
| `--bench [frames]` / `--bench-frames N` | measured frames |
| `--bench-warmup N` | discarded frames |
| `--bench-csv PATH` | default `System/perf_frames.csv` |
| `--bench-views 0..3` | 1x1, 1x2, 2x1, 2x2; applied after the INI layout load |
| `--bench-no-overlays` | hide the profiler overlay |

The editor benchmark never writes `System/AngelEd.ini` (`SavePanelLayouts`
returns early while a benchmark is active) and disables detached panes.

## What the measurements already found

`Sektor_01_SilentPines`, 1280x720, vsync off, dev machine:

| | Before | After |
|---|---|---|
| client p50 / p95 | 22.6 / 28.3 ms (1280x700) | ~7.1 / ~9.5 ms (1280x700, correct frustum) |
| client Period profile p50 / p95 | - | ~4.9-7 / ~7-9 ms |
| editor 1 view p50 | (vsync-capped) | ~10.7 ms |
| editor 2x2 p50 / p95 | ~40.6 / ~70.7 ms | ~10.4 / ~12.3 ms (Panes:Fast) |
| visible brushes | all 267 (no cull) | 45 |
| `brush.perface` | 47 | 2 |
| `draw.calls` | 270 | 55 |
| `uniform.upload` | 6589 | ~828 |
| `tex.stat` | 270 | 0 |

Numbers vary by a few tens of percent between runs (background load, driver state);
compare the same camera and resolution, never two different ones.
The `--bench` harness pins 1280x720 unless `--bench-res` is given.

### Culling correctness (follow-up)

After the first culling pass, `Sektor_01_SilentPines` still lost visible geometry. The
cause was in `BuildFrustum`, not the AABBs: every side plane was built from only the
two **diagonal** corners of the near/far face, which are collinear with its centre.
Near/far normals came out zero and left/right/top/bottom were duplicated, so one
bogus plane rejected whole visible walls. The fix uses all four corners of each
face (eight points total); `tests/OzPawnSystem.test.cpp` now pins the shape.

Culling was also made less aggressive:

- Brush AABBs carry a margin of 1 unit + 2% of the largest extent.
- Prop bounds use the horizontal DIAGONAL (`sqrt(x²+z²)`) because yaw is ignored;
  per-axis bounds are not a yaw upper bound for an off-centre model.
- AngelEd passes the active PANE's aspect to `BuildFrustum`; using the window
  aspect made the 2×1 layout's frustum too narrow.
- The gizmo sync calls `RefreshRenderableBounds` after writing a transform, so a
  dragged brush is not culled against its old box.
- `/perf isolate cull off` (or client `--no-cull`) disables frustum tests for
  A/B diagnosis.

The two structural performance causes:

1. **`OzoneRenderable::hasBounds` was never assigned**, so every
   `if (r.hasBounds && !AabbInFrustum(...))` gate was dead. Frustum culling
   never culled anything in the client or the editor. `UpdateRenderableBounds`
   now runs on load, `AddBrushRenderable` and `UpdateBrushRenderable`.
2. **Brush-wide `texPath`/`texSlot`/UV forced the per-face path.**
   `NeedsPerFaceDraw()` treated any non-default texture as a shader-visible
   property, even though those values are already in the model material or
   baked into the mesh texcoords. Whole worlds paid 6 draws + ~12 uniform
   uploads per brush for no visual difference.

Secondary wins: light uniform locations are cached per program (and the
`uLightCount` loop bound removed the disabled tail entirely), mesh material gate
uniforms only upload on change, `ResolveWorldAssetPath` caches resolutions and
checks the `GameData/` prefix before `stat`, face-texture handles are cached per
renderable per face, the portal/glow/backdrop scans share one invalidated cache,
`DrawZoneGeometry` uses the cached world AABB instead of `GetMeshBoundingBox` per
brush per frame, and `Mesh.*` props are frustum-culled per pane.

## Period render profile

`Display > Rendering > Effects > Period Render Profile` (or `--period` on the
client CLI) is a low-end profile, not a post-FX setting:

- HDR bloom/grade is bypassed (`Resolve` is not called) and the scene target is
  created as **RGBA8** instead of RGBA16F.
- Both shaders loop to `uLightCount` (the number actually submitted) instead of
  the full 64-slot array.
- At most 800 particles are drawn (simulation and combat bursts are unchanged).
- The profile does not overwrite the player's Post FX settings; turning it off
  restores them.

Note that raylib 5.5 still requires OpenGL 3.3, so this is a **budget** profile
for modern low-end hardware; it is not a GL 1.x/Voodoo2 backend.

## Static budget audit

`worldcheck <World.ozone> --perf` prints the draw-call estimate, per-face brush
count, light budget and emitter demand from the OZONE text alone. It is warn-only
and runs with the other per-world checks in `make test`:

```
  brushes: 266 (fast 233, per-face 33, textured 266, flagged 33, material 0)
  estimated draw calls: ~431 (budget 500)
  lights: 19 (budget 24)
  particle demand: 291 steady (budget 20000)
```

Collision proxies are excluded (they are never drawn), so `TestMap` no longer
looks like a 1,375-draw world.

## Adding a scope

```cpp
#include "Diagnostics/FrameProfiler.hpp"

void SomeSystem::Draw() {
    OZ_PROFILE_SCOPE("some.system");
    ...
    OZ_PROFILE_COUNT("some.items");
    OZ_PROFILE_ADD("some.bytes", n);
}
```

The scope id is a function-local static, so the string is interned once. Keep
names in the `area.thing` form used by the catalogue (`world.*`, `ozone.*`,
`lighting.*`, `mesh.*`, `particles.*`, `ed.*`, `frame.*`) or the overlay's top
list becomes hard to read.

## Invariants

- `hasBounds` is load-bearing. Any new writer of `OzoneRenderable` must call
  `UpdateRenderableBounds`; forgetting it does not produce an error, it just
  silently puts everything back in the draw loop.
- `NeedsPerFaceDraw()` is about shader-visible properties and face overrides,
  not about whether a texture was authored. Texture/UV fields belong in the
  material or the mesh, not in that predicate.
- `Matrix`/`AABB` culling for a rotated brush must stay rotation-aware
  (`RotatedAabbY`), or a beam's ends pop out of its own cull box.
- A `SetShaderValue` is a `glUseProgram`; do not add per-draw uniform work
  without measuring `uniform.upload`.
- `ResolveWorldAssetPath`'s cache is world-scoped and cleared by
  `OzoneLoader::Unload`.
- `OzoneRenderable::faceTexHandle[6]` / `faceTexCached[6]` are invalidated by
  every surface writer: `SetRenderableFace`, both resets,
  `ApplyRenderableTexture`, `UpdateBrushRenderable` and world unload. Adding a
  new writer of `surface.def` without invalidating them leaves the old texture
  on that face until the next reload.
- `m_faceFlagCache` (portal/glow/backdrop) is invalidated by the same writers
  plus load/add/remove. A stale cache is invisible until the pass it gates runs:
  a portal added in the editor would not draw until the next surface edit.
- The stats sidebar caches its label text and window rect; `SetWindowTextW` and
  `SetWindowPos` only run when the value changed. Do not reintroduce a
  per-frame unconditional call - it invalidates the child STATICs and makes GDI
  repaint the dock on every frame.
- A new profiler scope/counter is one include and one macro; do not add a `.cpp`
  for it (the root Makefile, `AngelEd/Makefile` and `ci.yml`'s inline g++ list
  must stay in step).
- `test_pawn_system` links `Source/World/OzoneFrustum.cpp` because
  `DrawEntities` now culls props per pane.

## Tests

```bash
make test_profiler      # builds
./test_profiler         # runs (single-target builds do not run)
make test               # all suites, authoritative exit code
```

`tests/Profiler.test.cpp` pins nesting/self time, counters, the history window,
stutter capture, disabled mode and CSV output. `tests/Surface.test.cpp` pins the
fast-path eligibility rule.
