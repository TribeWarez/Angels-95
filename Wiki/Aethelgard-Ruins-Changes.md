# Aethelgard_Ruins — progressive redesign change log

World: `GameData/Worlds/Aethelgard_Ruins/World.ozone` (371 lines)
Derived from: `GameData/Worlds/EngineTest/World.ozone` (329 lines, left **byte-identical**)
Mission wiring: `GameData/Game/Angels95/Missions/Sefirah_3_Oasis333/Aethelgard_Ruins.ozls`
(`world = Aethelgard_Ruins`, `next = AlShaddai_Metropolis`, `objectives = Objective_CollectRelics`)

## What this world is now

The EngineTest tech demo — four ritual chambers, a great hall, a pyramid arena, a 200-unit
causeway and a surface-flag gallery — is now the playable mission world for Oasis 333. The geometry
is unchanged; what changed is identity (header, zone physics, ambient hooks), three genuine
rendering bugs (inert pan/glow), and the arc wiring (portal target, patrol graph, threshold trigger).

Narrative flow, west to east:

```
arrival court (0,8)      safe tier — lit chambers, pickups, both playerstarts
  ritual chambers        engine_water pool + engine_sound chamber
  great hall (-38)       engine_reverb, per-face drifting walls, dais + altar
  courtyard / pyramid    arena, ladder zone, glowing apex orb
  causeway (x 84..292)   Path of Dissolution: water -> acid -> lava, flag gallery
  terminal (x 296)       obelisk + sphinx orb, campaign exit portal
```

## Zone architecture

Names are **unchanged** from EngineTest on purpose: each `name=` is the lookup key for a sibling
`.ozls` def, and `LightningEntityRegistry::LoadWorldOverrides` drops a def owned by another world
(logging only at `OZ_DEBUG`). The defs were therefore copied into this directory — renaming a zone
without re-keying its def silently detaches fog/ambient and the pool damage timers.

| Zone (`name=`) | Type | Bounds (x / y / z) | What it does |
|---|---|---|---|
| `engine_sky` | sky | -90..320 / -90..90 / -30..90 | fog, ambient, skybox (`fly_mult=1.6`) |
| `engine_water` | water | -4.5..4.5 / -14.5..-5.5 / -0.2..3.5 | ritual pool (`water_gravity=6`, `swim_up=4`) |
| `arena_ladder` | ladder | 70.4..71.6 / 20..24 / 0..11 | pyramid climb (`ladder_speed=7`) |
| `engine_reverb` | reverb | 8.5..31.5 / -49.5..-26.5 / 0..8 | hall echo |
| `engine_sound` | sound | 12.5..27.5 / 2.5..17.5 / 0..8 | inert today (see Deferred) |
| `path_water` | water | 100..118 / -13..-7 / -1..2.5 | calm pool |
| `path_acid` | water | 148..166 / -13..-7 / -1..2.5 | 5 hp / 60 ticks |
| `path_lava` | water | 196..214 / -13..-7 / -1..2.5 | 8 hp / 30 ticks |
| `path_surface_demo` | sound | 82..302 / 4..15 / 0..8 | inert today (see Deferred) |

Physics escalation: `water_gravity` 6 → 5 → 4, `water_drag` 0.9 → 0.85 → 0.8, `swim_up` 4 → 3.5 → 3.
Honest caveat: the pools are 0.45 deep, so these numbers are barely perceptible — the real
escalation is the scripted damage in `path_acid.ozls` / `path_lava.ozls`.

## The three rendering fixes

`SURF_PAN_U` / `SURF_PAN_V` / `SURF_GLOW` are **flags**, and `Surface.fs` gates the uniform on them.
The three pools carried `panU=`/`panV=` (and lava carried `surfGlow=`) with `flags=2048`
(= `SURF_UNLIT`) only, so none of it had ever had any effect.

Flag values (from `Source/World/SurfaceFlags.hpp`, mirrored as `SF_*` in `GameData/Shaders/Surface.fs`):

```
SURF_UNLIT = 2048        SURF_GLOW = 16384      SURF_PAN_U = 262144
SURF_PAN_V = 524288      SURF_NO_BSP_CUTS = 268435456
UNLIT|PAN_U|PAN_V      = 788480
UNLIT|PAN_U|PAN_V|GLOW = 804864
GLOW|NO_BSP_CUTS       = 268451840
```

