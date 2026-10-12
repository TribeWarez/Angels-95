# World Format (OZONE)

OmegaTech uses a single world description format: **OZONE**.

> **Note:** The legacy **WDL** (World Description Language) format and its runtime
> fallback loader have been **removed**. OZONE plus the LightningScript/Pawn
> entity system are the only source of truth. A world without a
> `GameData/Worlds/<Name>/World.ozone` is a hard load error.

## OZONE Format

OZONE is a line-oriented text format supporting CSG brush primitives and
entity metadata (player starts, pickups, NPCs, zones, lights, meshes, particle
emitters, path nodes, wind zones).

### Diagnostics

The parser reports what it cannot use, with a line number, rather than dropping
it silently:

- an unrecognised **first token** name the line and the token;
- a malformed **value** (`gravity=abc`, a bare `effect=`, `surfAlpha=opaque`)
  keeps that field's default and reports — it used to throw
  `std::invalid_argument` out of the parser and kill the process with no message;
- a trailing token on an **entity** line reports and stops that line.

Unknown tokens on a **brush** line are still ignored by design (that is what
makes the surface kwargs extensible): `add box ... someFlag=1` is accepted and
`someFlag` dropped without complaint.

### Primitives

```
add box    <cx> <cy> <cz> <sx> <sy> <sz> <rot> [texPath=... texScaleU=.. texScaleV=.. texOffsetU=.. texOffsetV=.. flags=..]
add cyl    <cx> <cy> <cz> <rt> <rb> <h> <slices> <rot> [...]
add sph    ...
add pyr    ...
add pln    ...
sub / intersect <primitive> ...   # CSG carve
```

### Surface flags

| Bit | Name | Meaning |
|---|---|---|
| `1<<3` (8) | `SURF_FAKEBACKDROP` | brush renders as a sky backdrop (drawn by the zone pass, skipped by the world pass) |
| `1<<4` (16) | `SURF_COLLISION_PROXY` | generated AutoConvex collision box — feeds the CSG collision world but is **never drawn in game** |

`SURF_COLLISION_PROXY` lines are ordinary `add box` lines with a `flags=16`
kwarg; they round-trip through the text format unchanged. AngelEd writes them
when you use **Append AutoConvex Collision**, and only shows them when the
sidebar **View → Collision** toggle is on.

### Per-face surface properties (UT99 style)

A brush's surface properties are stored **per face**, not per brush: the floor
of a room can be a different texture from its walls, and only the top of a
pillar can glow. The data model is `oz::surface::BrushSurface` in
`Source/World/SurfaceFlags.hpp` — a brush-wide default plus up to six
per-face overrides.

In the editor you right-click a face in the viewport and pick **Surface
Properties (N Selected)**, which opens a `Flags` / `Alignment` / `Stats`
dialog. Hold **Shift** while right-clicking additional faces to select several
at once; `Apply` writes to all of them and `Reset Surface` drops their
overrides. Faces are identified by the dominant axis of the clicked triangle's
normal, so they are the same six buckets the renderer draws.

Faces are named in **engine Y-up** (`px nx py ny pz nz`) and are **not** swapped
for OZONE's Z-up: they are a renderer concept, not world coordinates.

**Every field below is genuinely per face**, including the texture and the UV
transform. Each face is drawn with its own resolved properties and its own
texture binding, so a face's `tex`/`texSlot` and its `uvScale*`/`uvOffset*` apply
to that face alone. Precedence for the texture is: face `tex` (path) > face
`texSlot` > brush-wide equivalents > the brush mesh's own (auto-selected or
positional) texture.

**Brush-wide kwargs** (on any brush line):

