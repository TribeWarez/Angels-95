# Engine Roadmap — Gap Analysis & Priorities

Status as of **b101** (delivery logs for b57–b101 below). This page is the living
gap analysis for the Angels95 / AngelTech engine: what exists, what is missing
compared with a conventional game engine, and the prioritized plan to close the
gaps. Each entry lists the `file:line` evidence so work can start without
re-auditing.

Legend: **[0]** security/correctness, **[1]** engine fundamentals,
**[2]** multiplayer completeness, **[3]** presentation, **[4]** tooling/UX.

---

## b83–b101 delivery log

### Tier 1 — engine fundamentals
- **Sky seams closed** by deriving V from world elevation in `Sky.fs`. The six
  skybox quads disagreed on their per-face V mapping (33–40 colour levels at a
  corner, measured with a pixel scan); corners fell to 1–5 levels and the cap
  band from 50 to 1. Legacy panoramas now sample their upper half only.
- **Generated content textures** with reproducible, deterministic PowerShell
  generators: `tools/gen_sky_textures.ps1` (sky gradients/caps) and
  `tools/gen_water_textures.ps1` (`water_pool`, `waterfall_streaks`, `foam`).
- **`tools/worldcheck` is part of the gate**: `make test` runs it over every
  shipped world with `--headroom-all` and `--asset-refs`, and now **fails** on a
  failing suite instead of grepping the log for `FAIL`.

### Tier 3 — presentation
- Campaign content: `World_endless_snow` (b83/b84), the EngineTest desert path
  (b93), the `Aethelgard_Ruins` mission world, SilentPines Gorge and Dusk Falls
  (b101). All-brush terrain, 0.9 risers, impact foam at every cascade.
- Surface decals and brush-wide surface scope (b89/b94); sky/water authoring
  recipes documented in `Wiki/Outdoor-Maps-Changes.md`.

### Tier 4 — tooling/UX
- Editor layer tree (Core/Subsystems/Resources/UI) and the event bus (b89/b90).
- Responsive title menu, server browser, character select, `--world` (b97);
  build-id versioning (`Source/Version.hpp`) and TDM team rules (b98).
- Issue templates and the README/docs refresh (b101).

## b57–b82 delivery log

### Tier 0 — security & correctness
- **Complete at b56** (see Tier 0 roadmap below). b59 adds the HTTP Bearer gate
  (`--auth-token`/`OZ_AUTH_TOKEN`) and `FILE_TRANSFER` reject-notice.

### Tier 1 — engine fundamentals
- Per-zone physics overrides (`gravity`/`jump`/`terminal`/`water_*`/
  `ladder_speed`/`fly_mult`) via `PhysicsInfo` + `ZoneEnvOverrides` (b80).
- `ZoneManager` extraction — zone volumes, portals, env-override merging live
  outside `PawnSystem` (b80/b82).
- Client settings persistence applied before `InitWindow` (VSync/MSAA/window
  size honoured at creation).

### Tier 2 — multiplayer completeness
- Scene sync, ammo sync, weapon grants, typed network NPCs with server-owned
  AI (b58); NPC death/respawn + broadcast, pickup `respawn_time` (b58).
- Server world seeding from `World.ozone`, `PlayerData.dat`, V2 world saves
  with autosave (b59/b61); join loads the server's map (b72); network NPC sync
  keyed by (world, npc, partition) — no pawn explosion after map switches (b73).
- Admin `COMMAND` (list/say/kick, b59/b61); stance replication (b78–b80).
- Internet discovery: HTTPS master uplink (WinHTTP/curl, no vendored crypto),
  default master `https://angels95.tribewarez.com/master`, working Internet
  browser (b61/b70); `--bind` + anti-spoof `publicip` for VPS (b65).

### Tier 3 — presentation
- First-person weapon view-models with Idle/Fire/Reload clips + procedural
  recoil (b74); FBX→GLB pipeline (b54+).
- Vertex-keyframe `.ozanim` morph animation + skeletal mesh clips (b74); wind
  sway shader (`Wind.vs`) (b74).
- Particle system replaced by a 4096-particle `OzParticleSimulationManager`
  pool (b74).
- Audio: `DspReverb` on the master mix (b57); `SoundManager` facade with zone
  ambience/reverb handling (b82).
- Skybox rendering/packaging overhaul — levelinfo priority, package `.dds`,
  editor viewport skybox (b63–b68); looping title video (b57).

### Tier 4 — tooling & UX
- Editor: full-document undo/redo (b81), world texture import (b81), Script
  Manager (b60), def-aligned Entity Properties (b60), animation authoring tool
  (b74), asset-scope tree + pack-on-import (b82).
