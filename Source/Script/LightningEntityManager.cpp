#include "LightningEntityManager.hpp"
#include "LightningEntityRegistry.hpp"
#include "../Package/PackageAssetLoader.hpp"
#include "../Audio/SoundManager.hpp"
#include "../Log.hpp"
// OMEGA_TEST_ENV builds this TU without raylib, so most of it is compiled out.
// OMEGA_PAWNSYSTEM_TEST (set only by test_pawn_system) opts back in to the
// PawnSystem/raylib types, because that target links both raylib and
// OzPawnSystem.cpp and wants the real melee path under test.
#if !defined(OMEGA_TEST_ENV) || defined(OMEGA_PAWNSYSTEM_TEST)
#define OMEGA_HAVE_PAWNSYSTEM 1
#include "../Pawn/OzPawnSystem.hpp"
#include "../Pawn/AngelPlayer/SlotBar.hpp"
#include "../Renderer/CombatFX.hpp"
#include "../GameplayFlags.hpp"
#include "../World/OzOzoneLoader.hpp"
#endif
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>

// Resolve a def-relative asset path (mirrors oz::ResolveMeshAsset without
// pulling the raylib Mesh layer into this TU / the headless tests).
static std::string ResolveDefPath(const std::string& baseDir, const std::string& rel) {
    if (rel.empty()) return rel;
    if (rel.rfind("GameData/", 0) == 0 || rel.rfind("GameData\\", 0) == 0) return rel;
    bool absolute = (rel.size() > 1 && rel[1] == ':') || rel[0] == '/' || rel[0] == '\\';
    if (absolute) return rel;
    if (!baseDir.empty()) {
        std::string dir = baseDir;
        if (dir.back() != '/' && dir.back() != '\\') dir += '/';
        std::string cand = dir + rel;
        if (IsPathFile(cand.c_str())) return cand;
    }
    return rel;
}

static std::string DefSourceDir(const std::string& sourcePath) {
    size_t s = sourcePath.find_last_of("/\\");
    return (s == std::string::npos) ? std::string() : sourcePath.substr(0, s + 1);
}

// Play a sound stat authored on an entity def (hit_sound, reload_sound,
// equip_sound, ...). Volume/pitch default to unity so an authored sound is not
// silent unless the author explicitly asked for silence.
//
// No fallback path: unlike fire/swing, these events have no sensible global
// default. A weapon with no hit_sound is simply a silent weapon, which is the
// honest reading of an unauthored key -- substituting one global "generic
// impact" clip would make every melee swing in every level sound identical.
void LightningEntityManager::PlayDefStatSound(EntityInstance* ent,
                                              const char* pathKey,
                                              const char* volKey,
                                              const char* pitchKey) {
    if (!ent || !ent->def) return;

    auto sit = ent->def->stats.strings.find(pathKey);
    if (sit == ent->def->stats.strings.end()) return;

    // Trim defensively: the stats parser stores values verbatim, so a quoted
    // path would keep its quotes and fail to resolve.
    std::string path = sit->second;
    size_t b = path.find_first_not_of(" \t\r\n\"");
    if (b == std::string::npos) return;
    size_t e = path.find_last_not_of(" \t\r\n\"");
    path = path.substr(b, e - b + 1);
    if (path.empty()) return;

    // Def-relative like projectile_mesh / viewmodel_mesh, so a weapon can
    // keep its sounds beside itself.
    path = ResolveDefPath(DefSourceDir(ent->def->sourcePath), path);

    auto readFloat = [&](const char* key, float defVal) {
        if (!key) return defVal;
        auto rit = ent->runtimeStats.find(key);
        if (rit != ent->runtimeStats.end()) return rit->second;
        auto fit = ent->def->stats.floats.find(key);
        return (fit != ent->def->stats.floats.end()) ? fit->second : defVal;
    };

    SoundManager::Instance().PlayStatSound(path, readFloat(volKey, 1.0f),
                                           readFloat(pitchKey, 1.0f));
}

namespace {

// Spark normal for a wall strike, where there is no surface normal to use.
Vector3 hitNormalOrAway(Vector3 origin, Vector3 dir) {
    return Vector3Normalize(dir);
}

}  // namespace

// ---------------------------------------------------------------------------
// FireSelectedWeapon — spawn projectiles (ranged) or swing-check (melee)
// Returns: >0 projectiles spawned (ranged), 0 melee swing, -1 didn't fire
// ---------------------------------------------------------------------------
// Swept segment vs the loaded world's collision volumes. Returns true when solid
// geometry sits between the two points.
//
// Uses the same SegmentVsAABB the projectile tracer/impact path uses, so melee
// and bullets agree on what counts as a wall. A segment that starts inside a
// volume reports no hit (the slab test's documented behaviour), which is what
// we want when the wielder is standing in a doorway or clipping a crate: the
// swing still lands.
bool LightningEntityManager::MeleeBlockedByGeometry(Vector3 from, Vector3 to) {
#if defined(OMEGA_HAVE_PAWNSYSTEM) && !defined(OMEGA_PAWNSYSTEM_TEST)
    // Only the shipping build reads world geometry. The test target has no
    // loaded world (and does not link the loader), so occlusion is a stub there.
    const auto& volumes = OzoneLoader::Instance().GetCollisionVolumes();
    float bestT = 1.0f;
    Vector3 n;
    for (const auto& cv : volumes) {
        if (cv.isHeightmap) continue;
        float t;
        Vector3 hitN;
        // Only geometry strictly between the two points blocks: a wall face the
        // segment merely grazes at t ~= 1 (the target standing in a doorway)
        // should not veto the hit.
        if (SegmentVsAABB(from, to, cv.aabb, t, hitN) && t > 0.001f && t < 0.999f && t < bestT) {
            bestT = t;
            n = hitN;
        }
    }
    return bestT < 1.0f;
#else
    (void)from; (void)to;
    return false;
#endif
}

// Weapon stash implementation (declared in the header).
//
// When a pickup of a WEAPON def lands while every hotbar cell is full, the
// pickup path used to despawn the instance and the player silently lost the
// pickup. The stash queues the def name so the next pickup attempt can land
// the weapon in the now-empty slot, and the HUD shows the count.

void LightningEntityManager::StashWeapon(const std::string& defName) {
    if (defName.empty()) return;
    m_stashedWeapons.push_back(defName);
}

bool LightningEntityManager::FlushNextStashedWeapon() {
    while (!m_stashedWeapons.empty()) {
        const std::string defName = m_stashedWeapons.front();
        m_stashedWeapons.erase(m_stashedWeapons.begin());
        const int idx = Spawn(defName.c_str());
        if (idx < 0) continue;     // def vanished; try the next stash entry
        if (HotbarPlaceFirstFree(idx)) return true;
        Despawn(idx);                // still no room; the entry is consumed
    }
    return false;
}

// Send a resolved melee hit to the server, which re-validates reach and applies
// the damage. Only meaningful for networkControlled pawns; local pawns are
// damaged directly by the caller.
void LightningEntityManager::ReportMeleeHit(const struct Pawn& target, int damage, float reach,
                                            float staminaCost, Vector3 origin, Vector3 dir) {
#ifdef OMEGA_HAVE_PAWNSYSTEM
    if (target.netNpcIndex < 0) return;
    // Routed through the client accessor hook rather than the file-static
    // OmegaClient in Main.cpp, which this translation unit cannot see.
    if (!m_on_melee_hit) return;
    m_on_melee_hit(target.netWorldIndex, target.netNpcIndex, target.netPartitionIndex,
                   damage, reach, staminaCost,
                   origin.x, origin.y, origin.z, dir.x, dir.y, dir.z);
#else
    (void)target; (void)damage; (void)reach; (void)staminaCost;
    (void)origin; (void)dir;
#endif
}

