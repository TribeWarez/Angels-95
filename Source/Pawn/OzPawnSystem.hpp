#pragma once
#include "raylib.h"
#include "../Renderer/LitLightning.hpp"
#include "../Renderer/SpellElement.hpp"
#include "../Renderer/Mesh/MeshCache.hpp"
#include "../Physics/PhysicsInfo.hpp"
#include "../Physics/PlayerPhysics.hpp"
#include "../World/LevelSettings.hpp"
#include "../World/ZoneManager.hpp"
#include "../World/ZoneTypes.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>

// ---------------------------------------------------------------------------
// PawnSystem — dynamic entity / NPC manager
//
// Replaces the hard-coded EntityCount=10 / Enemys[10] array with a
// growable vector of Pawn objects, each with its own FSM state.
//
// States: IDLE -> PATROL (circle around spawn) -> CHASE (follow player) ->
//         RETURN (go back to spawn) -> PATROL
//
// Zone volumes, level portals and the point-region tracker are NOT owned here
// anymore — see World/ZoneManager.hpp. Level metadata lives in
// World/LevelSettings.hpp and the zone taxonomy in World/ZoneTypes.hpp; the
// includes above re-export them so existing callers keep compiling.
//
// Usage:
//   PawnSystem::Instance().Spawn({10,0,10}, "Walker");
//   PawnSystem::Instance().Update(playerPos);
//   PawnSystem::Instance().DrawAll(camera);
// ---------------------------------------------------------------------------

enum class PawnState : uint8_t {
    IDLE,
    PATROL,
    CHASE,
    RETURN,
    DEAD
};

// How long a dead pawn stays in the world so its `anim_death` clip is visible.
// A pawn is NOT deactivated on death (that made `PawnState::DEAD` unreachable in
// DrawAll); it is deactivated and despawned when this expires. The server-owned
// (multiplayer) path uses the same window.
inline constexpr float kPawnCorpseSeconds = 6.0f;

// TODO: Gun/WEapon PawnDefs for stat block
// Template definition shared between spawn calls; stored in an internal
// registry so Spawn() can be called by name.
struct PawnDef {
    std::string name;           // logical name, e.g. "Walker"
    float speed = 1.5f;
    float aggroRange = 6.0f;
    float attackRange = 1.5f;
    float damage = 10.0f;
    int maxHealth = 100;
    std::string sprite_path;    // texture path (convention: GameData/Pawn/Pawns/<name>.png)
    std::string scream_path;    // sound path (convention: GameData/Pawn/Pawns/<name>.wav)
    // Optional 3D model — when set (and loadable) the pawn renders as a real
    // model instead of a 2D billboard. Paths may be GameData-rooted or relative
    // to the .cfg that defined them.
    std::string model_path;
    std::string model_texture;  // optional diffuse map for model_path
    std::string baseDir;        // directory of the defining .cfg (path resolution)
    // World-space multiplier applied to model_path. Source packs are authored at
    // wildly different units (1.4 to 4944 across the shipped library), so this
    // trims a model whose GLB was not pre-normalised. Default 1 = as authored.
    float model_scale = 1.0f;
    // Mesh taxonomy / animation (GameEngine.Mesh.Skeletal).
    std::string mesh_type;      // "" | "static" | "skeletal"
    std::string anim_idle;
    std::string anim_patrol;
    std::string anim_chase;
    std::string anim_return;
    std::string anim_death;
    float anim_speed = 1.0f;
    // External vertex-keyframe clip (.ozanim) for a STATIC model. The shipped
    // enemy GLBs are unrigged, so this is the only animation path that works for
    // them: oz::AnimatedMesh samples the clip and uploads vertex offsets. Ignored
    // when mesh_type=skeletal (that asset already has its own clips).
    std::string anim_file;

    // Vertical placement of model_path. 0 = derive from the loaded mesh bounds
    // (-bounds.min.y * model_scale), so the model's LOWEST point sits on the
    // pawn's feet. A GLB whose pivot is at its centre used to be drawn with its
    // origin at the feet, i.e. hovering by half its height - which is exactly
    // what every un-rigged enemy did. Non-zero = explicit world-space override.
    float model_y_offset = 0.0f;
    // Cached auto offset, filled in by EnsurePawnMesh (bounds are only known
    // after the asset loads). 0 is a valid offset, so the explicit override is
    // the only thing that disables the derivation.
    float auto_ground_offset = 0.0f;
    bool  ground_offset_resolved = false;

