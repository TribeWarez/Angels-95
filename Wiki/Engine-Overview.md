# Engine Overview

## Source Tree

Layout as of **b77+** (source tree reorganized; legacy WDL dropped):

```
Angels95/
  Source/
    Main.cpp              # Client entrypoint, game loop, rendering
    Core.hpp              # Engine init, splash, menu, world loading, render loop
    Data.hpp              # Game globals, models, sounds, flags
    Settings.hpp          # Runtime settings: fog, particles, debug, resolution
    ClientSettings.hpp    # Runtime client settings (System/Angels95.ini)
    DebugFlags.hpp        # Debug toggles
    WindowsCompat.hpp     # Win32/raylib name collision fixes (CloseWindow, etc.)
    Log.cpp/.hpp          # Logging system
    IniConfig.hpp         # INI config file reader
    PPGIO.hpp             # Save/config I/O helpers
    WorldState.hpp        # Scene/camera transition + toggle-flag state
    OzPack.cpp            # Standalone packer/unpacker CLI tool
    Screenshot.hpp        # --shot headless screenshot mode

    Client/
      Client.cpp/.hpp     # Client networking layer
      MasterList.hpp      # Master server address loader + default URL
      ProtocolHandler.hpp # angels95:// OS protocol handler (HKCU / .desktop)
      JoinUri.hpp         # angels95://join/<ip>:<port> URI parsing/validation

    World/                # OZONE world module (post-b77 extraction)
      LevelSettings.hpp   # Level metadata, game rules, skybox paths
      ZoneTypes.hpp       # ZoneType enum, ZoneEnvOverrides, GameplaySoundProfile
      ZoneManager.cpp/.hpp# Zone volume/portal storage + runtime queries
      OzoneParser.cpp/.hpp# Pure text-to-primitive OZONE parser (no raylib)
      OzOzoneLoader.cpp/.hpp # Geometry, heightmap, spatial chunk provider
      OzoneFrustum.cpp/.hpp  # Frustum culling math
      OzoneHeightmap.cpp     # Terrain mesh generation + height sampling

    Server/
      Server.cpp          # Dedicated server; HTTP API; --bind/auth/admin flags
      GameState.cpp/.hpp  # Server-side world state, NPCs, pickups, saves
      ServerHttp.cpp      # HTTP /map + /status endpoints
      ServerInternal.hpp  # Shared server internals

    Server/Network/
      Network.cpp/.hpp    # Custom UDP protocol, packed structs, LAN discovery

    Server/Master/
      Master.cpp          # AngelMaster daemon
      MasterClient.hpp    # Server -> master heartbeat uplink thread

    Network/
      NetworkSession.hpp  # Session/sequence state
      MasterProtocol.hpp  # Shared heartbeat + JSON list wire codec
      MasterHttp.hpp      # Minimal HTTP(S) client (WinHTTP on Windows, curl POSIX)

    Script/
      LightningScriptParser.cpp/.hpp   # LightningScript parser
      LightningScriptContext.cpp/.hpp  # Script execution context (VM)
      LightningEntityRegistry.cpp/.hpp # Entity type registry (.ozls)
      LightningEntityManager.cpp/.hpp  # Runtime entity management
      LightningEntityDef.hpp           # Entity definition structs

    Pawn/
      OzPawnSystem.cpp/.hpp  # Dynamic NPC system, FSM, projectiles, wind sampling
      Player.hpp             # Player state/capabilities
      PlayerMovement.hpp     # Movement physics (sprint/crouch/stance)
      PickupPawns.cpp/.hpp   # Networked-pickup collect loop
      Items.hpp              # Item definitions

    Pawn/AngelPlayer/        # Player behaviour modules (b78-b82)
      GameUi.cpp/.hpp        # GameUI object-bar bridge (data-only, never draws)
      SlotBar.cpp/.hpp       # SlotBar HUD renderer (atlas + slots, click-to-select)
      InventoryBehaviour.cpp/.hpp  # Inventory HUD/overlay + item/weapon hooks
      WeaponBehaviour.cpp/.hpp     # Fire/recoil/ADS + view-model clip triggers
      PlayerController.cpp/.hpp    # Toggles, vertical, game-over

    Physics/
      OzBsp.cpp/.hpp         # CSG AABB boolean processor
      WorldChunk.cpp/.hpp    # Spatial partitioning for collision
      PlayerPhysics.cpp/.hpp # Player collision resolution
      PhysicsInfo.hpp        # Per-zone physics overrides (oz::physics)

    Renderer/
      LitLightning.cpp/.hpp  # Lighting renderer (per-frame light pass)
      EngineBillboard.hpp    # Billboard sprite rendering (pickups, icons)
      TextSystem.hpp         # Text rendering system
      Video.hpp              # Video playback support
      WindShader.hpp         # Wind sway shader uniforms
      ViewModel.cpp/.hpp     # First-person weapon view-model
      CombatFX.hpp           # Combat effects

    Renderer/Mesh/           # oz::Mesh taxonomy (b74; namespaced vs raylib ::Mesh)
      Mesh.cpp/.hpp          # oz::Mesh base
      StaticMesh.cpp/.hpp    # Static mesh (OBJ/GLB)
      SkeletalMesh.cpp/.hpp  # Skeletal mesh with named clips (GLB/GLTF/IQM)
      AnimatedMesh.cpp/.hpp  # Vertex-keyframe .ozanim morph animation
      MeshCache.cpp/.hpp     # Shared model cache (intentionally leaked)

    Particle/
      OzParticleSimulationManager.cpp/.hpp  # 4096-particle pool (sim + render)

    Package/
      PackageAssetLoader.hpp # Runtime asset loading from .oz* packages
      OzPackage.hpp          # OzPackage format reader/writer (zlib/miniz)
      OzAssetMapper.cpp/.hpp # Engine/item texture mapper

    Package/Anim/
      OzAnimFormat.cpp/.hpp  # .ozanim text format (raylib-free)

    Audio/
      SoundManager.cpp/.hpp  # Single facade for every sound/music call
      DspReverb.hpp          # Schroeder reverb DSP (master mix)

    Menu/
      TitleMenu.hpp          # Title/home screen menu
      InternetBrowser.hpp    # Async Internet server browser
      SkillTree.hpp          # Ethereal skill tree (b78)
      SettingsMenu.hpp       # Settings menu

    UI/
      UiHandler.cpp/.hpp     # Win32 native menu bar (oz::ui::CreateNativeMenuBar)

    Renderer/plmpeg/         # pl_mpeg MPEG1 video decoder
    Renderer/rlights/        # raylib lights helper
    Renderer/raygui/         # raygui UI library
    miniz/                   # Vendored miniz (OzPackage zlib compression)

  AngelEd/                  # Level editor (Win32 only; Makefile errors out on Linux)
    Source/
      Main.cpp              # Editor entrypoint, toolbar, 3D viewport, selection
      Editor.hpp            # Editor state, camera, lighting, cached models
      Core/                 # EditorShell, EditorDispatcher, EditorState, EditorPanelState, EditorLog
      Subsystems/           # Placement, PropsApply, SurfaceOps, LevelState, History (real TUs)
                            #   + Selection, EntityOps, WorldIO, OzoneExport, WorldGraphBridge,
                            #     AnimEditing (still unity fragments, #included by Main.cpp)
      UI/                   # Win32 native panels; UiShell.cpp is the single unity TU
      Resources/            # AssetScope, PackageIO, AssetScan - shared by two panels each
      EditorIcons.cpp/.hpp  # Toolbar icon loader (AngelEd/UI/*.bmp)
      PPGIO.hpp             # Save/config I/O helpers (shared with Source/)
      raygui/               # Bundled raygui (dark.h, raygui.c/.h)
    UI/                     # Toolbar icon .bmp files
    Makefile                # Separate editor Makefile

  GameData/                 # Loose assets, worlds, saves
    Worlds/
      <WorldName>/
        World.ozone         # World description (OZONE format)
        Models/             # .obj/.glb models, textures, heightmap, Skybox.png
        Music/              # Background music
        oztex/              # Tileset + free-placement textures
    Global/
      PawnDefs/*.cfg        # Data-driven NPC definitions
      UI/GameUI.ozls        # Authored HUD object-bar def
      gun/                  # Weapon .ozls defs + FBX-derived .glb view-models
      sky/                  # Skybox textures (.dds)
    Saves/                  # Binary save files (gitignored)

  System/                   # Release directory (gitignored, assembled by scripts)
    Angels95.exe / AngelServ.exe / AngelEd.exe / AngelMaster.exe / OzPack.exe
    Data/*.oz*              # Packaged assets
    Cache/                  # Runtime temp cache (model extraction)
    angels95-serv.service   # VPS systemd unit (AngelServ/AngelMaster)
```