- CI: raylib 5.5 build fix (b75), per-binary release assets (b69).

---

## What the engine already has

- Server-authoritative world simulation on a dedicated server (`AngelServ`, no
  raylib dep) with raw-socket UDP networking (`Source/Network/Network.hpp`).
- CSG brush worlds in the OZONE format with a Win32 editor (`AngelEd`) and
  per-zone scripting (`.ozls`).
- Data-driven pawn/weapon/item systems via LightningScript entities
  (`Source/Script/`, `Source/Pawn/`).
- Package streaming for models/textures/sounds/music/worlds
  (`Source/Package/`).
- A working 3D forward renderer: CSG brushes, billboards, rlights
  directional/point/spot, particle effects, post-processing blits
  (`Source/Renderer/`).
- Internet server discovery: `AngelMaster` master server + `AngelServ`
  heartbeat uplink + client Internet browser (`Source/Master/`,
  `Source/Menu/InternetBrowser.hpp`; see `Wiki/Master-Server.md`).
- Build + test matrix (`make test`, CI on Linux + Windows).

---

## Gap matrix

### Rendering & content pipeline

| Gap | Detail | Evidence |
|---|---|---|
| **[3]** Shadows | `castShadow` flag parsed but never used by the light pass | `Source/Renderer/LitLightning.hpp` |
| **[3]** Materials | Diffuse-only; no normal/specular/PBR maps | `Source/Renderer/LitLightning.cpp` |
| **[3]** LOD / instancing | Not present; every renderable drawn every frame | `Source/World/OzOzoneLoader.cpp` |
| **[3]** Post shaders | Toon/Sobel/Line loaded but never applied | `Source/Core.hpp` |

*Closed since b56:* frustum culling (`Source/World/OzoneFrustum.*`),
vertex-keyframe + skeletal mesh animation and first-person view-models (b74),
4096-particle pool (b74), VSync/MSAA applied at window creation, looping title
video (b57).

### Physics & gameplay

| Gap | Detail | Evidence |
|---|---|---|
| **[1]** Collision is AABB-only | CSG brushes as AABBs; `MAX_SPLITS=64` | `Source/Physics/OzBsp.hpp` |
| **[1]** No collision APIs | No raycast, sphere/capsule cast, or query — gameplay can't reuse the BSP | `Source/Physics/` |
| **[1]** Position restore | Player slide solved by restoring pre-frame snapshot; no swept/continuous collision | `Source/Core.hpp` |
| **[3]** Rigid bodies | No physics engine at all (no joints, no ragdolls) | — |
| **[3]** Navmesh/AI perception | No true pathfinding — server PATROL follows `PathNode` graphs (b74), everything else is distance checks | `Source/Server/GameState.cpp` |
| **[3]** Input | No rebinding, no gamepad tuning, no mouse-look sensitivity setting | — |

*Closed since b56:* dual WDL/OZONE collision paths (WDL dropped b77), per-zone
physics overrides (b80).

### Audio & video

| Gap | Detail | Evidence |
|---|---|---|
| **[3]** Positional audio | Zero pan/attenuation calls; all sounds global | `Source/Audio/` |

*Closed since b56:* zone ambience/reverb via `SoundManager::UpdateSoundZones` +
`DspReverb` on the master mix (b57/b82); title video wired (b57).

### Networking & multiplayer

| Gap | Detail | Evidence |
|---|---|---|
| **[0/2]** Prediction/interpolation | NPC interpolation landed (lerp + shortest-arc yaw); remote-player prediction still open | `Source/Server/Server.cpp` |
| **[0/2]** Reliability | `sequence` is never set/checked (raw UDP, no acks) | `Source/Network/Network.cpp` |
| **[2]** Entity replication | NPCs keyed by (world, npc, partition) triples (b73); still no stable net IDs | `Source/Network/Network.hpp` |
| **[2]** Game modes | `levelinfo` maxPlayers/friendlyFire enforced (b58); gameType/timeLimit/scoreLimit still cosmetic; no scoreboards/timers | `Source/Server/GameState.cpp` |
| **[2]** Respawn | Server NPC death/respawn implemented (b58); player death/respawn flow still open server-side | `Source/Server/GameState.cpp` |
| **[x] Admin** | `COMMAND` (b59/b61): list/say/kick via `--admin-token`/`OZ_ADMIN_TOKEN`; ban list deferred (UDP identity is ip:port) | `Source/Server/Server.cpp` |
| **[x] Ping** | Real RTT (PING/PONG sequence-paired round-trip) | `Source/Network/Network.cpp` |
| **[x] Join UX** | LAN + Internet browsers; join/host ports functional from UI; `angels95://join` deep link (b60/b71) | `Source/Menu/TitleMenu.hpp`; `Source/Network/Network.cpp` |
| **[x] Internet discovery** | HTTPS master uplink (b70), default `https://angels95.tribewarez.com/master`, client browser with `/status` RTT | `Source/Network/MasterHttp.hpp`; `Source/Menu/InternetBrowser.hpp`; `Wiki/Master-Server.md` |
| **[2]** Reconnect | None — dropped client must restart | `Source/Network/Network.cpp` |
| **[x] Game stats** | Player persistence (b61): `Saves/PlayerData.dat`, saved on disconnect + 60s autosave + shutdown; world state V2 saves | `Source/Server/GameState.cpp` |

