# Engine & Game Limits

The hard numbers the current build enforces. Most are compiled-in defaults; where a level or a
def can override one, the row says so. Everything here is checked against the source or the level
format, not remembered.

**Rule of thumb for authoring:** any limit marked *silent* fails without an error, so a level can
look correct and still be unplayable. Those are the ones `make test` / `worldcheck` guards.

## Player and movement

| Limit | Value | Where | Notes |
|---|---|---|---|
| Body height | **2.8** | `PlayerMovement::Height` | anchored at the **feet** |
| Jump rise | **2.25** | `jump^2 / (2 * gravity)` at the 9/18 defaults | |
| Clearance needed to jump | **5.05** | 2.8 + 2.25 | a gap below this is a **trap**: you can stand, not jump |
| Step height | **1.0** | `kStepHeight` | author **0.9**; 1.0 sits on a float boundary |
| Doorway opening | **5.5** | shipped convention | a lintel below 5.05 jams a jump in the doorway |
| Storey height | **8.0** floor-to-ceiling | shipped convention | 7.55 usable over a 0.45 slab; tallest standable prop is 2.5 |
| Gravity | **18** | `PhysicsInfo` default | per-zone `gravity=` |
| Jump impulse | **9** | `PhysicsInfo` default | per-zone `jump=` |
| Terminal velocity | **60** | `PhysicsInfo` default | per-zone `terminal=` |
| Water gravity / drag / swim-up | **8 / 0.95 / 5** | `PhysicsInfo` defaults | per-zone `water_gravity=`, `water_drag=`, `swim_up=` |
| Ladder speed | **6** | `PhysicsInfo` default | per-zone `ladder_speed=` |
| Fly / noclip multiplier | **1.5** | `PhysicsInfo` default | per-zone `fly_mult=` |
| Fall damage | **none** | by design | a long drop is survivable |

`worldcheck --headroom-all` audits the 5.05 rule over every floor plane and reports WARNINGS, not
errors, because the shipped worlds carry pre-existing instances.

## Collision and level geometry

| Limit | Value | Notes |
|---|---|---|
| Collision shape | **AABB only** | rotation is handled by a conservative rotated AABB, which over-covers corners but never under-covers |
| Heightmap lateral collision | **none** | a heightmap supplies a surface (ground clamp) only; a steep heightmap cliff is **walk-through** |
| CSG carve budget | **4096 splits per brush** | measured against the growth one brush causes, not the world total; `overflow_count()` is surfaced as a warning |
| AutoConvex budget | **2048 boxes** per action | the build **aborts** rather than returning a partial hull |
| AutoConvex cell size | `max(0.25, extent / 16)` | editor default |
| `sub` / `intersect` | **modifiers, not shapes** | they carve solids that already exist; a world containing only a `sub` has no collision at all *(silent)* |

## Rendering

| Limit | Value | Notes |
|---|---|---|
| raylib | **5.5** | system-wide on Linux/macOS; vendored build on Windows |
| Light slots | **32** per program | `MAX_LIGHTS` in `rlights.h`, `LitFog.fs` and `Surface.fs`; a test fails if the three disagree |
| Transient lights | **8** reserved | submitted first and never evicted; a level authored to 32 loses 8 the moment a weapon fires |
| Design budget | **24 authored lights** | 32 − 8 |
| Spot cone | **cosines**, not degrees | `inner_cone` / `outer_cone` |
| Fog density | **exponential rate per world unit** | not a linear multiplier; shipped values are 0.0022 – 0.007 |
| Skybox | 6 quads at **700** units, V from world elevation | capped at 55° elevation by the wall quads |
| Per-face surfaces | up to **6 draw calls** per brush | only when `NeedsPerFaceDraw()` is true |
| Shadows / lens flare | **none** | `castShadow` and the flare are stored but have no implementation; see `Wiki/Lighting-Plan.md` |
| God rays, PBR, refraction | **none** | water is a scrolling translucent sheet |

## Game rules and networking

| Limit | Value | Where |
|---|---|---|
| Protocol version | **2** (enforced) | `net::PROTOCOL_VERSION` — a mismatched `CLIENT_AUTH` is dropped |
| Wire player cap | **32** | `net::MAX_PLAYERS` |
| Advertised slots | **`levelinfo maxPlayers`** (shipped **8**) | the browser shows the level's number, not 32 |
| Hotbar | **8 slots** | `HOTBAR_SIZE`, mirrored by `slot < 8` on the server |
| Zone types | **6** — water, ladder, sky, reverb, gameplay-sound, portal | `ZoneType` |
| Objective ids | bounded by `net::kObjectiveIdMax` (`ObjectiveStateData::id` width) | a longer id is warned once at seed time and would truncate on the wire |
| Master listing expiry | **90 s** | heartbeats are 30 s with jitter |
| LAN discovery | **UDP 27100** | optional `;http=;map=;pw=` tail |
| Save formats | `TF.sav`, `POS.sav` (`x:y:z:level:world:`), `Script.sav`, `Campaign.sav` **v1** (v0 still loads) | an unknown version is rejected → fresh start |
| Player profiles | `System/PlayerProfiles.ini`, one `[Profiles]` section | |
| Auth payload | **v2**, `structSize`-guarded; every field client-claimed and sanitised | shorter payload = v1 peer, still accepted |

Matches run a fixed 20 Hz simulation tick with interpolated movement sub-steps; `dt` is clamped so
a stalled frame cannot tunnel the player through collision.

## Content and authoring constraints

| Constraint | Why |
|---|---|
| **No folder name may contain a space or a parenthesis** | the `.ozls` stats parser reads one whitespace-delimited token and OZONE has no quote parsing |
| **No quote parsing in OZONE** | `texPath="my tex.png"` splits at the space; only whole-token fields tolerate quotes |
| **A `.ozls` stat value is one token** | `slot_rects`, sound paths and vec3s must be written without spaces (quotes are only stripped by some consumers) |
| **`zone` needs an explicit, unique `name=`** | auto-generated names are per-load counters and collide across worlds *(silent)* |
| **A zone's `name=` must match its `.ozls` entity name** | the registry drops a def owned by another world, logging only at DEBUG *(silent)* |
| **Only `playerstart` #1 is used** | #2 onwards is ignored by the client |
| **A `once` trigger containing the spawn burns its fire on step 1** | `TriggerSystem` treats "inside on the first sample" as an enter |
| **`panU=`/`panV=`/`surfGlow=` need their flag bit** | the shader gates the uniform on the flag *(silent)* |
| **`flags=` replaces the whole set** | OR the bits; never overwrite a brush that carries `flags=8` |
| **`pln` is a fixed 10×10 quad** | use it as a patch, and give it `NO_BSP_CUTS` or it becomes invisible collision |
| **`cyl` / `pyr` `cz` is the TOP** | `box` is centre-based; the two conventions differ |
| **UPnP / port forwarding is not automatic** | the server announces `--public-ip`; the host must forward both ports |

## Where to change them

- Movement and physics defaults: `Source/Physics/PhysicsInfo.hpp` (per-zone overrides live in
  `Source/World/ZoneTypes.hpp`).
- Light budget: `Source/Renderer/rlights/rlights.h`, `GameData/Shaders/LitFog.fs`,
  `GameData/Shaders/Surface.fs` — **all three together**; `tests/Surface.test.cpp` enforces parity.
- Protocol and wire limits: `Source/Network/Network.hpp`.
- CSG and AutoConvex budgets: `Source/Physics/OzBsp.hpp`, `Source/Physics/AutoConvex.hpp`.
- Player slots per level: the world's `levelinfo` line.
