'use strict';

// Authoritative reference: Source/World/OzoneParser.cpp, Source/World/OzOzoneLoader.cpp,
// Source/World/SurfaceFlags.hpp, Source/World/ZoneTypes.hpp, Source/World/LevelSettings.hpp.
// This file is editor metadata only; it mirrors the engine's accepted spelling.

const PRIMITIVES = {
  box: {
    sig: 'add box <x> <y> <z> <w> <h> <d> [rotation] [texSlot]',
    args: [
      'X position (OZONE Z-up)',
      'Y position (OZONE Z-up)',
      'Z position — **box is CENTRE-based**',
      'width',
      'height',
      'depth',
      'rotation in **degrees** (stored as radians)',
      'texSlot — 1-based index into `<world>/oztex/tileset/`; 0 = auto'
    ],
    doc: 'Box brush. `box` is the only primitive whose authored Z is its CENTRE.'
  },
  cyl: {
    sig: 'add cyl <x> <y> <z> <rTop> <rBot> <h> <slices> [rotation] [texSlot]',
    args: [
      'X position (OZONE Z-up)',
      'Y position (OZONE Z-up)',
      'Z position — **the TOP of the cylinder** (body spans `[z-h, z]`)',
      'top radius (`rTop != rBot` is a real taper)',
      'bottom radius',
      'height',
      'slices (default 16)',
      'rotation in **degrees**',
      'texSlot — 1-based tileset index; 0 = auto'
    ],
    doc: 'Cylinder brush. Unlike `box`, the authored `z` is the TOP and the body hangs down.'
  },
  sph: {
    sig: 'add sph <x> <y> <z> <r> [segments]',
    args: ['X position', 'Y position', 'Z position (centre)', 'radius', 'segments (default 16)'],
    doc: 'Sphere brush, centre-based.'
  },
  pyr: {
    sig: 'add pyr <x> <y> <z> <w> <d> <h> [texSlot]',
    args: [
      'X position',
      'Y position',
      'Z position — **the TOP of the pyramid** (body spans `[z-h, z]`)',
      'width',
      'depth',
      'height',
      'texSlot — 1-based tileset index; 0 = auto'
    ],
    doc: 'Pyramid brush. The authored Z is the TOP, same rule as `cyl`.'
  },
  pln: {
    sig: 'add pln <x> <y> <z> <nx> <ny> <nz> <dist>',
    args: [
      'X position',
      'Y position',
      'Z position',
      'normal X (Z-up)',
      'normal Y (Z-up)',
      'normal Z (Z-up)',
      'plane distance (`n · p = dist`)'
    ],
    doc: 'Oriented 10x10 plane quad. Its normal can point in any direction.'
  },
  skybox: {
    sig: 'skybox <texPath> <cx> <cy> <cz> <size> [scale=]',
    args: ['texture path (single whitespace-free token)', 'centre X', 'centre Y', 'centre Z', 'size', 'extra size multiplier (`scale=`)'],
    doc: 'Render-only sky cube. Forced `UNLIT | TWO_SIDED | NO_FOG | NO_BSP_CUTS` and never enters the CSG pass.'
  },
  heightmap: {
    sig: 'heightmap <imagePath> <texturePath> <x> <y> <z> <scale> <sizeX> <sizeY> <sizeZ>',
    args: ['height image path', 'terrain texture path', 'X position', 'Y position', 'Z position', 'height scale', 'size X', 'size Y', 'size Z'],
    doc: 'Terrain heightmap patch.'
  }
};