## Key Systems (b57–b82)

### GameUI / SlotBar object-bar HUD (b82)
`GameData/Tex/Engine/GameUI.ozls` (`EntityType::GAMEUI`) is the authored source
of the player's object bar: one atlas `texture =` plus a comma-separated
`slot_rects` stat (source-texture pixels, no spaces). `GameUi.{hpp,cpp}` is the
data-only bridge (`SlotCount()`, `SlotRect(i)`, layout stats); `SlotBar.{hpp,cpp}`
is the single renderer — it maps the atlas to the screen, fits each occupied
cell's entity icon, draws slot numbers/selection, and does click-to-select. Both
call sites (`LightningEntityManager::DrawHotbar` and
`InventoryBehaviour::DrawOverlay`) share it, each keeping a plain-rectangle
fallback when `GameUI.ozls` is absent.

### AngelPlayer behaviour modules (b78–b80)
The player is assembled from `Source/Pawn/AngelPlayer/` modules instead of one
monolithic block: `PlayerController` (toggles, vertical, game-over),
`WeaponBehaviour` (fire/recoil/ADS + view-model clip triggers),
`InventoryBehaviour` (HUD/overlay + item/weapon collect hooks) and the `GameUi`
bridge. Sprint (`Shift`) / crouch (`Ctrl`) and the resulting stance are
replicated to the server; `PlayerPhysics` + `PhysicsInfo.hpp` hold the movement
defaults.

