# Outdoor maps — SilentPines Gorge + Dusk Falls

Two mission worlds promoted from 34-line placeholders to playable maps:

| World | Mission | Objective | Next |
|---|---|---|---|
| `GameData/Worlds/Sektor_01_SilentPines` | Sektor_01_SilentPines (Sefirah 1, Terran Prime) | `Objective_ClearArea` — kill 5 | Sektor_02_MegacityGrid |
| `GameData/Worlds/KryoArchipel_Severny` | KryoArchipel_Severny (Sefirah 2, Hydrosis Prime) | `Objective_CollectRelics` — collect 3 | Refinery_Complex_02B |

Direction was Unreal-'98 outdoor set pieces (NyLeve's Falls / a dusk cascade). Both are
**all-brush** maps: 150 and 136 primitives, 0 errors and 0 headroom traps on `worldcheck`.

## Why all-brush rather than a heightmap

Collision is AABB brushes only. A heightmap supplies a terrain *surface* (per-cell
`SampleHeightmapY` ground clamp) but **no lateral collision**, so a heightmap cliff is
walk-through. `Dust_Ravine` is the only shipped heightmap and it is nearly flat (10 units of
relief over 180), which is exactly the safe role: gentle ground relief, with all real
elevation from brushes. These maps skip the heightmap and build the gorge/terraces from boxes
and cylinders.

Every walkable rise is **0.9**, not 1.0: `kStepHeight` is exactly `1.0f`, so a 1.0 step sits
on a float comparison boundary.

## The waterfall kit

```
# Cascade sheet (0.2 thick), 0.1 clear of the solid face it hangs on
add box <x> <y> <zmid> 0.2 <h> <d> 0 \
  texPath=GameData/Tex/Global/EFX/waterfall_streaks.png texScaleU=1.5 texScaleV=1 \
  flags=268962944 surfAlpha=0.9 panV=-0.35
```

`268962944` = `SURF_UNLIT` 2048 | `SURF_TRANSLUCENT` 128 | `SURF_TWO_SIDED` 1024 |
`SURF_PAN_V` 524288 | `SURF_NO_BSP_CUTS` 268435456.

- `UNLIT` matters: without it the sheet takes scene lighting and a dusk waterfall renders as a
  grey slab.
- `NO_BSP_CUTS` keeps the sheet cosmetic so it is not an invisible wall.
- Negative `panV` scrolls the texture downward (`uv.y += uPan.y * uTime`).
- Pools/rivers use `flags=264320` (UNLIT | PAN_U | TRANSLUCENT) with `surfAlpha=0.8`.

SilentPines' falls also needed a **rock face behind the notch** (`x 86.1..87.5`): with the notch
open to the skybox the water read as a blue panel rather than a fall.

## Water textures (`tools/gen_water_textures.ps1`)

The repo shipped one water texture, `liquid_water.png`: a high-contrast blue tile pattern. On a
large pool it reads as bathroom tiling, and on a cascade sheet as a flat blue panel. Two
textures were generated instead (deterministic — fixed RNG seeds, so re-running rewrites
identical bytes):

| Texture | Size | Purpose |
|---|---|---|
| `water_pool.png` | 64x64 RGB | Dark, low-contrast ripple field (mean RGB ≈ 18/40/58). Pool slabs are `UNLIT|PAN_U|TRANSLUCENT` at `surfAlpha=0.8`, so the stone bed shows through: dark reads as depth, bright reads as a pool. |
| `waterfall_streaks.png` | 64x128 RGBA | Vertical streaks with **punched transparent gaps** (12.3% of pixels alpha 0, mean alpha 156). |

The alpha is the whole point: `Surface.fs` multiplies the sampled texture's alpha by `uAlpha`,
so transparent holes are what turn a sheet into falling water. The first version of the
generator drew 150 anti-aliased lines over a 64px width and measured **84% of pixels inked** —
a near-solid pale panel. The script now draws 90 thin streaks and then erases 34 small
rectangles with `CompositingMode.SourceCopy`, which is what guarantees real holes rather than
low-alpha haze.

## The UV trap that dominated the first art pass

`OzoneLoader::BuildBox` multiplies generated UVs by **16 when `h >= 1.0`** and **8 when thin**,
and the authored `texScaleU/V` is applied *on top of that*. Values in the shipped-demo range
(8–14) therefore compound to hundreds of repeats on a large brush and render as moire noise.
Author `texScale` near **1** for big surfaces. This was the single largest visual defect found
by rendering the maps, and it is invisible to `worldcheck`.

## Validation

```
worldcheck --headroom-all   both worlds: 0 errors, headroom traps: 0
worldcheck --asset-refs     both: no new dangling asset references, 0 case mismatches
client --shot               all captures render; every texPath loads and decodes
in-game log                 sky zone defs bind and their on_enter actions fire
```

One benign warning per world: `multiple playerstarts; only #1 is ever used` — the maps author a
second `playerstart` for parity with the other worlds; `PawnSystem::GetFirstPlayerStart` only
reads #1.

## Known visual limitations (engine-level, not content bugs)

1. **Skybox cube seams.** The skybox is a six-quad cube with direction-projected UVs; the top
   face reads as a bright band across the horizon and the cube edges are visible. Every shipped
   outdoor world has this.
2. **Water is still a scrolling texture, not a simulation.** No foam sheet, refraction or depth
   term exists; `waterfall_streaks.png` and `water_pool.png` are paint, and the motion comes from
   the pan. The look is now deliberate rather than accidental, but it is 1998, not 2004.
3. **Outdoor scale reads arena-like.** With no terrain and AABB-only collision, a wide valley is
   a floor plate between tall boxes. Dusk Falls was tightened for this reason (depth 168 -> 102,
   pools 27x24/44x24/38x28 -> 20x18/26x18/26x22, aqueduct narrowed, seven ruin fragments added);
   the terraces now have landmarks and a horizon instead of reading as empty plains.
4. **No waterfall audio loop.** Script `play_sound` is one-shot; the keyed loop API is
   stat-driven only. The falls are silent.

## Reusable lessons

- Boundary treatment decides whether an outdoor map reads as outdoors: a 44-unit painted ring at
  the map bounds turns every sightline into a courtyard wall. A ~12-unit **rock ridge** leaves the
  sky visible and still stops the player walking off the world.
- Place the spawn at the *edge* of a height, not in the middle of a plateau, or the first frame is
  a floor.
- Box arg order is `x y z w h d rot`: the 5th number is vertical size, the 6th is Y depth.