const ENTITIES = {
  playerstart: {
    sig: 'playerstart <x> <y> <z> [yaw]',
    args: ['X position', 'Y position', 'Z position', 'yaw in degrees'],
    doc: 'Player spawn. **Only `playerstart` #1 is ever used** — it alone decides a level\'s first frame.'
  },
  pickup: {
    sig: 'pickup <typeName> <x> <y> <z> [respawnSeconds]',
    args: ['pickup def name (e.g. `rifle_01`)', 'X position', 'Y position', 'Z position', 'respawn seconds (a BARE number — `respawn=` is not parsed)'],
    doc: 'Weapon / item pickup. The 4th number is the respawn time; a `respawn=` kwarg is ignored.'
  },
  npc: {
    sig: 'npc <defName> <x> <y> <z> [yaw]',
    args: ['pawn def name from `GameData/Pawn/PawnDefs/*.cfg`', 'X position', 'Y position', 'Z position', 'yaw (parsed but currently ignored)'],
    doc: 'NPC spawn. The def supplies the model, animation, health and speed.'
  },
  light: {
    sig: 'light <point|spot|directional> … [name=] [effect=] [flare=] [corona=]',
    args: [],
    doc: 'Light entity. The authored point for `directional` is the **SOURCE**, aimed at the world origin. `name=` resolves a `: light` def as a defaults layer. A `point` light with no radius is invisible (radius 0).'
  },
  zone: {
    sig: 'zone <subtype> <minX> <minY> <minZ> <maxX> <maxY> <maxZ> [intensity] [fog...] [ambient...] [name=] [physics...]',
    args: [],
    doc: 'Zone volume. Scripted by the sibling `<name>.ozls` skyzone def. Zones are authored Z-up; min/max order does not matter.'
  },
  portal: {
    sig: 'portal <targetWorld> <minX> <minY> <minZ> <maxX> <maxY> <maxZ> [spawnX spawnY spawnZ] [bidir]',
    args: [
      'target world name',
      'min X', 'min Y', 'min Z', 'max X', 'max Y', 'max Z',
      'spawn X', 'spawn Y', 'spawn Z',
      'bidirectional flag'
    ],
    doc: 'World transition volume. `bidir` only takes effect when the spawn triple is present — it is read from arg 9.'
  },
  trigger: {
    sig: 'trigger <defName> <minX> <minY> <minZ> <maxX> <maxY> <maxZ> [once] [team=N]',
    args: ['trigger def name', 'min X', 'min Y', 'min Z', 'max X', 'max Y', 'max Z', 'once flag', 'team id'],
    doc: 'Script volume. A short line (fewer than 6 box numbers) is dropped with no runtime effect. A `once` volume containing playerstart #1 fires at load.'
  },
  levelinfo: {
    sig: 'levelinfo <gameType> <maxPlayers> <respawnTime> <timeLimitEnabled> <timeLimitMinutes> <scoreLimit> <friendlyFire> <skyboxPath> [sideSkyboxPath] [gametype=]',
    args: [
      'game type id (0 singleplayer … see `gametype=`)',
      'max players — this is what the master browser advertises',
      'respawn seconds',
      'time limit enabled (0/1)',
      'time limit minutes',
      'score limit',
      'friendly fire (0/1)',
      'skybox texture path',
      'side skybox texture path',
      'game type key (wins over the positional id)'
    ],
    doc: 'Level metadata and game rules.'
  },
  particles: {
    sig: 'particles <type> <density> <speed> <r> <g> <b> <windX> <windZ>',
    args: ['type: none | snow | rain | void | psychic', 'density', 'speed', 'red', 'green', 'blue', 'wind X', 'wind Z'],
    doc: 'Ambient weather particle field.'
  },
  emitter: {
    sig: 'emitter <sound|music> <x> <y> <z>',
    args: ['kind — `music` selects MUSIC; anything else is SOUND', 'X position', 'Y position', 'Z position'],
    doc: 'Positional sound/Music emitter.'
  },
  'Mesh.Static': {
    sig: 'Mesh.Static <meshPath> <x> <y> <z> <yaw> [scale=] [tex=] [material=] [wind]',
    args: ['model path (single whitespace-free token)', 'X position', 'Y position', 'Z position', 'yaw in degrees', 'scale'],
    doc: 'Static prop. Props have **no collision** — append AutoConvex collision in AngelEd if the player must stand on it.'
  },
  'Mesh.Skeletal': {
    sig: 'Mesh.Skeletal <meshPath> <x> <y> <z> <yaw> [scale=] [tex=] [material=] [anim=] [animfile=] [speed=] [wind]',
    args: ['model path', 'X position', 'Y position', 'Z position', 'yaw in degrees', 'scale'],
    doc: 'Skeletal or vertex-keyframe prop. `anim=` selects an embedded clip, `animfile=` an `.ozanim` vertex clip.'
  },
  ParticleEmitter: {
    sig: 'ParticleEmitter <type> <x> <y> <z> [rate life speed spread sizeStart sizeEnd r g b rEnd gEnd bEnd gravity radius dirX dirY dirZ yaw] [tex=]',
    args: [
      'editor label only (fire / smoke / mist / snow …)',
      'X position', 'Y position', 'Z position',
      'rate', 'life', 'speed', 'spread (cone HALF-ANGLE in DEGREES)',
      'size start', 'size end', 'red', 'green', 'blue',
      'end red', 'end green', 'end blue', 'gravity', 'radius',
      'direction X', 'direction Y', 'direction Z', 'yaw'
    ],
    doc: 'Particle emitter. `spread` is a cone half-angle in **degrees**, not a unit-vector offset.'
  },
  PathNode: {
    sig: 'PathNode <name> <x> <y> <z> [radius=] [next=a,b,c] [loop]',
    args: ['node name (links use this, not an index)', 'X position', 'Y position', 'Z position', 'radius'],
    doc: 'Patrol waypoint. `next=` lists linked node names.'
  },
  WindZone: {
    sig: 'WindZone <minX> <minY> <minZ> <maxX> <maxY> <maxZ> <dirX> <dirY> <dirZ> <strength> [freq]',
    args: ['min X', 'min Y', 'min Z', 'max X', 'max Y', 'max Z', 'direction X', 'direction Y', 'direction Z', 'strength', 'frequency'],
    doc: 'Wind volume. `strength` is a **world-space displacement in units** — author 0.2-0.5. A value of 5-7 lays foliage flat.'
  },
  door: {
    sig: 'door <meshPath> <x> <y> <z> <yaw> [key=] [anim=] [locked=1]',
    args: ['model path', 'X position', 'Y position', 'Z position', 'yaw in degrees'],
    doc: 'Door entity. Note that `door` is not in the engine\'s entity-primitive skip list, so it also builds a fallback brush.'
  }
};