int LightningEntityManager::FireSelectedWeapon(const Vector3& origin, const Vector3& direction) {
    EntityInstance* ent = SelectedEntity();
    if (!ent || !ent->def) return -1;
    if (ent->def->type != EntityType::WEAPON) return -1;

    // Check cooldown
    if (ent->cooldownRemaining > 0.0f) return -1;

    auto readStat = [&](const std::string& key, float defVal) -> float {
        auto rit = ent->runtimeStats.find(key);
        if (rit != ent->runtimeStats.end()) return rit->second;
        auto dit = ent->def->stats.floats.find(key);
        return (dit != ent->def->stats.floats.end()) ? dit->second : defVal;
    };

    float projectileDamage = readStat("damage", 10.0f);
    float reach = readStat("reach", 0.0f);

    if (reach > 0.0f) {
        // ---- MELEE ----
        float swingSpeed = readStat("swing_speed", readStat("fire_rate", 0.25f));

        // Stamina gate. Both shipped melee defs author stamina_cost (18 / 15)
        // and it previously had no code reference, so a sword could be swung
        // as fast as its cooldown allowed with no cost. Refused swings spend
        // nothing and set no cooldown, so mashing does not bank a free hit.
        const float staminaCost = readStat("stamina_cost", 0.0f);
        if (!HasStamina(staminaCost)) return -1;

        ent->cooldownRemaining = swingSpeed;
        SpendStamina(staminaCost);

        // Run on_swing action (drains queued side-effects)
        RunAction(ent, "on_swing");

        // Honor script-set cooldown override (set_cooldown N)
        float scriptCd = ent->ctx.TakePendingFloat("__cooldown");
        if (scriptCd > 0.0f) ent->cooldownRemaining = scriptCd;

// Target selection lives in PawnSystem::ResolveMeleeTarget so it is
        // unit-testable headlessly. It returns the NEAREST valid pawn in the arc
        // — the old inline loop took the first container match, so which NPC you
        // hit depended on spawn order rather than distance.
#ifdef OMEGA_HAVE_PAWNSYSTEM
        const Pawn* bestPawn = PawnSystem::Instance().ResolveMeleeTarget(origin, direction, reach);

        if (bestPawn) {
            // Wall check: if solid geometry sits between the wielder and the
            // target, the swing stops at the wall instead of reaching through.
            const bool blocked = MeleeBlockedByGeometry(origin, bestPawn->position);

            if (!blocked) {
                RunAction(ent, "on_hit");

                // Melee connect. Inside the `!blocked` branch on purpose: a
                // swing that stops at a wall takes the else-branch below, which
                // deliberately runs no on_hit, so it must not make a connect
                // sound either.
                PlayDefStatSound(ent, "hit_sound", "hit_volume", "hit_pitch");

                Vector3 hitPos = Vector3Add(bestPawn->position, Vector3{0.0f, 0.5f, 0.0f});
                Vector3 hitNormal = Vector3Normalize(
                    Vector3Subtract(bestPawn->position, origin));

// Impact FX: sparks at the contact point plus a brief light, so
                // a melee connect reads in an unlit room like a gunshot did.
                CombatFX::Instance().SpawnImpact(hitPos, hitNormal,
                                                 Color{255, 210, 120, 255}, 9, 5.0f);
                if (g_goreEnabled)
                    CombatFX::Instance().SpawnBlood(hitPos, hitNormal, 14, 1.0f);
                CombatFX::Instance().ArmTransientLight(hitPos,
                                                        Color{255, 190, 120, 255}, 0.10f, 4.0f);
                Pawn* target = PawnSystem::Instance().Get((int)bestPawn->id);
                if (target) {
                    if (target->networkControlled) {
                        // Server owns the damage in MP: report the resolved hit
                        // so the server can re-validate reach and apply it.
                        // ApplyPawnDamage deliberately skips these pawns.
                        ReportMeleeHit(*target, (int)projectileDamage, reach,
                                       staminaCost, origin, direction);
                    } else {
                        PawnSystem::Instance().ApplyPawnDamage(*target, (int)projectileDamage);
                    }
                }
            }
else {
                // Swung into a wall in front of the target: sparks at the wall,
                // no damage, and no on_hit (nothing was hit).
                Vector3 wallHit = Vector3Add(origin, Vector3Scale(direction, reach * 0.6f));
                CombatFX::Instance().SpawnImpact(wallHit, hitNormalOrAway(origin, direction),
                                                 Color{200, 200, 210, 255}, 6, 4.0f);
            }
        }
#endif  // OMEGA_HAVE_PAWNSYSTEM
        (void)staminaCost;
        return 0; // melee swing performed
    }

    // ---- RANGED ----
    float projectileSpeed = readStat("projectile_speed", 20.0f);
    // Documented key is projectile_lifetime; legacy defs may use lifetime
    float lifetime = readStat("projectile_lifetime", readStat("lifetime", 2.0f));
    int projectileCount = (int)readStat("projectile_count", 1.0f);
    float spreadDeg = readStat("spread", 0.0f);
    float fireRate = readStat("fire_rate", 0.25f);

    // Per-weapon projectile visual (model + submesh + tint), resolved from stats.
    std::string defDir = DefSourceDir(ent->def->sourcePath);
    std::string projMesh, projTex;
    {
        auto it = ent->def->stats.strings.find("projectile_mesh");
        if (it != ent->def->stats.strings.end()) projMesh = ResolveDefPath(defDir, it->second);
        auto itt = ent->def->stats.strings.find("projectile_texture");
        if (itt != ent->def->stats.strings.end()) projTex = ResolveDefPath(defDir, itt->second);
    }
    int projSub = (int)readStat("projectile_submesh", 0.0f);
    float projScale = readStat("projectile_scale", 0.05f);
    float projYaw = readStat("projectile_yaw", 0.0f);
    float projCol[3] = {1.0f, 0.8f, 0.3f};
    {
        auto it = ent->def->stats.vec3s.find("projectile_color");
        if (it != ent->def->stats.vec3s.end()) {
            projCol[0] = it->second[0]; projCol[1] = it->second[1]; projCol[2] = it->second[2];
        }
    }

    // Ammo check
    float magazine = readStat("magazine", 0.0f);
    if (magazine > 0.0f) {
        // Initialize ammo on first fire
        auto ammoIt = ent->runtimeStats.find("ammo");
        if (ammoIt == ent->runtimeStats.end()) {
            ent->runtimeStats["ammo"] = magazine;
            ammoIt = ent->runtimeStats.find("ammo");
        }
        if (ammoIt->second <= 0.0f) {
            // Out of ammo — auto-reload
            float reloadTime = readStat("reload_time", 2.0f);
            ent->cooldownRemaining = reloadTime;
            ent->runtimeStats["ammo"] = magazine;
            RunAction(ent, "on_reload");
            float scriptCd = ent->ctx.TakePendingFloat("__cooldown");
            if (scriptCd > 0.0f) ent->cooldownRemaining = scriptCd;
            if (m_on_ammo_changed) m_on_ammo_changed(SelectedSlot(), (int)magazine, (int)magazine, 1);
            m_reloadStarted = true;  // auto-reload plays the clip too
            PlayDefStatSound(ent, "reload_sound", "reload_volume", "reload_pitch");
            return -1;
        }
        ammoIt->second -= 1.0f;
        if (m_on_ammo_changed) m_on_ammo_changed(SelectedSlot(), (int)ammoIt->second, (int)magazine, 0);
    }

    ent->cooldownRemaining = fireRate;

    // Trigger on_fire script action if defined
    RunAction(ent, "on_fire");

    // Honor script-set cooldown override (set_cooldown N)
    float scriptCd = ent->ctx.TakePendingFloat("__cooldown");
    if (scriptCd > 0.0f) ent->cooldownRemaining = scriptCd;

#ifndef OMEGA_TEST_ENV
    for (int i = 0; i < projectileCount; i++) {
        float spreadRad = spreadDeg * DEG2RAD;
        float angleOffset = (i - (projectileCount - 1) * 0.5f) * spreadRad;
        float yawOffset = (float)(rand() % 1000 - 500) / 500.0f * spreadRad * 0.5f;

        Vector3 dir = direction;
        float cosA = cosf(angleOffset);
        float sinA = sinf(angleOffset);
        Vector3 up = {0, 1, 0};
        Vector3 right = Vector3CrossProduct(dir, up);
        right = Vector3Normalize(right);
        dir = Vector3Normalize(Vector3Add(dir, Vector3Scale(right, sinA)));

        dir.y += yawOffset * 0.3f;
        dir = Vector3Normalize(dir);

        ProjectileNode p;
        p.position = origin;
        p.velocity = Vector3Scale(dir, projectileSpeed);
        p.damage = projectileDamage;
        p.lifetime = lifetime;
        p.speed = projectileSpeed;
        p.ownerId = -1;
        p.meshPath = projMesh;
        p.texturePath = projTex;
        p.submesh = projSub;
        p.scale = projScale;
        p.yawOffset = projYaw;
        p.tint = Color{(unsigned char)(projCol[0] * 255.0f),
                       (unsigned char)(projCol[1] * 255.0f),
                       (unsigned char)(projCol[2] * 255.0f), 255};
        PawnSystem::Instance().SpawnProjectile(p);
    }
#endif
    return projectileCount;
}

// ---------------------------------------------------------------------------
// Spell bar + casting
// ---------------------------------------------------------------------------
void LightningEntityManager::UpdateSpellPools(float dt) {
    if (dt <= 0.0f) return;

    // Rates deliberately match the server's pools (Server/GameState.hpp ticks
    // mana +1 per 8 sim ticks and PE +1 per 12) so a spell has the same budget
    // single-player and multiplayer. They cannot be shared because the client
    // does not link the server header.
    const float maxMana = GetPlayerMaxMana();
    if (maxMana > 0.0f) {
        const float cur = GetPlayerMana();
        if (cur < maxMana) SetPlayerMana(fminf(cur + 100.0f / 8.0f * dt, maxMana));
    }
    const float maxPe = GetPlayerMaxPsychicEnergy();
    if (maxPe > 0.0f) {
        const float cur = GetPlayerPsychicEnergy();
        if (cur < maxPe) SetPlayerPsychicEnergy(fminf(cur + 100.0f / 12.0f * dt, maxPe));
    }
}

bool LightningEntityManager::HasMana(float cost) const {
    if (cost <= 0.0f) return true;
    return GetPlayerMana() >= cost;
}

bool LightningEntityManager::SpendMana(float cost) {
    if (cost <= 0.0f) return true;
    const float have = GetPlayerMana();
    // Refuse without spending: a partial spend would let the player chip a spell
    // they cannot afford, and would then refuse the NEXT cast for free.
    if (have < cost) return false;
    SetPlayerMana(have - cost);
    return true;
}

bool LightningEntityManager::HasPsychicEnergy(float cost) const {
    if (cost <= 0.0f) return true;
    return GetPlayerPsychicEnergy() >= cost;
}

bool LightningEntityManager::SpendPsychicEnergy(float cost) {
    if (cost <= 0.0f) return true;
    const float have = GetPlayerPsychicEnergy();
    if (have < cost) return false;
    SetPlayerPsychicEnergy(have - cost);
    return true;
}

const std::string& LightningEntityManager::GetSpellInSlot(int slot) const {
    static const std::string kEmpty;
    if (slot < 0 || slot >= SPELL_BAR_SIZE) return kEmpty;
    return m_spellBar[slot];
}

void LightningEntityManager::SetSpellInSlot(int slot, const std::string& defName) {
    if (slot < 0 || slot >= SPELL_BAR_SIZE) return;
    m_spellBar[slot] = defName;

    // Despawn the instance the old def owned, so reassigning a slot cannot leave
    // the previous spell's cooldown ticking against an unrelated def.
    for (int i = 0; i < (int)m_instances.size(); i++) {
        if (m_instances[i].def && m_instances[i].spellSlot == slot) { Despawn(i); break; }
    }
}

EntityInstance* LightningEntityManager::GetSpellInstance(int slot) {
    if (slot < 0 || slot >= SPELL_BAR_SIZE) return nullptr;
    const std::string& name = m_spellBar[slot];
    if (name.empty()) return nullptr;

    // Reuse the live instance so a cast's cooldown and any runtime stat
    // overrides persist across casts, exactly as they do for a hotbar weapon.
    for (auto& in : m_instances) {
        if (in.def && in.def->name == name && in.spellSlot == slot) return &in;
    }

    const EntityDef* def = LightningEntityRegistry::Instance().Find(name);
    if (!def || def->type != EntityType::SPELL) return nullptr;
    const int idx = Spawn(name);
    if (idx < 0 || idx >= (int)m_instances.size()) return nullptr;
    m_instances[idx].spellSlot = slot;
    return &m_instances[idx];
}

float LightningEntityManager::GetSpellCooldown(int slot) const {
    if (slot < 0 || slot >= SPELL_BAR_SIZE) return 0.0f;
    const std::string& name = m_spellBar[slot];
    if (name.empty()) return 0.0f;
    for (const auto& in : m_instances)
        if (in.def && in.def->name == name && in.spellSlot == slot)
            return in.cooldownRemaining;
    return 0.0f;
}

float LightningEntityManager::GetSpellCost(int slot) const {
    if (slot < 0 || slot >= SPELL_BAR_SIZE) return 0.0f;
    const std::string& name = m_spellBar[slot];
    if (name.empty()) return 0.0f;
    const EntityDef* def = LightningEntityRegistry::Instance().Find(name);
    if (!def) return 0.0f;
    // Default pool is mana; a def opts into PE with cost_type.
    auto ct = def->stats.strings.find("cost_type");
    const bool usesPe = (ct != def->stats.strings.end() &&
                         (ct->second == "psychic_energy" || ct->second == "pe"));
    auto c = def->stats.floats.find(usesPe ? "pe_cost" : "mana_cost");
    return (c != def->stats.floats.end()) ? c->second : 0.0f;
}