| New line | Was | Now |
|---|---|---|
| 226 water | `flags=2048 panU=0.05 panV=0.03` | `flags=788480 …` (drift actually runs) |
| 232 acid | `flags=2048 panU=-0.04 panV=0.02` | `flags=804864 … surfGlow=(0.05,0.18,0.05)` |
| 238 lava | `flags=2048 panU=0.03 panV=0.05 surfGlow=(0.12,0.04,0.01)` | `flags=804864 … surfGlow=(0.18,0.04,0.01)` |
| 140 apex orb | `flags=16384` | `flags=268451840` (+`SURF_NO_BSP_CUTS`) |
| 243 sphinx orb | `flags=16384` | `flags=268451840` |
| 242 obelisk | no flags | `flags=524288 panV=0.06` (slow vertical drift) |

The two orbs are the only `add sph` solids; without `SURF_NO_BSP_CUTS` each contributed an invisible
standable AABB above the pyramid/obelisk. `NO_BSP_CUTS` skips them in `RebuildCollisionVolumes`
without touching the render mesh or the glow pass.

Gallery boxes (the 12 relief cubes) were **not** touched — they already demonstrate one bit each
(UNLIT / FAKE_LIT / GLOW / PAN_U / PAN_V / TRANSLUCENT / ALPHABLEND / MASKED / NO_BSP_CUTS /
NO_FOG / FAKEBACKDROP).

## Dreamlike layer (OZONE-only, no post-process)

- Great Hall walls (new lines 73–74), **per-face** so nothing else about the brush changes:
  `facepx_flags=262144 facepx_panU=0.08` and `facenx_flags=262144 facenx_panU=-0.08`.
  Opposite signs make the two walls read as one sliding volume rather than two independent drifts.
- Obelisk vertical drift (above).
- `pln` 135 keeps `268435456`; `pln` 193 keeps `268436480` (`NO_BSP_CUTS + TWO_SIDED` — the second
  bit is load-bearing, do not "normalise" it away).
- `WindZone 42 -26 0 76 26 12 1 0 0.3 7 0.5` unchanged; all 9 `ParticleEmitter fire` lines unchanged;
  all 27 lights unchanged (2 directional + 25 point, 18 of them `effect=2` torches). No lights added:
  the shader budget is 32 with 8 reserved for transients.

## Clearance audit (no geometry was raised or moved)

The brief asked for ceilings ≥ 8.25 and clearance ≥ 5.25; the engine's real numbers are a **5.05**
requirement (`2.8` body + `2.25` jump = `9²/(2·18)`) and shipped storeys of **8.0**. The map already
satisfies both, so Phase 3 was an audit rather than surgery.

| Element | Measurement | Requirement | Verdict |
|---|---|---|---|
| Chamber / hall ceilings | box centre 8.25, h 0.5 → underside **8.0** | ≥ 5.05 above floor | pass |
| Room floors | centre 0.25, h 0.4 → top **0.45** (0.25 is the *centre*, not the top) | — | — |
| Usable height under ceiling | 8.0 − 0.45 = **7.55** | 2.5 prop + 5.05 = 7.55 | pass (exactly) |
| Doorway lintels | bottom **5.5** | ≥ 5.5 | pass |
| Hall doorway clearance | 5.5 − 0.45 floor = **5.05** | ≥ 5.05 | pass (marginal, intentional) |
| Pyramid treads | 0.5 → 5.5 in **1.0** steps | 1.0 | pass |
| `sub` / `intersect` openings | **0 lines** | none (piers/lintels only) | pass |

`worldcheck --headroom-all` reports **0 headroom traps** for both EngineTest and this world.

## Arc wiring and entity changes

| Change | Reason |
|---|---|
| `portal Dust_Ravine … -66 8 2 bidir` → `portal AlShaddai_Metropolis 300 -3 0 302.5 3 5.5 0 0 1 bidir` | Dust_Ravine is not this mission's `next`; `-66` is outside AlShaddai's 80×80 floor. Moved to the terminal chamber so the causeway is the way out instead of a skip-at-the-entrance. |
| `PathNode path_e 294 0 0.5 next=arena_d` → `288 0 0.5 next=terminal` | 294 is inside the terminal dais (solid to z=1.5 across x 292.5..299.5), so the old waypoint was unreachable. |
| new `PathNode terminal 290 0 0.5 radius=5 next=arena_d` | Closes the loop `path_e → terminal → arena_d` without an orphan node. |
| new `trigger Trigger_EnterVolume 82 -17 0 84 17 8 once` | Threshold cue onto the causeway. Placed off both playerstarts — a `once` volume containing `playerstart` #1 spends its single fire on the first sim step. |
| `playerstart`, 15 `pickup`, 14 `npc`, 27 `light`, 13 `Mesh.*`, `levelinfo` | Unchanged (verified by diff and by primitive counts). |

