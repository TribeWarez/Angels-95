# World Design Pass - Campaign Maps

This pass rebuilt the eleven **scaffold** campaigns worlds. They were 28-line
placeholders: one 80x80 floor slab, four 2-unit perimeter walls (shorter than the
3-unit player, so you could jump out of the level), no tileset (everything rendered
as flat grey), no cover, no verticality, and five NPCs dropped in the open.

The eight established worlds (`CitadelRuins`, `Dessert_Dreams`, `Dust_Ravine`,
`EngineTest`, `Aethelgard_Ruins`, `KryoArchipel_Severny`, `Sektor_01_SilentPines`,
`World_endless_snow`, `TestMap`) were the quality bar and were not relaid - they
already have intent, cover, verticality and lighting. The rules below were derived
from them plus what rendering the new levels exposed.

Rebuilt: `Sektor_02_MegacityGrid`, `Sektor_03_AegisBaseAlpha`,
`AlShaddai_Metropolis`, `Chasm_ForgottenMemories`, `Refinery_Complex_02B`,
`Digital_Purgatory`, `Phantom_Hubs`, `SoulEater_Vaults`, `Citadel_AstralMind`,
`Esoteric_Altar_Chambers`, `NearDeath_Threshold`.

## Design standard

Every one of those worlds now carries its **design document in the file header**:
player fantasy, the one moment the level is built around, a pacing arc with
timestamps, the flow graph, and geometry notes. It is a comment block at the top of
`World.ozone`, so it survives an AngelEd round-trip and cannot drift away from the
map the way a separate document does.

The shared beat structure:

```
READ      12-25 units to the first feature: you arrive, you look, nothing is in
          melee reach. Everything you need to route by is visible in one frame.
PRESSURE  the first encounter, with cover you can reach and a fallback behind you.
CHOICE    at least one junction: critical path + an optional branch that pays.
CLIMAX    the guarded space the level is named for.
EXIT      the route out, framed by a landmark so it reads from the far side.
```

Rules applied to every level:

- **The critical path is legible.** The exit landmark is visible from the entry in
  every world (hangar doors, transit gate, shrine arch, gatehouse, spire).
- **An optional branch always pays.** Rifle on the deck, melee weapon behind the
  seal, coin on the market roof, vial in a side vault. Never a dead end with nothing.
- **Every encounter has two approaches.** Gauntlets are crossed by a second route:
  the bridge versus the canyon floor (Chasm), the corridor versus the track trench
  (Hubs), the deck versus the monolith lanes (Digital), the bridge versus the mirror
  field (Threshold).
- **Difficulty is spatial.** Chokes are 4-8 wide with cover at the end, not open
  fields with more enemies. Cover tops out at 1.5-2.4 so it blocks instead of being
  stepped over.
- **Environmental storytelling.** Rotated fallen spans in the canyon, an overgrown
  market roof, a furnace still burning, candles still lit at the altar, a departure
  board still updating on an empty platform.

## The levels

| World | Shape | The one moment | Optional branch | Exit |
|---|---|---|---|---|
| Sektor_02_MegacityGrid | crossroads (A) + relay plaza (B) + tower | stepping out of the south avenue into the crossroads with the tower over the gate | market roof, substation, promenade, vault | the tower top, 20 up |
| Sektor_03_AegisBaseAlpha | compound (A) + motor pool (B) + annex (C) | the hangar doors open on an EMPTY portal cradle | barracks / armoury / tower / bunker roofs / cells | the annex rig |
| AlShaddai_Metropolis | walled town + arcade | six columns of shade over an empty bazaar | wall walk (rifle) | caravan gate, north |
| Chasm_ForgottenMemories | two rims + 3 routes | mid-bridge, shrine on the far rim | upper bridge (rifle), canyon floor (pistol) | shrine arch |
| Refinery_Complex_02B | yard + tank farm + hall | crossing the yard to a still-burning stack | catwalk ring (vial) | control room frame |
| Digital_Purgatory | grid + monolith forest | the core disc and its four bridges | four platforms, one reward each | north platform |
| Phantom_Hubs | concourse + platforms | the mezzanine over both platforms | track trench, mezzanine (rifle) | service corridor, east |
| SoulEater_Vaults | nave + 6 side vaults | the soul well's single long sightline | three vaults a side (loot each) | sealed door |
| Citadel_AstralMind | island + 4 satellites | a 4-wide bridge over nothing | satellites, plus the plain below | north satellite |
| Esoteric_Altar_Chambers | 3 chambers in a line | the sanctum roof breaks, altar under sky | two chapels (pistol / rifle) | *none - end of the arc* |
| NearDeath_Threshold | bridge over a mirror field | the field reflecting a second bridge | under-route (rifle, coin) | far gate |