int LightningEntityManager::CastSpell(int slot, const Vector3& origin, const Vector3& direction) {
    EntityInstance* ent = GetSpellInstance(slot);
    if (!ent || !ent->def) return -1;
    if (ent->def->type != EntityType::SPELL) return -1;
    if (ent->cooldownRemaining > 0.0f) return -1;

    auto readStat = [&](const std::string& key, float defVal) -> float {
        auto rit = ent->runtimeStats.find(key);
        if (rit != ent->runtimeStats.end()) return rit->second;
        auto dit = ent->def->stats.floats.find(key);
        return (dit != ent->def->stats.floats.end()) ? dit->second : defVal;
    };

    // Cost is resolved from the DEF, not from a runtime stat, so a script cannot
    // `playerstat mana -= N` its way to a free cast: the deduction below is the
    // only one that happens on this path.
    const bool usesPe = [&]{
        auto it = ent->def->stats.strings.find("cost_type");
        return it != ent->def->stats.strings.end() &&
               (it->second == "psychic_energy" || it->second == "pe");
    }();
    const float cost = readStat(usesPe ? "pe_cost" : "mana_cost", 0.0f);

    // Affordability BEFORE any effect is committed. A refused cast spends
    // nothing, sets no cooldown and runs no script - so mashing the key while
    // empty cannot bank a free cast for when mana trickles back.
    if (usesPe) { if (!HasPsychicEnergy(cost)) return -1; }
    else        { if (!HasMana(cost)) return -1; }

    const float cooldown = readStat("cooldown", readStat("fire_rate", 0.5f));
    const float damage = readStat("damage", 10.0f);

    // `radius` > 0 is an area effect resolved at a point `reach` units along the
    // aim; `targets_self` is a spell that acts on the caster. Both are INSTANT
    // (no projectile). Deliberately a different discriminator from
    // FireSelectedWeapon's `reach`, because there reach alone picks the melee
    // branch and a spell has no swing.
    const float radius = readStat("radius", 0.0f);
    const float reach = readStat("reach", 6.0f);
    const bool targetsSelf = readStat("targets_self", 0.0f) > 0.0f;
    const float heal = readStat("heal", 0.0f);

    ent->cooldownRemaining = cooldown;
    if (usesPe) SpendPsychicEnergy(cost); else SpendMana(cost);
    RunAction(ent, "on_cast");

    float scriptCd = ent->ctx.TakePendingFloat("__cooldown");
    if (scriptCd > 0.0f) ent->cooldownRemaining = scriptCd;

#ifdef OMEGA_HAVE_PAWNSYSTEM
    if (radius > 0.0f || targetsSelf) {
        // ---- INSTANT: resolves an effect now, spawns nothing ----
        const Vector3 centre = Vector3Add(origin, Vector3Scale(direction, reach));
        int hits = 0;

        // Support spells (heal) and area attacks (damage) are separate effects,
        // not one "apply area damage" with a negative number: a heal that
        // scrolled through the damage path would clamp against max_health
        // exactly once instead of healing toward it, and would report hits for
        // pawns it never touched.
        if (heal > 0.0f) {
            const float cur = GetPlayerHealth();
            const float maxHp = GetPlayerMaxHealth();
            if (cur < maxHp) { SetPlayerHealth(fminf(cur + heal, maxHp)); hits = 1; }
        }
        if (radius > 0.0f)
            hits += ApplyAreaDamage(centre, radius, (int)damage);

        // on_hit only when something was actually affected, so a whiffed cast
        // does not fire the connect sound. Mirrors the melee !blocked rule.
        if (hits > 0) {
            RunAction(ent, "on_hit");
            PlayDefStatSound(ent, "hit_sound", "hit_volume", "hit_pitch");
        }
        PlayDefStatSound(ent, "cast_sound", "cast_volume", "cast_pitch");
        if (radius > 0.0f)
            CombatFX::Instance().SpawnImpact(centre, hitNormalOrAway(origin, direction),
                                             Color{180, 150, 255, 255}, 14, 6.0f);
        return 0;   // instant: no projectile
    }

    // ---- PROJECTILE ----
    const float projSpeed = readStat("projectile_speed", 30.0f);
    const float projLife = readStat("projectile_lifetime", readStat("lifetime", 2.0f));
    const int projCount = (int)readStat("projectile_count", 1.0f);
    const float spreadDeg = readStat("spread", 0.0f);

    std::string projMesh, projTex;
    const std::string defDir = DefSourceDir(ent->def->sourcePath);
    auto mit = ent->def->stats.strings.find("projectile_mesh");
    if (mit != ent->def->stats.strings.end()) projMesh = ResolveDefPath(defDir, mit->second);
    auto tit = ent->def->stats.strings.find("projectile_texture");
    if (tit != ent->def->stats.strings.end()) projTex = ResolveDefPath(defDir, tit->second);
    const int projSub = (int)readStat("projectile_submesh", 0.0f);
    const float projScale = readStat("projectile_scale", 0.15f);
    const float projYaw = readStat("projectile_yaw", 0.0f);
    float projCol[3] = {0.65f, 0.5f, 1.0f};
    auto cit = ent->def->stats.vec3s.find("projectile_color");
    if (cit != ent->def->stats.vec3s.end()) {
        projCol[0] = cit->second[0]; projCol[1] = cit->second[1]; projCol[2] = cit->second[2];
    }

    for (int i = 0; i < projCount; i++) {
        Vector3 dir = direction;
        if (projCount > 1 && spreadDeg > 0.0f) {
            // Fan across the aim direction rather than jittering around it, so a
            // multi-projectile spell reads as a deliberate spread.
            const float t = ((float)i / (float)(projCount - 1)) - 0.5f;
            Vector3 right = Vector3Normalize(Vector3CrossProduct(dir, Vector3{0.0f, 1.0f, 0.0f}));
            dir = Vector3Normalize(Vector3Add(dir, Vector3Scale(right, t * spreadDeg * DEG2RAD)));
        }

        ProjectileNode p;
        p.position = origin;
        p.velocity = Vector3Scale(dir, projSpeed);
        p.damage = damage;
        p.lifetime = projLife;
        p.speed = projSpeed;
        p.ownerId = -1;
        p.meshPath = projMesh;
        p.texturePath = projTex;
        p.submesh = projSub;
        p.scale = projScale;
        p.yawOffset = projYaw;
        p.tint = Color{(unsigned char)(projCol[0] * 255.0f),
                       (unsigned char)(projCol[1] * 255.0f),
                       (unsigned char)(projCol[2] * 255.0f), 255};
        PawnSystem::Instance().SpawnProjectile(p);
    }
    return projCount;
#else
    (void)damage; (void)reach; (void)heal; (void)targetsSelf;
    return 0;
#endif  // OMEGA_HAVE_PAWNSYSTEM
}

#ifdef OMEGA_HAVE_PAWNSYSTEM
// Damage every live pawn within `radius` of `centre`. Returns the hit count so
// the caller can distinguish a connect from a whiff.
//
// Network-controlled pawns are skipped rather than damaged locally: the server
// owns their health (see ApplyPawnDamage), so damaging them here would be a
// second, client-authoritative source of truth - exactly the duplicate-damage
// bug WeaponBehaviour used to have.
int LightningEntityManager::ApplyAreaDamage(const Vector3& centre, float radius, int damage) {
    if (radius <= 0.0f || damage <= 0.0f) return 0;
    int hits = 0;
    for (const Pawn& p : PawnSystem::Instance().GetPawns()) {
        if (p.health <= 0.0f) continue;
        if (p.networkControlled) continue;
        // Pawn centre sits at the origin of its AABB but its model stands on the
        // ground, so compare against a point lifted to body height.
        const Vector3 body = Vector3Add(p.position, Vector3{0.0f, 1.0f, 0.0f});
        if (Vector3Distance(body, centre) > radius) continue;
        Pawn* target = PawnSystem::Instance().Get((int)p.id);
        if (target) { PawnSystem::Instance().ApplyPawnDamage(*target, damage); hits++; }
    }
    return hits;
}
#endif

// ---------------------------------------------------------------------------
// ReloadSelectedWeapon — manually reload the selected weapon
// Returns: true if reload was performed
// ---------------------------------------------------------------------------
bool LightningEntityManager::ReloadSelectedWeapon() {
    EntityInstance* ent = SelectedEntity();
    if (!ent || !ent->def) return false;
    if (ent->def->type != EntityType::WEAPON) return false;

    float magazine = 0.0f;
    {
        auto rit = ent->runtimeStats.find("magazine");
        auto dit = ent->def->stats.floats.find("magazine");
        magazine = (rit != ent->runtimeStats.end()) ? rit->second
                 : (dit != ent->def->stats.floats.end()) ? dit->second : 0.0f;
    }
    if (magazine <= 0.0f) return false; // no magazine stat = no reload needed

    float currentAmmo = 0.0f;
    auto ammoIt = ent->runtimeStats.find("ammo");
    if (ammoIt != ent->runtimeStats.end()) currentAmmo = ammoIt->second;
    if (currentAmmo >= magazine) return false; // already full

    float reloadTime = 0.0f;
    {
        auto rit = ent->runtimeStats.find("reload_time");
        auto dit = ent->def->stats.floats.find("reload_time");
        reloadTime = (rit != ent->runtimeStats.end()) ? rit->second
                   : (dit != ent->def->stats.floats.end()) ? dit->second : 2.0f;
    }

    ent->cooldownRemaining = reloadTime;
    ent->runtimeStats["ammo"] = magazine;

    RunAction(ent, "on_reload");
    float scriptCd = ent->ctx.TakePendingFloat("__cooldown");
    if (scriptCd > 0.0f) ent->cooldownRemaining = scriptCd;
    if (m_on_ammo_changed) m_on_ammo_changed(SelectedSlot(), (int)magazine, (int)magazine, 1);
    m_reloadStarted = true;
    PlayDefStatSound(ent, "reload_sound", "reload_volume", "reload_pitch");
    return true;
}

// ---------------------------------------------------------------------------
// Init — called at startup after registry is populated
// ---------------------------------------------------------------------------
LightningEntityManager::LightningEntityManager() {
    for (int i = 0; i < HOTBAR_SIZE; i++) m_hotbar[i] = -1;
    for (int i = 0; i < EQUIP_SLOT_COUNT; i++) m_equipment[i] = -1;
    m_selectedSlot = 0;
    m_playerEntityIndex = -1;
}

void LightningEntityManager::Init() {
    for (int i = 0; i < HOTBAR_SIZE; i++) m_hotbar[i] = -1;
    for (int i = 0; i < EQUIP_SLOT_COUNT; i++) m_equipment[i] = -1;
    m_selectedSlot = 0;
    m_playerEntityIndex = -1;
    m_stamina = GetPlayerMaxStamina();
    m_instances.clear();
    m_resources.clear();
    m_pendingFog = false;
    m_pendingSkybox.clear();
    m_pendingAmbient = false;
    m_pendingMessage.clear();
    m_playerHurt = false;
    // The spell bar holds def NAMES, not indices, so it is not touched by the
    // index remapping elsewhere in this class - but it must still be reset here
    // or a New Game in an existing process inherits the previous session's
    // bindings (the same reason m_hotbar is cleared above).
    m_spellBar.assign(SPELL_BAR_SIZE, std::string());

    // Auto-spawn player entity
    int playerIdx = Spawn("Player");
    if (playerIdx >= 0) {
        m_playerEntityIndex = playerIdx;
        EntityInstance* p = Get(playerIdx);
        if (p && p->def) {
            // Fall back to the compiled-in defaults for any stat the def did not
            // author. This used to assign unconditionally, which overwrote
            // whatever Spawn had already copied out of Player.ozls's stats block
            // — so authored health/mana/xp were silently discarded.
            auto seed = [&](const char* key, float fallback) {
                if (p->runtimeStats.find(key) == p->runtimeStats.end())
                    p->runtimeStats[key] = fallback;
            };
            seed("health", p->def->defaultHealth);
            seed("max_health", p->def->defaultMaxHealth);
            seed("mana", p->def->defaultMana);
            seed("max_mana", p->def->defaultMaxMana);
            seed("psychic_energy", p->def->defaultPsychicEnergy);
            seed("max_psychic_energy", p->def->defaultMaxPsychicEnergy);
            seed("level", (float)p->def->defaultLevel);
            seed("xp", (float)p->def->defaultXP);
            seed("xp_to_next", (float)p->def->defaultXPToNext);
        }
        // Bind stat resolver so scripts can read $health/$mana/etc.
        auto& playerCtx = m_instances[m_playerEntityIndex].ctx;
        playerCtx.SetStatResolver([this](const std::string& name) -> float {
            return ResolveScriptStat(name);
        });
        OZ_INFO("LightningEntityManager: player entity spawned at idx %d", playerIdx);
    } else {
        OZ_WARN("LightningEntityManager: could not spawn player entity");
    }

    OZ_INFO("LightningEntityManager: ready, registry has %d defs",
            LightningEntityRegistry::Instance().Count());
}

// ---------------------------------------------------------------------------
// Update — tick cooldowns and active instance scripts
// ---------------------------------------------------------------------------
void LightningEntityManager::Update(float dt) {
    for (auto& inst : m_instances) {
        if (inst.def == nullptr) continue;
        if (inst.cooldownRemaining > 0) inst.cooldownRemaining -= dt;

        // Per-frame on_tick hook (only for defs that define it)
        if (inst.ctx.FindJumpLabel("on_tick") >= 0) {
            RunAction(&inst, "on_tick");
            ApplyEntityScriptEffects(inst);
        }

        // Tick instance script context for active behaviors
        if (inst.ctx.HasMore()) {
            // Run up to 10 instructions per frame to avoid stalls
            for (int step = 0; step < 10 && inst.ctx.HasMore(); step++)
                inst.ctx.ExecuteNext();

            ApplyEntityScriptEffects(inst);
        }
    }

}