| Kwarg | Meaning |
|---|---|
| `flags=N` | the flag bitmask (table above and below) |
| `surfTexSlot=N` | 1-based index into `<world>/oztex/tileset/`; 0 = keep the brush's own texture |
| `surfTex=path` | free-placement texture, **wins** over `surfTexSlot` |
| `uvScaleU=` / `uvScaleV=` | UV multiplier on top of the generated mesh UVs |
| `uvOffsetU=` / `uvOffsetV=` | UV shift |
| `panU=` / `panV=` | U/V scroll in texture units per second (needs the `U-Pan` / `V-Pan` flag) |
| `pan=u,v` | both at once, space-free |
| `surfAlpha=0..1` | alpha multiplier |
| `surfCutoff=0..1` | alpha-test threshold; `> 0` switches the face to masked |
| `surfGlow=(r,g,b)` | self-illumination colour, 0..1 |
| `surfGlowScale=` | glow intensity multiplier |

The older `texPath=`, `texScaleU/V` and `texOffsetU/V` names are **aliases** and
remain valid. `texPath=` additionally accepts a quoted value
(`texPath="my stone.png"`) — quote-aware parsing was previously dead code, so
such a path silently truncated at the first space.

**Per-face kwargs** — one family per face, so every token is independently
parseable and order-independent. Only faces you actually touch get emitted, so
an untouched face inherits the brush default on reload:

```
face<name>_<field>=value
```

where `<name>` is `px|nx|py|ny|pz|nz` and `<field>` is any of `flags`, `texSlot`,
`tex`, `uvScaleU`, `uvScaleV`, `uvOffsetU`, `uvOffsetV`, `pan`, `panU`, `panV`,
`alpha`, `cutoff`, `glow`, `glowScale`.

A face inherits every field it does not restate, so flipping one flag needs one
token:

```
add box 0 0 0 8 3 8 0 flags=2048 facepy_flags=12 facepz_texSlot=3
```

> Historical: `texSlot` / `tex` / `uvScale*` / `uvOffset*` were stored, exported
> and round-tripped for as long as they existed and **nothing ever bound them** —
> the Surface Properties dialog's texture and alignment fields only took effect
> after a save and reload, and per-face values never did. Flags, alpha, pan and
> glow were already applied per face (they travel as uniforms); texture and UV
> were the two that were not.

Precedence on load and on export: **face override → brush default → engine
default.**

#### Full flag list

| Bit | Name | Effect today |
|---|---|---|
| `1<<3` (8) | `SURF_FAKEBACKDROP` | painted backdrop, drawn at full value in the sky pass (uniformly 0.55x, never lit) |
| `1<<4` (16) | `SURF_COLLISION_PROXY` | collision only, never drawn in game |
| `1<<5` (32) | `SURF_INVISIBLE` | not drawn (still collides) |
| `1<<6` (64) | `SURF_MASKED` | alpha test against `alphaCutoff`; **inert unless `cutoff > 0`** |
| `1<<7` (128) | `SURF_TRANSLUCENT` | alpha blend, no depth write, **fogs normally** |
| `1<<8` (256) | `SURF_ALPHABLEND` | alpha blend, no depth write, **skips fog** |
| `1<<9` (512) | `SURF_MODULATED` | multiply blend |
| `1<<10` (1024) | `SURF_TWO_SIDED` | back faces drawn too |
| `1<<11` (2048) | `SURF_UNLIT` | full bright, lighting bypassed |
| `1<<12` (4096) | `SURF_FAKE_LIT` | ambient-only lit surface: `baseColor * (amb/10)`, no light contribution. NOT the same as `UNLIT` |
| `1<<13` (8192) | `SURF_SPECIAL_LIT` | reserved |
| `1<<14` (16384) | `SURF_GLOW` | self-illumination + additive halo pass; **inert unless the colour is non-zero** |
| `1<<15` (32768) | `SURF_PORTAL` | reserved |
| `1<<16` (65536) | `SURF_MIRROR` | reserved |
| `1<<17` (131072) | `SURF_ENVIRONMENT` | reserved |
| `1<<18` (262144) | `SURF_PAN_U` | scroll U; **inert unless `panU != 0`** |
| `1<<19` (524288) | `SURF_PAN_V` | scroll V; **inert unless `panV != 0`** |
| `1<<20` (1048576) | `SURF_SMALL_WAVY` | reserved |
| `1<<21` (2097152) | `SURF_BRIGHT_CORNERS` | reserved |
| `1<<22` (4194304) | `SURF_DIRTY_SHADOWS` | reserved |
| `1<<23` (8388608) | `SURF_SHADOW_HI` | reserved |
| `1<<24` (16777216) | `SURF_SHADOW_LO` | reserved |
| `1<<25` (33554432) | `SURF_NO_SMOOTH` | flat shading hook |
| `1<<26` (67108864) | `SURF_NO_FOG` | skip the fog term |
| `1<<27` (134217728) | `SURF_NO_BOUNDS_REJECT` | reserved |
| `1<<28` (268435456) | `SURF_NO_BSP_CUTS` | skip this brush in the collision pass (cosmetic geometry opts out) |
| `1<<29` (536870912) | `SURF_ZONE_HACK` | reserved |
| `1<<30` (1073741824) | `SURF_INVISIBLE_OCCLUDER` | reserved |
| `1<<31` (2147483648) | `SURF_FORCE_VIEW_ZONE` | reserved |