    // Hovering enemies (Floater). Gravity and ground snapping are skipped and the
    // model bobs around its SPAWN height, so "floating" is deliberate instead of
    // a side effect of an ungrounded pivot. Authored in the .cfg.
    bool  hover = false;
    float hover_amplitude = 0.35f;   // metres
    float hover_speed = 1.6f;        // radians/second
};

// A single spawned pawn instance.
struct Pawn {
    uint32_t id = 0;
    bool active = false;
    PawnState state = PawnState::IDLE;
    PawnState prevState = PawnState::IDLE;

    Vector3 position{0, 0, 0};
    Vector3 velocity{0, 0, 0};
    Vector3 spawnPosition{0, 0, 0};
    float yaw = 0.0f;
    // Vertical physics state. Only driven when a world collision query is
    // injected into Update() (the client single-player path); without one the
    // pawn keeps the legacy flat movement (tests, editor, server-owned pawns).
    bool onGround = false;

    float speed = 1.5f;
    float aggroRange = 6.0f;
    float attackRange = 1.5f;
    float damage = 10.0f;
    int health = 100;
    int maxHealth = 100;

    float patrolTimer = 0.0f;
    float stateTimer = 0.0f;
    // Corpse lifetime, counted down while state == DEAD. Reaching 0 despawns the
    // pawn. Without this the pawn had to be deactivated the instant it died -
    // which made the DEAD branch of DrawAll (and therefore anim_death) dead code.
    float deadTimer = 0.0f;
    // Client-side path-node patrol cursor (index into PawnSystem::m_pathNodes).
    // The server already follows PathNode graphs; the client FSM used to ignore
    // them and walk a slow arc instead.
    int   pathTarget = 0;
    // Hover phase for `hover` defs (Floater). Visual only: it never writes
    // position.y, so collision/ground queries stay stable.
    float hoverPhase = 0.0f;

    Texture2D sprite;       // billboard frame (loaded externally)
    Sound scream;           // aggro sound (loaded externally)
    float lastScreamTime = -10.0f; // last time the scream was played (aggro cooldown)
    std::string defName;    // name of the pawn definition (e.g., "Walker", "Skaarj")
    int scriptInstanceIndex = -1; // LightningEntityManager instance index, -1 = none
    bool networkControlled = false; // true = AI/position owned by the server; local FSM skipped
    // Server identity for a networkControlled pawn, so a local hit can be
    // reported to the server. Without these, melee could never damage a
    // server-owned NPC: ApplyPawnDamage skips them and the pawn had no address
    // to send. -1 = local (single-player) pawn.
    int netWorldIndex     = -1;
    int netNpcIndex       = -1;
    int netPartitionIndex = -1;
    // The wire carries position + yaw + state but NOT velocity, so a client-side
    // server-owned pawn derives its velocity from successive snapshots. That is
    // what makes the static-model walk bob (and any future blend) see motion;
    // snapshots arrive at ~10 Hz, so the last direction is held between them.
    Vector3 netPrevPosition{0, 0, 0};
    bool    netHasPrev = false;

    // Shared render mesh (GameEngine.Mesh.Static/Skeletal) + per-instance anim state.
    std::shared_ptr<oz::Mesh> mesh;
    int animClip = -1;
    float animTime = 0.0f;
    // Forced one-shot clip (play_clip opcode): while forcedClipTimer > 0 the FSM
    // clip is overridden, so a scripted "Attack"/"Roar" plays to completion
    // before the state animation resumes.
    int   forcedClip = -1;
    float forcedClipTimer = 0.0f;
};

