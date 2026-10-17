# Angels95 — World/Game Hierarchy & Engine↔Game Separation

Status: **scaffold (Phase 0)**. This page is the single source of truth for the
World/Game hierarchy (SP Campaign / MP COOP) and for the boundary between the
OmegaTech engine base and the Angels95 game layer. It records the target design;
the Phase-0 pass only lays down the spec, the content scaffold and inert code
stubs, so read the **Roadmap** section for what is actually implemented.

Related: `Wiki/Engine-Overview.md`, `Wiki/LightningScript.md`,
`Wiki/World-Format-OZONE.md`, `Wiki/Engine-Roadmap.md`.

---

## 1. Why

The engine already has the right primitives but no hierarchy and no boundary:

- **No Game/Campaign object.** `GameType` (11 modes incl. `CAMPAIGN`,
  `COOPERATIVE`) is resolved and consumed **only by the server**
  (`Source/Server/GameState.cpp` `init_worlds`); the client reads a single rule
  (`LevelSettings.respawnTime`). Campaign/Coop exist as enum rows and UI labels.
- **No event/trigger/objective system.** Hooks are ad-hoc string
  `RunAction("on_xxx")` calls hardcoded at ~20 C++ sites
  (`Source/Script/LightningEntityManager.cpp`, `Source/Pawn/OzPawnSystem.cpp`,
  `Source/Main.cpp`). `Wiki/Engine-Roadmap.md` lists "Event bus" as a gap.
- **Gameplay is hardcoded in the engine**: pawn `.cfg` parsing
  (`Source/Core.hpp` OmegaTechInit), item effects (`Source/Pawn/Items.hpp`),
  weapon/spell discriminators (`LightningEntityManager::FireSelectedWeapon` /
  `CastSpell`), the zone-physics `switch` (`Source/Main.cpp`), skill bonuses,
  the equipment-slot table, 3-death game-over, and the portal "campaign" globals.
- **Worlds are flat.** `GameData/Worlds/<Name>/World.ozone` with `portal
  targetWorld` links; no ordering, no mission/mode binding, no graph.

## 2. Principles

1. The engine knows *worlds, entities, triggers, objectives, events, rulesets* —
   never *Sefirot, souls, guilds*. Game content lives in `.ozls` + OZONE.
2. Gameplay is **authored data** plus a thin game module (`Source/Game/`).
3. **One stat source.** Unify `.ozls` `EntityDef` and PawnDef `.cfg` into the
   single def system (Phase 5).
4. **Extend, don't special-case.** New world/gameplay concepts are boxed types +
   bus events + opcodes, never a new `if (type == X)` branch.
5. **Additive and backwards-compatible.** Existing worlds/defs keep loading; the
   new machinery is opt-in.

## 3. Layers

| Layer | Owns | Code |
|---|---|---|
| L0 engine core | renderer, physics, packages, OZONE, UDP transport | `Source/Renderer`, `Source/Physics`, `Source/Package`, `Source/World`, `Source/Network` |
| L1 engine systems | Script VM, BoxedType registry, EventBus, TriggerSystem, ObjectiveSystem, GameType rules | `Source/Script`, `Source/Events`, `Source/Boxed`, `Source/World/GameType.*` |
| L2 game layer (Angels95) | GameManager, game boxed types, game opcodes | `Source/Game` |
| L3 content | OZONE worlds, `.ozls` defs, packages | `GameData/` |

The engine may call **down** into L1 freely. L2 is reached only through the
narrow `GameManager` interface (mode + active mission + objectives), so the
engine never names a game concept.

## 4. Data model (`.ozls` types)

Phase 0 registers these `EntityType` values (metadata-only; no runtime consumer
yet) in `Source/Script/LightningEntityDef.hpp`. All custom fields are authored
inside a `stats { }` block (the `.ozls` body parser only knows a fixed set of
top-level keys; unknown top-level keys warn). String values are quote-free and
space-free — use comma-separated lists with no spaces.