Bits 0-2 are unallocated and **must stay that way**: only `flags=8` appears in
the shipped worlds, so nothing written today can collide with 1<<5 and above.
The flag values are duplicated as `#define`s in `GameData/Shaders/Surface.fs`;
if you renumber a bit you must change both places. `tests/Surface.test.cpp`
parses the shader and asserts every `SF_*` equals the corresponding `SURF_*`,
so a renumber that misses the shader fails the build rather than shipping a flag
that silently does nothing.

Flags marked *reserved* are stored, exported and round-tripped, but the renderer
does not act on them yet.

#### Where each flag is applied

A brush's surface is resolved **per face**: face override, else brush-wide
`flags=`, else engine default. Each face is then drawn in **exactly one** pass -
the world pass, the sky (backdrop) pass, or the glow halo - and the filter for all
three is `FaceBelongsToPass` in `Source/World/OzOzoneLoader.cpp`, keyed off the
resolved face. Two consequences worth knowing when authoring:

* **`SURF_FAKEBACKDROP` on one face moves that face to the sky pass** and it is no
  longer drawn by the world pass. Painting it on the whole brush (`flags=8`, the
  form all 69 shipped backdrops use) is equivalent and cheaper.
* **`SURF_INVISIBLE` on one face hides that face only.** The brush stays pickable
  and selectable in the editor as long as any face is visible, which is how you get
  a face back.

### Skybox primitive

```
skybox <texPath> cx cy cz size [panU=] [panV=] [scale=] [topTex=]
```

Builds six **inward-facing** quads with UVs projected from each vertex's
direction relative to the cube centre, so the authored textures read as one
coherent projected sky rather than six stretched billboards. It is forced
`UNLIT + TWO_SIDED + NO_FOG + NO_BSP_CUTS` by the parser.

A skybox is **render-only**: it is filtered out of the CSG collision pass, so
subtracting the shell around it never removes the sky and never puts an
invisible wall around the player. The usual authoring is an `add box` room
shell, a `sub box` for the interior, and the `skybox` primitive inside it.

### Collision is AABB-only

The collision world is built by `CsgProcessor` (`Source/Physics/OzBsp.hpp`), which
consumes **axis-aligned boxes only**. Three consequences:

- `sub` / `intersect` are **modifiers**, not shapes. They subtract from, or
  intersect with, solids that already exist. A world whose only brush is
  `sub box ...` has **zero** collision volumes — the brush still renders, so it
  looks like a floor while the player falls straight through it. Author floors
  as `add`, and carve openings with `sub`.