// Placed map-object mesh (GameEngine.Mesh.Static / GameEngine.Mesh.Skeletal).
// Distinct from pawns: these are world props/objects, not AI actors.
struct MeshObjectNode {
    uint32_t id = 0;
    std::string meshPath;
    std::string texturePath;
    std::string baseDir;
    // Baked material graph name (GameData/Materials/<name>.ozmat). Empty = use
    // the model's own texture only.
    std::string materialName;
    Vector3 position{0, 0, 0};
    float yaw = 0.0f;
    float scale = 1.0f;
    bool skeletal = false;
    bool windAffected = false;  // foliage: swayed by enclosing WindZones
    std::string animClip;       // clip name (skeletal only)
    std::string animFile;       // external .ozanim vertex-keyframe clip (skeletal)
    float animSpeed = 1.0f;     // playback speed multiplier
    float animTime = 0.0f;
    bool animPaused = false;    // editor scrub: keep animTime fixed while drawing
    // Editor-only preview of an EMBEDDED GLB clip (the animation panel can list
    // and scrub those, but they are read-only - the clip lives in the asset).
    // Kept separate from animClip so previewing never rewrites the clip the world
    // exports; Main.cpp clears it when the node stops being the anim target.
    bool animPreview = false;
    std::string previewClip;
    // Editor-only live vertex pose (dense offsets, vertexCount*3). When set,
    // DrawEntities uploads this instead of sampling the clip. Null at runtime.
    std::shared_ptr<std::vector<float>> editPose;
    std::shared_ptr<oz::Mesh> mesh; // resolved asset (owned by MeshCache)
};

// WindZone — a region that sways wind-affected foliage meshes. Cosmetic /
// client-only; sampled per mesh at draw time.
struct WindZoneNode {
    uint32_t id = 0;
    BoundingBox bounds;
    Vector3 direction{1, 0, 0};  // wind direction (XZ used for sway)
    float strength = 1.0f;       // sway amplitude
    float frequency = 1.0f;      // gust frequency
    bool active = true;
};

// GameEngine.PathNode — NPC waypoint. Client-side copy is used for editor
// placement/visualization and round-trip; the server owns path-following AI.
struct PathNode {
    uint32_t id = 0;
    std::string name;
    Vector3 position{0, 0, 0};
    std::vector<std::string> next; // successor node names
    bool loop = false;
    float radius = 1.0f;
};

// GameEngine.Door — keyed door. Client-side copy for editor placement/round-trip;
// runtime open state + collision removal are driven by the key world flag.
struct DoorNode {
    uint32_t id = 0;
    std::string meshPath;
    Vector3 position{0, 0, 0};
    float yaw = 0.0f;
    std::string keyName;   // required key (empty = unlocked)
    std::string animClip;  // .ozanim clip played on open
    bool locked = false;
    bool open = false;
};

// Player start node - position and orientation for player spawn
struct PlayerStartNode {
    uint32_t id = 0;
    Vector3 position{0, 0, 0};
    float yaw = 0.0f;
};

// Projectile node - fired by weapons
struct ProjectileNode {
    uint32_t id = 0;
    Vector3 position{0, 0, 0};
    Vector3 velocity{0, 0, 0};
    float lifetime = 2.0f;
    float age = 0.0f;
    float damage = 10.0f;
    float speed = 20.0f;
    int ownerId = -1;          // pawn or player id that fired it
    bool active = true;
    Texture2D* sprite = nullptr; // optional trail/glow texture
    // Previous frame's position, used to draw the travel tracer as a segment
    // from where the round was to where it is. The tracer used to be drawn
    // inline in DrawProjectiles, which meant it re-rendered every frame from
    // the current position (a fixed-length streak glued to the bullet) and
    // bypassed CombatFX, so it could not fade or be lit.
    Vector3 prevPosition{0, 0, 0};
    // Optional per-weapon projectile visual (model + submesh), resolved by
    // FireSelectedWeapon from the weapon def's stats.
    std::string meshPath;      // resolved model path (empty = sphere/tracer)
    std::string texturePath;   // optional diffuse texture
    int   submesh = 0;         // mesh index within meshPath's model
    float scale = 0.05f;       // world scale
    float yaw = 0.0f;          // degrees; orients the round's long axis along travel
    float yawOffset = 0.0f;    // per-weapon correction (projectile_yaw stat)
    Color tint{255, 200, 50, 255};
    // True when this round came from a spell (CastSpell) rather than a weapon.
    // The impact path needs it: a spell bolt must not spray blood and must use
    // its own element colour for the burst, while a bullet keeps the gore and
    // the warm spark look.
    bool spell = false;
    // Element identity (halo shape, future status effect). Only meaningful when
    // `spell` is set; guns stay Arcane.
    SpellElement element = SpellElement::Arcane;
};

