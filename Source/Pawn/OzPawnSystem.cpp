#include "OzPawnSystem.hpp"
#include "../Renderer/OzAssetMapper.hpp"
#include "../Package/PackageAssetLoader.hpp"
#include "../Renderer/EngineBillboard.hpp"
#include "../Renderer/CombatFX.hpp"
#include "../Renderer/CombatFeedback.hpp"
#include "../Renderer/WindShader.hpp"
#include "../Particle/OzParticleSimulationManager.hpp"
#include "../Audio/SoundManager.hpp"
#include "../Script/LightningEntityManager.hpp"
#include "../Script/LightningEntityRegistry.hpp"
#include "../Material/MaterialRegistry.hpp"
#include "PlayerMovement.hpp"
#include "Items.hpp"
#include "../DebugFlags.hpp"
#include "../GameplayFlags.hpp"
#include "../Events/EventBus.hpp"
#include <rlgl.h>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>

// NPC locomotion constants. A pawn is an AABB that shares the player's collision
// model (heightmap first, then brush tops); gravity matches the engine's default
// PhysicsInfo gravity so a falling pawn reads like a falling player.
static constexpr float kPawnGravity   = 18.0f;
static constexpr float kPawnTerminal  = 40.0f;
static constexpr float kPawnHalfWidth = 0.5f;
static constexpr float kPawnHeight    = 1.8f;