// ---------------------------------------------------------------------------
// ApplyEntityScriptEffects — drain one instance's queued side-effects into the
// host-facing pending state (sound/fog/skybox/ambient/msg/stat ops/pickup spawn)
// ---------------------------------------------------------------------------
void LightningEntityManager::ApplyEntityScriptEffects(EntityInstance& inst) {
    if (!inst.def) return;

    std::string sound = inst.ctx.PopPendingSound();
    if (!sound.empty())
        SoundManager::Instance().PlayScriptSound(sound);

    // restore_* opcodes. Handled before the set_* reads below so a restore in
    // the same body as a set wins.
    if (inst.ctx.PopPendingFogRestore()) m_pendingFogRestore = true;
    if (inst.ctx.PopPendingAmbientRestore()) m_pendingAmbientRestore = true;
    if (inst.ctx.PopPendingSkyboxRestore()) m_pendingSkyboxRestore = true;

    float fr=0, fg=0, fb=0, fd=0;
    if (inst.ctx.PopPendingFog(fr, fg, fb, fd)) {
        m_pendingFog = true;
        m_fogR = fr; m_fogG = fg; m_fogB = fb; m_fogDensity = fd;
    }

    std::string sky = inst.ctx.PopPendingSkybox();
    if (!sky.empty()) {
        m_pendingSkybox = sky;
    }

    float ar=0, ag=0, ab=0;
    if (inst.ctx.PopPendingAmbient(ar, ag, ab)) {
        m_pendingAmbient = true;
        m_ambientR = ar; m_ambientG = ag; m_ambientB = ab;
    }

    // HUD message (msg opcode)
    std::string msg = inst.ctx.PopPendingMessage();
    if (!msg.empty()) m_pendingMessage = msg;

    // Deferred player stat writes (heal/damage/playerstat opcodes)
    auto statOps = inst.ctx.PopPlayerStatOps();
    if (!statOps.empty()) ApplyPlayerStatOps(statOps);

    // Scripted damage flash
    if (inst.ctx.PopPendingHurt()) m_playerHurt = true;

#ifndef OMEGA_TEST_ENV
    // Process pending pawn spawn from spawn_pawn opcode
    auto pawnReq = inst.ctx.PopPendingPawnSpawn();
    if (pawnReq.valid && !pawnReq.name.empty()) {
        PawnSystem::Instance().Spawn(
            {pawnReq.x, pawnReq.y, pawnReq.z},
            pawnReq.name.c_str());
    }

    // Process pending pickup spawn from spawn_pickup opcode
    auto pickupReq = inst.ctx.PopPendingPickupSpawn();
    if (pickupReq.valid && !pickupReq.name.empty()) {
        PickupNode pn;
        pn.position = {pickupReq.x, pickupReq.y, pickupReq.z};
        pn.typeName = pickupReq.name;
        pn.respawnTime = pickupReq.respawnTime;
        pn.active = true;
        PawnSystem::Instance().AddPickup(pn);
    }
#endif
}

// ---------------------------------------------------------------------------
// Spawn — create a new instance from a registered entity definition
// ---------------------------------------------------------------------------
int LightningEntityManager::Spawn(const std::string& defName) {
    const EntityDef* def = LightningEntityRegistry::Instance().Find(defName);
    if (!def) {
        OZ_WARN("LightningEntityManager: unknown entity '%s'", defName.c_str());
        return -1;
    }
    return Spawn(def);
}

// Spawn — def-based variant (used when the caller already resolved the def,
// e.g. world-scoped zone defs that must not be re-looked-up by name)
int LightningEntityManager::Spawn(const EntityDef* def) {
    if (!def) return -1;

    if ((int)m_instances.size() >= MAX_ENTITIES) {
        OZ_WARN("LightningEntityManager: max entities (%d) reached", MAX_ENTITIES);
        return -1;
    }

    OZ_INFO("[CHAIN] LEM::Spawn def='%s' type=%d mesh='%s' tex='%s' icon='%s'",
            def->name.c_str(), (int)def->type, def->mesh.c_str(), def->texture.c_str(),
            def->icon.c_str());

    // Variant selection. `variants { "lvlN" { mesh_override = ... } }` parsed
    // into the def but nothing ever read it, so every weapon always rendered its
    // base mesh. Pick the highest variant tier that does not exceed the player's
    // level; the declared order is the tier order.
    std::string mesh   = def->mesh;
    std::string tex    = def->texture;
    std::string icon   = def->icon;
    int variantIdx = -1;
    if (!def->variants.empty() && def->type == EntityType::WEAPON) {
        const int level = (int)GetPlayerLevel();
        int bestTier = -1, bestIdx = -1;
        for (size_t i = 0; i < def->variants.size(); i++) {
            // "lvl3" -> 3. A name without a level prefix is treated as tier 0.
            const std::string& name = def->variants[i].name;
            int tier = 0;
            const size_t p = name.rfind("lvl");
            if (p != std::string::npos && p + 3 < name.size())
                tier = std::atoi(name.c_str() + p + 3);
            if (tier <= level && tier > bestTier) { bestTier = tier; bestIdx = (int)i; }
        }
        if (bestIdx >= 0 && !def->variants[(size_t)bestIdx].meshOverride.empty()) {
            mesh = def->variants[(size_t)bestIdx].meshOverride;
            if (!def->variants[(size_t)bestIdx].textureOverride.empty())
                tex = def->variants[(size_t)bestIdx].textureOverride;
            variantIdx = bestIdx;
            OZ_INFO("LEM::Spawn '%s' using variant '%s' (mesh='%s')",
                    def->name.c_str(), def->variants[(size_t)bestIdx].name.c_str(),
                    mesh.c_str());
        }
    }

    EntityInstance inst;
    inst.def = def;
    inst.variantIndex = variantIdx;

    // Load resources. The icon is resolved def-relative exactly like
    // projectile_mesh / viewmodel_mesh / the sound stats: a weapon keeps its
    // icon beside its def (`icon = "selenite_blade_icon.png"`), which resolves
    // from a loose checkout as well as from a package. Caching the raw def
    // string only worked when the basename happened to match a packaged entry.
    if (!mesh.empty())  inst.modelIdx   = CacheModel(mesh);
    if (!tex.empty())   inst.textureIdx = CacheTexture(tex);
    if (!icon.empty()) {
        icon = ResolveDefPath(DefSourceDir(def->sourcePath), icon);
        inst.iconIdx = CacheTexture(icon);
    }
    OZ_INFO("[CHAIN] LEM::Spawn cached model=%d tex=%d icon=%d",
            inst.modelIdx, inst.textureIdx, inst.iconIdx);

    // Initialize runtime stats from def
    for (auto& [k, v] : def->stats.floats)
        inst.runtimeStats[k] = v;

    // Initialize ammo for weapons
    if (def->type == EntityType::WEAPON) {
        auto magIt = def->stats.floats.find("magazine");
        if (magIt != def->stats.floats.end()) {
            inst.runtimeStats["ammo"] = magIt->second;
        }
    }

    // Load instance script from action blocks
    // We build a script that can be triggered by action name
    // For now, prep all action blocks as labels
    std::string scriptText;
    for (auto& action : def->actions) {
        scriptText += action.name + ":\n";
        for (auto& line : action.scriptLines)
            scriptText += line + "\n";
    }
    inst.ctx.Load(scriptText);
    // Instances start idle: park the pc at end-of-script so the first action
    // body does NOT auto-run via the Update leftover executor (consumables
    // would apply on_use at pickup time). Actions run only when triggered.
    inst.ctx.MarkCompleted();
    // Resolve player/selected-weapon stats in this instance's scripts ($health,
    // $ammo, ...) — same resolver the player entity's context gets in Init().
    inst.ctx.SetStatResolver([this](const std::string& name) { return ResolveScriptStat(name); });
    char tag[128];
    snprintf(tag, sizeof(tag), "%s:%d", def->name.c_str(), (int)m_instances.size());
    inst.ctx.SetDebugTag(tag);

    inst.owned = true;
    // variantIndex was already resolved above; do not clobber it here.
    m_instances.push_back(std::move(inst));

    int idx = (int)m_instances.size() - 1;
    OZ_INFO("[CHAIN] LEM::Spawn done '%s' index=%d", def->name.c_str(), idx);
    return idx;
}

// ---------------------------------------------------------------------------
// Despawn — remove an instance by index
// ---------------------------------------------------------------------------
void LightningEntityManager::Despawn(int index) {
    if (index < 0 || index >= (int)m_instances.size()) return;
    auto& inst = m_instances[index];

    // Free resources
    if (inst.modelIdx >= 0) UncacheResource(inst.modelIdx);
    if (inst.textureIdx >= 0) UncacheResource(inst.textureIdx);
    if (inst.iconIdx >= 0) UncacheResource(inst.iconIdx);

    // Remove from hotbar if present
    for (int s = 0; s < HOTBAR_SIZE; s++) {
        if (m_hotbar[s] == index) m_hotbar[s] = -1;
    }
    // Remove from equipment slots if present
    for (int s = 0; s < EQUIP_SLOT_COUNT; s++) {
        if (m_equipment[s] == index) m_equipment[s] = -1;
    }

    // Swap with last to keep array compact
    if (index < (int)m_instances.size() - 1) {
        m_instances[index] = std::move(m_instances.back());
        // Update hotbar references
        for (int s = 0; s < HOTBAR_SIZE; s++) {
            if (m_hotbar[s] == (int)m_instances.size() - 1) m_hotbar[s] = index;
        }
        // Update equipment references
        for (int s = 0; s < EQUIP_SLOT_COUNT; s++) {
            if (m_equipment[s] == (int)m_instances.size() - 1) m_equipment[s] = index;
        }
        // ...and the player index, which is the same kind of index with the same
        // swap semantics and was NOT remapped. Health, mana, XP, stamina and every
        // `$health`-style script variable resolve through m_playerEntityIndex, so a
        // stale one attaches them to a different entity with no diagnostic.
        //
        // DEFENSIVE, not a live bug: m_playerEntityIndex is assigned in exactly one
        // place — Init(), immediately after Spawn("Player") on a just-cleared
        // vector — so it is always 0, and the swap only moves index 0 when the
        // player is the LAST element, which needs size == 1, where
        // `index < size - 1` cannot hold. It is written because this is the third
        // index into m_instances and two of the three were maintained: an omission
        // here is a trap for whoever spawns the player anywhere other than first.
        if (m_playerEntityIndex == (int)m_instances.size() - 1) {
            m_playerEntityIndex = index;
        }
    }
    // Despawning the player itself. This one IS reachable and was a live bug:
    // m_playerEntityIndex kept naming a slot that no longer held the player. Every
    // read site bounds-checks, so nothing corrupted memory, but HasPlayerEntity() is
    // only `>= 0` — it reported a live player over a slot holding something else,
    // and every $health script variable fell back to its compiled-in default.
    if (m_playerEntityIndex == index) {
        m_playerEntityIndex = -1;   // the documented "no player entity" sentinel
    }
    m_instances.pop_back();
}

EntityInstance* LightningEntityManager::Get(int index) {
    if (index < 0 || index >= (int)m_instances.size()) return nullptr;
    return &m_instances[index];
}

// ---------------------------------------------------------------------------
// RunAction — execute a named action label on an instance
// ---------------------------------------------------------------------------
void LightningEntityManager::RunAction(EntityInstance* inst, const std::string& actionName) {
    if (!inst || !inst->def) return;
    inst->ctx.RunAction(actionName, 30);
    // Drain the side-effects the body queued (msg / play_sound / heal / damage /
    // playerstat / spawn_pawn / spawn_pickup).
    //
    // This was the single biggest scripting bug: effects were only drained from
    // on_tick, the use-selected-item path and TriggerEntityAction. Every other
    // hook - on_fire, on_swing, on_hit, on_reload, on_equip, on_unequip, the
    // pawn FSM transitions - queued its effects into the context and then
    // stranded them. All four shipped pawn .ozls death messages never displayed,
    // and `say` in a weapon action was the only thing that appeared to work
    // (because say writes straight to stdout rather than queueing).
    ApplyEntityScriptEffects(*inst);
}