const ZONE_TYPES = {
  water: 'Water volume: applies the zone physics (gravity / drag / swim) and script hooks.',
  ladder: 'Ladder volume. Routed to `PlayerPhysics::UpdateLadderVertical`, which suspends gravity.',
  sky: 'Sky zone: owns fog, ambient and skybox for the level. Its `.ozls` def usually authors `fog_color`/`ambient_light`.',
  reverb: 'Reverb zone: drives `DspReverb` mix/decay.',
  sound: 'Gameplay-sound zone: `SoundManager::UpdateSoundZones` handles the music/ambience crossfade. `on_enter`/`on_exit` are suppressed for this type.',
  gameplay_sound: 'Alias of `sound`.',
  portal: 'Portal zone. (The editor writes `water` for this subtype — `WDLZoneTypeName` has no portal case.)',
  gravity: 'Gravity zone. (The editor writes `water` for this subtype — `WDLZoneTypeName` has no gravity case.)'
};

const LIGHT_TYPES = {
  point: 'Omnidirectional light. Args: `<x> <y> <z> <r> <g> <b> <intensity> <radius>` plus optional `[effect] [flare] [corona]`. A missing radius becomes 0, which renders nothing.',
  spot: 'Cone light. Args: `<x> <y> <z> <tx> <ty> <tz> <r> <g> <b> <intensity> <radius> <innerCone> <outerCone>` plus `[effect] [flare] [corona]`. Cones are **cosines**, not degrees.',
  directional: 'Sun light. The authored point is the SOURCE, aimed at the world origin. `(0,0,0)` warns and becomes a straight-down sun.'
};