| Type | File | Fields (stats) |
|---|---|---|
| `: game` | `GameData/Game/Angels95/Angels95.ozls` | `modes`, `start_sector`, `save_enabled`, `coop_max_players`, `checkpoint_policy` |
| `: campaign` | `Campaign.ozls` | `game`, `sectors` (ordered list) |
| `: sector` | `Sectors/<Sefirah>.ozls` | `numerology`, `theme`, `role`, `skybox`, `missions` |
| `: mission` | `Missions/<Sector>/<SubSector>.ozls` | `sector`, `world`, `mode`, `objectives`, `next` |
| `: objective` | `Objectives/*.ozls` | `type`, `target`, `count` |
| `: trigger` | `Triggers/*.ozls` | `topic`, `once`, `team` |
| `: event` | `Events/*.ozls` | (actions only) |

`SectorDef`/`MissionDef`/`CampaignDef`/`GameDef` are declared raylib-free in
`Source/Game/GameDef.hpp`; the parser that fills them is Phase 2.

## 5. Hierarchy map

The universe is 5 macro-zones (Sefirot nodes). **A Sefirah is a sector; a
sub-sector is an OZONE world** (one mission each), because the engine loads one
world at a time and already links worlds via `ZonePortal`.

| Sector (Sefirah) | Numerology | Theme | Role | Mission worlds |
|---|---|---|---|---|
| `Sefirah_1_TerranPrime` | 1 | `terran_prime` | hub | `Sektor_01_SilentPines`, `Sektor_02_MegacityGrid`, `Sektor_03_AegisBaseAlpha` |
| `Sefirah_2_HydrosisPrime` | 2 | `hydrosis_prime` | resource | `KryoArchipel_Severny`, `Refinery_Complex_02B` |
| `Sefirah_3_Oasis333` | 3 | `zeroth_desert` | open_world | `Aethelgard_Ruins`, `AlShaddai_Metropolis`, `Chasm_ForgottenMemories` |
| `Sefirah_4_CybrosisSubnet` | 4 | `cybrosis_subnet` | dungeon | `Digital_Purgatory`, `Phantom_Hubs`, `SoulEater_Vaults` |
| `Sefirah_5_ShatteredRealm` | 5 | `dmt33` | raid | `Citadel_AstralMind`, `NearDeath_Threshold`, `Esoteric_Altar_Chambers` |

The `Campaign.ozls` spine orders the sectors; each `: mission` carries `next=`
so the spine can later become a graph without changing the file shape.

## 6. Content scaffold layout

```
GameData/Game/Angels95/
  Angels95.ozls                 # : game
  Campaign.ozls                 # : campaign
  Sectors/Sefirah_{1..5}_*.ozls # : sector
  Missions/<Sector>/<Sub>.ozls  # : mission (14)
  Objectives/*.ozls             # : objective (examples)
  Triggers/*.ozls               # : trigger (example)
  Events/*.ozls                 # : event (example)
GameData/Worlds/<SubSector>/World.ozone   # 14 placeholder worlds
```

Placeholder worlds are floor + perimeter + `playerstart` + one directional light
+ one named sky zone; they are intentionally minimal and pass `worldcheck`.

## 7. Engine-generic mechanisms (Phase 1)

### 7.1 EventBus — `Source/Events/EventBus.*`
Topic-keyed publish/subscribe with a fixed payload (`Event`: topic, actor,
target, worldIndex, position, `i0..i3`, `f0..f3`, `s0`). The engine emits facts
at the existing hook sites; scripts subscribe with `actions { on_<topic> }`.
`TriggerEntityAction` becomes `EventBus::Emit`.

Topics: `world.load/unload`, `zone.enter/exit`, `trigger.enter/exit/interact`,
`entity.spawn/death/damage/kill`, `player.spawn/death/hurt/level_up`,
`pickup.collect`, `item.use/equip`, `projectile.hit`,
`objective.progress/complete/fail`, `mission.start/complete`, `wave.start/clear`,
plus the existing pawn FSM topics (`patrol`/`chase`/`return`/`death`).