// Pickup node - collectible items in the world
struct PickupNode {
    uint32_t id = 0;
    // Server-side pickup id for this node, or -1 when it has none.
    //
    // `id` above is a LOCAL handle from a counter shared with every other entity
    // type, so it is meaningless to the server. In multiplayer the server owns
    // pickup state, so the drawn node has to be reconcilable against the server's
    // list or a collect never hides it (the mesh stays on the floor and every
    // re-walk re-requests an already-consumed pickup).
    //
    // LOCKSTEP REQUIREMENT: this is stamped with the file-order index of the
    // ENTITY_PICKUP primitive in World.ozone, which must match the numbering
    // GameState::seed_world_entities assigns (`nextPickupId++`, Server/
    // GameState.cpp). Both sides walk the same parsed primitives in the same
    // order, so the Nth pickup line is id N on both — but that only holds while
    // both enumerations stay in file order. tests/GameState.test.cpp asserts the
    // server side is 0..N-1 to catch a reordering here.
    int netId = -1;
    Vector3 position{0, 0, 0};
    std::string typeName;   // e.g., "HealthVial", "ManaVial", "EnergyCrystal", "Key", "Coin", "Powerup"
    bool active = true;
    float respawnTimer = 0.0f;
    float respawnTime = 30.0f;  // default respawn time
};

// Emitter type for sound/music markers
enum class EmitterType : uint8_t {
    SOUND = 0,
    MUSIC = 1
};

// Emitter node - positional sound/music markers in the world
struct EmitterNode {
    uint32_t id = 0;
    Vector3 position{0, 0, 0};
    EmitterType type = EmitterType::SOUND;
};

// GameEngine.ParticleEmitter — 3D particle source definition (boxed here; the
// simulation runs in the isolated OzParticleSimulationManager).
struct ParticleEmitterNode {
    uint32_t id = 0;
    std::string type = "fire";      // logical kind (fire/sparks/smoke/dust...)
    Vector3 position{0, 0, 0};
    Vector3 direction{0, 1, 0};     // base emission direction (unit-ish)
    float yaw = 0.0f;               // orientation, degrees around +Y
    float rate = 20.0f;             // particles per second
    float lifetime = 1.0f;          // seconds
    float speed = 2.0f;             // initial speed
    float spread = 12.0f;           // emission cone HALF-ANGLE, degrees (shared with Burst())
    float sizeStart = 0.4f;
    float sizeEnd = 0.0f;
    Color colorStart{255, 180, 80, 255};
    Color colorEnd{60, 20, 10, 0};
    float gravity = 0.0f;
    float radius = 0.0f;            // spawn volume radius (0 = point)
    std::string texturePath;        // optional billboard texture (EFX)
    bool billboard = true;
    bool active = true;
};

// WorldInfo — global world metadata + default environment fallback
struct WorldInfo {
    ZoneEnvOverrides defaultEnv;  // defaults: fog, ambient, reverb
    std::string defaultSkybox;
    std::string defaultMusic;
    std::vector<ZonePortal> portals;  // legacy same-level portals (level links live in ZoneManager)
    BoundingBox worldBounds;
    std::string name;
    std::string author;
    LevelSettings settings;       // game rules + weather metadata (LevelInfo/Particles)
};

// Sky zone node — runtime state for isolated skybox chamber rendering
struct SkyZoneNode {
    uint32_t id = 0;
    Vector3 position{0,0,0};      // camera origin inside skybox chamber
    BoundingBox bounds;            // trigger volume for player detection
    Vector3 rotation{0,0,0};       // current sky orientation / angular velocity
    Vector3 scrollSpeed{0,0,0};    // UV scroll speed for cloud/stars layers
    float fov = 60.0f;             // sky camera field of view
    bool bHighDetail = false;      // high/low detail variant toggle
    uint32_t skyEntityInstance = UINT32_MAX; // index into LightningEntityManager
    std::string skyboxPath;        // current skybox texture path
    Texture2D skyboxTex{0};        // loaded skybox texture (unloaded on clear)
    std::string name;              // matching .ozls entity name for script hookup
    const struct EntityDef* def = nullptr; // resolved .ozls def (world-scoped)
    bool active = false;
    float intensity = 1.0f;
};

class PawnSystem {
public:
    // Register a PawnDef so Spawn() can use it by name
    void RegisterDef(const PawnDef& def);

    // Spawn a new pawn from a named template at world position
    int Spawn(Vector3 position, const char* defName);

    // Remove a single pawn by id
    void Despawn(int id);