// ---------------------------------------------------------------------------
// Hotbar
// ---------------------------------------------------------------------------
void LightningEntityManager::HotbarAssign(int slot, int instanceIndex) {
    OZ_INFO("[CHAIN] LEM::HotbarAssign slot=%d inst=%d", slot, instanceIndex);
    if (slot < 0 || slot >= HOTBAR_SIZE) return;
    int oldIdx = m_hotbar[slot];
    if (oldIdx >= 0 && oldIdx < (int)m_instances.size())
        RunAction(&m_instances[oldIdx], "on_unequip");
    m_hotbar[slot] = instanceIndex;
    if (instanceIndex >= 0 && instanceIndex < (int)m_instances.size())
        RunAction(&m_instances[instanceIndex], "on_equip");
    OZ_INFO("[CHAIN] LEM::HotbarAssign done slot=%d inst=%d", slot, instanceIndex);
}

// ---------------------------------------------------------------------------
// Hotbar drag reorder.
//
// HotbarSwap had no caller outside the tests, so the hotbar could not be
// rearranged at all. The drag is driven from whichever renderer knows the slot
// rectangles (SlotBar for the authored atlas bar, DrawHotbar's fallback for the
// plain rectangles) via the three calls below: the geometry stays with the
// renderer, the drag state and the swap stay here.
//
// Selecting a slot and dragging it are separate: pressing on a slot selects it
// as before, and only a press-then-move-then-release reorders. Releasing on the
// source slot is a no-op, so a plain click never disturbs the order.
// ---------------------------------------------------------------------------
void LightningEntityManager::HotbarBeginDrag(int slot) {
    if (slot < 0 || slot >= HOTBAR_SIZE) { m_dragFrom = -1; return; }
    if (m_hotbar[slot] < 0) { m_dragFrom = -1; return; }  // nothing to move
    m_dragFrom = slot;
    m_dragTo = slot;
}

void LightningEntityManager::HotbarUpdateDrag(int hoveredSlot) {
    if (m_dragFrom < 0) return;
    m_dragTo = (hoveredSlot >= 0 && hoveredSlot < HOTBAR_SIZE) ? hoveredSlot : -1;
}

void LightningEntityManager::HotbarEndDrag() {
    const int from = m_dragFrom;
    const int to   = m_dragTo;
    m_dragFrom = -1;
    m_dragTo = -1;
    if (from < 0 || to < 0 || from == to) return;
    // Both ends must be in range; the drop may land on an empty slot, which is
    // a legitimate "move into the gap" reorder.
    if (from < 0 || from >= HOTBAR_SIZE || to < 0 || to >= HOTBAR_SIZE) return;
    HotbarSwap(from, to);
}

// Complete a pending drag against a hovered slot. The fallback bar calls this
// once per frame (not once per cell) so the release edge is handled once.
// Returns true when a drag was pending.
bool LightningEntityManager::HotbarEndDragOnRelease(int hoveredSlot) {
    if (!IsHotbarDragging()) return false;
#ifndef OMEGA_TEST_ENV
    if (IsMouseButtonReleased(MOUSE_LEFT_BUTTON)) {
        HotbarUpdateDrag(hoveredSlot);
        HotbarEndDrag();
    } else {
        HotbarUpdateDrag(hoveredSlot);
    }
#else
    // Headless: no mouse, so only track the hover for direct-drive tests.
    HotbarUpdateDrag(hoveredSlot);
#endif
    return true;
}

bool LightningEntityManager::IsHotbarDragging() const { return m_dragFrom >= 0; }

int LightningEntityManager::HotbarDragFrom() const { return m_dragFrom; }

int LightningEntityManager::HotbarDragTo() const { return m_dragTo; }

void LightningEntityManager::HotbarSwap(int slotA, int slotB) {
    if (slotA < 0 || slotA >= HOTBAR_SIZE || slotB < 0 || slotB >= HOTBAR_SIZE) return;
    std::swap(m_hotbar[slotA], m_hotbar[slotB]);
}

int LightningEntityManager::HotbarAt(int slot) const {
    if (slot < 0 || slot >= HOTBAR_SIZE) return -1;
    return m_hotbar[slot];
}

int LightningEntityManager::HotbarFirstFreeSlot() const {
    for (int s = 0; s < HOTBAR_SIZE; s++)
        if (m_hotbar[s] < 0) return s;
    return -1;
}

bool LightningEntityManager::HotbarPlaceFirstFree(int instanceIndex) {
    if (instanceIndex < 0) return false;
    int slot = HotbarFirstFreeSlot();
    if (slot < 0) {
        OZ_WARN("Hotbar full - instance %d not placed", instanceIndex);
        return false;
    }
    HotbarAssign(slot, instanceIndex);
    return true;
}

void LightningEntityManager::SelectSlot(int slot) {
    if (slot < 0 || slot >= HOTBAR_SIZE) return;
    if (slot == m_selectedSlot) return;
    OZ_INFO("[CHAIN] LEM::SelectSlot slot=%d", slot);
    int oldIdx = m_hotbar[m_selectedSlot];
    if (oldIdx >= 0 && oldIdx < (int)m_instances.size())
        RunAction(&m_instances[oldIdx], "on_unequip");
    m_selectedSlot = slot;
    int newIdx = m_hotbar[slot];
    if (newIdx >= 0 && newIdx < (int)m_instances.size())
        RunAction(&m_instances[newIdx], "on_equip");

    // Per-weapon equip_sound when the def declares one, otherwise the shared
    // global handle. Preserves the existing behaviour exactly (including the
    // empty-slot clunk, which the unconditional PlayWeaponLoad() has always
    // produced) rather than silently changing it as a side effect.
    EntityInstance* wep = (newIdx >= 0 && newIdx < (int)m_instances.size())
                              ? &m_instances[newIdx] : nullptr;
    const bool authoredEquip = wep && wep->def &&
        wep->def->stats.strings.find("equip_sound") != wep->def->stats.strings.end();
    if (authoredEquip)
        PlayDefStatSound(wep, "equip_sound", "equip_volume", "equip_pitch");
    else
        SoundManager::Instance().PlayWeaponLoad();

    OZ_INFO("[CHAIN] LEM::SelectSlot done slot=%d inst=%d", slot, newIdx);
}

void LightningEntityManager::SelectSlotSkippingEmpty(int from, int dir) {
    if (dir == 0) return;
    if (from < 0) from = 0;
    if (from >= HOTBAR_SIZE) from = HOTBAR_SIZE - 1;

    // Walk at most HOTBAR_SIZE steps so a full ring of empty slots terminates.
    for (int step = 1; step <= HOTBAR_SIZE; step++) {
        int slot = ((from + dir * step) % HOTBAR_SIZE + HOTBAR_SIZE) % HOTBAR_SIZE;
        if (m_hotbar[slot] >= 0) {
            if (slot != m_selectedSlot) SelectSlot(slot);
            return;
        }
    }
    // Everything is empty: leave the selection alone rather than flailing.
}

EntityInstance* LightningEntityManager::SelectedEntity() const {
    int idx = m_hotbar[m_selectedSlot];
    EntityInstance* e = (idx >= 0 && idx < (int)m_instances.size())
        ? const_cast<EntityInstance*>(&m_instances[idx]) : nullptr;
    static int s_lastIdx = -2;
    if (idx != s_lastIdx) {
        OZ_INFO("[CHAIN] LEM::SelectedEntity slot=%d idx=%d def=%s", m_selectedSlot, idx,
                (e && e->def) ? e->def->name.c_str() : "(none)");
        s_lastIdx = idx;
    }
    return e;
}

// ---------------------------------------------------------------------------
// Resource caching
// ---------------------------------------------------------------------------
int LightningEntityManager::CacheModel(const std::string& path) {
#ifndef OMEGA_TEST_ENV
    Model model = LoadModelWithFallback(path.c_str());
#else
    Model model = LoadModel(path.c_str());
#endif
    if (model.meshes == nullptr) return -1;
    CachedResource res;
    res.type = 1;
    res.model = model;
    m_resources.push_back(res);
    return (int)m_resources.size() - 1;
}

int LightningEntityManager::CacheTexture(const std::string& path) {
#ifndef OMEGA_TEST_ENV
    Texture2D tex = LoadTextureWithFallback(path.c_str());
#else
    Texture2D tex = LoadTexture(path.c_str());
#endif
    if (tex.id == 0) return -1;
    CachedResource res;
    res.type = 2;
    res.texture = tex;
    m_resources.push_back(res);
    return (int)m_resources.size() - 1;
}

void* LightningEntityManager::GetTexture(int idx) const {
    if (idx < 0 || idx >= (int)m_resources.size() || m_resources[idx].type != 2) return nullptr;
    return (void*)&m_resources[idx].texture;
}

void* LightningEntityManager::GetIcon(int idx) const {
    return GetTexture(idx);
}

int LightningEntityManager::PrecacheModelForDef(const std::string& defName) {
    const EntityDef* def = LightningEntityRegistry::Instance().Find(defName);
    if (!def || def->mesh.empty()) return -1;
    return CacheModel(def->mesh);
}

Model* LightningEntityManager::GetModelByResourceIdx(int idx) const {
    if (idx < 0 || idx >= (int)m_resources.size() || m_resources[idx].type != 1) return nullptr;
    return const_cast<Model*>(&m_resources[idx].model);
}

void LightningEntityManager::UncacheResource(int idx) {
    if (idx < 0 || idx >= (int)m_resources.size()) return;
    if (m_resources[idx].type == 1) UnloadModel(m_resources[idx].model);
    else if (m_resources[idx].type == 2) UnloadTexture(m_resources[idx].texture);
    m_resources[idx].type = 0; // Mark as unused instead of erasing (avoids index shift)
    m_resources[idx].model = Model{0};
    m_resources[idx].texture = Texture2D{0};
}

// ---------------------------------------------------------------------------
// HandleSpellInput — Z X C V B cast spell-bar slots 0..4.
// ---------------------------------------------------------------------------
void LightningEntityManager::HandleSpellInput(bool uiBlocking, Camera3D& cam) {
    // Same modal contract as HandleInput: the spellbook and inventory own the
    // keyboard while they are up, or a cast would fire underneath the menu.
    if (uiBlocking) return;

    static const KeyboardKey kKeys[SPELL_BAR_SIZE] = {
        KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B
    };
    for (int slot = 0; slot < SPELL_BAR_SIZE; slot++) {
        if (!IsKeyPressed(kKeys[slot])) continue;
        if (GetSpellInSlot(slot).empty()) continue;   // empty slot: silent, no FX

        // Aim from the camera, matching how a shot is fired: forward from the
        // camera position, not from the muzzle.
        const Vector3 dir = Vector3Normalize(
            Vector3Subtract(cam.target, cam.position));
        CastSpell(slot, cam.position, dir);
        break;   // one cast per keypress even if several keys changed
    }
}