// ---------------------------------------------------------------------------
// AssignLightZones — bind each light to the first zone volume that contains it.
// zoneId -1 means "affects every zone" (the default). Called by the world
// orchestrator (Core.hpp LoadWorld / AngelEd open) once the OZONE entity set has
// been injected, so the loader itself never writes entity-system state.
// ---------------------------------------------------------------------------
void PawnSystem::AssignLightZones() {
    auto& zones = ZoneManager::Instance().GetZones();
    for (auto& l : m_lights) {
        l.zoneId = -1;
        for (auto& z : zones) {
            if (l.position.x >= z.bounds.min.x && l.position.x <= z.bounds.max.x &&
                l.position.y >= z.bounds.min.y && l.position.y <= z.bounds.max.y &&
                l.position.z >= z.bounds.min.z && l.position.z <= z.bounds.max.z) {
                l.zoneId = (int)z.id;
                break;   // first containing zone wins
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------
PawnSystem& PawnSystem::Instance() {
    static PawnSystem instance;
    return instance;
}

// ---------------------------------------------------------------------------
// Find a registered PawnDef by name (case-insensitive)
// ---------------------------------------------------------------------------
PawnDef* PawnSystem::FindDef(const char* name) {
    if (!name) return nullptr;
    for (auto& d : m_defs) {
        if (d.name.size() == strlen(name) &&
            std::equal(d.name.begin(), d.name.end(), name,
                       [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); }))
            return &d;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// AllocSlot — find a free slot or grow the vector
// ---------------------------------------------------------------------------
int PawnSystem::AllocSlot() {
    if (!m_freeIds.empty()) {
        int id = m_freeIds.back();
        m_freeIds.pop_back();
        return id;
    }
    int id = (int)m_pawns.size();
    m_pawns.emplace_back();
    return id;
}

// ---------------------------------------------------------------------------
// RegisterDef
// ---------------------------------------------------------------------------
void PawnSystem::RegisterDef(const PawnDef& def) {
    if (FindDef(def.name.c_str())) return;
    m_defs.push_back(def);
}

// ---------------------------------------------------------------------------
// UpdateDef / RemoveDef — editor hot reload
// ---------------------------------------------------------------------------
bool PawnSystem::UpdateDef(const PawnDef& def) {
    PawnDef* existing = FindDef(def.name.c_str());
    if (!existing) return false;

    // Fields that decide WHICH asset is loaded. A change here invalidates the
    // mesh each live pawn of this def already resolved.
    const bool assetChanged =
        existing->model_path    != def.model_path    ||
        existing->model_texture != def.model_texture ||
        existing->mesh_type     != def.mesh_type     ||
        existing->anim_file     != def.anim_file     ||
        existing->model_scale   != def.model_scale;
    // The billboard sprite is loaded per pawn at Spawn(), so it needs the same
    // treatment as the mesh when its path changes.
    const bool spriteChanged = existing->sprite_path != def.sprite_path;

    *existing = def;
    // The bounds-derived ground offset belongs to the OLD mesh. PawnDef is
    // copied wholesale, so a stale "resolved" flag would keep the next asset
    // floating by the previous model's pivot offset.
    existing->ground_offset_resolved = false;
    existing->auto_ground_offset = 0.0f;

    // Spawn() copies the movement/combat knobs onto the instance, so an edit
    // would otherwise only affect FUTURE spawns. Health is instance state and is
    // intentionally not reset.
    for (auto& p : m_pawns) {
        if (!p.active) continue;
        if (FindDef(p.defName.c_str()) != existing) continue;
        p.speed       = def.speed;
        p.aggroRange  = def.aggroRange;
        p.attackRange = def.attackRange;
        p.damage      = def.damage;
        if (assetChanged) p.mesh.reset();
        if (spriteChanged) {
            if (p.sprite.id > 0) UnloadTexture(p.sprite);
            p.sprite = Texture2D{0};
            if (!def.sprite_path.empty())
                p.sprite = LoadTextureWithFallback(def.sprite_path.c_str());
            if (p.sprite.id == 0) {
                const std::string fallback =
                    std::string("GameData/Pawn/Pawns/") + def.name + ".png";
                p.sprite = LoadTextureWithFallback(fallback.c_str());
            }
        }
    }
    return true;
}

bool PawnSystem::RemoveDef(const char* name) {
    PawnDef* def = FindDef(name);
    if (!def) return false;
    // FindDef returned a pointer into m_defs, so the erase slot is its index.
    m_defs.erase(m_defs.begin() + (def - m_defs.data()));
    return true;
}

// ---------------------------------------------------------------------------
// Spawn
// ---------------------------------------------------------------------------
int PawnSystem::Spawn(Vector3 pos, const char* defName) {
    PawnDef* def = FindDef(defName);
    if (!def) {
        OZ_WARN("PawnSystem: unknown def '%s'", defName);
        return -1;
    }

    int slot = AllocSlot();
    Pawn& p = m_pawns[slot];
    p.id = m_nextId++;
    p.active = true;
    p.position = pos;
    p.spawnPosition = pos;
    p.velocity = {0, 0, 0};
    p.yaw = 0.0f;
    p.state = PawnState::IDLE;
    p.prevState = PawnState::IDLE;

    p.speed = def->speed;
    p.aggroRange = def->aggroRange;
    p.attackRange = def->attackRange;
    p.damage = def->damage;
    p.health = def->maxHealth;
    p.maxHealth = def->maxHealth;

    p.patrolTimer = 0.0f;
    p.stateTimer = 0.0f;
    p.deadTimer = 0.0f;
    p.defName = defName;
    // Slots are recycled, so every piece of per-instance state must be reset or
    // a respawned pawn inherits the previous one's hover phase / patrol waypoint
    // / mesh (the mesh is per-DEF, so a recycled slot must not keep the old one).
    p.hoverPhase = 0.0f;
    p.pathTarget = 0;
    p.onGround = false;
    p.animClip = -1;
    p.animTime = 0.0f;
    p.forcedClip = -1;
    p.forcedClipTimer = 0.0f;
    p.netPrevPosition = pos;
    p.netHasPrev = false;
    // Network ownership is per-instance too, and Despawn does NOT clear it: the
    // server stamps it on a freshly spawned NPC (Main.cpp), so a slot recycled
    // from an earlier multiplayer world would come back server-owned in single
    // player - no local FSM (the NPC stands still), no local damage (ApplyPawnDamage
    // returns early, so a melee swing cannot kill it), no contact damage
    // (IsPlayerAttacked skips it), and MELEE_HIT reports aimed at the previous
    // occupant's netNpcIndex on a server that is not there. The identity fields
    // travel with the flag, so they are reset together.
    p.networkControlled = false;
    p.netWorldIndex     = -1;
    p.netNpcIndex       = -1;
    p.netPartitionIndex = -1;
    // Status effects are per-instance too. A recycled slot must not inherit a
    // live burn (it would damage the new occupant) or a chill speed reduction
    // (it would move at half speed forever).
    p.status = StatusEffect::None;
    p.statusTimer = 0.0f;
    p.statusTickTimer = 0.0f;
    p.statusDamage = 0;
    p.statusBaseSpeed = 0.0f;
    p.mesh.reset();
    // Slots are recycled from m_freeIds. If LEM::Spawn below fails, this must
    // not keep pointing at the previous occupant's script entity — Spawn() is
    // the only writer, so an inherited index is silently wrong forever.
    p.scriptInstanceIndex = -1;

    // Load sprite and scream using def paths or convention (with package fallback)
    {
        if (!def->sprite_path.empty())
            p.sprite = LoadTextureWithFallback(def->sprite_path.c_str());
        if (p.sprite.id == 0) {
            std::string fallbackSprite = std::string("GameData/Pawn/Pawns/") + defName + ".png";
            p.sprite = LoadTextureWithFallback(fallbackSprite.c_str());
        }

        // Aggro stinger: explicit def path, then GameData/Pawn/Pawns/<name>.{wav,mp3}
        p.scream = SoundManager::LoadPawnScream(def->name, def->scream_path);
    }

    // Optionally create a LightningScript entity instance for script hooks
#ifndef OMEGA_TEST_ENV
    int lemIdx = LightningEntityManager::Instance().Spawn(defName);
    if (lemIdx >= 0) p.scriptInstanceIndex = lemIdx;
#endif

    OZ_DEBUG("Pawn spawned: id=%d def=%s sprite=%d scream=%d at (%.1f, %.1f, %.1f)",
             p.id, defName, p.sprite.id, p.scream.frameCount, pos.x, pos.y, pos.z);
    return (int)p.id;
}

// ---------------------------------------------------------------------------
// Despawn
// ---------------------------------------------------------------------------
void PawnSystem::Despawn(int id) {
    for (size_t i = 0; i < m_pawns.size(); i++) {
        if (m_pawns[i].active && m_pawns[i].id == (uint32_t)id) {
            OZ_DEBUG("Pawn despawned: id=%d def=%s", id, m_pawns[i].defName.c_str());
#ifndef OMEGA_TEST_ENV
            if (m_pawns[i].scriptInstanceIndex >= 0)
                LightningEntityManager::Instance().Despawn(m_pawns[i].scriptInstanceIndex);
#endif
            m_pawns[i].scriptInstanceIndex = -1;
            // ZERO the handles after unloading them. raylib takes the Sound/
            // Texture2D by value, so its own `frameCount`/`id` reset only touches
            // the copy — the slot keeps the dead handle. UnloadSound frees the
            // AudioBuffer it points at, so a second pass over the same slot is a
            // double free (see DespawnAll).
            if (m_pawns[i].sprite.id != 0) {
                UnloadTexture(m_pawns[i].sprite);
                m_pawns[i].sprite = Texture2D{0};
            }
            if (m_pawns[i].scream.frameCount != 0) {
                UnloadSound(m_pawns[i].scream);
                m_pawns[i].scream = Sound{0};
            }
            m_pawns[i].mesh.reset();
            m_pawns[i].active = false;
            m_freeIds.push_back((int)i);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// DespawnAll
// ---------------------------------------------------------------------------
void PawnSystem::DespawnAll() {
    for (auto& p : m_pawns) {
        // ZERO every released handle. `m_pawns` is a GROWING vector and this
        // function walks EVERY slot, including ones the next world does not
        // reuse: a 14-NPC world followed by a 12-NPC world leaves two slots
        // holding handles that were already unloaded. UnloadSound frees the
        // AudioBuffer the stale pointer names, so the next world change freed it
        // a second time — the crash was a portal transition after a campaign
        // launch that had first loaded a different (bigger) world. raylib takes
        // the struct by value, so its internal reset never reaches this slot.
        if (p.sprite.id != 0) {
            UnloadTexture(p.sprite);
            p.sprite = Texture2D{0};
        }
        if (p.scream.frameCount != 0) {
            UnloadSound(p.scream);
            p.scream = Sound{0};
        }
        p.mesh.reset();
        p.active = false;
    }
    m_freeIds.clear();
    for (size_t i = 0; i < m_pawns.size(); i++)
        m_freeIds.push_back((int)i);
}

// ---------------------------------------------------------------------------
// Get
// ---------------------------------------------------------------------------
Pawn* PawnSystem::Get(int id) {
    for (auto& p : m_pawns) {
        if (p.active && p.id == (uint32_t)id)
            return &p;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Light node management
// ---------------------------------------------------------------------------
int PawnSystem::AddLight(const LightNode& node) {
    LightNode n = node;
    if (n.id == 0) n.id = m_nextLightId++;
    m_lights.push_back(n);
    return (int)n.id;
}

void PawnSystem::RemoveLight(int id) {
    auto it = std::remove_if(m_lights.begin(), m_lights.end(),
        [id](const LightNode& n) { return n.id == (uint32_t)id; });
    m_lights.erase(it, m_lights.end());
}

void PawnSystem::ClearLights() {
    m_lights.clear();
}

LightNode* PawnSystem::GetLight(int id) {
    for (auto& n : m_lights) {
        if (n.id == (uint32_t)id) return &n;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// IsPlayerAttacked � check if any pawn is close enough to damage the player
// ---------------------------------------------------------------------------
bool PawnSystem::IsPlayerAttacked(Vector3 playerPos, float& outDamage) {
    outDamage = 0.0f;
    for (auto& p : m_pawns) {
        if (!p.active || p.state == PawnState::DEAD) continue;
        // Server-owned NPCs apply damage server-side (PLAYER_HURT) � skip locally.
        if (p.networkControlled) continue;

        float dx = playerPos.x - p.position.x;
        float dz = playerPos.z - p.position.z;
        float dist = sqrtf(dx * dx + dz * dz);

        if (dist < p.attackRange) {
            outDamage = p.damage;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// ApplyPawnDamage � damage + death transition (fires on_death script hook)
// ---------------------------------------------------------------------------
// Resolve which pawn a melee swing hits: the NEAREST live pawn inside the
// forward arc and within `hitRadius` of the swing line. Returns nullptr when
// nothing qualifies.
//
// This lived inline in FireSelectedWeapon and took the first container match,
// so which NPC you hit depended on spawn order rather than distance — a swing
// through two enemies could damage the far one. Split out so it can be tested
// without the render layer.
const Pawn* PawnSystem::ResolveMeleeTarget(const Vector3& origin,
                                           const Vector3& direction,
                                           float reach) {
    const float hitRadius = 2.0f;  // pawn half-width, matches the prior test
    const Pawn* best = nullptr;
    float bestDistSq = 0.0f;

    for (const auto& pawn : m_pawns) {
        if (!pawn.active || pawn.state == PawnState::DEAD) continue;
        // Forward arc only: the projection along `direction` must be in
        // [0, reach], so a swing cannot hit something behind the wielder.
        Vector3 toPawn = Vector3Subtract(pawn.position, origin);
        float t = Vector3DotProduct(toPawn, direction);
        if (t < 0.0f || t > reach) continue;
        // ...and within hitRadius of the swing line.
        Vector3 closest = Vector3Add(origin, Vector3Scale(direction, t));
        if (Vector3Distance(closest, pawn.position) >= hitRadius) continue;
        // Nearest by distance from the wielder, not from the arc centre: two
        // NPCs either side of the swing must resolve to the one faced.
        float distSq = Vector3DistanceSqr(origin, pawn.position);
        if (!best || distSq < bestDistSq) {
            best = &pawn;
            bestDistSq = distSq;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// ApplyStatus — burn / chill, single-status and refresh-on-reapply
// ---------------------------------------------------------------------------
namespace {
constexpr float kStatusTickSeconds = 0.5f;   // burn DoT cadence
constexpr float kChillSpeedMul    = 0.5f;   // frost movement multiplier
}

void PawnSystem::ApplyStatus(Pawn& p, StatusEffect s, float duration,
                             int tickDamage, Color color) {
    if (!p.active || p.state == PawnState::DEAD) return;
    if (s == StatusEffect::None || duration <= 0.0f) return;

    // Replacing a status must undo the previous one's movement change first, or
    // a chill overwritten by a burn leaves the pawn permanently slowed.
    //
    // This runs BEFORE the networkControlled guard below and releases the status
    // outright, because that guard is an early RETURN: anything after it is
    // skipped. A chilled pawn that becomes server-owned mid-life would otherwise
    // keep the 0.5x speed forever - Update() early-outs for those pawns too, so
    // nothing would ever reach the expiry block that restores it.
    if (p.status != StatusEffect::None) {
        if (p.status == StatusEffect::Chill && p.statusBaseSpeed > 0.0f)
            p.speed = p.statusBaseSpeed;
        p.status = StatusEffect::None;
        p.statusTimer = 0.0f;
        p.statusTickTimer = 0.0f;
        p.statusDamage = 0;
        p.statusBaseSpeed = 0.0f;
    }

    // Server-owned pawns take damage server-side; a local DoT would be a second,
    // client-authoritative source of truth. See StatusEffect.hpp.
    if (p.networkControlled) return;

    p.status = s;
    p.statusTimer = duration;
    p.statusTickTimer = 0.0f;      // first tick on the next Update
    p.statusDamage = tickDamage > 0 ? tickDamage : 0;
    p.statusColor = color;
    if (s == StatusEffect::Chill) {
        if (p.statusBaseSpeed <= 0.0f) p.statusBaseSpeed = p.speed;
        p.speed = p.statusBaseSpeed * kChillSpeedMul;
    }

    // One-shot application burst so the effect is visible at the moment it lands.
    CombatFX::Instance().SpawnImpact(
        {p.position.x, p.position.y + 0.7f, p.position.z},
        {0.0f, 1.0f, 0.0f}, color, 8, 3.0f);
}

void PawnSystem::ApplyPawnDamage(Pawn& p, int damage) {
    if (!p.active || p.state == PawnState::DEAD) return;
    // Server-owned NPCs take damage via NPC_DAMAGE relay, not local hits
    if (p.networkControlled) return;
    p.health -= damage;
    if (p.health <= 0) {
        // Gore on death: a gib burst at the body plus a ground splat, before the
        // pawn is hidden. Gated on the "Gore" setting.
        if (g_goreEnabled)
            CombatFX::Instance().SpawnGibs({p.position.x, p.position.y, p.position.z});
        // Do NOT deactivate here. `active = false` made the pawn vanish on the
        // frame it died, which also made the DEAD branch of DrawAll (and so
        // `anim_death`, which every def is allowed to author) unreachable. The
        // corpse stays for kPawnCorpseSeconds and Update() despawns it then.
        p.velocity = {0, 0, 0};
        p.deadTimer = kPawnCorpseSeconds;
        TransitionState(p, PawnState::DEAD);
    }
}

// ---------------------------------------------------------------------------
// Player Start Nodes
// ---------------------------------------------------------------------------
void PawnSystem::AddPlayerStart(const PlayerStartNode& node) {
    PlayerStartNode n = node;
    if (n.id == 0) n.id = m_nextEntityId++;
    m_playerStarts.push_back(n);
}

void PawnSystem::RemovePlayerStart(int id) {
    auto it = std::remove_if(m_playerStarts.begin(), m_playerStarts.end(),
        [id](const PlayerStartNode& n) { return n.id == (uint32_t)id; });
    m_playerStarts.erase(it, m_playerStarts.end());
}

void PawnSystem::ClearPlayerStarts() {
    m_playerStarts.clear();
}

PlayerStartNode* PawnSystem::GetFirstPlayerStart() {
    if (!m_playerStarts.empty()) return &m_playerStarts[0];
    return nullptr;
}

void PawnSystem::RespawnPlayerAtStart(Camera3D& camera) {
    PlayerStartNode* ps = GetFirstPlayerStart();
    if (ps) {
        camera.position = {ps->position.x, ps->position.y + 2.0f, ps->position.z};
        float yawRad = ps->yaw * DEG2RAD;
        camera.target = {camera.position.x + sinf(yawRad), camera.position.y,
                         camera.position.z - cosf(yawRad)};
    } else {
        camera.position = {0.0f, 5.0f, 0.0f};
        camera.target = {0.0f, 5.0f, -5.0f};
    }
}

// ---------------------------------------------------------------------------
// Projectile nodes
// ---------------------------------------------------------------------------
int PawnSystem::SpawnProjectile(const ProjectileNode& node) {
    ProjectileNode p = node;
    p.id = m_nextEntityId++;
    p.active = true;
    p.age = 0.0f;
    // Orient a model projectile along its travel. The ammo GLBs' nose is along
    // +X. raylib's rendered Y-rotation maps +X to (cos, 0, -sin) (verified by
    // rendering against world axis lines), so the yaw that points +X along the
    // horizontal velocity (vx, vz) is atan2(-vz, vx). Using atan2(vz, vx) - the
    // CPU row-vector convention - mirrored the round across the X axis, so a
    // forward shot flew tail-first. yawOffset is an optional per-weapon
    // correction (projectile_yaw stat).
    if (!p.meshPath.empty() && (p.velocity.x != 0.0f || p.velocity.z != 0.0f))
        p.yaw = atan2f(-p.velocity.z, p.velocity.x) * 57.29578f + p.yawOffset;
    m_projectiles.push_back(p);
    return (int)m_projectiles.size() - 1;
}

void PawnSystem::UpdateProjectiles(float dt) {
    for (auto& p : m_projectiles) {
        if (!p.active) continue;
        p.age += dt;
        if (p.age >= p.lifetime) {
            p.active = false;
            continue;
        }
        p.prevPosition = p.position;
        p.position.x += p.velocity.x * dt;
        p.position.y += p.velocity.y * dt;
        p.position.z += p.velocity.z * dt;
        // Simple gravity on projectiles
        p.velocity.y -= 5.0f * dt;

        // Travel tracer for this step, emitted through CombatFX so it fades and
        // draws with every other combat effect. Only for tracer-styled rounds
        // (no model) - a mesh projectile draws its own geometry.
        if (p.meshPath.empty()) {
            float travelled = Vector3Distance(p.prevPosition, p.position);
            if (travelled > 0.01f) {
                CombatFX::Instance().AddTracer(p.prevPosition, p.position,
                                               Fade(p.tint, 0.75f), 0.06f);
            }
        }

        // Projectile vs Pawn collision
        for (auto& pawn : m_pawns) {
            if (!pawn.active || pawn.state == PawnState::DEAD) continue;
            float dist = Vector3Distance(p.position, pawn.position);
            if (dist < 1.5f) {
                p.active = false;
                Vector3 hitNormal = Vector3Normalize(Vector3Scale(p.velocity, -1.0f));
                Vector3 hitPos = {pawn.position.x, pawn.position.y + 0.5f, pawn.position.z};
                // Spell bolts use their own element colour and never spray
                // blood; a bullet keeps the warm spark + gore. Before this the
                // impact was hardcoded red for every projectile, so a frost
                // lance hit bled red and read as a gunshot.
                const Color impactCol = p.spell ? p.tint : Color{200, 30, 30, 255};
                CombatFX::Instance().SpawnImpact(
                    hitPos, hitNormal, impactCol, p.spell ? 14 : 10,
                    p.spell ? 6.5f : 4.0f);
                if (!p.spell && g_goreEnabled)
                    CombatFX::Instance().SpawnBlood(hitPos, hitNormal, 12, 1.0f);
                if (p.spell) {
                    CombatFX::Instance().ArmTransientLight(hitPos, p.tint, 0.18f, 6.0f);
                    // Element-shaped flash at the contact, so a frost hit shows a
                    // shard and a shock hit a star even in the same colour.
                    CombatFX::Instance().ArmMuzzleFlash(hitPos, 0.16f, p.tint, p.element);
                }
                ApplyPawnDamage(pawn, (int)p.damage);
                // Elemental condition: fire burns, frost chills. Applied after the
                // damage so a killing blow does not start a DoT on a corpse (the
                // status apply rejects a DEAD pawn).
                if (p.spell) {
                    const StatusEffect st = StatusForElement(p.element);
                    if (st != StatusEffect::None) {
                        const float dur = (st == StatusEffect::Burn) ? 4.0f : 2.5f;
                        const int tickDmg = (st == StatusEffect::Burn)
                            ? (int)fmaxf(1.0f, p.damage * 0.05f) : 0;
                        ApplyStatus(pawn, st, dur, tickDmg, p.tint);
                    }
                }
                // Hit confirmation for the shooter: the impact/blood above is
                // the 3D half, this drives the crosshair marker.
                oz::CombatFeedback::Instance().ConfirmHit(pawn.state == PawnState::DEAD);
                // ...and the floating number, which this path can afford because
                // the damage amount and the contact point are both local facts.
                oz::CombatFeedback::Instance().SpawnDamageNumber(
                    hitPos, (int)p.damage, pawn.state == PawnState::DEAD);
                break;
            }
        }
    }
    // Remove inactive projectiles
    m_projectiles.erase(
        std::remove_if(m_projectiles.begin(), m_projectiles.end(),
                       [](const ProjectileNode& p) { return !p.active; }),
        m_projectiles.end());
}

void PawnSystem::DrawProjectiles(Camera3D& camera, Shader litShader) {
    for (auto& p : m_projectiles) {
        if (!p.active) continue;
        // Per-weapon projectile visual: draw the configured model submesh when
        // available, else fall back to a glowing tracer sphere.
        if (!p.meshPath.empty()) {
            auto mesh = oz::MeshCache::Instance().GetStatic(p.meshPath, p.texturePath, "", true);
            if (mesh) {
                if (p.submesh < 0 || p.submesh >= mesh->MeshCount()) {
                    // A configured projectile model whose submesh index does not
                    // exist draws NOTHING (DrawSubmesh bounds-checks and returns),
                    // which is indistinguishable from a broken weapon. Name it
                    // once instead of letting it vanish silently.
                    static std::unordered_map<std::string, bool> s_subWarned;
                    if (!s_subWarned.count(p.meshPath)) {
                        s_subWarned[p.meshPath] = true;
                        OZ_WARN("Projectile '%s': submesh %d out of range (mesh has %d) - "
                                "check projectile_submesh in the weapon def",
                                p.meshPath.c_str(), p.submesh, mesh->MeshCount());
                    }
                } else {
                    oz::MeshTransform t;
                    t.position = p.position;
                    t.yaw = p.yaw;
                    t.scale = {p.scale, p.scale, p.scale};
                    mesh->DrawSubmesh(p.submesh, t, litShader, p.tint);
                }
                continue;
            }
            // meshPath set but the model failed to load (MeshCache caches the
            // negative result, so this fires once per path, not per frame). The
            // sphere below is then a fallback, and it must be reported or a
            // missing projectile GLB reads as "the weapon fires a ball".
            static std::unordered_map<std::string, bool> s_meshWarned;
            if (!s_meshWarned.count(p.meshPath)) {
                s_meshWarned[p.meshPath] = true;
                OZ_WARN("Projectile model '%s' failed to load - using fallback sphere",
                        p.meshPath.c_str());
            }
        }
        // Fallback tracer. Small and elongated-looking: the travel tracer is
        // emitted from UpdateProjectiles through CombatFX, so this sphere is
        // just the round itself and should not read as a placeholder ball.
        Color c = p.tint;
        float radius = p.spell ? 0.20f : 0.12f;
        DrawSphere(p.position, radius, c);
        // Spell core: an additive halo AROUND the sphere. A bare DrawSphere is
        // unlit and flat, which is exactly why every spell read as "an orb of a
        // different colour". The halo is the cheapest readable upgrade (one
        // billboard, no light, no particles) and it makes the element colour
        // visible in a dark corridor without spending a transient light slot.
        if (p.spell)
            CombatFX::Instance().DrawGlow(camera, p.position, c, 0.85f, p.element);
        // Optional glow sprite
        if (p.sprite && p.sprite->id > 0) {
            DrawBillboard(camera, *p.sprite, p.position, 0.5f, WHITE);
        }
    }
}

void PawnSystem::ClearProjectiles() {
    m_projectiles.clear();
}

// ---------------------------------------------------------------------------
// Pickup nodes
// ---------------------------------------------------------------------------
int PawnSystem::AddPickup(const PickupNode& node) {
    PickupNode n = node;
    if (n.id == 0) n.id = m_nextEntityId++;
    m_pickups.push_back(n);
    return (int)n.id;
}

void PawnSystem::RemovePickup(int id) {
    auto it = std::remove_if(m_pickups.begin(), m_pickups.end(),
        [id](const PickupNode& n) { return n.id == (uint32_t)id; });
    m_pickups.erase(it, m_pickups.end());
}

void PawnSystem::ClearPickups() {
    ClearWeaponPickupCache();
    m_pickups.clear();
}

PickupNode* PawnSystem::GetPickup(int id) {
    for (auto& n : m_pickups) {
        if (n.id == (uint32_t)id) return &n;
    }
    return nullptr;
}

void PawnSystem::ApplyPickupNetState(const std::vector<PickupNode>& net, int world_index) {
    if (world_index < 0) return;   // unknown world: nothing to reconcile against

    // Map the server's ids for this world once, rather than scanning the
    // snapshot per drawn node.
    std::unordered_map<int, bool> serverState;
    for (const auto& n : net) {
        if (n.netId < 0) continue;
        serverState[n.netId] = n.active;
    }
    if (serverState.empty()) return;   // nothing known yet; leave the level alone

    int hidden = 0, restored = 0, orphans = 0;
    for (auto& n : m_pickups) {
        // A node with no netId came from a script `spawn_pickup`, not the world
        // file. The server never issued it, so leave local authority alone
        // rather than hiding a scripted pickup that MP never owned.
        if (n.netId < 0) continue;

        auto it = serverState.find(n.netId);
        if (it == serverState.end()) {
            // Drawn but absent from the server's list. Either the server's world
            // differs from ours, or its snapshot has not arrived yet — both are
            // reported rather than silently blanking the pickup, because hiding
            // on a partial snapshot is how a whole level disappears.
            orphans++;
            continue;
        }
        if (it->second) {
            if (!n.active) { n.active = true; restored++; }
        } else if (n.active) {
            n.active = false;
            // Stop the single-player respawn clock. In MP the server owns
            // respawn and will send PICKUP_RESPAWN, so a local timer here would
            // make the pickup reappear on a clock the server knows nothing about.
            n.respawnTimer = 0.0f;
            hidden++;
        }
    }

    if (hidden || restored)
        OZ_INFO("Pickups reconciled (world %d): %d hidden, %d restored",
                world_index, hidden, restored);
    if (orphans)
        OZ_WARN("Pickups: %d drawn pickup(s) have no server entry in world %d "
                "(world files differ, or the server snapshot has not arrived)",
                orphans, world_index);
}

void PawnSystem::UpdatePickups(float dt, Vector3 playerPos, BoundingBox playerBounds) {
    for (auto& n : m_pickups) {
        if (!n.active) {
            // Handle respawn timer
            if (n.respawnTimer > 0.0f) {
                n.respawnTimer -= dt;
                if (n.respawnTimer <= 0.0f) {
                    n.active = true;
                }
            }
            continue;
        }

        // Check AABB collision with player
        BoundingBox pickupBox = {
            {n.position.x - 0.5f, n.position.y - 0.5f, n.position.z - 0.5f},
            {n.position.x + 0.5f, n.position.y + 0.5f, n.position.z + 0.5f}
        };

        if (CheckCollisionBoxes(pickupBox, playerBounds)) {
            n.active = false;

            // Map typeName to item ID via LightningScript entity registry
            int itemId = 0;
            PickupCategory category = PickupCategory::NONE;
            const EntityDef* edef = LightningEntityRegistry::Instance().Find(n.typeName);
            if (edef) {
                auto it = edef->stats.floats.find("item_id");
                if (it != edef->stats.floats.end())
                    itemId = (int)it->second;
                // Authored pickup_category. Ten defs carry it and it previously
                // had no code reference; it now drives the collect-flash tint
                // and, below, the item-id fallback.
                auto cit = edef->stats.strings.find("pickup_category");
                if (cit != edef->stats.strings.end())
                    category = PickupCategoryFromString(cit->second.c_str());
                // Def-stat respawn override (0 = never respawns, e.g. Key/Coin)
                auto rt = edef->stats.floats.find("respawn_time");
                if (rt != edef->stats.floats.end())
                    n.respawnTime = rt->second;
            }
            n.respawnTimer = n.respawnTime;
            if (itemId == 0) {
                // Fallback: scan ItemDB by name
                for (int i = 0; i < ITEM_DB_SIZE; i++) {
                    if (!ItemDB[i].name) continue; // uninitialized tail entries
                    std::string dbName(ItemDB[i].name);
                    // Remove spaces for comparison: "Health Vial" -> "HealthVial"
                    dbName.erase(std::remove(dbName.begin(), dbName.end(), ' '), dbName.end());
                    if (dbName == n.typeName) { itemId = ItemDB[i].id; break; }
                }
            }
            // Second fallback: map pickup_category onto an item id, for defs
            // that author a category but no item_id. Quest/energy pickups have
            // no inventory item, so they resolve to nothing here and are granted
            // by their on_collect hook instead.
            if (itemId == 0) {
                switch (category) {
                    case PickupCategory::HEALTH_VIAL:    itemId = item_id::HEALTH_VIAL; break;
                    case PickupCategory::MANA_VIAL:      itemId = item_id::MANA_VIAL; break;
                    case PickupCategory::ENERGY_CRYSTAL: itemId = PickupEnergyCrystalId(111); break;
                    case PickupCategory::KEY:            itemId = item_id::KEY; break;
                    case PickupCategory::COIN:           itemId = item_id::COIN; break;
                    case PickupCategory::POWERUP:        itemId = item_id::POWERUP; break;
                    case PickupCategory::AMMO:           itemId = item_id::AMMO; break;
                    default: break;  // consumable / quest: granted by script
                }
            }

            const ItemDBEntry* def = (itemId > 0) ? GetItemDef(itemId) : nullptr;
            if (def) {
                // Single shared effect path (see Items.hpp ApplyItemEffect).
                if (def->category == ItemCategory::COIN) {
                    gInventory.coins += def->value;
                } else if (!ApplyItemEffect(*def, 1)) {
                    if (!gInventory.AddToBackpack(itemId, 1))
                        OZ_WARN("Pickup: backpack full, '%s' lost", def->name);
                }
            } else if (edef && (edef->type == EntityType::WEAPON ||
                                edef->type == EntityType::ARMOR ||
                                edef->type == EntityType::CONSUMABLE ||
                                edef->type == EntityType::UPGRADE)) {
                auto& lem = LightningEntityManager::Instance();
                // Armor/upgrade defs with an authored equip_slot are equipped
                // directly; everything else goes to the first empty hotbar slot.
                if ((edef->type == EntityType::ARMOR || edef->type == EntityType::UPGRADE) &&
                    lem.AutoEquip(edef)) {
                    // equipped
                } else {
                    int instIdx = lem.Spawn(edef->name);
                    // Full hotbar used to despawn the instance, so picking up
                    // a weapon when every cell was occupied silently dropped
                    // the pickup (user reported: Pistol in slot 1, walked
                    // over the Etheral_Weaver, etheral_waver vanished with
                    // itemId=0). Queue the def name so the next flush can
                    // place it; the HUD shows the pending count.
                    if (instIdx >= 0 && !lem.HotbarPlaceFirstFree(instIdx)) {
                        lem.Despawn(instIdx);
                        if (edef->type == EntityType::WEAPON)
                            lem.StashWeapon(edef->name);
                    }
                }
            }

            // Fire the pickup's on_collect script hook (quest pickups, rewards).
            // Script hooks are local-authority; in MP the pickup is granted by
            // the server and this instance is not consulted (see Core.hpp, which
            // gates UpdatePickups on !g_network_enabled), so running the hook
            // here would double-fire every quest flag and reward.
            LightningEntityManager::Instance().TriggerCollectAction(n.typeName);

            // EventBus: pickup.collect, dispatched by the bridge to the pickup
            // def's on_pickup_collect. Additive - the direct hook above still
            // fires, so existing on_collect scripts are unaffected.
            {
                oz::events::Event ev;
                ev.topic = "pickup.collect";
                ev.s0 = n.typeName;
                ev.x = n.position.x; ev.y = n.position.y; ev.z = n.position.z;
                oz::events::EventBus::Instance().Emit(ev);
            }

            OZ_INFO("Pickup collected: %s (itemId=%d)", n.typeName.c_str(), itemId);
            m_pickupFeedback.collected = true;
            m_pickupFeedback.typeName = n.typeName;
            m_pickupFeedback.itemId = itemId;
            m_pickupFeedback.tint = PickupCategoryTintRGBA(category);
            m_pickupFeedback.category = PickupCategoryName(category);
            m_pickupFeedback.flashTimer = 0.5f;

            // Try to flush a stashed weapon into the now-maybe-free hotbar slot.
            // If nothing lands, the stash survives until the next pickup.
            LightningEntityManager::Instance().FlushNextStashedWeapon();
        }
    }
}


void PawnSystem::EnsurePortalVisual() {
    if (m_portalVisualReady) return;
    m_portalVisualReady = true;

    // Double-sided unit quad in the XY plane (faces +/-Z)
    Mesh mesh = {0};
    mesh.vertexCount = 8;
    mesh.triangleCount = 4;
    mesh.vertices  = (float*)RL_MALLOC(8 * 3 * sizeof(float));
    mesh.normals   = (float*)RL_MALLOC(8 * 3 * sizeof(float));
    mesh.texcoords = (float*)RL_MALLOC(8 * 2 * sizeof(float));
    mesh.indices   = (unsigned short*)RL_MALLOC(12 * sizeof(unsigned short));

    static const float px[8]  = {-0.5f,  0.5f, 0.5f, -0.5f, -0.5f,  0.5f, 0.5f, -0.5f};
    static const float py[8]  = {-0.5f, -0.5f, 0.5f,  0.5f, -0.5f, -0.5f, 0.5f,  0.5f};
    static const float pz[8]  = {1,1,1,1, -1,-1,-1,-1};
    static const float uvs[16] = {0,1, 1,1, 1,0, 0,0,  0,1, 1,1, 1,0, 0,0};
    static const unsigned short idx[12] = {0,1,2, 0,2,3, 4,6,5, 4,7,6};

    for (int i = 0; i < 8; i++) {
        mesh.vertices[i*3+0] = px[i];
        mesh.vertices[i*3+1] = py[i];
        mesh.vertices[i*3+2] = 0.0f;
        mesh.normals[i*3+0] = 0.0f;
        mesh.normals[i*3+1] = 0.0f;
        mesh.normals[i*3+2] = pz[i];
        mesh.texcoords[i*2+0] = uvs[i*2];
        mesh.texcoords[i*2+1] = uvs[i*2+1];
    }
    for (int i = 0; i < 12; i++) mesh.indices[i] = idx[i];

    UploadMesh(&mesh, false);
    m_portalQuadModel = LoadModelFromMesh(mesh);
    m_portalTexture = LoadTextureWithFallback("GameData/Tex/Global/EFX/cas_win_black#.png");
    if (m_portalTexture.id == 0)
        OZ_WARN("Portal shimmer texture not found: GameData/Tex/Global/EFX/cas_win_black#.png");
}

void PawnSystem::UnloadPortalVisual() {
    if (!m_portalVisualReady) return;
    m_portalVisualReady = false;
    if (m_portalTexture.id > 0) {
        UnloadTexture(m_portalTexture);
        m_portalTexture.id = 0;
    }
    if (m_portalQuadModel.meshes != nullptr || m_portalQuadModel.materials != nullptr) {
        // Detach texture first so UnloadModel's material cleanup doesn't free it twice
        if (m_portalQuadModel.materials != nullptr)
            m_portalQuadModel.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture.id = 0;
        UnloadModel(m_portalQuadModel);
    }
    m_portalQuadModel = Model{0};
}

// ---------------------------------------------------------------------------
// SkyZoneNode management
// ---------------------------------------------------------------------------
int PawnSystem::AddSkyZone(const SkyZoneNode& node) {
    SkyZoneNode n = node;
    if (n.id == 0) n.id = m_nextEntityId++;
    m_skyZones.push_back(n);
    return (int)m_skyZones.size() - 1;
}

void PawnSystem::RemoveSkyZone(int id) {
    // Unload texture before removal
    for (auto& z : m_skyZones) {
        if (z.id == (uint32_t)id && z.skyboxTex.id > 0) {
            UnloadTexture(z.skyboxTex);
            break;
        }
    }
    auto it = std::remove_if(m_skyZones.begin(), m_skyZones.end(),
        [id](const SkyZoneNode& n) { return n.id == (uint32_t)id; });
    m_skyZones.erase(it, m_skyZones.end());
    if (m_activeSkyZoneIndex >= (int)m_skyZones.size())
        m_activeSkyZoneIndex = -1;
}

void PawnSystem::ClearSkyZones() {
    for (auto& z : m_skyZones)
        if (z.skyboxTex.id > 0) UnloadTexture(z.skyboxTex);
    m_skyZones.clear();
    m_activeSkyZoneIndex = -1;
}

SkyZoneNode* PawnSystem::GetSkyZone(int id) {
    for (auto& n : m_skyZones) {
        if (n.id == (uint32_t)id) return &n;
    }
    return nullptr;
}

void PawnSystem::SetActiveSkyZone(int index) {
    m_activeSkyZoneIndex = (index >= 0 && index < (int)m_skyZones.size()) ? index : -1;
}

SkyZoneNode* PawnSystem::GetActiveSkyZone() {
    if (m_activeSkyZoneIndex >= 0 && m_activeSkyZoneIndex < (int)m_skyZones.size())
        return &m_skyZones[m_activeSkyZoneIndex];
    return nullptr;
}

// ---------------------------------------------------------------------------
// UpdateSkyZone detect if player is inside a SkyZoneNode trigger volume
// ---------------------------------------------------------------------------
void PawnSystem::UpdateSkyZone(Vector3 playerPos, BoundingBox playerBounds) {
    int prevActive = m_activeSkyZoneIndex;

    // Find the sky zone the player is inside
    m_activeSkyZoneIndex = -1;
    for (int i = 0; i < (int)m_skyZones.size(); i++) {
        auto& n = m_skyZones[i];
        if (playerPos.x >= n.bounds.min.x && playerPos.x <= n.bounds.max.x &&
            playerPos.y >= n.bounds.min.y && playerPos.y <= n.bounds.max.y &&
            playerPos.z >= n.bounds.min.z && playerPos.z <= n.bounds.max.z) {
            m_activeSkyZoneIndex = i;
            break;
        }
    }

    // Lazily (re)load the skybox texture whenever the active zone's path changes.
    // The loader stores only skyboxPath; skyboxTex was never populated, which
    // made scripted/selected skyboxes render as the world default (or nothing).
    if (m_activeSkyZoneIndex >= 0) {
        auto& n = m_skyZones[m_activeSkyZoneIndex];
        if (!n.skyboxPath.empty() && n.skyboxTex.id == 0) {
            n.skyboxTex = LoadTextureWithFallback(n.skyboxPath.c_str());
            if (n.skyboxTex.id > 0)
                OZ_INFO("SkyZone: loaded skybox '%s'", n.skyboxPath.c_str());
            else
                OZ_WARN("SkyZone: skybox '%s' not found", n.skyboxPath.c_str());
        }
    }

    // ANY active-zone change emits exit(prev) then enter(new) - including the
    // A->B transition, which used to emit NOTHING (both branches were keyed on
    // "was outside" / "is outside"): walking from one sky zone directly into
    // an adjacent/overlapping one left the first zone's fog/ambient/music
    // applied and never ran the new zone's enter. Exit comes first so an old
    // zone's on_exit restore_* opcodes cannot stomp the env the new zone's
    // enter is about to apply (Main.cpp's zone-volume path emits in the same
    // order).
    if (prevActive != m_activeSkyZoneIndex) {
        if (prevActive >= 0) {
            auto& n = m_skyZones[prevActive];
            OZ_INFO("SkyZone exit: name=%s pos=(%.1f,%.1f,%.1f)", n.name.c_str(),
                    playerPos.x, playerPos.y, playerPos.z);
            oz::events::Event ev;
            ev.topic = "zone.exit";
            ev.s0 = n.def ? n.def->name : (n.name.empty() ? "zone_sky_0" : n.name);
            ev.x = playerPos.x; ev.y = playerPos.y; ev.z = playerPos.z;
            oz::events::EventBus::Instance().Emit(ev);
        }
        if (m_activeSkyZoneIndex >= 0) {
            auto& n = m_skyZones[m_activeSkyZoneIndex];
            OZ_INFO("SkyZone enter: name=%s def=%s pos=(%.1f,%.1f,%.1f)", n.name.c_str(),
                    n.def ? n.def->name.c_str() : "null",
                    playerPos.x, playerPos.y, playerPos.z);
            oz::events::Event ev;
            ev.topic = "zone.enter";
            ev.s0 = n.def ? n.def->name : (n.name.empty() ? "zone_sky_0" : n.name);
            ev.x = playerPos.x; ev.y = playerPos.y; ev.z = playerPos.z;
            oz::events::EventBus::Instance().Emit(ev);
        }
    }
}

// ---------------------------------------------------------------------------
// SyncSkyboxState � pull pending script effects into active SkyZoneNode
// ---------------------------------------------------------------------------
void PawnSystem::SyncSkyboxState() {
    SkyZoneNode* sky = GetActiveSkyZone();
    if (!sky) return;
    auto& lem = LightningEntityManager::Instance();
    if (lem.HasPendingSkybox()) {
        sky->skyboxPath = lem.PendingSkybox();
        lem.ClearPendingSkybox();
    }
}

// ---------------------------------------------------------------------------
// Update - tick AI for all pawns
// ---------------------------------------------------------------------------
void PawnSystem::Update(Vector3 playerPos, float dt,
                        oz::physics::PlayerPhysics::CollisionQuery* collision) {
    for (auto& p : m_pawns) {
        if (!p.active) continue;
        // A corpse is not AI: it stays where it fell and plays the death clip.
        // The timer has to run before the networkControlled early-out, because
        // a server-owned pawn's death is signalled from the network layer.
        if (p.state == PawnState::DEAD) {
            p.deadTimer -= dt;
            if (p.deadTimer <= 0.0f) Despawn((int)p.id);
            continue;
        }
        // Network-controlled pawns: the server owns AI/position � no local FSM
        // (prevents double simulation and fights over position).
        if (p.networkControlled) continue;

        // Timed status (burn / chill). Ticked before the FSM so a burn kill stops
        // the pawn on the frame it dies; local pawns only, matching ApplyStatus.
        if (p.status != StatusEffect::None) {
            p.statusTimer -= dt;
            p.statusTickTimer -= dt;
            if (p.statusTickTimer <= 0.0f) {
                p.statusTickTimer = kStatusTickSeconds;
                if (p.status == StatusEffect::Burn && p.statusDamage > 0)
                    ApplyPawnDamage(p, p.statusDamage);   // handles death/corpse
                // A light tell while the status lasts: a burst per tick, so it
                // reads as a continuing condition rather than a one-off hit.
                CombatFX::Instance().SpawnImpact(
                    {p.position.x, p.position.y + 0.7f, p.position.z},
                    {0.0f, 1.0f, 0.0f}, p.statusColor, 2, 1.2f);
            }
            if (p.statusTimer <= 0.0f) {
                if (p.status == StatusEffect::Chill && p.statusBaseSpeed > 0.0f)
                    p.speed = p.statusBaseSpeed;   // undo the slow
                p.status = StatusEffect::None;
                p.statusDamage = 0;
            }
            if (p.state == PawnState::DEAD) continue;
        }

        // Snapshot for the horizontal-collision rollback below: the FSM ticks
        // move x/z directly, so the pre-tick position is what a wall test must
        // fall back to.
        const float oldX = p.position.x;
        const float oldZ = p.position.z;

        float dx = playerPos.x - p.position.x;
        float dz = playerPos.z - p.position.z;
        float distToPlayer = sqrtf(dx * dx + dz * dz);
        p.stateTimer += dt;

        PawnDef* def = FindDef(p.defName.c_str());
        const bool hover = def && def->hover;

        switch (p.state) {
            case PawnState::IDLE:
                if (distToPlayer < p.aggroRange) TransitionState(p, PawnState::CHASE);
                else if (p.stateTimer > 2.0f) TransitionState(p, PawnState::PATROL);
                break;
            case PawnState::PATROL:
                TickPatrol(p, dt);
                if (distToPlayer < p.aggroRange) TransitionState(p, PawnState::CHASE);
                break;
            case PawnState::CHASE:
                TickChase(p, playerPos, dt);
                break;
            case PawnState::RETURN:
                TickReturn(p, dt);
                break;
            case PawnState::DEAD:
                break;
        }

        // Face the direction of travel (shortest-angle smoothing) so a 3D model
        // turns instead of snapping, and a static model is not glued to the
        // camera. Billboards still face the camera in DrawAll - that branch only
        // runs when no model could be loaded.
        const float vx = p.velocity.x, vz = p.velocity.z;
        if (vx * vx + vz * vz > 0.0025f) {
            const float target = atan2f(vx, vz) * RAD2DEG;
            float d = target - p.yaw;
            while (d > 180.0f)  d -= 360.0f;
            while (d < -180.0f) d += 360.0f;
            p.yaw += d * fminf(1.0f, dt * 8.0f);
        }
        if (hover) {
            const float speed = (def->hover_speed > 0.0f) ? def->hover_speed : 1.6f;
            p.hoverPhase += dt * speed;
        }

        // Vertical + collision integration. The FSM above only ever moved x/z;
        // without this the pawn kept its spawn Y forever (floating) and walked
        // straight through walls and floors. Skipped when no world query was
        // injected, so tests/editor keep the old behaviour. Hovering defs keep
        // their authored height and are never snapped to the floor.
        if (collision && !hover) {
            p.velocity.y -= kPawnGravity * dt;
            if (p.velocity.y < -kPawnTerminal) p.velocity.y = -kPawnTerminal;
            p.position.y += p.velocity.y * dt;

            oz::physics::GroundSample ground =
                collision->SampleGround(p.position.x, p.position.z, p.position.y);
            if (ground.valid && p.position.y <= ground.y + oz::physics::kGroundEpsilon) {
                p.position.y = ground.y;
                p.velocity.y = 0.0f;
                p.onGround = true;
            } else {
                p.onGround = false;
            }

            // Horizontal blocker test: a wall must not be walked through. Resolve
            // axis-separated so a pawn scraping a wall can still slide along it.
            auto pawnBox = [&]() {
                return (BoundingBox){
                    {p.position.x - kPawnHalfWidth, p.position.y, p.position.z - kPawnHalfWidth},
                    {p.position.x + kPawnHalfWidth, p.position.y + kPawnHeight, p.position.z + kPawnHalfWidth}};
            };
            if (collision->OverlapsObstacle(pawnBox(), p.position.y)) {
                const float nx = p.position.x, nz = p.position.z;
                p.position.x = oldX;                        // try X rolled back
                if (collision->OverlapsObstacle(pawnBox(), p.position.y)) {
                    p.position.x = nx; p.position.z = oldZ; // try Z rolled back
                    if (collision->OverlapsObstacle(pawnBox(), p.position.y)) {
                        p.position.x = oldX; p.position.z = oldZ;
                        p.velocity.x = 0.0f;
                        p.velocity.z = 0.0f;
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// DrawAll - draw pawn billboards
// ---------------------------------------------------------------------------
void PawnSystem::DrawAll(Camera3D& camera, Shader litShader) {
    const float dt = GetFrameTime();
    for (auto& p : m_pawns) {
        // Corpses are drawn (state == DEAD) so the death clip is actually seen;
        // they are removed by the corpse timer in Update(), not by this skip.
        if (!p.active) continue;

        // Prefer a real 3D model when the pawn def provides one (billboard as
        // fallback keeps old sprite-only defs working). A loaded model uses the
        // FSM's smoothed yaw; the camera-facing fallback is only for billboards.
        PawnDef* def = FindDef(p.defName.c_str());
        if (def) {
            if (!p.mesh) p.mesh = EnsurePawnMesh(*def);
            if (p.mesh && p.mesh->Valid()) {
                auto* skel = dynamic_cast<oz::SkeletalMesh*>(p.mesh.get());
                auto* vmesh = dynamic_cast<oz::AnimatedMesh*>(p.mesh.get());
                bool animated = skel && skel->ClipCount() > 0;

                if (animated) {
                    SyncPawnAnim(p, def, dt);
                    if (p.animClip >= 0) skel->ApplyPose(p.animClip, p.animTime);
                } else if (vmesh && vmesh->ClipCount() > 0) {
                    // Vertex-keyframe clip (.ozanim): the shipped enemy GLBs are
                    // unrigged, so this is what lets them walk at all.
                    const std::string* clipName = nullptr;
                    switch (p.state) {
                        case PawnState::IDLE:   clipName = &def->anim_idle;   break;
                        case PawnState::PATROL: clipName = &def->anim_patrol; break;
                        case PawnState::CHASE:  clipName = &def->anim_chase;  break;
                        case PawnState::RETURN: clipName = &def->anim_return; break;
                        case PawnState::DEAD:   clipName = &def->anim_death;  break;
                    }
                    int wanted = vmesh->ResolveClip(clipName ? *clipName : std::string());
                    if (wanted < 0) wanted = 0;
                    if (wanted != p.animClip) { p.animClip = wanted; p.animTime = 0.0f; }
                    p.animTime += dt * (def->anim_speed > 0.0f ? def->anim_speed : 1.0f);
                    vmesh->ApplyVertexPose(p.animClip, p.animTime);
                    animated = true;
                }

                // Put the model's LOWEST point on the pawn's feet. A GLB whose
                // pivot is centred (most of the un-rigged enemy library) used to
                // be drawn with its origin at the feet, i.e. hovering by half its
                // height - the "air floating" enemy bug.
                const float offset = (def->model_y_offset != 0.0f)
                                         ? def->model_y_offset
                                         : def->auto_ground_offset;
                float bob = def->hover ? sinf(p.hoverPhase) * def->hover_amplitude : 0.0f;
                // A static, walking model has no step cycle of its own; a small
                // vertical bob is the cheapest honest substitute (the `.ozanim`
                // path above replaces it when an authored clip exists).
                if (!animated && !def->hover) {
                    const float vx = p.velocity.x, vz = p.velocity.z;
                    if (vx * vx + vz * vz > 0.02f) {
                        p.animTime += dt * 8.0f * (def->anim_speed > 0.0f ? def->anim_speed : 1.0f);
                        bob += fabsf(sinf(p.animTime)) * 0.06f;
                    }
                }
                // A corpse does not bob: the death clip already collapsed it, and
                // the hover oscillation read as "still alive" (a dead Floater
                // floated up and down).
                if (p.state == PawnState::DEAD) bob = 0.0f;

                oz::MeshTransform t;
                t.position = {p.position.x, p.position.y + offset + bob, p.position.z};
                t.yaw = p.yaw;
                t.scale = {def->model_scale, def->model_scale, def->model_scale};
                p.mesh->Draw(t, litShader);
                continue;
            }
        }

        if (p.sprite.id != 0) {
            if (litShader.id > 0) {
                EngineBillboard::DrawSprite(camera, p.sprite, p.position, 2.0f, WHITE, litShader);
            } else {
                DrawBillboard(camera, p.sprite, p.position, 2.0f, WHITE);
            }
        } else if (g_debugEnabled || m_showAuthoringGizmos) {
            // No model and no sprite: fall back to the editor's pawn-node gizmo
            // rather than a magenta missing-icon grid in a shipping frame.
            EngineBillboard::Draw(camera, "PawnNode", p.position, 2.0f, litShader);
        }
    }
}

// ---------------------------------------------------------------------------
// Emitter Nodes (Sound / Music markers)
// ---------------------------------------------------------------------------
int PawnSystem::AddEmitter(const EmitterNode& node) {
    EmitterNode n = node;
    if (n.id == 0) n.id = m_nextEntityId++;
    m_emitters.push_back(n);
    return (int)n.id;
}

void PawnSystem::RemoveEmitter(int id) {
    auto it = std::remove_if(m_emitters.begin(), m_emitters.end(),
        [id](const EmitterNode& n) { return n.id == (uint32_t)id; });
    m_emitters.erase(it, m_emitters.end());
}

void PawnSystem::ClearEmitters() {
    m_emitters.clear();
}

// ---------------------------------------------------------------------------
// Mesh object nodes (GameEngine.Mesh.Static / GameEngine.Mesh.Skeletal props)
// ---------------------------------------------------------------------------
int PawnSystem::AddMeshObject(const MeshObjectNode& node) {
    MeshObjectNode n = node;
    if (n.id == 0) n.id = m_nextEntityId++;
    // baseDir is only a fallback for relative paths; leave empty for the
    // GameData-rooted / absolute / package paths the browser and OZONE supply.
    m_meshObjects.push_back(n);
    return (int)n.id;
}

void PawnSystem::RemoveMeshObject(int id) {
    auto it = std::remove_if(m_meshObjects.begin(), m_meshObjects.end(),
        [id](const MeshObjectNode& n) { return n.id == (uint32_t)id; });
    m_meshObjects.erase(it, m_meshObjects.end());
}

void PawnSystem::ClearMeshObjects() {
    m_meshObjects.clear();
}

MeshObjectNode* PawnSystem::GetMeshObject(int id) {
    for (auto& n : m_meshObjects)
        if (n.id == (uint32_t)id) return &n;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Particle emitter nodes (GameEngine.ParticleEmitter)
// ---------------------------------------------------------------------------
int PawnSystem::AddParticleEmitter(const ParticleEmitterNode& node) {
    ParticleEmitterNode n = node;
    if (n.id == 0) n.id = m_nextEntityId++;
    m_particleEmitters.push_back(n);
    return (int)n.id;
}

void PawnSystem::RemoveParticleEmitter(int id) {
    auto it = std::remove_if(m_particleEmitters.begin(), m_particleEmitters.end(),
        [id](const ParticleEmitterNode& n) { return n.id == (uint32_t)id; });
    m_particleEmitters.erase(it, m_particleEmitters.end());
}

void PawnSystem::ClearParticleEmitters() {
    m_particleEmitters.clear();
}

ParticleEmitterNode* PawnSystem::GetParticleEmitter(int id) {
    for (auto& n : m_particleEmitters)
        if (n.id == (uint32_t)id) return &n;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Path node waypoints (GameEngine.PathNode)
// ---------------------------------------------------------------------------
int PawnSystem::AddPathNode(const PathNode& node) {
    PathNode n = node;
    if (n.id == 0) n.id = m_nextEntityId++;
    m_pathNodes.push_back(n);
    return (int)n.id;
}

void PawnSystem::RemovePathNode(int id) {
    // Drop the node, then strip any links that referenced it by name.
    std::string removedName;
    for (auto& n : m_pathNodes)
        if (n.id == (uint32_t)id) { removedName = n.name; break; }
    auto it = std::remove_if(m_pathNodes.begin(), m_pathNodes.end(),
        [id](const PathNode& n) { return n.id == (uint32_t)id; });
    m_pathNodes.erase(it, m_pathNodes.end());
    if (!removedName.empty()) {
        for (auto& n : m_pathNodes) {
            n.next.erase(std::remove(n.next.begin(), n.next.end(), removedName), n.next.end());
        }
    }
}

void PawnSystem::ClearPathNodes() {
    m_pathNodes.clear();
}

PathNode* PawnSystem::GetPathNode(int id) {
    for (auto& n : m_pathNodes)
        if (n.id == (uint32_t)id) return &n;
    return nullptr;
}

PathNode* PawnSystem::FindPathNodeByName(const std::string& name) {
    for (auto& n : m_pathNodes)
        if (n.name == name) return &n;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Wind zones (foliage sway)
// ---------------------------------------------------------------------------
int PawnSystem::AddWindZone(const WindZoneNode& node) {
    WindZoneNode n = node;
    if (n.id == 0) n.id = m_nextEntityId++;
    m_windZones.push_back(n);
    return (int)n.id;
}

void PawnSystem::RemoveWindZone(int id) {
    auto it = std::remove_if(m_windZones.begin(), m_windZones.end(),
        [id](const WindZoneNode& n) { return n.id == (uint32_t)id; });
    m_windZones.erase(it, m_windZones.end());
}

void PawnSystem::ClearWindZones() {
    m_windZones.clear();
}

WindZoneNode* PawnSystem::GetWindZone(int id) {
    for (auto& n : m_windZones)
        if (n.id == (uint32_t)id) return &n;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Doors (keyed)
// ---------------------------------------------------------------------------
int PawnSystem::AddDoor(const DoorNode& node) {
    DoorNode n = node;
    if (n.id == 0) n.id = m_nextEntityId++;
    m_doors.push_back(n);
    return (int)n.id;
}

void PawnSystem::RemoveDoor(int id) {
    auto it = std::remove_if(m_doors.begin(), m_doors.end(),
        [id](const DoorNode& n) { return n.id == (uint32_t)id; });
    m_doors.erase(it, m_doors.end());
}

void PawnSystem::ClearDoors() {
    m_doors.clear();
}

DoorNode* PawnSystem::GetDoor(int id) {
    for (auto& n : m_doors)
        if (n.id == (uint32_t)id) return &n;
    return nullptr;
}

Vector4 PawnSystem::SampleWind(Vector3 worldPos) const {
    Vector3 dirAccum = {0.0f, 0.0f, 0.0f};
    float strengthAccum = 0.0f;
    float freqAccum = 0.0f;
    float weightSum = 0.0f;

    for (const auto& z : m_windZones) {
        if (!z.active) continue;

        // Hard AABB test (keep for now)
        if (worldPos.x < z.bounds.min.x || worldPos.x > z.bounds.max.x ||
            worldPos.y < z.bounds.min.y || worldPos.y > z.bounds.max.y ||
            worldPos.z < z.bounds.min.z || worldPos.z > z.bounds.max.z)
            continue;

        float w = z.strength;
        if (w <= 0.0f) continue;

        dirAccum.x += z.direction.x * w;
        dirAccum.z += z.direction.z * w;
        strengthAccum += w;
        freqAccum += z.frequency * w;
        weightSum += w;
    }

    if (weightSum < 1e-5f) 
        return {0.0f, 0.0f, 0.0f, 0.0f};

    // Normalize direction
    float len = sqrtf(dirAccum.x * dirAccum.x + dirAccum.z * dirAccum.z);
    if (len > 1e-4f) {
        dirAccum.x /= len;
        dirAccum.z /= len;
    }

    // Average strength + hard safety clamp
    float strength = strengthAccum / weightSum;
    strength = std::min(strength, 2.5f);          // ← tune this (1.5 – 3.0)

    float freq = freqAccum / weightSum;

    return {dirAccum.x, dirAccum.z, strength, freq};
}

// ---------------------------------------------------------------------------
// ClearWeaponPickupCache - release every cached render mesh asset
// ---------------------------------------------------------------------------
void PawnSystem::ClearWeaponPickupCache() {
    for (auto& p : m_pawns) p.mesh.reset();
    for (auto& n : m_meshObjects) n.mesh.reset();
    oz::MeshCache::Instance().Clear();
}

// Resolve a model/texture path declared by a .ozls/.cfg entity (delegates to
// the shared oz::Mesh resolver so all taxonomy assets resolve identically).
static std::string ResolveDefAsset(const std::string& baseDir, const std::string& rel) {
    return oz::ResolveMeshAsset(baseDir, rel);
}

// Directory (with trailing separator) of a def's source file, so paths declared
// relative to the .ozls (e.g. mesh = "automag_lvl1.obj") resolve beside it.
static std::string DefAssetDir(const std::string& sourcePath) {
    size_t slash = sourcePath.find_last_of("/\\");
    if (slash == std::string::npos) return "";
    return sourcePath.substr(0, slash + 1);
}

// Load (once, via the internal MeshCache) the shared mesh for a pawn definition,
// honoring the def's mesh_type ("skeletal" -> animated, otherwise static).
std::shared_ptr<oz::Mesh> PawnSystem::EnsurePawnMesh(PawnDef& def) {
    if (def.model_path.empty()) return nullptr;
    std::string meshPath = ResolveDefAsset(def.baseDir, def.model_path);
    std::string texPath  = ResolveDefAsset(def.baseDir, def.model_texture);
    const bool pointFilter = true; // PS1 look
    std::shared_ptr<oz::Mesh> mesh;
    if (!def.anim_file.empty() && def.mesh_type != "skeletal")
        mesh = oz::MeshCache::Instance().GetAnimated(meshPath, texPath, def.anim_file,
                                                     def.baseDir, pointFilter);
    else if (def.mesh_type == "skeletal")
        // Checked: a mesh whose bone indices do not fit raylib's single loaded
        // skeleton comes back as a StaticMesh instead of deforming out of bounds.
        mesh = oz::MeshCache::Instance().GetSkeletalChecked(meshPath, texPath, def.baseDir, pointFilter);
    else
        mesh = oz::MeshCache::Instance().GetStatic(meshPath, texPath, def.baseDir, pointFilter);

    // Ground offset from the asset's own bounds: world_y = scale * min.y + pos.y,
    // so -scale * min.y puts the lowest point exactly on pos.y (the feet).
    if (mesh && mesh->Valid() && !def.ground_offset_resolved) {
        const float s = (def.model_scale > 0.0f) ? def.model_scale : 1.0f;
        def.auto_ground_offset = -mesh->Bounds().min.y * s;
        def.ground_offset_resolved = true;
    }
    return mesh;
}

// Map the pawn FSM state to its animation clip and advance playback time.
void PawnSystem::SyncPawnAnim(Pawn& p, PawnDef* def, float dt) {
    if (!def || !p.mesh) return;
    auto* skel = dynamic_cast<oz::SkeletalMesh*>(p.mesh.get());
    if (!skel || skel->ClipCount() <= 0) return;

    // play_clip override: hold a scripted one-shot clip until its timer expires.
    // Death wins over a stale timer, or a corpse holds the attack pose.
    if (p.state != PawnState::DEAD && p.forcedClipTimer > 0.0f) {
        p.forcedClipTimer -= dt;
        if (p.forcedClip >= 0 && p.forcedClip < skel->ClipCount()) {
            if (p.forcedClip != p.animClip) { p.animClip = p.forcedClip; p.animTime = 0.0f; }
            p.animTime += dt * (def->anim_speed > 0.0f ? def->anim_speed : 1.0f);
            return;
        }
    }

    const std::string* clipName = nullptr;
    switch (p.state) {
        case PawnState::IDLE:   clipName = &def->anim_idle;   break;
        case PawnState::PATROL: clipName = &def->anim_patrol; break;
        case PawnState::CHASE:  clipName = &def->anim_chase;  break;
        case PawnState::RETURN: clipName = &def->anim_return; break;
        case PawnState::DEAD:   clipName = &def->anim_death;  break;
    }
    int wanted = skel->ResolveClip(clipName ? *clipName : std::string());
    if (wanted < 0) return;
    if (wanted != p.animClip) {
        p.animClip = wanted;
        p.animTime = 0.0f;
    }
    p.animTime += dt * (def->anim_speed > 0.0f ? def->anim_speed : 1.0f);
    if (p.state == PawnState::DEAD) {
        // Hold the last frame. ApplyPose wraps with fmod (every clip loops by
        // default), so an unclamped corpse would stand back up and replay the
        // death clip while it waits to be despawned.
        const float dur = skel->ClipSeconds(wanted);
        const float lastFrame = 1.0f / oz::kSkeletalClipFps;
        if (dur > 0.0f && p.animTime > dur - lastFrame)
            p.animTime = dur - lastFrame;
        if (p.animTime < 0.0f) p.animTime = 0.0f;
    }
}

int PawnSystem::PlayPawnClip(const std::string& defName, const std::string& clipName,
                             float hold) {
    if (defName.empty() || clipName.empty()) return 0;
    int affected = 0;
    for (auto& p : m_pawns) {
        if (!p.active || p.defName != defName) continue;
        if (!p.mesh) {
            PawnDef* d = FindDef(p.defName.c_str());
            if (d) p.mesh = EnsurePawnMesh(*d);
        }
        auto* skel = p.mesh ? dynamic_cast<oz::SkeletalMesh*>(p.mesh.get()) : nullptr;
        if (!skel || skel->ClipCount() <= 0) continue;
        const int idx = skel->FindClip(clipName);
        if (idx < 0) continue;
        p.forcedClip = idx;
        p.forcedClipTimer = (hold > 0.0f ? hold : 1.0f);
        p.animClip = idx;
        p.animTime = 0.0f;
        affected++;
    }
    return affected;
}

// ---------------------------------------------------------------------------
// DrawEntities - draw player starts, pickups, zones, emitters as billboards
// ---------------------------------------------------------------------------
void PawnSystem::DrawEntities(Camera3D& camera, Shader litShader, Shader windShader) {
    // Player start billboards. Editor/debug visualisation only - these are gizmos
    // marking authoring anchors, not level art, so they are hidden unless Debug
    // is on. Previously unconditional, which put a marker at the player's feet in
    // every frame of normal play and in every --shot capture.
    const bool gizmos = g_debugEnabled || m_showAuthoringGizmos;
    if (gizmos) {
        for (auto& n : m_playerStarts) {
            EngineBillboard::Draw(camera, "PlayerStart",
                {n.position.x, n.position.y + 0.5f, n.position.z}, 1.2f, litShader);
        }
    }

    // Light gizmos. Debug/editor-only, same reasoning as the player starts.
    // A light contributes nothing to the frame on its own - it only tints
    // surfaces - so without a marker there is literally nothing to click in
    // the viewport and the light is invisible in the editor AND in a debug
    // play session. Drawn as a type-tinted billboard plus a shape hint
    // (point = sphere, spot = cone to its target, directional = arrow) so the
    // three subtypes are distinguishable at a glance.
    if (gizmos) {
        for (auto& l : m_lights) {
            if (!l.active) continue;
            Color tint = l.color;
            tint.a = 255;
            // Billboard sits slightly above the emitter so it does not z-fight
            // the shape hint drawn on the exact position.
            EngineBillboard::DrawTinted(camera, "Light",
                {l.position.x, l.position.y + 0.4f, l.position.z}, 1.0f, tint, litShader);
            if (l.type == LitLightType::POINT) {
                DrawSphereWires(l.position, l.radius > 0.1f ? l.radius * 0.25f : 1.0f,
                                12, 6, (Color){tint.r, tint.g, tint.b, 70});
            } else if (l.type == LitLightType::SPOT) {
                Vector3 toTarget = Vector3Subtract(l.target, l.position);
                DrawLine3D(l.position, l.target, (Color){tint.r, tint.g, tint.b, 160});
                if (Vector3Length(toTarget) > 0.01f) {
                    Vector3 dir = Vector3Normalize(toTarget);
                    Vector3 tip = Vector3Add(l.position, Vector3Scale(dir, 1.0f));
                    DrawSphere(tip, 0.2f, (Color){tint.r, tint.g, tint.b, 200});
                    // Cone footprint: a ring at the target end, sized from the
                    // authored outer cone half-angle and the throw distance.
                    float dist = Vector3Distance(l.position, l.target);
                    float coneR = dist * tanf(acosf(fminf(fmaxf(l.outerCone, -1.0f), 1.0f)));
                    if (coneR > 0.05f) {
                        Vector3 up = fabsf(dir.y) > 0.99f ? Vector3{1,0,0} : Vector3{0,1,0};
                        Vector3 right = Vector3Normalize(Vector3CrossProduct(dir, up));
                        Vector3 fwd = Vector3Normalize(Vector3CrossProduct(right, dir));
                        for (int i = 0; i < 12; i++) {
                            float a0 = (float)i / 12.0f * 2.0f * PI;
                            float a1 = (float)(i + 1) / 12.0f * 2.0f * PI;
                            Vector3 p0 = Vector3Add(l.target,
                                Vector3Add(Vector3Scale(right, cosf(a0) * coneR),
                                           Vector3Scale(fwd, sinf(a0) * coneR)));
                            Vector3 p1 = Vector3Add(l.target,
                                Vector3Add(Vector3Scale(right, cosf(a1) * coneR),
                                           Vector3Scale(fwd, sinf(a1) * coneR)));
                            DrawLine3D(p0, p1, (Color){tint.r, tint.g, tint.b, 90});
                        }
                    }
                }
            } else { // DIRECTIONAL
                // The loader aims a directional light from its authored position
                // at the world origin, so the arrow points along
                // (origin - position), i.e. the direction the light travels.
                Vector3 travel = Vector3Subtract({0.0f, 0.0f, 0.0f}, l.position);
                if (Vector3Length(travel) < 0.01f) travel = Vector3{0, -1, 0};
                Vector3 dir = Vector3Normalize(travel);
                Vector3 tail = Vector3Add(l.position, Vector3Scale(dir, -2.0f));
                Vector3 head = Vector3Add(l.position, Vector3Scale(dir, 4.0f));
                DrawLine3D(tail, head, (Color){tint.r, tint.g, tint.b, 200});
                DrawSphere(head, 0.25f, (Color){tint.r, tint.g, tint.b, 220});
            }
            // Lens flare / corona read as extra rings around the emitter so
            // the two visual-only flags are visible while authoring.
            if (l.corona)
                DrawSphereWires(l.position, 0.9f, 16, 8, (Color){tint.r, tint.g, tint.b, 120});
            if (l.flare)
                DrawSphereWires(l.position, 0.45f, 12, 6, (Color){255, 255, 255, 160});
        }
    }

    // Pickups with bobbing: render the entity's 3D model when it declares one
    // (top-level mesh/texture or stats strings), otherwise the billboard icon.
    for (auto& n : m_pickups) {
        if (!n.active) continue;
        const EntityDef* edef = LightningEntityRegistry::Instance().Find(n.typeName);
        float bob = sinf((float)GetTime() * 3.0f) * 0.15f;
        Vector3 pos = {n.position.x, n.position.y + 0.5f + bob, n.position.z};

        std::shared_ptr<oz::Mesh> mesh;
        bool animated = false;
        if (edef) {
            std::string meshPath = edef->mesh;
            std::string texPath = edef->texture;
            if (meshPath.empty()) {
                auto m = edef->stats.strings.find("mesh");
                if (m != edef->stats.strings.end()) meshPath = m->second;
            }
            if (texPath.empty()) {
                auto t = edef->stats.strings.find("texture");
                if (t != edef->stats.strings.end()) texPath = t->second;
            }
            if (!meshPath.empty()) {
                std::string baseDir = DefAssetDir(edef->sourcePath);
                mesh = (edef->meshType == "skeletal")
                    ? oz::MeshCache::Instance().GetSkeletalChecked(meshPath, texPath, baseDir, true)
                    : oz::MeshCache::Instance().GetStatic(meshPath, texPath, baseDir, true);
                auto* skel = dynamic_cast<oz::SkeletalMesh*>(mesh.get());
                animated = skel && skel->ClipCount() > 0;
            }
        }

        if (mesh && mesh->Valid()) {
            float yaw = atan2f(camera.position.x - pos.x, camera.position.z - pos.z) * RAD2DEG;
            if (animated) {
                auto* skel = static_cast<oz::SkeletalMesh*>(mesh.get());
                // Idle by default rather than clip 0: a weapon's first exported
                // clip is `Fire`, so a pickup with no authored clip name would
                // mime firing its gun forever.
                skel->ApplyPose(skel->ResolveClip(edef->animIdle), (float)GetTime());
            }
            oz::MeshTransform t;
            t.position = pos;
            t.yaw = yaw;
            // 1.5 is the historical pickup size; "mesh_scale" in the .ozls
            // multiplies it so an authored model can be dialled in per pickup.
            float s = 1.5f;
            if (edef) {
                auto ms = edef->stats.floats.find("mesh_scale");
                if (ms != edef->stats.floats.end() && ms->second > 0.0f) s *= ms->second;
            }
            t.scale = {s, s, s};
            mesh->Draw(t, litShader);
        } else {
            EngineBillboard::DrawPickup(camera, n.typeName.c_str(), n.position, 0.8f, litShader);
        }
    }

    // Placed static/skeletal map-object meshes (GameEngine.Mesh.*)
    for (auto& n : m_meshObjects) {
        if (!n.mesh) {
            if (!n.animFile.empty())
                n.mesh = oz::MeshCache::Instance().GetAnimated(
                    n.meshPath, n.texturePath, n.animFile, n.baseDir, true);
            else if (n.skeletal)
                n.mesh = oz::MeshCache::Instance().GetSkeletalChecked(
                    n.meshPath, n.texturePath, n.baseDir, true);
            else
                n.mesh = oz::MeshCache::Instance().GetStatic(
                    n.meshPath, n.texturePath, n.baseDir, true);
        }
        if (!n.mesh || !n.mesh->Valid()) continue;

        if (!n.animFile.empty()) {
            // External vertex-keyframe clip (apply-pose-then-draw).
            auto* am = dynamic_cast<oz::AnimatedMesh*>(n.mesh.get());
            if (am && am->ClipCount() > 0) {
                // Editor live-edit pose takes precedence over clip sampling.
                if (n.editPose && n.editPose->size() == (size_t)am->TotalVertexCount() * 3) {
                    am->UploadOffsets(*n.editPose);
                } else {
                    const int clip = am->ResolveClip(n.animClip);
                    if (!n.animPaused)
                        n.animTime += GetFrameTime() * (n.animSpeed > 0.0f ? n.animSpeed : 1.0f);
                    am->ApplyVertexPose(clip, n.animTime);
                }
            }
        } else if (n.skeletal) {
            auto* skel = dynamic_cast<oz::SkeletalMesh*>(n.mesh.get());
            if (skel && skel->ClipCount() > 0) {
                // The animation panel previews a GLB clip through previewClip; the
                // authored animClip stays untouched so an editor scrub cannot
                // rewrite what the world exports.
                const int clip = skel->ResolveClip(n.animPreview ? n.previewClip : n.animClip);
                // The vertex-keyframe branch above already honoured animPaused;
                // this one advanced n.animTime unconditionally, so the editor's
                // Pause button did nothing for a GLB clip.
                if (!n.animPaused)
                    n.animTime += GetFrameTime() * (n.animSpeed > 0.0f ? n.animSpeed : 1.0f);
                skel->ApplyPose(clip, n.animTime);
            }
        }
        // Wind-swayed foliage uses the wind shader; upload the zone sample so
        // the height-weighted displacement reflects the enclosing WindZone(s).
        Shader shader = litShader;
        if (n.windAffected && windShader.id > 0) {
            shader = windShader;
            Vector4 wind = SampleWind(n.position);
            if (wind.z > 0.0f) {
                const BoundingBox& b = n.mesh->Bounds();
                oz::SetWindUniforms(shader, wind, (float)GetTime(), b.min.y,
                                    b.max.y - b.min.y);
            } else {
                oz::SetWindUniforms(shader, {0, 0, 0, 0}, 0.0f, 0.0f, 1.0f);
            }
        }

        // Baked material graph, if this prop names one. Bound per draw and
        // cleared afterwards because the Mesh is cached and shared.
        oz::MeshMaterial meshMat;
        if (!n.materialName.empty()) {
            if (const ozmat::MaterialAsset* asset =
                    ozmat::MaterialRegistry::Instance().Find(n.materialName)) {
                meshMat.diffuse = asset->diffuse;
                meshMat.normal = asset->normal;
                meshMat.emissive = asset->emissive;
                meshMat.normalStrength = asset->normalStrength;
                meshMat.emissiveScale = asset->emissiveScale;
                n.mesh->SetMaterial(meshMat);
            }
        }

        oz::MeshTransform t;
        t.position = n.position;
        t.yaw = n.yaw;
        t.scale = {n.scale, n.scale, n.scale};
        n.mesh->Draw(t, shader);
        n.mesh->ClearMaterial();
    }

    // Zone billboards at center of bounding box (volumes live in ZoneManager),
    // plus sound/music emitter markers. Both are authoring gizmos, so they are
    // Debug-only in the game - otherwise a big icon floats in the middle of every
    // zone in normal play and in every capture. The editor opts in through
    // SetShowAuthoringGizmos so its viewport is not blank.
    if (gizmos) {
        for (auto& n : ZoneManager::Instance().GetZones()) {
            Vector3 center = {
                (n.bounds.min.x + n.bounds.max.x) * 0.5f,
                (n.bounds.min.y + n.bounds.max.y) * 0.5f,
                (n.bounds.min.z + n.bounds.max.z) * 0.5f
            };
            const char* icon = "ZoneInfo";
            if (n.zoneType == ZoneType::ZONE_WATER) icon = "ZoneWater";
            else if (n.zoneType == ZoneType::ZONE_LADDER) icon = "ZoneLadder";
            else if (n.zoneType == ZoneType::ZONE_SKY) icon = "ZoneSky";
            else if (n.zoneType == ZoneType::ZONE_GAMEPLAY_SOUND) icon = "ZoneSound";
            else if (n.zoneType == ZoneType::ZONE_REVERB) icon = "ZoneReverb";
            EngineBillboard::Draw(camera, icon, center, 1.0f, litShader);
        }

        for (auto& n : m_emitters) {
            const char* icon = (n.type == EmitterType::SOUND) ? "Sound" : "Music";
            EngineBillboard::Draw(camera, icon, {n.position.x, n.position.y + 0.5f, n.position.z}, 1.0f, litShader);
        }
    }

    // Portal visuals: shimmer plane on the thinnest face of the trigger volume
    const auto& portals = ZoneManager::Instance().GetPortals();
    if (portals.empty())
        UnloadPortalVisual();   // nothing left to shimmer — release the quad
    for (auto& p : portals) {
        if (!p.enabled) continue;
        EnsurePortalVisual();
        Vector3 center = {
            (p.bounds.min.x + p.bounds.max.x) * 0.5f,
            (p.bounds.min.y + p.bounds.max.y) * 0.5f,
            (p.bounds.min.z + p.bounds.max.z) * 0.5f
        };
        Vector3 size = {
            p.bounds.max.x - p.bounds.min.x,
            p.bounds.max.y - p.bounds.min.y,
            p.bounds.max.z - p.bounds.min.z
        };
        Vector3 rotAxis = {0, 1, 0};
        float rotAngle = 0.0f;
        Vector3 scale = {size.x, size.y, 1.0f};
        if (size.z <= size.x && size.z <= size.y) {
            // Door in XY plane, faces +/-Z: no rotation
        } else if (size.x <= size.y) {
            // Door in ZY plane, faces +/-X: yaw the quad 90 degrees
            rotAngle = 90.0f;
            scale = {size.z, size.y, 1.0f};
        } else {
            // Horizontal hatch, faces +/-Y: pitch the quad 90 degrees
            rotAxis = {1, 0, 0};
            rotAngle = 90.0f;
            scale = {size.x, size.z, 1.0f};
        }
        if (m_portalQuadModel.meshes != nullptr && m_portalQuadModel.materials != nullptr) {
            if (litShader.id > 0)
                m_portalQuadModel.materials[0].shader = litShader;
            m_portalQuadModel.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = m_portalTexture;
            BeginBlendMode(BLEND_ALPHA);
            rlDisableDepthMask();
            DrawModelEx(m_portalQuadModel, center, rotAxis, rotAngle, scale, WHITE);
            rlEnableDepthMask();
            EndBlendMode();
        }
        EngineBillboard::Draw(camera, "Portal", center, 1.2f, litShader);
    }

    // GameEngine.ParticleEmitter � particles live in (and are drawn by) the
    // isolated simulation manager; the emitter nodes themselves are boxed here.
    OzParticleSimulationManager::Instance().Draw(camera, &LitLightning_TransientLights());
}

// ---------------------------------------------------------------------------
// FSM tick helpers
// ---------------------------------------------------------------------------
void PawnSystem::TickPatrol(Pawn& p, float dt) {
    // Follow the authored PathNode graph when the world has one. The server has
    // always done this; the client walked a slow arc regardless, so an NPC never
    // visited the waypoints the author placed.
    if (!m_pathNodes.empty()) {
        if (p.pathTarget < 0 || p.pathTarget >= (int)m_pathNodes.size()) p.pathTarget = 0;
        const PathNode& node = m_pathNodes[(size_t)p.pathTarget];
        Vector3 dir = {node.position.x - p.position.x, 0.0f,
                       node.position.z - p.position.z};
        const float dist = sqrtf(dir.x * dir.x + dir.z * dir.z);
        const float arrive = (node.radius > 0.0f ? node.radius : 1.0f) + 0.25f;
        if (dist <= arrive) {
            // Advance along `next` (links are by NAME so they survive reordering),
            // else wrap/step so a linear chain still progresses.
            int nextIdx = -1;
            for (const std::string& name : node.next) {
                for (int i = 0; i < (int)m_pathNodes.size(); ++i) {
                    if (m_pathNodes[(size_t)i].name == name) { nextIdx = i; break; }
                }
                if (nextIdx >= 0) break;
            }
            if (nextIdx < 0)
                nextIdx = node.loop ? 0 : (p.pathTarget + 1) % (int)m_pathNodes.size();
            p.pathTarget = nextIdx;
            p.velocity.x = p.velocity.z = 0.0f;
        } else {
            const float speed = p.speed * 0.5f;
            p.velocity.x = (dir.x / dist) * speed;
            p.velocity.z = (dir.z / dist) * speed;
            p.position.x += p.velocity.x * dt;
            p.position.z += p.velocity.z * dt;
        }
        return;
    }

    p.patrolTimer += dt;
    if (p.patrolTimer > 2.0f) {
        // Random direction change
        float angle = PState(p).angle;
        p.velocity.x = cosf(angle) * p.speed * 0.5f;
        p.velocity.z = sinf(angle) * p.speed * 0.5f;
        PState(p).angle += dt * 1.5f;
        p.patrolTimer = 0.0f;
    }

    p.position.x += p.velocity.x * dt;
    p.position.z += p.velocity.z * dt;

}

void PawnSystem::TickChase(Pawn& p, const Vector3& playerPos, float dt) {
    Vector3 dir = {playerPos.x - p.position.x, 0, playerPos.z - p.position.z};
    float dist = sqrtf(dir.x * dir.x + dir.z * dir.z);
    if (dist > 0.1f) {
        dir.x /= dist;
        dir.z /= dist;
        p.velocity.x = dir.x * p.speed;
        p.velocity.z = dir.z * p.speed;
        p.position.x += p.velocity.x * dt;
        p.position.z += p.velocity.z * dt;
    } else {
        p.velocity.x = 0.0f;
        p.velocity.z = 0.0f;
    }

    // Return to patrol if player out of aggro range
    if (dist > p.aggroRange * 1.5f) {
        TransitionState(p, PawnState::RETURN);
    }
}

void PawnSystem::TickReturn(Pawn& p, float dt) {
    Vector3 dir = {p.spawnPosition.x - p.position.x, 0, p.spawnPosition.z - p.position.z};
    float dist = sqrtf(dir.x * dir.x + dir.z * dir.z);
    if (dist < 1.0f) {
        p.velocity.x = 0.0f;
        p.velocity.z = 0.0f;
        TransitionState(p, PawnState::PATROL);
    } else if (dist > 0.1f) {
        dir.x /= dist;
        dir.z /= dist;
        p.velocity.x = dir.x * p.speed;
        p.velocity.z = dir.z * p.speed;
        p.position.x += p.velocity.x * dt;
        p.position.z += p.velocity.z * dt;
    }
}

void PawnSystem::TransitionState(Pawn& p, PawnState newState) {
    p.prevState = p.state;
    p.state = newState;
    p.stateTimer = 0.0f;

    // Aggro scream: play when the pawn spots the player (with per-pawn cooldown
    // so wolf-pack encounters don't machine-gun the same sample).
#ifndef OMEGA_TEST_ENV
    if (newState == PawnState::CHASE && p.scream.frameCount > 0) {
        float now = (float)GetTime();
        if (now - p.lastScreamTime > 4.0f) {
            p.lastScreamTime = now;
            SoundManager::Instance().PlayScream(p.scream);
        }
    }
#endif

    // Fire LightningScript hook on state change if entity instance exists
#ifndef OMEGA_TEST_ENV
    if (p.scriptInstanceIndex >= 0) {
        const char* action = nullptr;
        switch (newState) {
            case PawnState::CHASE:  action = "on_chase"; break;
            case PawnState::PATROL: action = "on_patrol"; break;
            case PawnState::RETURN: action = "on_return"; break;
            case PawnState::DEAD:   action = "on_death"; break;
            default: break;
        }
        if (action) {
            auto* inst = LightningEntityManager::Instance().Get(p.scriptInstanceIndex);
            LightningEntityManager::Instance().RunAction(inst, action);
        }
    }

    // EventBus: emit a generic entity fact. The bridge maps it to
    // on_entity_<state> on the pawn's .ozls def, so a script can subscribe
    // without this call site naming the def. Additive to the direct hooks above.
    {
        const char* topic = nullptr;
        switch (newState) {
            case PawnState::CHASE:  topic = "entity.chase"; break;
            case PawnState::PATROL: topic = "entity.patrol"; break;
            case PawnState::RETURN: topic = "entity.return"; break;
            case PawnState::DEAD:   topic = "entity.death"; break;
            default: break;
        }
        if (topic) {
            oz::events::Event ev;
            ev.topic = topic;
            ev.s0 = p.defName;
            ev.actor = p.id;
            ev.x = p.position.x; ev.y = p.position.y; ev.z = p.position.z;
            oz::events::EventBus::Instance().Emit(ev);
        }
    }
#endif
}

PawnSystem::PatrolState& PawnSystem::PState(Pawn& p) {
    static std::unordered_map<uint32_t, PatrolState> states;
    return states[p.id];
}