    // Remove all pawns
    void DespawnAll();

    // Tick AI for every active pawn.
    //
    // `collision` supplies the world ground/obstacle queries so pawns fall under
    // gravity and are blocked by geometry instead of floating and clipping
    // through walls. It is optional: passing nullptr keeps the legacy flat
    // horizontal movement, which is what the headless tests and the editor want
    // (neither has a loaded collision world). The client passes an
    // OzoneCollisionQuery from Core.hpp.
    void Update(Vector3 playerPos, float dt,
                oz::physics::PlayerPhysics::CollisionQuery* collision = nullptr);

    // Draw billboard sprites for every active pawn (optional lit shader for fog/lighting)
    void DrawAll(Camera3D& camera, Shader litShader = {0});

    // Authoring-gizmo overlay (player starts, lights, zones, emitters).
    //
    // These markers used to be gated behind the client's Debug flag
    // (g_debugEnabled), which AngelEd never sets - so the editor viewport had no
    // visible representation for any of these entity types and they were
    // effectively unselectable by hand. The editor now opts in explicitly via
    // SetShowAuthoringGizmos(true) at startup, so the two callers stay
    // independent: the game still hides them unless Debug is on, the editor
    // always shows them.
    void SetShowAuthoringGizmos(bool on) { m_showAuthoringGizmos = on; }
    bool ShowAuthoringGizmos() const { return m_showAuthoringGizmos; }

    // Access individual pawns
    Pawn* Get(int id);
    int Count() const { return (int)m_pawns.size(); }
    const std::vector<Pawn>& GetPawns() const { return m_pawns; }

    // Check if any pawn is attacking the player at given position
    bool IsPlayerAttacked(Vector3 playerPos, float& outDamage);

    // Apply damage to a pawn and transition to DEAD (with on_death script
    // hook) when its health drops to 0. Used by projectile hits and melee.
    void ApplyPawnDamage(Pawn& p, int damage);

    // play_clip opcode: force a named skeletal clip on every active pawn whose
    // def matches `defName`, for `hold` seconds (the FSM clip resumes after).
    // Returns the number of pawns affected (0 = no matching skeletal pawn).
    int PlayPawnClip(const std::string& defName, const std::string& clipName,
                     float hold = 1.0f);

    // Which pawn a melee swing hits: the NEAREST live pawn inside the forward
    // arc (projection in [0, reach]) and within a 2u radius of the swing line.
    // Returns nullptr when nothing qualifies. Split out of the weapon manager so
    // the selection is unit-testable without the render layer.
    const Pawn* ResolveMeleeTarget(const Vector3& origin, const Vector3& direction,
                                   float reach);

    // Feedback state for UI (last collected pickup info)
    struct PickupFeedback {
        bool collected = false;
        std::string typeName;
        int itemId = 0;
        float flashTimer = 0.0f;
        // Authored `pickup_category`, packed RGBA (see PickupCategoryTintRGBA).
        // Ten pickup defs author the key and it had no code reference, so the
        // collect flash was always the same colour. Drives the HUD tint.
        unsigned int tint = 0;
        // Category name, kept for the pickup log / future HUD labelling.
        const char* category = "unknown";
    };
    PickupFeedback m_pickupFeedback;

    // --- Entity node management ---

    // Player start nodes
    void AddPlayerStart(const PlayerStartNode& node);
    void RemovePlayerStart(int id);
    void ClearPlayerStarts();
    std::vector<PlayerStartNode>& GetPlayerStarts() { return m_playerStarts; }
    const std::vector<PlayerStartNode>& GetPlayerStarts() const { return m_playerStarts; }
    PlayerStartNode* GetFirstPlayerStart();
    void RespawnPlayerAtStart(Camera3D& camera);

    // Projectile nodes
    int SpawnProjectile(const ProjectileNode& node);
    void UpdateProjectiles(float dt);
    void DrawProjectiles(Camera3D& camera, Shader litShader = {0});
    void ClearProjectiles();
    std::vector<ProjectileNode>& GetProjectiles() { return m_projectiles; }
    const std::vector<ProjectileNode>& GetProjectiles() const { return m_projectiles; }