const KWARGS = {
  flags: 'Surface flag bitmask (decimal only; `0x8` parses as 0). Hover the number to decode the bits.',
  texPath: 'Texture path. `texPath=` is the legacy brush-wide field and is honoured by both load paths.',
  tex: 'Texture path (surface). A path beats `texSlot`.',
  surfTex: 'Texture path (surface). A path beats `texSlot`.',
  texSlot: '1-based index into `<world>/oztex/tileset/`. `0` = auto (`h < 1` → slot 1, otherwise slot 2).',
  surfTexSlot: 'Per-surface tileset slot. Setting this forces the brush onto the per-face draw path.',
  material: 'Material NAME (not a path): `GameData/Materials/<Name>.ozmat`.',
  surfMaterial: 'Material NAME (not a path): `GameData/Materials/<Name>.ozmat`.',
  uvScaleU: 'Extra U multiplier on the generated UVs.',
  texScaleU: 'Alias of `uvScaleU`.',
  uvScaleV: 'Extra V multiplier on the generated UVs.',
  texScaleV: 'Alias of `uvScaleV`.',
  uvOffsetU: 'U offset on the generated UVs.',
  texOffsetU: 'Alias of `uvOffsetU`.',
  uvOffsetV: 'V offset on the generated UVs.',
  texOffsetV: 'Alias of `uvOffsetV`.',
  panU: 'Scroll speed in texture units/second. **Inert unless `SURF_PAN_U` is set.**',
  panV: 'Scroll speed in texture units/second. **Inert unless `SURF_PAN_V` is set.**',
  pan: 'Signed scroll pair `u,v` (space-free) or a single U speed.',
  alpha: 'Surface alpha 0..1.',
  cutoff: 'Alpha-test cutoff. **`SURF_MASKED` is inert unless this is greater than 0.**',
  glow: 'Glow colour `(r,g,b)` 0..1 (space-free). **`SURF_GLOW` is inert when all three are 0.**',
  surfGlow: 'Alias of `glow`.',
  glowScale: 'Emissive/halo strength multiplier.',
  surfAlpha: '**NOT PARSED.** `OzoneParser` does not read `surfAlpha=`; use `alpha=`. The exporter writes it anyway.',
  surfCutoff: '**NOT PARSED.** `OzoneParser` does not read `surfCutoff=`; use `cutoff=`.',
  surfGlowScale: '**NOT PARSED.** `OzoneParser` does not read `surfGlowScale=`; use `glowScale=`.',
  gravity: 'Zone gravity magnitude (default 18). Also sets `gravityVector` to `(0, -v, 0)`.',
  gravity_vector: 'Full gravity vector `(x,y,z)` in OZONE Z-up (space-free).',
  jump: 'Zone jump speed (default 9).',
  terminal: 'Zone terminal velocity (default 60).',
  water_gravity: 'Gravity while in water (default 8).',
  water_drag: 'Water drag (default 0.95).',
  swim_up: 'Swim-up speed (default 5).',
  ladder_speed: 'Ladder climb speed (default 6).',
  fly_mult: 'Fly speed multiplier (default 1.5).',
  effect: 'Light effect: 0 NONE, 1 WATERY, 2 TORCH, 3 FIRE, 4 LAMP. The tint is applied by the shader.',
  flare: 'Billboard halo (0/1).',
  corona: 'Wider, dimmer halo (0/1).',
  name: 'Zone script-hook name (resolves the sibling `<name>.ozls`), or a `: light` def name used as a defaults layer.',
  scale: 'Model scale (or extra skybox size multiplier on a `skybox` line).',
  anim: 'Embedded skeletal clip name (e.g. `Idle`).',
  animfile: 'External vertex-keyframe clip path (`.ozanim`).',
  speed: 'Animation playback speed multiplier.',
  wind: 'Mark the mesh as wind-affected. Requires an enclosing `WindZone` and `wind=1`.',
  radius: 'Emitter radius, PathNode radius, or light radius (positional on a light line).',
  next: 'Comma-separated list of linked PathNode names.',
  rate: 'Particle spawn rate (particles/second).',
  life: 'Particle lifetime in seconds.',
  spread: 'Emitter cone **HALF-ANGLE IN DEGREES**.',
  sizestart: 'Particle start size.',
  sizeend: 'Particle end size.',
  rend: 'Particle end red.',
  gend: 'Particle end green.',
  bend: 'Particle end blue.',
  dirx: 'Emitter direction X (OZONE Z-up).',
  diry: 'Emitter direction Y (OZONE Z-up).',
  dirz: 'Emitter direction Z (OZONE Z-up).',
  yaw: 'Emitter yaw.',
  key: 'Door key name.',
  locked: 'Door locked flag. `locked=abc` throws out of the parser — one of the few unguarded reads left.',
  team: 'Trigger team filter.',
  gametype: 'Game type key (e.g. `coop`, `campaign`, `deathmatch`). Wins over the positional id.'
};