Each world also got:

- a **tileset** (`oztex/tileset/01..05`), copied from the established worlds, so the
  positional `texSlot` works and the world is self-contained in its package;
- a **skyzone `.ozls`** matching its `zone sky ... name=`, so fog, ambient and the
  sky are actually driven (the scaffolds had a zone with no script, so an unnamed
  auto-zone from another world could win);
- **path nodes** for NPC patrol, with raised levels given their own loops;
- **8-10 lights** under the 24-light design budget, with effects (torch / fire /
  lamp / watery) instead of uniform white points;
- **6-7 pickups** on real surfaces, never inside a brush.

## The two extended levels

`Sektor_02_MegacityGrid` and `Sektor_03_AegisBaseAlpha` were later doubled in
length. Each is now three connected places with **two independent routes**, so
the level is a loop instead of a corridor. Geometry roughly doubles (154 / 163
brushes, 18 / 19 NPCs, 10 / 11 pickups, 24 patrol nodes, 19 / 21 lights).

**Sektor_02 - crossroads -> relay plaza -> tower top.**
District A is the original crossroads, unchanged, except that its north wall now
has a real gate and the old east transit gate is bricked up behind a
**substation** dead end (the sealed arch is the story beat: the line is dead).
Through the gate, **district B** is a 104x68 plaza with a 20-tall relay tower at
its centre, an **arcade** promenade down the west side and a **vault** in the
north-east. From the ground you can take the plaza, the arcade, or the tower
base; from the west stair you reach a **ring deck at 10** and a second flight to
the **tower top at 20**, where the exit portal is. The high route skips the
plaza entirely, so clearing the vault and the promenade is a choice. The tower
is visible over the gate wall from the spawn, so the second district is
announced before it is entered.

**Sektor_03 - compound -> motor pool -> containment annex.**
District A is the original compound, except the hangar's rig is now **empty**:
the dais, pylons and lintel are there, the field is not, and the feed lines are
cut on the floor. That absence is the level's hook and the reason to go north.
The north gate opens onto a 96-wide **motor pool** with two **battery bunkers**
(roofs walkable via a six-tread stair, firing slits facing the apron), a
**fuel island** under its own canopy, and a roofed **vehicle bay**. Beyond it,
the **containment annex** holds the relocated rig in a 14-high hall with a
**control gallery at 8** along the west wall and two **containment cells** on the
east. Two routes to the rig: the hall floor, or the gallery stairs.

Lessons this extension added to the checklist below:

- **`worldcheck` treats every solid top as standable.** Any prop under a ceiling
  must satisfy `ceiling - propTop >= 5.05` or it reports a jump trap, whether or
  not the prop is actually reachable. Four spaces were re-tuned for it (the
  substation ceiling, the arcade canopy, the fuel canopy, the gallery props).
- **A pickup billboard is 2-3 units tall**, so a pickup on a doorway sightline
  fills the frame from 20 units away. Keep them off the axis.
- **A landmark needs a light material.** The relay tower in the dark metal slot
  read as a black silhouette from the plaza even with two lamps on it; concrete
  plus a lit band at the top fixes it.
- **The sun is aimed at the world origin**, so a district built far from the
  origin takes no directional light on its south face. Districts B and C needed
  their own lamps on the spine; ambient alone left them flat.

## Authoring rules this pass produced

These are the traps that cost real time. They are all silent failures.

### 1. Glow surfaces render TWICE