    // Pickup nodes
    int AddPickup(const PickupNode& node);
    void RemovePickup(int id);
    void ClearPickups();
    std::vector<PickupNode>& GetPickups() { return m_pickups; }
    const std::vector<PickupNode>& GetPickups() const { return m_pickups; }
    PickupNode* GetPickup(int id);
    void UpdatePickups(float dt, Vector3 playerPos, BoundingBox playerBounds);

    // Reconcile the DRAWN pickup nodes against the server's pickup state
    // (multiplayer only). `net` is the client's view of the server's list; see
    // Client.hpp's ClientPickup.
    //
    // Why this exists: the networked collect used to flip `active` on
    // OmegaClient's own copy of the list, which nothing renders. The node the
    // player actually sees stayed `active` forever, so the mesh never vanished
    // and every walk back over it re-requested a pickup the server had already
    // consumed. The server's state is the truth in MP, so it drives the nodes.
    //
    // Matching is by `netId`, not by id: `PickupNode::id` is a local handle
    // from a counter shared with every other entity type.
    //
    // `net` MUST already be scoped to ONE world. Pickup ids restart at 0 per
    // world, so a multi-world snapshot collides on id and the last one silently
    // wins. The caller filters on `world_index` (Main.cpp's
    // set_on_pickups_changed lambda, next to the field that makes it visible);
    // the parameter here is for logging only.
    void ApplyPickupNetState(const std::vector<PickupNode>& net, int world_index);

    // Zone volumes, level portals and player region tracking live in
    // World/ZoneManager.hpp (ZoneManager::Instance()).

    // WorldInfo management
    void SetWorldInfo(const WorldInfo& wi) { m_worldInfo = wi; }
    const WorldInfo& GetWorldInfo() const { return m_worldInfo; }
    WorldInfo& GetWorldInfo() { return m_worldInfo; }

    // Sound/music emitter nodes
    int AddEmitter(const EmitterNode& node);
    void RemoveEmitter(int id);
    void ClearEmitters();
    std::vector<EmitterNode>& GetEmitters() { return m_emitters; }
    const std::vector<EmitterNode>& GetEmitters() const { return m_emitters; }

    // GameEngine.ParticleEmitter nodes (simulated by OzParticleSimulationManager)
    int AddParticleEmitter(const ParticleEmitterNode& node);
    void RemoveParticleEmitter(int id);
    void ClearParticleEmitters();
    std::vector<ParticleEmitterNode>& GetParticleEmitters() { return m_particleEmitters; }
    const std::vector<ParticleEmitterNode>& GetParticleEmitters() const { return m_particleEmitters; }
    ParticleEmitterNode* GetParticleEmitter(int id);

    // GameEngine.PathNode waypoints (NPC patrol graph)
    int AddPathNode(const PathNode& node);
    void RemovePathNode(int id);
    void ClearPathNodes();
    std::vector<PathNode>& GetPathNodes() { return m_pathNodes; }
    const std::vector<PathNode>& GetPathNodes() const { return m_pathNodes; }
    PathNode* GetPathNode(int id);
    PathNode* FindPathNodeByName(const std::string& name);

    // WindZone regions (foliage sway)
    int AddWindZone(const WindZoneNode& node);
    void RemoveWindZone(int id);
    void ClearWindZones();
    std::vector<WindZoneNode>& GetWindZones() { return m_windZones; }
    const std::vector<WindZoneNode>& GetWindZones() const { return m_windZones; }
    WindZoneNode* GetWindZone(int id);

    // GameEngine.Door
    int AddDoor(const DoorNode& node);
    void RemoveDoor(int id);
    void ClearDoors();
    std::vector<DoorNode>& GetDoors() { return m_doors; }
    const std::vector<DoorNode>& GetDoors() const { return m_doors; }
    DoorNode* GetDoor(int id);
    // Combined wind at a world point: (dirX, dirZ, strength, frequency).
    Vector4 SampleWind(Vector3 worldPos) const;

    // Draw entities (billboards for player starts, pickups, zones, emitters).
    // windShader is the Wind.vs variant used for wind-affected foliage meshes.
    void DrawEntities(Camera3D& camera, Shader litShader = {0}, Shader windShader = {0});
    void ClearWeaponPickupCache();

    // Placed static/skeletal map-object meshes (GameEngine.Mesh.*)
    int AddMeshObject(const MeshObjectNode& node);
    void RemoveMeshObject(int id);
    void ClearMeshObjects();
    std::vector<MeshObjectNode>& GetMeshObjects() { return m_meshObjects; }
    const std::vector<MeshObjectNode>& GetMeshObjects() const { return m_meshObjects; }
    MeshObjectNode* GetMeshObject(int id);