- **A rotated brush collides as a conservative AABB**, not as its rotated shape.
  The box is the tightest axis-aligned box that CONTAINS the rotated geometry
  (`RotatedAabbY`), so it over-covers the corners but never under-covers — a long
  beam rotated 90° still blocks along its whole length.
- A placed `Mesh.Static` / `Mesh.Skeletal` entity has **no collision at all**;
  the mesh entities are client-cosmetic. Use AngelEd's
  **Append AutoConvex Collision** to generate boxes for it.

#### The per-brush carve budget

`sub`/`intersect` have a budget (`MAX_SPLITS`, 4096 volumes) on how much ONE
brush may grow the solid list before the rest of its carving is abandoned, with
the untouched solids left **whole**. It is per BRUSH, so a level may hold as many
storage solids as it likes and each `sub` still carves.

Exceeding it means a cut the author drew is **missing** — the render mesh shows
the opening and the collision does not. The client logs an `OZ_WARN`, the editor
logs every occurrence and, since world-open now checks
`OzoneLoader::LastCsgOverflowCount()`, opens with a dialog rather than quietly.

Note: this budget used to be compared against the **total** solid count, so any
world with more than ~58 solids silently stopped carving altogether. If you are
reading an older revision of a level and a doorway does not work, that is why.


### Entities

| Entity | Syntax |
|---|---|
| Player start | `playerstart <x> <y> <z> <yaw>` |
| Pickup | `pickup <name> <x> <y> <z> [respawn=]` |
| NPC | `npc <type> <x> <y> <z> <yaw>` |
| Light | `light <point|spot|dir> <x> <y> <z> ...` |
| Zone | `zone <type> <minX> <minY> <minZ> <maxX> <maxY> <maxZ> <intensity>` |
| Static mesh | `Mesh.Static <path> <x> <y> <z> <yaw> [scale=] [tex=] [wind=1]` |
| Skeletal mesh | `Mesh.Skeletal <path> <x> <y> <z> <yaw> [scale=] [tex=] [anim=Clip] [speed=] [animfile=] [wind=1]` |
| Particle emitter | `ParticleEmitter <type> <x> <y> <z> [rate life speed ...] [tex=]` |
| Path node | `PathNode <name> <x> <y> <z> [radius=R] [next=a,b,c] [loop]` |
| Wind zone | `WindZone <minX> <minY> <minZ> <maxX> <maxY> <maxZ> <dirX> <dirY> <dirZ> <strength> [freq]` |

Zones are scripted by sibling `<name>.ozls` skyzone files. The axis convention
is **Z-up**.

#### Per-zone physics overrides (b80)

Any `zone` line may carry named kwargs that override
`oz::physics::PhysicsInfo` (defaults: gravity 18, jump 9, terminal 60,
water gravity 8 / drag 0.95 / swim-up 5, ladder speed 6, fly multiplier 1.5):

```
zone water ... gravity=12 jump=7 terminal=40 water_gravity=6 water_drag=0.9 swim_up=4
zone ladder ... ladder_speed=8
zone sky ... fly_mult=2.0
```

Accepted keys: `gravity=`, `jump=`, `terminal=`, `water_gravity=`,
`water_drag=`, `swim_up=`, `ladder_speed=`, `fly_mult=`. Also order-independent
named kwargs on entity lines: `name=`, `tex=`, `anim=`, `speed=`, `next=`,
`loop`, `radius=`, `scale=` (see authoring traps below for the positional
traps).

#### Light `.ozls` defaults (`: light`)

A `light` line's `name=` resolves an `.ozls` def of type `: light` — exactly
the way a zone's `name=` resolves its skyzone def. A world-scoped `torch.ozls`
sitting beside `World.ozone` is picked up automatically; the reference def is
`GameData/Global/Lights/Light.ozls`.

```
light point 0 0 4 255 180 90 1.2 12 name=torch flare=1
```

**The def is a defaults layer, not an override.** Anything the light line
authored always wins, which means:

| Key | Effect | When |
|---|---|---|
| `effect` | `LitLightEffect` (0 none, 1 watery, 2 torch, 3 fire, 4 lamp) | line omitted `effect=` |
| `flare` / `corona` | billboard halo flags | line omitted the matching kwarg |
| `period` | flicker cycle length, seconds | always |
| `cast_shadow` / `is_static` | `LightNode::castShadow` / `::isStatic` | always |
| `inner_cone` / `outer_cone` | spot cone **cosines** (`LightNode` stores cosines, not degrees) | always |

`intensity`, `radius`, `color`, `position` and `target` are **not** in the schema:
they are positional on every light line, so a def able to override them would
silently retune every already-saved level the next time the def was touched.

### Editor

AngelEd loads, edits and exports OZONE only. Export path:
`ExportToOzone()` in `AngelEd/Source/Main.cpp`. Reference worlds:
`GameData/Worlds/*/World.ozone`.

### Shared I/O helpers

`Source/PPGIO.hpp` provides `LoadFile`, `WSplitValue`, `WReadValue`, `ToFloat`
(save/config parsing only).

## Authoring traps

Each of these fails **silently** - the line parses, the value is just ignored,
or the wrong thing happens. All four shipped worlds hit at least one of them
before being fixed.

### `texSlot` — positional or keyword

Both spellings work and mean the same thing:

```
add box 0 0 0.75 4 1.5 4 0 3                # positional
add box 0 0 0.75 4 1.5 4 0 texSlot=3       # keyword
add box 0 0 0.75 4 1.5 4 0 surfTexSlot=3   # explicit surface-slot form
```

The positional slot is argument 8 for `box` (and 9 for `cyl`, 7 for `pyr`),
i.e. immediately after `<rot>`. A bare number in a trailing `#` comment can
still land in that slot - see *Trailing comments eat bare numbers*.

The keyword forms land in the brush's surface block instead, which is what the
**Surface Properties** dialog edits, and they work **per face**: a face's own
`texSlot`/`tex` overrides the brush-wide one, which overrides the auto-selection.
`tex=`/`surfTex=` (an explicit path) always wins over a slot number.

When no slot is given the loader auto-selects: `h < 1` -> slot 1 (floor
texture), otherwise slot 2 (wall texture). The brush still looks *plausible*,
just wrongly textured.

Tile slot order is the tileset filenames **sorted ascending**
(`oztex/tileset/*.png`), so `01_..`, `02_..`, `03_..` map to slots 1, 2, 3.
The loader sorts explicitly, so slots are stable across machines.

> Historical: the keyword form used to be parsed and then ignored — it neither
> set the texture nor round-tripped, and because it made the brush "non-default"
> it silently switched it to the per-face shader with the *auto-selected*
> texture. If a level's textures look wrong after a reload, that is why.

### `light directional <x> <y> <z>` is the light SOURCE

The authored point is where the sun/moon **is**; the light is aimed at the world
origin. A large positive `z` therefore puts the source overhead and lights the
ground. Writing it the other way round (target above the map) makes `lightDir`
point downward, every up-facing surface gets `NdotL = 0`, and the level renders
almost black regardless of how high you crank the intensity.

### `cyl` spans `z = [cz-h, cz]`, and `pyr` likewise

The authored `cz` is the **top** of the cylinder (or pyramid), not its centre and
not its base. A `cyl` with `cz=4 h=4` occupies `z` 0..4; `cz=3.5 h=2` occupies
1.5..3.5. `box` is **centre-based**, so the two primitives differ — this is the
one place that trips people up.

You can see this in the level files themselves: a "support column" with `cz=4
h=4` is commented *"spans z 0..4"*, a finial with `cz=3.5 h=2` *"spans z
1.5..3.5"*, and `World_endless_snow`'s cylindrical building (`cz=12 h=12`) is
capped by a roof slab at `cz=12.2 h=0.4` — which only lines up if the building
occupies `[0, 12]`.