### ZoneManager (b80/b82)
`ZoneManager` owns `ZoneVolumeNode`, `ZonePortal`, `PointRegion` and every
runtime zone query (`GetActiveZones`, `CheckZoneCollision`,
`CheckPortalCollision`, `UpdatePlayerRegion`). Zone entry/volume detection and
env-override merging go through `ZoneManager::Instance()`, not `PawnSystem`.
`ZoneEnvOverrides` (fog/ambient/reverb) layer lowest-priority-first so the
highest-priority zone wins; `GameplaySoundProfile` is consumed by
`SoundManager::UpdateSoundZones`. Per-zone physics (`gravity`, `jump`,
`terminal`, `water_*`, `ladder_speed`, `fly_mult`) override
`oz::physics::PhysicsInfo`.

### SoundManager facade (b57, refactored b82)
`Source/Audio/SoundManager.{hpp,cpp}` is the single entry point for every sound
and music call — no gameplay file touches raylib sound handles directly. It
owns world music, zone ambience, the reverb hook (`DspReverb` Schroeder reverb
attached to the master mix) and the script-sound cache. `play_sound` routes
through `SoundManager::PlayScriptSound` and is package-aware (`.ozsnd`
resolves). The old `g_prevSoundZone`/`g_defaultWorldMusic`/`g_ambienceHandle`
globals in `Core.hpp` are gone.