### 7.2 TriggerSystem — `Source/Events/TriggerSystem.*`
New OZONE line `trigger <def> minX minY minZ maxX maxY maxZ [once] [team=]` →
`TriggerVolume`; the `: trigger` def declares the topic + filters. The engine
queries volumes per frame and emits `trigger.enter/exit`.

### 7.3 ObjectiveSystem — `Source/Events/ObjectiveSystem.*`
Keyed per-world/session state (`id`, `progress`, `target`, `complete`, `failed`)
with `Define/Add/Set/Complete/Fail`, `Serialize/Deserialize`. Persisted with the
world save; replicated in COOP (Phase 4).

### 7.4 BoxedTypeRegistry — `Source/Boxed/BoxedTypeRegistry.*`
Each world-entity type registers `{name, parse, inject, update, draw, serialize}`;
`OzOzoneLoader::InjectOzoneEntities` iterates registered types instead of its
hardcoded switch. Game types register from `Source/Game/GameBoxedTypes.cpp`.

### 7.5 LightningScript extensions
- Opcodes: `emit_event`, `objective`, `set_worldflag`/`get_worldflag`,
  `wait`/`timer`, `grant_item`, `spawn_projectile`, `apply_area_damage`,
  `give_xp`, `teleport`, `change_world`, `play_clip`.
- Conditions: `&&`, `||`, parentheses.
- `wait`/timers and the richer conditions close two `Engine-Roadmap.md` gaps.

## 8. Server-authoritative COOP (Phase 4)

- **Per-mission rules.** Bind `GameTypeInfo`/`MatchState` to the active
  mission/session instead of the current single global object that is overwritten
  by the last-loaded world (`GameState::init_worlds`).
- **Session model.** `{ missionId, worldIndex, mode, objectives[], party }`.
  The server owns objective state and mission progression; clients mirror.
- **Protocol (additive; no `PROTOCOL_VERSION` bump, new `MessageType`s are
  compatible by definition):** `OBJECTIVE_STATE` (S→C), `MISSION_CHANGE` /
  `CAMPAIGN_STATE` (S→C), `PARTY_STATE` (optional). Use the established
  forward-compatible tail pattern (gate on a `...SizeBase` constant).
- **Spawn/respawn.** Server-side `playerstart` resolution and player respawn
  (today `spawn_world_change` always sends `(0,0,0)`).
- **World scoping.** Fix global NPCs aggroing players in another world
  (`GameState::tick_npcs`, `part_idx == -1` path).

## 9. Engine → game migration table (Phase 5)

| Hardcoded today | Target |
|---|---|
| Pawn `.cfg` parser (`Core.hpp` OmegaTechInit) | `: pawn` `.ozls` defs (unify with `EntityDef`) |
| Item effects (`Source/Pawn/Items.hpp`) | `on_use`/`on_collect` scripts + generic opcodes |
| Weapon/spell discriminators (`LightningEntityManager`) | `on_fire`/`on_swing`/`on_cast` + `spawn_projectile`/`apply_area_damage` |
| Zone-physics `switch` (`Main.cpp`) | drive from merged `PhysicsInfo` data |
| Skill bonuses / equipment slots | data-driven bonus/slot defs |
| 3-death game-over / respawn | `: game`/`: mission` rules + `on_player_death` |
| Portal "campaign" globals (`Core.hpp`) | `: campaign`/`: mission` + `GameManager` |
| Contact damage (`Core.hpp`) | trigger/NPC event |

## 10. Build / packaging / editor wiring

- New `.cpp` files are raylib-free and wired into the root `Makefile`
  (`OBJS` + a compile rule each). They are **not** in `AngelEd/Makefile` or
  `ci.yml` yet because the editor does not use them; add them in Phase 2 if it
  does (the three-place rule).
- `build-data.ps1` packs `GameData/Game` → `System/Data/game.ozpak` (OZPK).
- Phase 2 must classify `: game`/`: campaign`/`: sector`/`: mission` as **global**
  defs (not world-scoped) in `LightningEntityRegistry::LoadWorldOverrides`.
- Phase 2 must wire the new types into AngelEd's `ScriptPanel` type→folder map
  (`Tex/Global/Gun` for weapons, etc.) and any `PropsPanel` schemas.