### Engine core & persistence

| Gap | Detail | Evidence |
|---|---|---|
| **[1]** Fixed timestep | Client runs a 60 Hz accumulator; server tick capped at 10 Hz (done); uncapped paths remain in `Main.cpp` | `Source/Main.cpp`; `Source/Server/Server.cpp` |
| **[2]** Save UX | Client save drops stats (hotbar names only) | `Source/Script/LightningEntityManager.cpp` |
| **[4]** Profiling/telemetry | None | — |
| **[4]** Crash reporting | None (no crashpad/minidumps) | — |
| **[4]** Mod loading | None; GameData is baked at pack time | `Source/Package/` |
| **[4]** Streaming | No async asset loading; home screen scans disk at boot | `Source/Main.cpp` |
| **[3]** Cutscenes/sequences | None | — |
| **[4]** Localization | String literals throughout | — |

*Closed since b56:* client settings persistence (`System/Angels95.ini` read
before `InitWindow`, saved on exit).

### Scripting (LightningScript)

| Gap | Detail | Evidence |
|---|---|---|
| **[2]** Timers/delays | No `wait`/timed actions; all script is synchronous | `Source/Script/LightningScriptContext.*` |
| **[2]** Data types | No arrays/strings; only flat fields | `Source/Script/LightningScriptParser.*` |
| **[2]** Functions/coroutines | No user functions or coroutines | `Source/Script/` |
| **[2]** Debugger | No breakpoints/step/trace | — |
| **[2]** Event bus | Actions fire ad-hoc; no publish/subscribe | `Source/Script/` |

### Editor (AngelEd)

| Gap | Detail | Evidence |
|---|---|---|
| **[4]** Multi-select / prefabs | Single-object ops only | `AngelEd/Source/Editor.hpp` |
| **[4]** Lightmap baking | none | — |
| **[4]** CI drift | CI builds editor with inline g++ commands, not the `AngelEd/Makefile` | `.github/workflows/ci.yml` |
| **[2]** Hardcoded pickups | `EditorPickupType` still not fully registry-mapped (Weapons tree landed b58) | `AngelEd/Source/Editor.hpp` |

*Closed since b56:* full-document undo/redo (b81), world texture import (b81),
Script Manager + def-aligned properties (b60), animation authoring tool (b74),
asset-scope tree + pack-on-import (b82).

### Engineering / CI

| Gap | Detail | Evidence |
|---|---|---|
| **[4]** `make test` in CI | CI only smoke-tests the server binary | `.github/workflows/ci.yml` |
| **[4]** Formatting/lint | No clang-format / clang-tidy / `.editorconfig` | — |
| **[4]** Sanitizers | No ASan/UBSan build variant | `Makefile` |

---

## Roadmap

### Tier 0 — security & correctness (RCE/OOB/trust bugs) — **complete (b56)**

- [x] Roadmap doc (this page)
- [x] S6 — clamp world-list JSON to `MAX_MESSAGE_SIZE` on join
- [x] S1 — clamp client `SCENE_UPDATE`/`CHAT` payload reads
- [x] S2 — ownership + clamps on `PLAYER_HURT`/`PLAYER_KILL`; drop spoofed
  client `PICKUP_COLLECTED`; clamp `WEAPON_AMMO`
- [x] S3 — finite/teleport validation on `PLAYER_UPDATE`; server-authoritative
  relayed health
- [x] S4 — auth race: client confirms on first non-challenge message; retry
  sends `CLIENT_AUTH`; server re-acks re-auth; idempotent `add_player`
- [x] S5 — explicit `COMMAND`/`GAME_STATE` handling (warn + drop)
- [x] Test coverage for the above

### Tier 1 — engine fundamentals

- [x] Real RTT ping (PING/PONG sequence-paired round-trip; client heartbeat at
  `Network.cpp`, HUD already renders `get_ping_ms`)
- [x] Enforced 10 Hz server tick (fixed-timestep accumulator, catch-up capped) +
  client fixed-timestep (60 Hz accumulator for movement physics/pawns/projectiles)