### Master HTTPS uplink (b59/b70)
`MasterHttp.hpp` speaks TLS with **no vendored crypto**: WinHTTP/Schannel on
Windows, `curl` on POSIX (the Windows Makefile links `-lwinhttp`). The default
master is `https://angels95.tribewarez.com/master` (client browser GET
`/master/api/servers`, server uplink POST `/master/api/heartbeat`). See
`Wiki/Master-Server.md`.

### LightningScript VM (b59)
The runtime became a real VM: `$flag<idx>` reads instance toggle flags,
assignments support RHS arithmetic (`$x = $x - 1`), `if (...)` accepts both
brace and `endif` styles, and `toggle_flag`/`jump` opcodes landed. Each
instance carries a per-instance stat resolver and its hotbar/equipment flags
persist across saves.

### Mesh taxonomy + vertex-keyframe animation + wind (b74)
`oz::Mesh`/`StaticMesh`/`SkeletalMesh`/`AnimatedMesh` unify the ad-hoc mesh
paths behind one cache (`MeshCache`, intentionally leaked so GL teardown never
runs after `CloseWindow`). `.ozanim` is a raylib-free text morph format
(`Source/Package/Anim/`); `AnimatedMesh` samples clips and CPU-uploads
positions. Wind-affected meshes (`wind=1`) draw with `Wind.vs` (height-weighted
sine sway); `oz::SetWindUniforms` uploads the sway params per draw.

### Screenshot mode (b82)
`--shot <out.png>` plus `--shot-delay`, `--shot-res`, `--shot-cam` (repeatable,
OZONE Z-up, yaw/pitch degrees) and `--shot-hud` capture deterministic,
HUD-free frames. It suppresses splash/menu/audio, forces vsync/MSAA/pixel/
jitter/fog/head-bob/debug off, freezes the camera via `isNoClip`, and exits when
every camera is captured. See `Source/Screenshot.hpp`.

## Key Architecture Points

### Single g++ invocation, no CMake
Every target is compiled and linked with a single `g++` command. Flags: `-O3 --std=c++20`. The `Makefile` defines per-target object lists manually.

### No raylib dependency for server
`AngelServ` (dedicated server) uses raw POSIX/Winsock sockets only. No raylib headers or libraries are linked. The `SERVER_CXX` compiler is used for server-side code.

### WindowsCompat.hpp
Included early in any file that touches both raylib and `winsock2.h`. Renames conflicting Windows symbols (`CloseWindow`, `ShowCursor`, `Rectangle`, `DrawText`) before `#include <windows.h>`, then `#undef`s them.

### using namespace std
Used in `PPGIO.hpp`, `Data.hpp`, `TextSystem.hpp`.

### #pragma pack(push,1)
Used for all network packet structs to ensure binary compatibility between client and server.

## Package System (OzPackage)

Assets can be distributed as loose files in `GameData/` OR packaged into `.oz*` containers:

| Extension | Type | Magic | Contents |
|---|---|---|---|
| `.ozpak` | Generic | OZPK | Models, scripts, shaders |
| `.oztex` | Texture | OZTX | PNG textures |
| `.ozsnd` | Sound | OZSD | WAV/MP3/OGG |
| `.ozmux` | Music | OZMX | WAV/MP3 |
| `.ozone` | World | OZWN | World files |

Loading is handled by `PackageAssetLoader::Instance().Init()` which scans `System/Data/*.oz*`. Use `*WithFallback` wrappers (`LoadTextureWithFallback`, `LoadModelWithFallback`, etc.) which check the filesystem first, then search packages.

Models require a temp-file cache in `System/Cache/` because raylib has no `LoadModelFromMemory`.

Packaging is done via `.\build-data.ps1` which uses `OzPack.exe`.

## Particle System

`Source/Particle/OzParticleSimulationManager.{hpp,cpp}` replaces the legacy
50-particle `ParticleDemon` with a pool of **4096 particles** (b74). Emitters
are authored as `GameEngine.ParticleEmitter` nodes (or OZONE
`ParticleEmitter <type> ...` lines) and boxed in `PawnSystem`. The manager is
ticked once per frame in `Core.hpp` *after* the weapon/NPC/projectile sim loop
and only reads emitter defs — it never calls gameplay code, so particles cannot
interrupt weapon mechanics or NPC ticks. Client-only/cosmetic (not networked).