## 11. Invariants

- The `.ozls` parser stays non-throwing and line-numbered; new types must not
  add a throwing read.
- `EventBus`/`ObjectiveSystem`/`GameDef`/`BoxedTypeRegistry` stay raylib-free so
  a `test_events` suite can link them headlessly.
- Placeholder content must pass `worldcheck` and `worldcheck --asset-refs`.
- Do not renumber the on-disk `GameType` ids 0–6.

## 12. Roadmap

- **Phase 0 — landed.** Spec (this page), content scaffold, code stubs +
  build wiring, `game.ozpak`. No behaviour change.
- **Phase 1 — landed (engine generic).**
  - `EventBus` has wildcard (`"*"`) dispatch; `LightningEntityManager` subscribes
    one bridge that turns every emitted event into `on_<topic>` (dots →
    underscores) on the def named by `Event::s0`.
  - Engine emits at: zone-independent `world.load`, pawn FSM
    (`entity.chase/patrol/return/death`), `pickup.collect`, `player.hurt/death`,
    and `trigger.enter/exit`.
  - `ObjectiveSystem` gained cross-instance world flags (`set_worldflag` /
    `get_worldflag`).
  - New opcodes in `LightningScriptContext`: `emit_event`, `objective`,
    `set_worldflag`/`get_worldflag`, `give_xp`, `teleport`, `change_world`,
    `grant_item`, `apply_area_damage`, `spawn_projectile`, `play_clip`. Drained
    by `LightningEntityManager` (engine-generic + pawn) and by `Core.hpp`
    (camera/world-dependent: `teleport`/`change_world`/`apply_area_damage`/
    `spawn_projectile`).
  - Conditions gained `&&`, `||` and balanced parentheses.
  - `TriggerSystem` wired end-to-end: OZONE `trigger <def> minX..maxZ [once]`
    line → `TriggerNode` → `TriggerSystem`, queried per sim step by `Core.hpp`
    (`UpdateForPoint`) → `trigger.enter/exit` on the bus.
  - Tests: `tests/Events.test.cpp` (bus/objectives/triggers) + new
    `LightningScriptContext` cases.
  - **Follow-up landed:** `InjectOzoneEntities` now iterates `BoxedTypeRegistry`
    (engine registers its own types there); zone enter/exit is fully on the bus
    (the direct `TriggerZoneAction` call sites are gone — `zone.enter`/`zone.exit`
    dispatch `on_zone_enter`/`on_zone_exit` or fall back to `on_enter`/`on_exit`,
    and declarative zone env is applied by the bridge); `play_clip` plays a
    forced one-shot skeletal clip on matching pawns (`PawnSystem::PlayPawnClip`).
- **Phase 2 — core landed.** `GameManager` loads the hierarchy from the registry
  (`: game`/`: campaign`/`: sector`/`: mission`), resolves sectors/missions, and
  seeds `ObjectiveSystem` for the active mission. Client: `/campaign [mission]`
  console command starts a mission; the active mission persists in
  `GameData/Saves/Campaign.sav`. Test: `tests/GameManager.test.cpp`.
  **Open:** a full Campaign/Coop title-menu pane (today it is the console +
  existing world grid) and editor wiring for the new def types.
- **Phase 3 — SP content.** Real geometry for the 5 sectors / 14 worlds,
  objectives, portals → mission links.
- **Phase 4 — COOP (partial).** Per-world rules now live in `WorldState`
  (fixing the last-world-wins global bug; world 0 mirrors into the global match).
  COOP/Campaign worlds seed a `ClearArea` objective advanced by NPC kills, and
  the server broadcasts `OBJECTIVE_STATE` (~1 Hz) which the client mirrors and
  shows in the HUD. **Open:** party/lobby, coordinated mission transitions,
  server-side scripted objectives (server has no LightningScript runtime),
  objective advancement from pickups, and respawn/spawnpoints.
- **Phase 5 — migration.** Move §9 out of the engine; delete the `.cfg` parser.

*Keep this page current: when a phase lands, move its bullet to a "landed"
note and cite the `file:line`.*