// ---------------------------------------------------------------------------
// HandleInput — keyboard 1-8 for hotbar, mouse wheel to cycle
// ---------------------------------------------------------------------------
void LightningEntityManager::HandleInput(bool uiBlocking) {
    // A modal owns the keyboard and the wheel while it is up.
    if (uiBlocking) return;

    if (IsKeyPressed(KEY_ONE))   SelectSlot(0);
    if (IsKeyPressed(KEY_TWO))   SelectSlot(1);
    if (IsKeyPressed(KEY_THREE)) SelectSlot(2);
    if (IsKeyPressed(KEY_FOUR))  SelectSlot(3);
    if (IsKeyPressed(KEY_FIVE))  SelectSlot(4);
    if (IsKeyPressed(KEY_SIX))   SelectSlot(5);
    if (IsKeyPressed(KEY_SEVEN)) SelectSlot(6);
    if (IsKeyPressed(KEY_EIGHT)) SelectSlot(7);

    // Mouse wheel cycles slots. GetMouseWheelMove is the only wheel source in
    // the engine (raygui's is unused here), and nothing read it before, so the
    // scroll behaviour documented in the Wiki did not exist.
    //
    // Two details keep it from feeling wrong:
    //  - one notch moves exactly one slot, but high-resolution wheels can emit
    //    several fractional notches per flick, so a direction is latched rather
    //    than applied per event;
    //  - empty slots are skipped, so scrolling lands on a weapon instead of
    //    landing on nothing. Wraps around both ends.
    const float wheel = GetMouseWheelMove();
    if (wheel != 0.0f) {
        if (wheel > 0.0f)      m_wheelDir = +1;
        else if (wheel < 0.0f) m_wheelDir = -1;
    }
    if (m_wheelDir != 0) {
        // ~0.15s lockout so one flick is one slot change.
        if (m_wheelLock > 0.0f) {
            m_wheelLock -= GetFrameTime();
        } else {
            SelectSlotSkippingEmpty(m_selectedSlot + m_wheelDir, m_wheelDir);
            m_wheelLock = 0.15f;
            // Consume the direction once it has been used. Without this the
            // latched direction never cleared and a single flick kept stepping
            // the hotbar every 0.15s forever, long after the user let go.
            m_wheelDir = 0;
        }
    }

    // R to reload selected weapon
    if (IsKeyPressed(KEY_R)) {
        ReloadSelectedWeapon();
    }

    // Enter/E to use selected item
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_E)) {
        EntityInstance* sel = SelectedEntity();
        if (sel && sel->def) {
            // Trigger on_use action. Goes through RunAction so its queued
            // side-effects (playerstat/heal/damage/msg/play_sound) are drained
            // like every other hook; the explicit drain that used to follow is
            // now redundant.
            RunAction(sel, "on_use");

            // 'consume' removes the item from the hotbar after use
            if (sel->ctx.ConsumeRequested()) {
                sel->ctx.ClearConsume();
                int idx = m_hotbar[m_selectedSlot];
                RunAction(sel, "on_unequip");
                m_hotbar[m_selectedSlot] = -1;
                Despawn(idx);
                // A consumed item frees its cell; a weapon queued while the bar
                // was full can land now.
                FlushNextStashedWeapon();
            }
        }
    }
}

// ---------------------------------------------------------------------------
// DrawHotbar — render the GameUI object bar, falling back to plain rectangles
// when GameUI.ozls declares no usable bar (missing texture or slot_rects).
// ---------------------------------------------------------------------------
void LightningEntityManager::DrawHotbar() {
    // Authored path: GameData/Tex/Engine/GameUI.ozls supplies the atlas plus a
    // per-cell rect list, and SlotBar does the mapping/drawing.
#ifndef OMEGA_TEST_ENV
    {
        // all defaults: centred, bottom, click-to-select, drag-to-reorder
        SlotBarOptions opt;
        opt.dragToReorder = true;
        int hover = -1;
        if (DrawSlotBar(opt, hover)) return;
    }
#endif

    int sw = GetScreenWidth();
    int sh = GetScreenHeight();
    int slotSize = 50;
    int margin = 4;
    int totalWidth = HOTBAR_SIZE * (slotSize + margin) - margin;
    int startX = (sw - totalWidth) / 2;
    int y = sh - slotSize - 20;

    // Same drag reorder as the authored bar, driven from these plain rectangles.
    // Only reachable when the authored bar is absent, since DrawSlotBar returns
    // early otherwise and owns the interaction there.
    int hoveredCell = -1;
    for (int s = 0; s < HOTBAR_SIZE; s++) {
        int cx = startX + s * (slotSize + margin);
        Rectangle cell{(float)cx, (float)y, (float)slotSize, (float)slotSize};
        if (CheckCollisionPointRec(GetMousePosition(), cell)) { hoveredCell = s; break; }
    }
    if (hoveredCell >= 0 && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        SelectSlot(hoveredCell);
        HotbarBeginDrag(hoveredCell);
    }
    if (IsHotbarDragging()) HotbarEndDragOnRelease(hoveredCell);

    for (int s = 0; s < HOTBAR_SIZE; s++) {
        int x = startX + s * (slotSize + margin);
        Color bg = (s == m_selectedSlot) ? (Color){60, 60, 120, 200} : (Color){30, 30, 30, 180};
        DrawRectangle(x, y, slotSize, slotSize, bg);
        DrawRectangleLines(x, y, slotSize, slotSize, (s == m_selectedSlot) ? WHITE : GRAY);

        int idx = m_hotbar[s];
        if (idx >= 0 && idx < (int)m_instances.size()) {
            EntityInstance& inst = m_instances[idx];
            if (inst.def) {
                // Draw icon if available
                if (inst.iconIdx >= 0 && inst.iconIdx < (int)m_resources.size() && m_resources[inst.iconIdx].type == 2) {
                    Texture2D& icon = m_resources[inst.iconIdx].texture;
                    if (icon.id > 0) {
                        float iconSize = slotSize - 8;
                        DrawTexturePro(icon,
                            (Rectangle){0,0,(float)icon.width,(float)icon.height},
                            (Rectangle){(float)x+4, (float)y+4, iconSize, iconSize},
                            (Vector2){0,0}, 0, WHITE);
                    }
                }
                // Draw name
                DrawText(inst.def->name.c_str(), x + 2, y + slotSize - 14, 10, LIGHTGRAY);
            }
        }

        // Draw slot number
        char num[2] = {(char)('1' + s), '\0'};
        DrawText(num, x + 2, y + 2, 12, (s == m_selectedSlot) ? WHITE : DARKGRAY);
    }
}

// ---------------------------------------------------------------------------
// TriggerZoneAction — called from PawnSystem when player enters/exits a zone
// ---------------------------------------------------------------------------
void LightningEntityManager::TriggerZoneAction(const std::string& zoneName,
                                                const std::string& actionName) {
    const EntityDef* def = LightningEntityRegistry::Instance().Find(zoneName);
    if (!def) return;
    TriggerZoneAction(def, actionName);
}

// TriggerZoneAction — def-based variant (world-scoped defs resolved at zone
// creation avoid the global registry's cross-world name collisions)
void LightningEntityManager::TriggerZoneAction(const EntityDef* def,
                                               const std::string& actionName) {
    if (!def) return;
    // Only allow zone-type entities to trigger via TriggerZoneAction
    if (def->type != EntityType::SKYZONE) return;

    // Declarative zone environment. These four body keys parse into the def but
    // nothing ever read them, so every skyzone authored fog_color /
    // ambient_light / fog_density / music and got the engine default instead.
    // Applied on enter; on_exit the script's restore_fog / restore_ambient /
    // restore_skybox opcodes take over (and now actually work).
    if (actionName == "on_enter") ApplyZoneEnvFields(*def);

    TriggerEntityAction(def, actionName);
}

void LightningEntityManager::ApplyZoneEnvFields(const EntityDef& def) {
    bool applied = false;

    auto vc = def.stats.vec3s.find("fog_color");
    if (vc != def.stats.vec3s.end()) {
        m_pendingFog = true;
        m_fogR = vc->second[0];
        m_fogG = vc->second[1];
        m_fogB = vc->second[2];
        // fog_density is optional; fall back to a mild default.
        auto fd = def.stats.floats.find("fog_density");
        m_fogDensity = (fd != def.stats.floats.end()) ? fd->second : 0.3f;
        applied = true;
        OZ_INFO("Zone '%s': fog_color (%.2f, %.2f, %.2f) density %.2f",
                def.name.c_str(), m_fogR, m_fogG, m_fogB, m_fogDensity);
    }

    auto al = def.stats.vec3s.find("ambient_light");
    if (al != def.stats.vec3s.end()) {
        m_pendingAmbient = true;
        m_ambientR = al->second[0];
        m_ambientG = al->second[1];
        m_ambientB = al->second[2];
        applied = true;
        OZ_INFO("Zone '%s': ambient_light (%.2f, %.2f, %.2f)",
                def.name.c_str(), m_ambientR, m_ambientG, m_ambientB);
    }

    // music: the authored world track. SoundManager owns playback, so hand it
    // the asset prefix rather than duplicating stream handling here.
    if (!def.music.empty()) {
        m_pendingMusic = def.music;
        applied = true;
        OZ_INFO("Zone '%s': music '%s'", def.name.c_str(), def.music.c_str());
    }

    if (applied && !def.name.empty()) m_activeEnvZoneDef = def.name;
}

// ---------------------------------------------------------------------------
// TriggerEntityAction — run a named action on an instance of the given def
// (creates a persistent instance on first use, like zone scripts)
// ---------------------------------------------------------------------------
void LightningEntityManager::TriggerEntityAction(const EntityDef* def,
                                                 const std::string& actionName) {
    if (!def) return;

    // Skip if the def doesn't define this action (no instance needed)
    bool hasAction = false;
    for (const auto& a : def->actions) {
        if (a.name == actionName) { hasAction = true; break; }
    }
    if (!hasAction) return;

    // Find existing instance or create one
    int instIdx = -1;
    for (int i = 0; i < (int)m_instances.size(); i++) {
        if (m_instances[i].def == def) { instIdx = i; break; }
    }
    const bool created = (instIdx < 0);
    if (created) instIdx = Spawn(def);
    if (instIdx < 0) return;

    EntityInstance* inst = Get(instIdx);
    if (!inst) return;

    // Jump to the action label and execute (stops at the next action label).
    // Uses the 50-step budget rather than RunAction's 30: zone enter bodies
    // routinely carry several opcodes.
    inst->ctx.RunAction(actionName, 50);
    ApplyEntityScriptEffects(*inst);

    // An instance is only worth keeping if something will run on it later.
    // Defs with an on_tick hook stay resident; everything else (zone enter/exit,
    // pickup collect) was being spawned and never released, so every collected
    // pickup and every entered zone permanently consumed a slot out of
    // MAX_ENTITIES and eventually stopped spawning.
    if (created) {
        const bool needsResident = inst->ctx.FindJumpLabel("on_tick") >= 0;
        // Use Despawn, not erase: it swaps-with-last and rewrites the hotbar /
        // equipment / player index references that a raw erase would invalidate.
        if (!needsResident) Despawn(instIdx);
    }
}

// ---------------------------------------------------------------------------
// TriggerCollectAction — fire a pickup's on_collect script when collected
// ---------------------------------------------------------------------------
void LightningEntityManager::TriggerCollectAction(const std::string& defName) {
    const EntityDef* def = LightningEntityRegistry::Instance().Find(defName);
    if (!def) return;
    if (def->type == EntityType::SKYZONE) return;
    TriggerEntityAction(def, "on_collect");
}

// ---------------------------------------------------------------------------
// Equipment slots
// ---------------------------------------------------------------------------
int LightningEntityManager::EquipmentAt(int slot) const {
    if (slot < 0 || slot >= EQUIP_SLOT_COUNT) return -1;
    return m_equipment[slot];
}

void LightningEntityManager::EquipmentAssign(int slot, int instanceIndex) {
    if (slot < 0 || slot >= EQUIP_SLOT_COUNT) return;
    // Fire on_unequip on old occupant, then despawn
    if (m_equipment[slot] >= 0) {
        int oldIdx = m_equipment[slot];
        if (oldIdx >= 0 && oldIdx < (int)m_instances.size())
            RunAction(&m_instances[oldIdx], "on_unequip");
        Despawn(oldIdx);
    }
    m_equipment[slot] = instanceIndex;
    if (instanceIndex >= 0 && instanceIndex < (int)m_instances.size())
        RunAction(&m_instances[instanceIndex], "on_equip");
}