`OzoneLoader::DrawGlowGeometry` re-draws every `SURF_GLOW` face in
`BLEND_ADDITIVE` after the world pass. A glow face therefore lands as
`2 * (base + glowColour * glowScale)`, so **a bright texture clips to white no
matter how low the glow is**. Every portal aperture, furnace mouth and gate sheet
in the first draft was a flat white card.

Working recipes:

- **A light source** (portal, lava, gate): a *coloured* texture, `UNLIT`, and no
  glow at all. This is what `Dust_Ravine` does with its lava and acid pools:
  `texPath=GameData/Tex/Global/EFX/liquid_lava.png flags=788480 panV=0.18`.
- **An accent** (window strip, grid inlay, shard): `flags=16384` on a dark texture
  with `surfGlowScale` <= 0.2. Above that, the doubling saturates.

### 2. `levelinfo`'s two sky paths are CAP and SIDE, and should match

A different cap image leaves a visible trapezoid where the cap meets the walls
(`Wiki/World-Format-OZONE.md`). Passing the same gradient twice is what the worlds
that look right do. `Refinery` originally passed `sky_darknoon` + `sky_dead` and
rendered as a black ceiling over a red horizon band.

### 3. A ceiling is a jump trap unless it is 5.05 above the floor

`worldcheck` measures this (it uses 5.05; the docs say 5.25). A low ceiling does
not clip the jump - `OverlapsObstacle` rejects the move and rolls back the
horizontal part too, so the player appears to **stick to the wall** beside them.

Three concrete cases fixed:

- a 3.4 clearance under the AlShaddai wall walk -> made the walk a solid terrace;
- a 5.0 clearance under the Refinery catwalk -> raised the hall ceiling to 13;
- a 4.8 clearance under a rail on the Hubs mezzanine -> lowered the rail to 6.9.

**Do not put a stair under a bridge.** `Citadel_AstralMind`'s recovery stair ran
under the south bridge and had 5.0 of headroom for ten treads; it was re-routed to
climb the south satellite instead.

### 4. Spawn, first feature and first patrol node

- A feature 40 units away is a speck at eye height 2; a feature 6 units away fills
  the frame. 12-25 units is the readable band. The Chasm spawn was 38 units from
  the rim and the chasm was invisible - the level's whole premise was missing from
  the first frame.
- The nearest **patrol node** must be >= 10 units from `playerstart`. A node on the
  spawn walks an NPC into the player's face during the first two seconds.
- A **trigger volume containing playerstart #1** spends its single `once` fire on
  the load frame. Every trigger in these worlds sits 10+ units in.

### 5. Coplanar tops z-fight; stairs land BESIDE a platform, not in it

A stair whose last tread tops out at the same height as the platform it serves, and
overlaps its footprint, puts two upward faces on one plane. End the run exactly at
the platform edge. Same class: a door lintel whose top equals the roof underside,
a platform skin laid into the platform instead of on top of it, and an arch lintel
that shares its top plane with the piers (give the lintel its own height).

Corner overlaps of two same-height walls are unavoidable and invisible (both are
hidden by the roof) - `worldcheck` does not care and neither should you.

### 6. Everything else that bit this pass

- `cyl`'s `cz` is the **top** (`[cz-h, cz]`); `box` is centre-based.
- A **degenerate box** (`z0 == z1`) is easy to emit from a helper and draws nothing.
- An **entry gate narrower than the bridge** it fronts leaves a 3-unit overhang
  over whatever is below it (Threshold). Match the opening to the deck width.
- **Light positions must not be inside solids.** Worldcheck reports "buried inside
  a solid - it lights nothing"; cylinders (tanks, columns, well shafts) and thin
  glow sheets are the usual culprits. Nine were fixed.
- **Raised-level patrols need their own loop.** A street node linking to a roof
  node draws a patrol line through the stair geometry.
- NPCs are placed with their **feet** at the surface z, not +1. The shipped maps
  use +1, which is a real one-unit hover.
- A single offset spawn (`(0, -34)` instead of `(0, -46)`) is often all a
  composition needs; do not rebuild geometry to fix framing.

## Verification

Two independent passes, both reproducible:

### 1. Connection audit (cross-world)

Walks the mission `next=` graph, then for every portal checks that the target
world exists, that the **arrival point is a real standable cell** (not inside
geometry), and that **from that arrival point the player can reach the target
world's own exit**. That last check is the whole progression question: a portal
that drops you behind a sealed door is a dead campaign. `bidir` drops a runtime
return portal at the arrival point (armed once you step away), so backtracking
works without a second authored portal.

Result: the 14-world chain verifies end to end. Two arrivals were broken and are
fixed - see below.

### 2. Walkability audit (inside a world)

A grid graph over the world's collision AABBs (1.0-unit cells, multi-level per
cell, box yaw-rotated as the engine rotates it, `cyl`/`sph`/`pyr` included),
with edges for a 1.0 step, any drop, and a 2.2-rise / 3.1-reach jump. Then:

- BFS from `playerstart #1` -> what is actually reachable;
- reverse BFS from the exit portal -> what can still finish the level, so
  "reachable but cannot reach the exit" is reported as a **softlock**;
- every pickup / NPC / path node / trigger must own a reachable cell;
- "embedded in a solid" is reported separately from "unreachable", because they
  are different bugs with different fixes.

### Progression bugs it found (all fixed)

| # | World | Bug | Severity |
|---|---|---|---|
| 1 | Sektor_02 | the 12-wide north gate had **no floor**: district A's slab ended at y=52 and district B's began at y=54, so the gate was a hole and the tower-top exit was unreachable | **blocks the level** |
| 2 | Esoteric | the rite hall's north wall was solid - the sanctum, the altar and the ending of the arcade could not be entered | **seals the finale** |
| 3 | Sektor_02 | exit portal arrival in Sektor_03 was `(0,0,0)`; in KryoArchipel that point is inside the basin slab | **player arrives in rock** |
| 4 | Chasm | arrival in Digital_Purgatory at `(0,0,0)` is inside the core disc (r=10, z 0..6) | **player arrives in rock** |
| 5 | Phantom_Hubs | the mezzanine slab ended at x=-4 but the stair climbed at x -1..3, so the whole upper deck had no access | blocks content |
| 6 | Phantom_Hubs / S03 / Digital / AlShaddai / Threshold | 8 patrol nodes and 3 NPCs stood **inside** stair treads, a stall, a fuel pump, the core column, or a standing stone | dead patrol |
| 7 | Refinery / S03 / Esoteric / Chasm / SoulEater | pickups on a 2.4 cover block (past the 2.25 jump), on an unclimbable roof, on the altar cap, inside a pier, inside the well shaft | dead pickups |
| 8 | S02 / S03 | two NPCs inside stair runs; the S02 gate also blocked the trigger | dead/NPC |
| 9 | Dessert_Dreams | `rifle_01` and `Powerup` inside the 3x22x6 curtain wall; `Floater` inside a 6x6x3 outbuilding; `mesa_top` inside the 14-tall mesa; `gate_in` and a `Walker` authored 3 above the floor | dead content |
| 10 | Aethelgard_Ruins | `ManaVial` inside a 3.5-tall platform block; `Powerup` on a 9-tall pillar with no route up | dead content |

Rows 9-10 are **pre-existing** in the two earliest maps, and they are the same
class of bug: an entity authored at a z that belongs to a solid rather than to a
surface. Nothing about them looked wrong in the editor.

## What "sloppy" actually means

A blockout and a finished level can share the same layout. The difference is a
short list of tells, and every one of them is cheap:

| tell | fix | cost |
|---|---|---|
| one huge floor slab with visible tiling | pave a processional/squared area 0.10-0.15 proud, and vary texScale per region | 2-4 brushes |
| walls are naked extruded boxes | plinth at 0-0.5, cornice at the top, pilasters every ~20 | 2 brushes per face |
| the sky meets a wall with nothing behind it | `flags=8` ring outside the boundary | 8-10 brushes |
| nothing at eye height (0-2m) | pots, crates, barrels in corners | 1 brush each |
| interiors are uniformly lit | point lights with contrast + a lower ambient per zone | data |
| no water in a map that should have it | basin + `NO_BSP_CUTS` panning sheet + `zone water` | ~12 brushes |
| a landmark hidden behind a stall from the spawn | move the stall, not the landmark | 0 |
| pickups read as floating icons | place them against geometry, off sightlines | 0 |