## Pawn System

NPC definitions are loaded from `GameData/Pawn/PawnDefs/*.cfg` (name, speed, aggroRange, attackRange, damage, maxHealth, sprite_path, scream_path). Fallback hardcoded defs: Walker, Skaarj, Brute, Floater.

FSM states: IDLE, PATROL, CHASE, RETURN, DEAD (no ATTACK state — see
`Source/Pawn/OzPawnSystem.hpp:25`). State transitions fire `.ozls` actions
(`on_patrol`/`on_chase`/`on_return`/`on_death`) only when a pawn-named def
exists (e.g. `Walker.ozls`). Server-owned NPCs run the FSM server-side and are
networked (`npc_type` in `NpcStateUpdateData`, `networkControlled=true`).

Animated pawns: `.cfg` keys `mesh_type` (`static`/`skeletal`) + `anim_*` clips.
A skeletal pawn uses `oz::SkeletalMesh` (GLB/GLTF/IQM) and maps `PawnState` to
a clip; yaw tracks movement for skeletal pawns, static/billboard pawns face the
camera.

NPCs attack via melee range check (no ranged NPC fire). Projectile damage is
detected client-side in `PawnSystem::UpdateProjectiles()` (collision radius
1.5) and server-side in `GameState::tick_projectiles()` (radius 2.0).

## Weapon System

Weapons are data-driven `.ozls` entities of type `weapon`. Two types:

- **Ranged** — fires `ProjectileNode` objects with configurable speed, spread, damage, lifetime. Supports ammo (`magazine` stat), reload (`reload_time`), and cooldown-based auto-fire.
- **Melee** — forward range check using `reach` stat; no projectile spawn. Runs `on_swing`/`on_hit` script actions.

Key source files:

| File | Role |
|---|---|
| `Source/Script/LightningEntityManager.cpp` | `FireSelectedWeapon()` — reads weapon stats, handles ammo/reload/cooldown, runs `on_fire`/`on_swing`/`on_reload` actions, dispatches ranged (projectile) or melee (range check) logic |
| `Source/Pawn/OzPawnSystem.cpp` | `SpawnProjectile()`, `UpdateProjectiles()` (movement + gravity + pawn collision), `DrawProjectiles()` |
| `Source/Server/GameState.cpp` | `spawn_projectile()`, `tick_projectiles()` — server-authoritative projectile simulation with NPC and player collision |
| `Source/Pawn/AngelPlayer/WeaponBehaviour.cpp` | `FireWeapon()` — camera ray, trigger, recoil, muzzle flash, crosshair, ADS, view-model clip triggers |
| `Source/Renderer/ViewModel.cpp/.hpp` | First-person weapon view-model: attached to the camera, Idle/Fire/Reload clips + procedural recoil kick / reload dip |

Weapon `stats` may set `viewmodel_mesh`, `viewmodel_texture`,
`viewmodel_offset/rot/scale`, `recoil`, and per-weapon `projectile_mesh` /
`projectile_submesh` / `projectile_scale` / `projectile_color` for drawn
tracers. FBX art is converted via `tools/convert_fbx.ps1` (FBX2glTF) and
`tools/merge_glb_anims.py` folds Fire/Reload/Idle clips into one `.glb`.

Pickup types come from the LightningScript entity registry (`.ozls` definitions).

## Known Editor Gaps

The editor is covered in detail by `Wiki/Editor-Usage.md`; the remaining
limitations (as of b82):

- Render meshes are not CSG-carved (CSG booleans process collision volumes only)
- Undo/redo covers world state only; camera and selection are not restored
- No test-play save prompts ("Reload world from playtest changes?")
- Lighting effects (watery, torch, fire, lamp) are UI-only — not rendered in the viewport
- Model/Texture preview rendering requires the raylib viewport to be focused