void LightningEntityManager::EquipmentUnequip(int slot) {
    if (slot < 0 || slot >= EQUIP_SLOT_COUNT) return;
    if (m_equipment[slot] >= 0) {
        int idx = m_equipment[slot];
        // Fire on_unequip before despawning. EquipmentAssign always did this;
        // dropping a piece by hand did not, so an item's unequip hook never ran
        // when the player removed it.
        if (idx < (int)m_instances.size())
            RunAction(&m_instances[idx], "on_unequip");
        m_equipment[slot] = -1;
        Despawn(idx);
    }
}

// Swap the occupants of two equipment slots (drag-and-drop between rows).
//
// Both indices are read out and both slots CLEARED before either is re-filled,
// because EquipmentAssign despawns whatever already occupies the slot it writes:
// two back-to-back Assign calls would have the second one despawn the item the
// first one had just moved in, so dragging a helmet onto a full ring deleted one
// of the two pieces. on_unequip/on_equip still fire, once per affected instance.
void LightningEntityManager::EquipmentSwap(int a, int b) {
    if (a < 0 || a >= EQUIP_SLOT_COUNT) return;
    if (b < 0 || b >= EQUIP_SLOT_COUNT) return;
    if (a == b) return;

    const int idxA = m_equipment[a];
    const int idxB = m_equipment[b];
    // Drop a piece that is being moved off its slot (on_unequip), but do NOT
    // despawn - it is either going back into the other slot or being discarded.
    if (idxA >= 0 && idxA < (int)m_instances.size())
        RunAction(&m_instances[idxA], "on_unequip");
    if (idxB >= 0 && idxB < (int)m_instances.size())
        RunAction(&m_instances[idxB], "on_unequip");

    m_equipment[a] = -1;
    m_equipment[b] = -1;

    m_equipment[a] = idxB;
    if (idxB >= 0 && idxB < (int)m_instances.size())
        RunAction(&m_instances[idxB], "on_equip");
    m_equipment[b] = idxA;
    if (idxA >= 0 && idxA < (int)m_instances.size())
        RunAction(&m_instances[idxA], "on_equip");
}

void LightningEntityManager::EquipmentClear() {
    for (int s = 0; s < EQUIP_SLOT_COUNT; s++) {
        if (m_equipment[s] >= 0) {
            int idx = m_equipment[s];
            m_equipment[s] = -1;
            if (idx < (int)m_instances.size())
                RunAction(&m_instances[idx], "on_unequip");
            Despawn(idx);
        }
    }
}

int LightningEntityManager::EquipmentFindFreeSlot() const {
    for (int s = 0; s < EQUIP_SLOT_COUNT; s++) {
        if (m_equipment[s] < 0) return s;
    }
    return -1;
}

// Authored `equip_slot` names -> slot indices (order matches EquipSlotType).
int LightningEntityManager::EquipmentSlotFromName(const std::string& raw) {
    std::string s = raw;
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        s = s.substr(1, s.size() - 2);
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (s == "armor")      return 0;
    if (s == "jewelry1")   return 1;
    if (s == "jewelry2")   return 2;
    if (s == "helmet")     return 3;
    if (s == "boots")      return 4;
    if (s == "legs")       return 5;
    if (s == "accessory1") return 6;
    if (s == "accessory2") return 7;
    return -1;
}

bool LightningEntityManager::AutoEquip(const EntityDef* def) {
    if (!def) return false;
    auto it = def->stats.strings.find("equip_slot");
    if (it == def->stats.strings.end()) return false;
    int slot = EquipmentSlotFromName(it->second);
    if (slot < 0) return false;
    int inst = Spawn(def->name);
    if (inst < 0) return false;
    EquipmentAssign(slot, inst);
    OZ_INFO("Equip: '%s' -> slot %d", def->name.c_str(), slot);
    return true;
}

float LightningEntityManager::EquipmentStatSum(const char* key) const {
    float sum = 0.0f;
    for (int s = 0; s < EQUIP_SLOT_COUNT; s++) {
        int idx = m_equipment[s];
        if (idx < 0 || idx >= (int)m_instances.size()) continue;
        const EntityDef* def = m_instances[idx].def;
        if (!def) continue;
        auto it = def->stats.floats.find(key);
        if (it != def->stats.floats.end()) sum += it->second;
    }
    return sum;
}

float LightningEntityManager::GetPlayerDefense() const {
    return EquipmentStatSum("defense");
}

// ---------------------------------------------------------------------------
// Player stat accessors
// ---------------------------------------------------------------------------
float LightningEntityManager::GetPlayerStat(const std::string& name, float defaultVal) const {
    if (m_playerEntityIndex < 0 || m_playerEntityIndex >= (int)m_instances.size())
        return defaultVal;
    auto it = m_instances[m_playerEntityIndex].runtimeStats.find(name);
    return (it != m_instances[m_playerEntityIndex].runtimeStats.end()) ? it->second : defaultVal;
}

void LightningEntityManager::SetPlayerStat(const std::string& name, float val) {
    if (m_playerEntityIndex >= 0 && m_playerEntityIndex < (int)m_instances.size())
        m_instances[m_playerEntityIndex].runtimeStats[name] = val;
}

float LightningEntityManager::GetPlayerHealth() const { return GetPlayerStat("health", 100.0f); }
float LightningEntityManager::GetPlayerMaxHealth() const {
    return GetPlayerStat("max_health", 100.0f) + EquipmentStatSum("max_health_bonus");
}
void LightningEntityManager::SetPlayerHealth(float v) { SetPlayerStat("health", v); }

float LightningEntityManager::GetPlayerMana() const { return GetPlayerStat("mana", 0.0f); }
float LightningEntityManager::GetPlayerMaxMana() const {
    return GetPlayerStat("max_mana", 100.0f) + EquipmentStatSum("max_mana_bonus");
}
void LightningEntityManager::SetPlayerMana(float v) { SetPlayerStat("mana", v); }

// ---------------------------------------------------------------------------
// Stamina — the melee swing budget.
//
// Both shipped melee defs author `stamina_cost`, but nothing ever read it, so
// stamina was purely decorative. It is backed by ordinary player stats so it
// participates in the existing max_health/max_mana/... stat machinery and in
// save/load, and the max is equipment-extensible like the others.
// ---------------------------------------------------------------------------
// Stamina is a dedicated member rather than a playerstat, unlike health/mana.
// GetPlayerStat/SetPlayerStat are no-ops while no player entity exists
// (m_playerEntityIndex < 0, which is the case before a world is loaded and in
// every headless test), so a stat-backed pool would silently read 0 and refuse
// every swing. The max still honours equipment bonuses, so a def can raise the
// ceiling via max_stamina_bonus.
float LightningEntityManager::GetPlayerStamina() const { return m_stamina; }

float LightningEntityManager::GetPlayerMaxStamina() const {
    // 100 default matches the server's SERVER_MAX_STAMINA so both pools agree.
    return 100.0f + EquipmentStatSum("max_stamina_bonus");
}

void LightningEntityManager::SetPlayerStamina(float v) {
    m_stamina = fminf(fmaxf(v, 0.0f), GetPlayerMaxStamina());
}

bool LightningEntityManager::HasStamina(float cost) const {
    if (cost <= 0.0f) return true;
    return GetPlayerStamina() >= cost;
}

bool LightningEntityManager::SpendStamina(float cost) {
    if (cost <= 0.0f) return true;
    const float have = GetPlayerStamina();
    if (have < cost) {
        // Fail without spending: a partial spend would let a player chip a
        // weapon they cannot afford to swing.
        return false;
    }
    SetPlayerStamina(have - cost);
    return true;
}

void LightningEntityManager::UpdateStamina(float dt) {
    if (dt <= 0.0f) return;
    const float max = GetPlayerMaxStamina();
    if (max <= 0.0f) return;
    const float cur = GetPlayerStamina();
    if (cur >= max) return;
    // ~6.7s from empty to full (15/s against a 100 pool). Must match
    // STAMINA_REGEN_PER_SECOND in Server/GameState.hpp, which owns the server
    // pool; the two are kept in step deliberately rather than shared because
    // the client cannot include the server header.
    SetPlayerStamina(cur + 15.0f * dt);
}

float LightningEntityManager::GetPlayerPsychicEnergy() const { return GetPlayerStat("psychic_energy", 0.0f); }
float LightningEntityManager::GetPlayerMaxPsychicEnergy() const {
    return GetPlayerStat("max_psychic_energy", 100.0f) + EquipmentStatSum("max_psychic_energy_bonus");
}
void LightningEntityManager::SetPlayerPsychicEnergy(float v) { SetPlayerStat("psychic_energy", v); }

int LightningEntityManager::GetPlayerLevel() const { return (int)GetPlayerStat("level", 1.0f); }
void LightningEntityManager::SetPlayerLevel(int v) { SetPlayerStat("level", (float)v); }

int LightningEntityManager::GetPlayerXP() const { return (int)GetPlayerStat("xp", 0.0f); }
void LightningEntityManager::SetPlayerXP(int v) { SetPlayerStat("xp", (float)v); }

int LightningEntityManager::GetPlayerXPToNext() const { return (int)GetPlayerStat("xp_to_next", 100.0f); }
void LightningEntityManager::SetPlayerXPToNext(int v) { SetPlayerStat("xp_to_next", (float)v); }

float LightningEntityManager::GetPlayerMovementSpeed() const {
    float speed = 1.0f;
    if (m_playerEntityIndex >= 0 && m_playerEntityIndex < (int)m_instances.size()) {
        const EntityDef* d = m_instances[m_playerEntityIndex].def;
        if (d && d->movementSpeed > 0.0f) speed = d->movementSpeed;
    }
    // Unlocked ethereal skills may carry a movement bonus.
    for (const auto& s : m_unlockedSkills) {
        const EntityDef* d = LightningEntityRegistry::Instance().Find(s);
        if (!d) continue;
        auto it = d->stats.floats.find("move_speed_bonus");
        if (it != d->stats.floats.end()) speed += it->second;
    }
    // Equipped items may also carry a movement bonus.
    speed += EquipmentStatSum("move_speed_bonus");
    return speed;
}

// ---------------------------------------------------------------------------
// Skills — unlocked ethereal/angelic tree nodes (client-side, persisted in sav)
// ---------------------------------------------------------------------------
bool LightningEntityManager::IsSkillUnlocked(const std::string& name) const {
    for (const auto& s : m_unlockedSkills)
        if (s == name) return true;
    return false;
}

void LightningEntityManager::UnlockSkill(const std::string& name) {
    if (name.empty() || IsSkillUnlocked(name)) return;
    m_unlockedSkills.push_back(name);

    // Apply one-time stat bonuses declared by the skill node.
    const EntityDef* d = LightningEntityRegistry::Instance().Find(name);
    if (!d) return;
    auto applyBonus = [&](const char* statKey, const char* targetStat) {
        auto it = d->stats.floats.find(statKey);
        if (it != d->stats.floats.end())
            SetPlayerStat(targetStat, GetPlayerStat(targetStat, 0.0f) + it->second);
    };
    applyBonus("max_health_bonus", "max_health");
    applyBonus("max_mana_bonus", "max_mana");
    applyBonus("max_psychic_energy_bonus", "max_psychic_energy");
    applyBonus("health_bonus", "health");

    // Run the node's on_unlock body. Every shipped skill def defines one, but
    // nothing ever invoked it - the label parsed fine and then never fired, so
    // its msg / play_sound / playerstat lines were dead.
    //
    // Needs a live script instance, so spawn a transient one, run the action on
    // it, then despawn it: skills are unlocked by name from the registry and
    // have no hotbar/entity instance otherwise.
    int instIdx = Spawn(name);
    if (instIdx >= 0) {
        RunAction(&m_instances[(size_t)instIdx], "on_unlock");
        Despawn(instIdx);
    } else {
        OZ_WARN("UnlockSkill: no instance for '%s' — on_unlock skipped", name.c_str());
    }
}