    // Sky zone node management
    int AddSkyZone(const SkyZoneNode& node);
    void RemoveSkyZone(int id);
    void ClearSkyZones();
    SkyZoneNode* GetSkyZone(int id);
    std::vector<SkyZoneNode>& GetSkyZones() { return m_skyZones; }
    const std::vector<SkyZoneNode>& GetSkyZones() const { return m_skyZones; }
    void SetActiveSkyZone(int index);
    SkyZoneNode* GetActiveSkyZone();
    int GetActiveSkyZoneIndex() const { return m_activeSkyZoneIndex; }

    // Sky zone tracking â€” returns true if player is inside a ZONE_SKY volume
    bool IsInSkyZone() const { return m_activeSkyZoneIndex >= 0; }
    BoundingBox GetSkyZoneBounds() const {
        if (m_activeSkyZoneIndex >= 0 && m_activeSkyZoneIndex < (int)m_skyZones.size())
            return m_skyZones[m_activeSkyZoneIndex].bounds;
        return {{0,0,0},{0,0,0}};
    }
    void UpdateSkyZone(Vector3 playerPos, BoundingBox playerBounds);

    // Synchronize LightningEntityManager pending skybox state into active SkyZoneNode
    void SyncSkyboxState();

    // Light node management
    int AddLight(const LightNode& node);
    void RemoveLight(int id);
    void ClearLights();
    std::vector<LightNode>& GetLights() { return m_lights; }
    const std::vector<LightNode>& GetLights() const { return m_lights; }
    LightNode* GetLight(int id);

    // Bind every light to the first zone volume that contains it
    // (-1 = affects all zones). Called by the world orchestrator after the
    // OZONE entity set has been injected.
    void AssignLightZones();

    // Access registered definitions
    const std::vector<PawnDef>& GetDefs() const { return m_defs; }

    // Singleton
    static PawnSystem& Instance();

private:
    std::vector<Pawn> m_pawns;
    std::vector<int> m_freeIds;
    std::vector<PawnDef> m_defs;
    std::vector<LightNode> m_lights;
    uint32_t m_nextId = 1;

    // Entity node storage
    std::vector<PlayerStartNode> m_playerStarts;
    std::vector<ProjectileNode> m_projectiles;
    std::vector<PickupNode> m_pickups;
    std::vector<EmitterNode> m_emitters;
    std::vector<ParticleEmitterNode> m_particleEmitters;
    std::vector<PathNode> m_pathNodes;
    std::vector<WindZoneNode> m_windZones;
    std::vector<DoorNode> m_doors;
    std::vector<MeshObjectNode> m_meshObjects;
    uint32_t m_nextEntityId = 1;
    uint32_t m_nextLightId = 1;
    bool m_showAuthoringGizmos = false;   // see SetShowAuthoringGizmos

    // Sky zone state
    std::vector<SkyZoneNode> m_skyZones;
    int m_activeSkyZoneIndex = -1;

    // Resolve a pawn def's shared render mesh through the internal MeshCache.
    std::shared_ptr<oz::Mesh> EnsurePawnMesh(PawnDef& def);
    // Map the pawn FSM state to the def's animation clip and advance its time.
    void SyncPawnAnim(Pawn& p, PawnDef* def, float dt);

    // Portal visual: shared double-sided quad + EFX shimmer texture (lazy loaded)
    Model m_portalQuadModel{0};
    Texture2D m_portalTexture{0};
    bool m_portalVisualReady = false;
    void EnsurePortalVisual();
    void UnloadPortalVisual();

    // World metadata + zone portal system
    WorldInfo m_worldInfo;

    PawnDef* FindDef(const char* name);
    int AllocSlot();

    // FSM tick helpers
    void TickPatrol(Pawn& p, float dt);   // member: patrol follows m_pathNodes
    static void TickChase(Pawn& p, const Vector3& playerPos, float dt);
    static void TickReturn(Pawn& p, float dt);
    static void TransitionState(Pawn& p, PawnState newState);

    // Patrol wander angle (accumulated across frames for smooth circles)
    struct PatrolState {
        float angle = 0.0f;
    };
    static PatrolState& PState(Pawn& p);
};