- [x] NPC interpolation on the client (smooth 2.5 Hz broadcasts via
  `prev`/`target` lerp + shortest-arc yaw)
- [x] Position/state validation moved into `GameState::update_player_position`
  (single choke point, finite + teleport clamp)
- [x] Client settings persistence (`LoadClientSettings` reads
  `System/Angels95.ini` before `InitWindow`; VSync/MSAA/window-size applied at
  window creation; saved on exit)
- [x] Frustum culling for renderables (camera-geometry planes + AABB test in
  `OzoneFrustum`/`OzOzoneLoader`)

### Tier 2 — multiplayer completeness

- [ ] Server-authoritative death/respawn + timers; scoreboard UI (NPC respawn done b58; player flow open)
- [ ] `levelinfo` → real game modes (DM timers, score limits; maxPlayers/friendlyFire enforced b58)
- [x] Kick/ban + server console (`COMMAND` implemented b61: list/say/kick behind --admin-token; ban list still open)
- [x] Player roster + LAN browser UI; honor the join port field (b57)
- [x] Internet discovery: `AngelMaster` master (UDP heartbeats + HTTP/JSON list, expiry/rate limits), `AngelServ` HTTPS uplink (b70), client Internet browser with direct status/RTT queries (see `Wiki/Master-Server.md`)
- [x] Server world population from world files + server saves (b59/b61: seed_world_entities, PlayerData.dat, V2 world saves, autosave); join loads the server's map (b72)
- [ ] Stable entity replication IDs (replace index-triples)
- [~] ACK/retry for critical messages — **pickup collect done**: refusals now name
  their reason and answer with a re-sync of the player's own world, state
  re-syncs on join / every 15 s / on a client `PICKUP_RESYNC`, and the collected
  node is actually hidden (`PickupNode::netId` + `ApplyPickupNetState`).
  Weapon fire still has no ack. Pinned by the loopback round-trip cases in
  `tests/GameState.test.cpp`.
- [ ] Client prediction + reconciliation for own player
- [ ] FILE_TRANSFER: rejected by design (b59 notice + rate limit); implement chunked transfer or drop the packet type

### Tier 3 — presentation

- [ ] Shadow pass (directional + point)
- [x] First-person weapon view-model (b74; Idle/Fire/Reload + procedural recoil)
- [x] Title video wired (b57: looping title video)
- [ ] Positional audio (attenuation/pan) — zone ambience/reverb landed via `SoundManager` (b57/b82)
- [x] 3D particles (b74: 4096-particle `OzParticleSimulationManager` pool)
- [x] Animation for NPCs + remote players: the three unrigged enemies (Walker/Skaarj/Floater) drive generated `.ozanim` Idle/Walk/Run/Death clips (`tools/gen_pawn_anims.py`), the Brute plays its embedded skeletal take, dropped guns idle-animate, and the Arcanist remote-player model is skeletal with measured idle/chase clips. A def still needs its own clips authored to animate.

### Tier 4 — tooling & UX

- [x] Editor undo/redo; [ ] multi-select; [ ] prefab library
- [ ] CI runs `make test` and a game-state suite; build editor via its Makefile
- [ ] clang-format/clang-tidy + `.editorconfig`
- [ ] ASan/UBSan CI build variant
- [ ] Save-slot UX + full player-data persistence
- [ ] FOV/ADS sensitivity settings

---

## Known-truth corrections (claims that are only half-true)

- "LAN discovery works": the server announces on UDP 27100 and the client's
  Multiplayer → Join page scans LAN on demand (b57) — but there is no passive
  background discovery; scans are manual.
- "Dedicated server standalone works": it does for world/NPC simulation, but
  player damage is client-authoritative for world hazards (`PLAYER_HURT`) — now
  closed by Tier 0 ownership checks.
- "INI config is supported": the **client** reads/writes
  `System/Angels95.ini` (window/VSync/MSAA/gfx/audio), and the **server** now
  reads `System/OzServer.ini` (`[Server]`/`[Auth]`/`[MasterServers]`) with CLI
  flags + env `OZ_AUTH_TOKEN`/`OZ_ADMIN_TOKEN` overriding it.
- "HTTP API is open": it still is unless `--auth-token` is set (b61 adds the
  Bearer gate); the token is not a security boundary on unencrypted LAN.
- "Linux server build": AngelServ/AngelMaster are standalone and POSIX-clean
  (MasterClient now includes its own socket headers); `--bind` on both controls
  the listening interface for VPS hosting. See `System/angels95-serv.service`.

---

*To add an item: file it under a tier, with `file:line` evidence, then tick it
off when landed. Keep this page the single source of truth for engine gaps.*