void LightningEntityManager::RespecSkills() {
    if (m_unlockedSkills.empty()) return;

    float refundMana = 0.0f, refundEnergy = 0.0f;
    for (const auto& name : m_unlockedSkills) {
        const EntityDef* d = LightningEntityRegistry::Instance().Find(name);
        if (!d) continue;
        // Undo the one-time stat bonuses (mirrors UnlockSkill).
        auto sub = [&](const char* statKey, const char* target) {
            auto it = d->stats.floats.find(statKey);
            if (it != d->stats.floats.end())
                SetPlayerStat(target, GetPlayerStat(target, 0.0f) - it->second);
        };
        sub("max_health_bonus", "max_health");
        sub("max_mana_bonus", "max_mana");
        sub("max_psychic_energy_bonus", "max_psychic_energy");
        sub("health_bonus", "health");

        auto cit = d->stats.floats.find("cost");
        float cost = (cit != d->stats.floats.end()) ? cit->second : 0.0f;
        auto tit = d->stats.strings.find("cost_type");
        std::string ct = (tit != d->stats.strings.end()) ? tit->second : "mana";
        if (ct.size() >= 2 && ct.front() == '"' && ct.back() == '"')
            ct = ct.substr(1, ct.size() - 2);
        if (ct == "psychic_energy") refundEnergy += cost;
        else                        refundMana += cost;
    }

    m_unlockedSkills.clear();
    SetPlayerHealth(std::min(GetPlayerHealth(), GetPlayerMaxHealth()));
    SetPlayerMana(std::min(GetPlayerMana() + refundMana, GetPlayerMaxMana()));
    SetPlayerPsychicEnergy(std::min(GetPlayerPsychicEnergy() + refundEnergy,
                                    GetPlayerMaxPsychicEnergy()));
    OZ_INFO("Respec: refunded %.0f mana, %.0f psychic energy", refundMana, refundEnergy);
}

// ---------------------------------------------------------------------------
// ResolveScriptStat — external stat provider for script $name tokens.
// Reads the player entity's runtimeStats first, then falls back to the
// selected weapon/entity instance's runtimeStats (ammo etc.).
// ---------------------------------------------------------------------------
float LightningEntityManager::ResolveScriptStat(const std::string& name) const {
    if (m_playerEntityIndex >= 0 && m_playerEntityIndex < (int)m_instances.size()) {
        auto& rs = m_instances[m_playerEntityIndex].runtimeStats;
        auto it = rs.find(name);
        if (it != rs.end()) return it->second;
    }
    EntityInstance* sel = SelectedEntity();
    if (sel && sel->def) {
        auto it = sel->runtimeStats.find(name);
        if (it != sel->runtimeStats.end()) return it->second;
        auto dit = sel->def->stats.floats.find(name);
        if (dit != sel->def->stats.floats.end()) return dit->second;
    }
    return 0.0f;
}

// ---------------------------------------------------------------------------
// ApplyPlayerStatOps — apply deferred script stat writes to the player entity
// ---------------------------------------------------------------------------
void LightningEntityManager::ApplyPlayerStatOps(
    const std::vector<LightningScriptContext::PlayerStatOp>& ops) {
    for (const auto& op : ops) {
        float cur = GetPlayerStat(op.name, 0.0f);
        float nv = cur;
        switch (op.op) {
            case 0: nv = op.value; break;
            case 1: nv = cur + op.value; break;
            case 2: nv = cur - op.value; break;
            case 3: nv = cur * op.value; break;
            case 4: if (op.value != 0.0f) nv = cur / op.value; break;
            default: break;
        }
        // Clamp the resource pools to sane ranges
        if (op.name == "health") {
            float maxh = GetPlayerStat("max_health", 100.0f);
            nv = std::max(0.0f, std::min(nv, maxh));
        } else if (op.name == "mana") {
            float maxm = GetPlayerStat("max_mana", 100.0f);
            nv = std::max(0.0f, std::min(nv, maxm));
        } else if (op.name == "psychic_energy") {
            float maxp = GetPlayerStat("max_psychic_energy", 100.0f);
            nv = std::max(0.0f, std::min(nv, maxp));
        }
        SetPlayerStat(op.name, nv);
    }
}

// ---------------------------------------------------------------------------
// Serialization — compact hotbar + equipment state for TF.sav
// Format: "hotbar:def1,def2,...|equip:def1,,,,...|"
// Empty slots are empty strings (consecutive commas).
// ---------------------------------------------------------------------------
std::string LightningEntityManager::SerializeState() const {
    std::string result;
    // Hotbar
    result += "hotbar:";
    for (int s = 0; s < HOTBAR_SIZE; s++) {
        if (s > 0) result += ",";
        int idx = m_hotbar[s];
        if (idx >= 0 && idx < (int)m_instances.size() && m_instances[idx].def)
            result += m_instances[idx].def->name;
    }
    result += "|equip:";
    for (int s = 0; s < EQUIP_SLOT_COUNT; s++) {
        if (s > 0) result += ",";
        int idx = m_equipment[s];
        if (idx >= 0 && idx < (int)m_instances.size() && m_instances[idx].def)
            result += m_instances[idx].def->name;
    }
    // Per-instance script toggle flags (sparse — only nonzero entries).
    // Token: <kind><slot>.<flagIdx>=<val> with kind h=hotbar, e=equip, p=player.
    result += "|flags=";
    bool firstTok = true;
    auto dumpFlags = [&](const std::string& kind, int slot, int idx) {
        if (idx < 0 || idx >= (int)m_instances.size()) return;
        const auto& ctx = m_instances[idx].ctx;
        for (int f = 0; f < LightningScriptContext::MAX_FLAGS; f++) {
            int v = ctx.GetFlag(f);
            if (v == 0) continue;
            if (!firstTok) result += ",";
            firstTok = false;
            result += kind + std::to_string(slot) + "." + std::to_string(f) + "=" + std::to_string(v);
        }
    };
    for (int s = 0; s < HOTBAR_SIZE; s++) dumpFlags("h", s, m_hotbar[s]);
    for (int s = 0; s < EQUIP_SLOT_COUNT; s++) dumpFlags("e", s, m_equipment[s]);
    if (m_playerEntityIndex >= 0) dumpFlags("p", 0, m_playerEntityIndex);
    result += "|";
    // Unlocked ethereal skill node names.
    result += "skills=";
    for (size_t i = 0; i < m_unlockedSkills.size(); i++) {
        if (i > 0) result += ",";
        result += m_unlockedSkills[i];
    }
    result += "|";
    // Spell bar. Def NAMES, not indices: unlike the hotbar this survives a def
    // that is merely unresolved at load time. Empty slots are empty strings, so
    // the entry count is always SPELL_BAR_SIZE.
    result += "spells=";
    for (int s = 0; s < SPELL_BAR_SIZE; s++) {
        if (s > 0) result += ",";
        result += GetSpellInSlot(s);
    }
    result += "|";
    return result;
}

bool LightningEntityManager::DeserializeState(const std::string& data) {
    // Parse "hotbar:def1,def2,...|equip:def1,def2,...|"
    // First clear existing
    for (int s = 0; s < HOTBAR_SIZE; s++) m_hotbar[s] = -1;
    for (int s = 0; s < EQUIP_SLOT_COUNT; s++) m_equipment[s] = -1;

    auto parseSection = [&](const std::string& prefix, int* arr, int arrSize) {
        size_t p = data.find(prefix);
        if (p == std::string::npos) return;
        p += prefix.size();
        size_t end = data.find('|', p);
        if (end == std::string::npos) end = data.size();
        std::string section = data.substr(p, end - p);
        size_t start = 0;
        for (int s = 0; s < arrSize; s++) {
            size_t comma = section.find(',', start);
            std::string name = (comma == std::string::npos)
                ? section.substr(start)
                : section.substr(start, comma - start);
            // Trim whitespace
            while (!name.empty() && (name[0] == ' ' || name[0] == '\t')) name.erase(0, 1);
            while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
            if (!name.empty()) {
                int idx = Spawn(name);
                if (idx >= 0) arr[s] = idx;
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    };

    parseSection("hotbar:", m_hotbar, HOTBAR_SIZE);
    parseSection("equip:", m_equipment, EQUIP_SLOT_COUNT);

    // Restore per-instance script toggle flags (see SerializeState).
    // Applied AFTER the spawns above — DeserializeState re-Spawns each def,
    // which fresh-loads the contexts and would wipe any earlier flag writes.
    {
        size_t p = data.find("flags=");
        if (p != std::string::npos) {
            p += 5;
            size_t end = data.find('|', p);
            if (end == std::string::npos) end = data.size();
            std::string section = data.substr(p, end - p);
            size_t start = 0;
            while (start < section.size()) {
                size_t comma = section.find(',', start);
                std::string tok = (comma == std::string::npos)
                    ? section.substr(start)
                    : section.substr(start, comma - start);
                if (!tok.empty()) {
                    // <kind><slot>.<flagIdx>=<val>
                    size_t dot = tok.find('.');
                    size_t eq = tok.find('=', dot);
                    if (dot != std::string::npos && eq != std::string::npos) {
                        char kind = tok[0];
                        int slot = atoi(tok.substr(1, dot - 1).c_str());
                        int flagIdx = atoi(tok.substr(dot + 1, eq - dot - 1).c_str());
                        int val = atoi(tok.substr(eq + 1).c_str());
                        int idx = -1;
                        if (kind == 'h' && slot >= 0 && slot < HOTBAR_SIZE) idx = m_hotbar[slot];
                        else if (kind == 'e' && slot >= 0 && slot < EQUIP_SLOT_COUNT) idx = m_equipment[slot];
                        else if (kind == 'p') idx = m_playerEntityIndex;
                        if (idx >= 0 && idx < (int)m_instances.size())
                            m_instances[idx].ctx.SetFlag(flagIdx, val);
                    }
                }
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }
    }

    // Restore unlocked ethereal skill node names.
    {
        size_t p = data.find("skills=");
        if (p != std::string::npos) {
            p += 7;
            size_t end = data.find('|', p);
            if (end == std::string::npos) end = data.size();
            std::string section = data.substr(p, end - p);
            size_t start = 0;
            m_unlockedSkills.clear();
            while (start < section.size()) {
                size_t comma = section.find(',', start);
                std::string name = (comma == std::string::npos)
                    ? section.substr(start)
                    : section.substr(start, comma - start);
                while (!name.empty() && (name[0] == ' ' || name[0] == '\t')) name.erase(0, 1);
                while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
                if (!name.empty()) UnlockSkill(name);
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }
    }

    // Restore the spell bar. Cleared unconditionally FIRST, so an old save with
    // no `spells=` section does not inherit whatever the process had bound.
    //
    // Assigned directly rather than through SetSpellInSlot: the bar stores def
    // names, and SetSpellInSlot would Spawn (and thus require) each def to be
    // resolvable right now. A name whose def is missing stays bound so it comes
    // back if the def is restored; GetSpellInstance resolves lazily on cast.
    {
        m_spellBar.assign(SPELL_BAR_SIZE, std::string());
        size_t p = data.find("spells=");
        if (p != std::string::npos) {
            p += 7;
            size_t end = data.find('|', p);
            if (end == std::string::npos) end = data.size();
            std::string section = data.substr(p, end - p);
            size_t start = 0;
            for (int s = 0; s < SPELL_BAR_SIZE; s++) {
                size_t comma = section.find(',', start);
                std::string name = (comma == std::string::npos)
                    ? section.substr(start)
                    : section.substr(start, comma - start);
                while (!name.empty() && (name[0] == ' ' || name[0] == '\t')) name.erase(0, 1);
                while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
                m_spellBar[s] = name;
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }
    }
    return true;
}