`AlShaddai_Metropolis` is the worked example: an oasis basin with a real
`zone water` and a cosmetic panning surface, wall plinth/cornice/pilasters, a
processional paving strip, kerbed pool rim, eight pots and six crates at eye
height, a painted dune ring, a cool pool light against the warm sand, and the
market stall moved off the gate-to-well axis. ~45 brushes, no new assets, no
change to the layout or the encounters - and it stops reading as a sandbox.


Notes for the next pass:

- **A pickup at `surface + 1` is not the same as `reachable + 1`.** Anything on
  top of a block taller than **2.25** can only be reached by a jump that does not
  exist. Cover height and pickup height are the same decision.
- **A patrol node is a position, not decoration.** Nodes inside geometry are
  silent: the NPC walks to a point it can never occupy and the patrol stalls.
- `worldcheck` cannot see any of the above - it checks geometry bounds, lights,
  zones and headroom, not connectivity.

### Presentation gap (measured)

Counts per world; established maps are the benchmark.

| | mesh props | particle emitters | weather | wind | water zones | backdrop |
|---|---|---|---|---|---|---|
| established (8 maps) | 0-40 | 0-10 | 0-1 | 0-1 | 0-4 | 0-16 |
| rebuilt (11 maps, before) | **0** | **0** | **0** | **0** | **0** | 0-6 |
| rebuilt (now) | 0 | 0-3 | 0-1 | 0 | 0 | 0-6 |

The rebuilt maps are brush-only. That is the whole difference between "a level"
and "a blockout", and it is why they read as flatter than `Dessert_Dreams` even
though the layouts are comparable.

Priority, highest value first:

1. **Water.** Every established outdoor map has a pool, river or cascade with a
   `zone water` volume and a panning surface. None of the rebuilt maps do -
   AlShaddai's well is dry and the Refinery has no coolant. This is the single
   biggest visual upgrade and it is data-only (split the floor slab around a
   basin, add the UNLIT|PAN surface, add the zone with physics overrides).
   Recipe: `Wiki/Outdoor-Maps-Changes.md`.
2. **Mesh props.** 0 vs 13-40. Rocks, foliage and industrial clutter are what
   break the silhouette; boxes cannot. Needs an asset pass: copy a shared prop
   set into `GameData/Environment/` and reference it per world. Remember
   `Mesh.Static` has **no collision** - props near walkable ground need
   AutoConvex boxes or placement outside the play area.
3. **Backdrops.** Only Sektor_02 has a painted skyline. The outdoor rebuilt maps
   end at a wall with bare sky behind it. A `flags=8` ring beyond the boundary
   gives a horizon, and it is the cheapest of the three (geometry only).
4. **Atmosphere.** Added a first slice: 1-3 particle emitters per world at fire
   and energy sources that were already lit, plus weather (`particles`) in the
   four genuinely open worlds. **These do not render on the current binary** -
   the shipped maps' own emitters are invisible in the same build, which is the
   in-flight particle-system rework, not level data. Re-check after it lands.
5. **Audio.** `PlayWorldMusic` looks for `<world>/Music/Main.mp3` and otherwise
   falls back to one shared `GameData/Audio/Ambience/Music_Atmo_1.wav`, so all
   eleven rebuilt worlds currently share the same ambience loop. Two options:
   per-world tracks (asset weight) or a sector→track map plus a build-time copy
   in `build-data.ps1`.

### Commands

```bash
# static: geometry, lights, spawns, zones, headroom, asset refs  (CI gate)
make worldcheck
./worldcheck GameData/Worlds/<Name>/World.ozone
./worldcheck GameData/Worlds/EngineTest/World.ozone --asset-refs

# visual: deterministic, HUD-free frame
./Angels95.exe --world <Name> --shot shot.png --shot-res 1280x720 \
               --shot-cam "x,y,z,yaw[,pitch]" --shot-delay 20
```