// Source/World/SurfaceFlags.hpp. flag values are decimal.
const FLAG_BITS = [
  [1, 'SURF_OCCLUDER', 'portal-plane occluder (CPU only)'],
  [2, 'SURF_QUANTUM', 'solid only when not looked at (CPU only)'],
  [8, 'SURF_FAKEBACKDROP', 'painted backdrop, drawn by the sky pass at gain 0.55'],
  [16, 'SURF_COLLISION_PROXY', 'AutoConvex box: collides, never drawn in game'],
  [32, 'SURF_INVISIBLE', 'face not drawn (still collides)'],
  [64, 'SURF_MASKED', 'alpha test — inert unless `cutoff > 0`'],
  [128, 'SURF_TRANSLUCENT', 'alpha blend, no depth write, still fogs'],
  [256, 'SURF_ALPHABLEND', 'alpha blend, skips fog'],
  [512, 'SURF_MODULATED', 'multiply blend'],
  [1024, 'SURF_TWO_SIDED', 'draw back faces'],
  [2048, 'SURF_UNLIT', 'flat albedo, no ambient'],
  [4096, 'SURF_FAKE_LIT', 'ambient-only lighting (not the same as UNLIT)'],
  [8192, 'SURF_SPECIAL_LIT', 'reserved'],
  [16384, 'SURF_GLOW', 'emissive + additive halo — inert unless glow colour is non-zero'],
  [32768, 'SURF_PORTAL', 'sky portal: the face shows the sky-zone view'],
  [65536, 'SURF_MIRROR', 'reserved (planned)'],
  [131072, 'SURF_ENVIRONMENT', 'reserved'],
  [262144, 'SURF_PAN_U', 'scroll U — inert unless `panU` is non-zero'],
  [524288, 'SURF_PAN_V', 'scroll V — inert unless `panV` is non-zero'],
  [1048576, 'SURF_SMALL_WAVY', 'reserved'],
  [2097152, 'SURF_BRIGHT_CORNERS', 'reserved'],
  [4194304, 'SURF_DIRTY_SHADOWS', 'reserved'],
  [8388608, 'SURF_SHADOW_HI', 'reserved'],
  [16777216, 'SURF_SHADOW_LO', 'reserved'],
  [33554432, 'SURF_NO_SMOOTH', 'shader hook (currently a no-op)'],
  [67108864, 'SURF_NO_FOG', 'skip fog'],
  [134217728, 'SURF_NO_BOUNDS_REJECT', 'reserved'],
  [268435456, 'SURF_NO_BSP_CUTS', 'excluded from the collision pass'],
  [536870912, 'SURF_ZONE_HACK', 'reserved'],
  [1073741824, 'SURF_INVISIBLE_OCCLUDER', 'reserved'],
  [2147483648, 'SURF_FORCE_VIEW_ZONE', 'reserved (not authorable via `flags=` — stoi range)']
];

const FACES = {
  px: '+X face (engine Y-up naming)',
  nx: '−X face',
  py: '+Y face',
  ny: '−Y face',
  pz: '+Z face',
  nz: '−Z face'
};

module.exports = {
  PRIMITIVES,
  ENTITIES,
  ZONE_TYPES,
  LIGHT_TYPES,
  KWARGS,
  FLAG_BITS,
  FACES
};