**Known exception:** Dessert_Dreams's cacti and Dust_Ravine's rock pillars author
`cz == h`, which puts their *bases* on the ground under this convention. They are
correct as authored.

> Historical: the mesh used to be built base-at-zero while the collision AABB
> was centre-based and the exporter added a `+h/2` "compensation" for a
> re-centring the loader never did. All three disagreed, and the exporter's
> offset moved every cylinder half a height upward on **every** save/open cycle.

`cyl` supports **true taper**: `rTop` and `rBot` are independent, so cones and
frustums are real shapes. Side normals follow the frustum slope, so a cone shades
as a cone rather than as a tube. In AngelEd these are the Properties panel's
**Radius Top** / **Radius Bottom** / **Height** fields, and they round-trip.

### `pln` is an oriented quad

`pln <x> <y> <z> <nx> <ny> <nz> <dist>` builds a 10x10 quad whose normal is
`(nx, ny, nz)` (authored in OZONE's Z-up space) and which lies in the plane
`n . p = dist`, centred on the authored position. Vertical and sloped planes are
supported. Collision is the quad's conservative AABB.

### Always name your zones, and expect `zone sound` to be script-inert

Auto-generated zone names (`zone_sky_0`, `zone_water_0`, ...) are per-load
counters, so **every world collides on them**. `.ozls` defs live in one global
registry keyed by name; `LightningEntityRegistry::LoadWorldOverrides()` is called
on every `LoadWorld()` to re-point that map at the world being played. Without a
`name=` you are relying on that, and a mismatch means the level runs another
world's `set_ambient` / `set_fog` / `set_skybox` (or none at all).

Relatedly, `Main.cpp` deliberately skips `ZONE_SKY` and `ZONE_GAMEPLAY_SOUND`
when dispatching LightningScript `on_enter`/`on_exit`. A `zone sound` still works
for audio - `SoundManager::UpdateSoundZones` consumes its `GameplaySoundProfile`
- but its `.ozls` message will never fire.

### Trailing comments eat bare numbers

Brush lines ignore unrecognised tokens, so `# at 12 4` is harmless. But
`std::stof` is attempted first, and a bare number in a comment is swallowed as
the *next positional argument* - which for `box` is `texSlot`. Write comments
without numbers, or put them on their own line.

### `sub` never cuts the render mesh

`sub` and `intersect` carve **collision volumes only**. The visible brush stays
whole, so a subtracted pit is an invisible hole the player falls into. Build
openings from separate pier/lintel boxes and stepped runs instead.

## Previewing / screenshotting a world

`Angels95.exe --world <Name> --shot <out.png> [--shot-delay N] [--shot-res WxH]
[--shot-cam "x,y,z,yaw[,pitch]"]... [--shot-hud]` captures deterministic,
HUD-free frames. Coordinates are **OZONE Z-up** with yaw/pitch in degrees.
Shot mode skips the splash, title menu and audio, forces vsync/MSAA/pixel/
jitter/fog/head-bob off, hides the hotbar, HUD, crosshair, view-model, message
log and all authoring gizmos, and freezes the camera. `GameData/Launch.conf` is
skipped so `--shot-res` is authoritative. See `Source/Screenshot.hpp`.

## Level-scale conventions

The player collision box is **3 units tall with a 2-unit eye height**
(`PlayerMovement::Height` / `PLAYER_EYE_HEIGHT`), and `kStepHeight = 1.0`
(`Physics/PlayerPhysics.hpp`).

- Floor tops at a single shared `z`. Overlapping slabs z-fight visibly.
- Ceiling undersides at `4.0` or more.
- Stair treads rise exactly `1.0`, or they are not walkable.
- Cover props must top out at least `1.5` above their floor - anything within
  `1.0` of the feet is a step, not a wall.

Walls taller than that only *read* as taller; a 10-unit wall in a 30-unit room
is what made the old maps read as shafts rather than halls.
