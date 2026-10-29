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

## Sky: the seam fix

Two independent defects, both measured rather than guessed:

1. **Cap band.** The skybox draws top/bottom faces with a *different texture* (`useCap`) on a horizontal quad, so every wall top met an unrelated image. The band was a 50-level (day) / 24-level (dusk) tonal step.
2. **Corner panels.** The six quads are separate planes and the four wall quads **do not agree on their V mapping**. Measured at a cube corner: adjacent walls differed by 33-40 colour levels at the same screen row, and the outer panels ramped horizontally *within* a row while the wall facing the camera was flat. That is a per-face V *offset*, so no texture can fix it — a vertically symmetric texture only cancels a flip (it took the step from 41 to 33).

The fix, in three parts:

- `tools/gen_sky_textures.ps1` generates **horizontally periodic, vertically symmetric** gradients (`sky_grad_day.png`, `sky_grad_dusk.png`): every horizontal variation is a sum of `sin(2*pi*k*u)` with integer k and the last column is overwritten with column 0, so corners match by construction; and `colour(y) == colour(h-1-y)`, so a V flip is invisible.
- **`Sky.fs` samples V from WORLD ELEVATION**, not from the quad's texcoord: `elevV = clamp(dirW.y * 0.5 + 0.5, 0, 1)`. Elevation is a property of the view ray, so every quad — walls and cap alike — agrees by construction. This is what actually removed the corners.
- Both worlds pass the **same gradient as cap and sides** (`levelinfo <grad> <grad>`); with a separate cap image the elevation V made the cap's flat zenith meet the walls' 91%-to-zenith row, leaving a faint trapezoid.

Measured on the same cameras (largest adjacent-pixel step in the sky):

| | before | after |
|---|---|---|
| SilentPines, seam column x=215 at rows 20 / 60 / 120 | 33 / 28 / 20 | **1 / 1 / 1** |
| KryoArchipel, seam column x=215 | 40 / 36 / 25 | **5 / 4 / 3** |
| SilentPines, cap band | 50 levels | **1** |
| KryoArchipel, cap band | 24 levels | **1** |
| EngineTest (legacy panorama), hard straight band | 16.06 lum | **2.13 lum** |

Residuals: a ≤5-level step at the **U** face boundary (each face still supplies its own `fragTexCoord.x`), and a faint trapezoid when looking steeply up at the zenith. Both are shading-level, not the hard panels of before.

**Behaviour change to know about:** because V now tracks elevation, a wall samples only V ≥ 0.5. Legacy panorama skies therefore show only the **upper half** of their image on the walls — `sky_noon.png` / `sky_day.png` now display their top half, which also moved that art's own baked 26-31 level wrap seam onto a constant elevation contour. Legacy worlds measured *better* (EngineTest's band fell 16.06 → 2.13), but the change is visible and is not confined to the two outdoor maps.



## Machine Scar branch (Sektor_01)

The falls' backing rock now has a 10-wide, 6-high opening, hidden behind a wider
waterfall sheet and a dog-leg baffle. It leads to an optional three-room branch
built to exercise the Phase-7 mechanics in real content:

| Room | Mechanic | Read / affordance |
|---|---|---|
| The Choir (south) | `zone gravity ... gravity=-18` (straight up) | A 26-unit shaft with full-height ribs and 8/16/24 bands; the ceiling ring and the top gallery are the visible destination. A `zone ladder` in the entry corridor is the return. |
| The Wound (east) | `zone crt` | A signal room with a screen bank on the entry wall, a reader pedestal, and cool blue/cyan lamps. The warp is per-frame, so `--shot` deliberately captures it off. |
| The Vault (north) | `SURF_QUANTUM` brushes | A quantum seal in the corridor and a quantum bridge over a 4-deep pit, on the entry axis. A bright ceiling eye is the "look up" cue; the bridge is solid only while it is out of the frustum. A 0.8-step block pile makes a fall recoverable. |

Design rules used:

- The branch is a loop, not a corridor: enter through the falls, exit down the
  ladder. It pays a `shotgun_01` and supplies, and never gates the shrine exit.
- The pit is on the entry axis so the choice is readable in one frame; the
  quantum bridge is deliberately never visible while solid.
- The interior is a separate sky volume (`silentpines_scar`) nested inside
  `silentpines_sky`; `UpdateSkyZone` takes the smallest containing volume, so the
  dark fog/ambient and its `say` line win inside and the gorge def resumes at
  the tunnel mouth.
- All ceilings are 5.05+ above standable surfaces, all walkable rises are
  <= 0.9, and `worldcheck` reports 0 errors and 0 headroom traps.

The larger-world fix that made this branch possible is in the renderer: the old
zone-membership PVS culled the shrine and the river whenever the camera left
their local zone. `DrawWorldGeometry` now culls only by frustum + explicit
occluder planes (see `AGENTS.md`). Verify with:

```text
./worldcheck GameData/Worlds/Sektor_01_SilentPines/World.ozone
client --shot --world Sektor_01_SilentPines --shot-cam "60,0,1.65,90,0"
```

### Large-world readability fixes in the same pass

- **The bridge rail was hiding the shrine.** The rail topped out at 2.2 while the
  eye is 1.65; at 8.6 units it blocked ~3.7°, and the 5.4-high shrine columns at
  60 units subtend ~3.6°. The sword was technically in the level and invisible
  from the bridge. The rail is now 1.0 high (top 1.7), so the altar/sword reads
  from the bridge as the design promises.
- **The plateau's east face was the grass box itself**, so the descent stair
  climbed a green wall. A 1-unit rock veneer (`Rock_Diffuse.png`) now faces the
  cliff; the stair reads as a cut in stone.
- **Large brushes used the same UV multiplier as a crate.** At the original
  2-3.5 range, a 26-high, 62-long cliff repeated its tile hundreds of times and
  read as noise. The outdoor cliff/ground surfaces now use 0.2-0.8 (grass stays
  finer than stone), while the scar's interiors use 0.2.
- `worldcheck` on the extended world: **0 errors, 0 headroom traps**, 231 solid
  brushes, 17 point lights, 8 zones, 405 primitives. The only warning is the
  pre-existing second `playerstart`.

## Known visual limitations (engine-level, not content bugs)

1. **Sky box cube seams - fixed (see above).** The hard panels, the cap band and the corner
   tonals are gone. What remains is a <=5-level step at each U face boundary (each face still
   supplies its own `fragTexCoord.x`) and a faint trapezoid when looking steeply up at the zenith.
2. **Water is still a scrolling texture, not a simulation.** No foam sheet, refraction or depth
   term exists; `waterfall_streaks.png`, `water_pool.png` and `foam.png` are paint, and the motion
   comes from the pan. Foam now reads at the cascade impacts (an impact band, a 3.4x faster churn
   wall, and shore strips), but it is 1998, not 2004.
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