The mission objective (`Objective_CollectRelics`) is type `collect`, count 3; the `target = relic`
string is not consumed. The 15 pickups satisfy it several times over.

## New `.ozls` defs (copied, not retyped)

| File | `entity` | Body change |
|---|---|---|
| `skyzone_engine_test.ozls` | `engine_sky` | wording only (fog/ambient/skybox values preserved) |
| `engine_water_room4.ozls` | `engine_water` | wording |
| `engine_ladder_corridorD.ozls` | **`arena_ladder`** | re-keyed — EngineTest declared `engine_ladder`, which matched no zone, so that zone had no script at all |
| `engine_reverb_largeroom.ozls` | `engine_reverb` | wording (`reverb_mix`/`reverb_decay` kept) |
| `engine_sound_room2.ozls` | `engine_sound` | wording + accurate comment about the suppressed hooks |
| `path_water.ozls` | `path_water` | comments only |
| `path_acid.ozls` | `path_acid` | comments only (5 hp / 60 ticks) |
| `path_lava.ozls` | `path_lava` | comments only (8 hp / 30 ticks) |

`path_surface_demo` intentionally has no def (same as EngineTest — it is the flag gallery).
Comments are pure ASCII; the em dash appears only in `World.ozone`'s identity header.

## Assets copied into the world directory

| Path | Why |
|---|---|
| `oztex/tileset/01..05_*.png` | texSlot `1..5` is a positional OZONE arg and indexes the **world-local** `oztex/tileset/` (sorted by name). Without these, every slot-textured brush renders untextured. |
| `Models/Skybox.png` | `LoadWorld` loads `%sModels/Skybox.png` unconditionally and sets `SkyboxEnabled` from it. |
| `World.ozone` line 153 `texPath=` | repointed from `GameData/Worlds/EngineTest/…` to `GameData/Worlds/Aethelgard_Ruins/…`. |

## Validation

`worldcheck` (the repo's OZONE auditor, links the real `OzoneParser`):

```
Aethelgard_Ruins --headroom-all   → 324 primitives, 176 solid brushes,
                                    2 directional + 25 point lights (0 buried),
                                    13 meshes, 9 zones (0 missing name=),
                                    headroom traps: 0
                                    === 0 error(s), 1 warning(s) ===
Aethelgard_Ruins --asset-refs     → all referenced assets resolve
                                    120 def/world files, 163 asset references
                                    no new dangling asset references
                                    === 0 error(s), 1 warning(s) ===
EngineTest (baseline)             → identical counts, 0 errors, same warning
```

The single warning (`multiple playerstarts; only #1 is ever used`) is inherited from EngineTest: the
world has two `playerstart` lines and only #1 is consulted. Not introduced here, and both spawns are
open to the sky.

Geometry integrity: `add` counts match exactly (208 total; 172 box / 32 cyl / 2 sph / 2 pln),
`npc` 14, `pickup` 15, `light` 27, `zone` 9, `Mesh.*` 13. The only primitive added is
`PathNode terminal`.

Still to check by hand (needs the game/editor, not a headless tool): visually confirm the pool scroll
and hall drift, the zone enter messages, acid/lava tick damage, and the portal landing in
AlShaddai_Metropolis.

## Deferred / known gaps (not fixed here)

- **World music**: no `Music/Main.mp3`. The candidate track (`Audio/Music/DD_sunset_heat.mp3`) is
  10.9 MB, so it was not duplicated into the world directory. `PlayWorldMusic` falls back to the
  shared ambience with **no warning and no dangling ref**.
- **`ZONE_GAMEPLAY_SOUND` is inert**: nothing populates `ZoneNode::soundProfile` from world data, so
  `engine_sound` / `path_surface_demo` produce no audio, and their `on_enter` is deliberately skipped
  by `Main.cpp`. Fixing that is an engine change.
- **`reverb_mix` / `reverb_decay` on a zone def have no consumer** (reverb comes from
  `ZoneEnvOverrides` / `SoundManager::UpdateReverb`), so `engine_reverb.ozls`'s values are decorative.
- `tools/worldcheck.cpp:810` still says "5.25" in a comment while `kNeedClear` is 5.05.
- The EngineTest world keeps the cross-world `texPath` into its own `oztex/` — untouched on purpose.