All eleven rebuilt worlds report **0 errors, 0 warnings** from `worldcheck`,
GameData-wide `--asset-refs` finds no new dangling paths, the walkability audit
finds no unreachable content and no softlocks, and the 14-world portal chain
verifies end to end.

A screenshot pass is still required: it is what caught the white glow, the black
sky, the invisible chasm and the NPC standing on the spawn - none of which
`worldcheck` can see. **Render every level from its spawn before calling it
done.**

## Polish pass 2 (after the template was proven)

Applied to the rest of the set, in the order that gave the most for the least:

1. **Sektor_02** got the AlShaddai treatment: the painted skyline is now a dark
   stone at 1:1 in two rows (a lit concrete panel at 4x6 was the "cardboard"),
   plinth + cornice + lane markings + a tower apron + banding ribs on the tower,
   eye-height drums and crates, and lit window strips on the two faces that make
   up the north avenue. Three props (two rocks with collision, two shrubs).
2. **Water** where it is thematic: a 14x14 coolant sump in the Refinery yard
   (acid texture, *lit* rather than `UNLIT` - at UNLIT it was a neon slab) and a
   14x10 seep on the Chasm canyon floor. Both use the AlShaddai pattern: the
   floor cut into four slabs, a basin, a `NO_BSP_CUTS` panning surface, a kerbed
   rim and a `zone water` with swim physics. Astral and Threshold get none on
   purpose (a void, and a mirror field).
3. **Shared props**: `GameData/Environment/Props/` now holds four rocks, two dry
   shrubs and two rock textures, copied from the only prop set in the project
   (`World_endless_snow/Models`). Three rules came out of it:
   - The source GLBs carry a large baked node translation (x ~ 147, z ~ -330).
     raylib applies a node transform at draw time, so a rock authored at the
     origin rendered 147 units away - invisible in a 1200x600 world, fatal in a
     hand-placed town. `recentre_props.py` bakes the offset out so every model is
     centred on x/z with its feet at y=0.
   - `Mesh.Static` has **no collision**. Every solid prop carries ONE `flags=16`
     box covering its footprint and height, which is what AutoConvex emits for a
     convex rock. Foliage carries none.
   - A prop under a ceiling is a headroom trap: the S02 plaza boulder's box sat
     0.55 under the arcade canopy and worldcheck caught it.
4. **Weapon pickup icons.** `Pistol_01/Textures/Pistol_01_silhouette.png` and the
   rifle equivalent were a **white gun on a white background**, so every
   `pistol_01` / `rifle_01` pickup rendered as a solid white billboard in every
   map - the most visible "sloppy" artefact in the game. Both regenerated as dark
   silhouettes on transparency.

### Open engine item

**A `SURF_GLOW` brush whose only texture is the positional `texSlot` renders its
glow face as an untinted WHITE panel.** Verified on the Chasm shard fissures by
varying one thing at a time: the glow colour and scale (down to 6%), the texture
slot (rock, then the dark tech panel), and a brush-wide `surfTexSlot=`. The face
stayed pure white in all four. The shipped glow surfaces all pair `flags=16384`
with a brush-wide `texPath=` (the lava and acid pools), which is the form that
works. The fissures were removed rather than shipped as white boxes. The
S02/AlShaddai window strips use the same pattern and read as lit windows only
because they are small - they are the same white face.

## Open items for the next pass

- **Showcase screenshots.** `Wiki/Home.md` shows three `--shot` renders per world
  for the established maps; the campaign maps have none yet.
- **Mission text.** `GameData/Game/Angels95/Missions/*.ozls` still say
  "scaffold" in their `on_start` / `on_complete` messages and their file headers.
- **Playtest the numbers.** NPC counts (6-10 per world), pickup placement and the
  optional-branch reward values are a first pass, not measured.
- **Server-side traversal.** NPCs cannot climb; raised patrol loops are separate
  graphs. A server-side step/ledge rule would let a patrol use a stair.
- **`Esoteric_Altar_Chambers` has no exit** because it is the end of the arc. If
  the campaign ever continues, it needs a portal and a following mission def.
