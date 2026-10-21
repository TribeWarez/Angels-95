#include "../../WindowsCompat.hpp"   // must precede raylib + winsock2 includes
#include "WeaponBehaviour.hpp"
#include "../../Script/LightningEntityManager.hpp"
#include "../../Client/Client.hpp"
#include "../../Renderer/ViewModel.hpp"
#include "../../Renderer/CombatFX.hpp"
#include "../../Audio/SoundManager.hpp"
#include "../../Input/Keybinds.hpp"
#include "raymath.h"
#include "../../Particle/OzParticleSimulationManager.hpp"
#include <cstdlib>
#include <cmath>

// ---------------------------------------------------------------------------
// Read a stat from the selected weapon entity
// ---------------------------------------------------------------------------
float WeaponBehaviour::SelectedWeaponStat(const std::string& key, float defVal) const {
    EntityInstance* ent = LightningEntityManager::Instance().SelectedEntity();
    if (!ent || !ent->def) return defVal;
    auto rit = ent->runtimeStats.find(key);
    if (rit != ent->runtimeStats.end()) return rit->second;
    auto dit = ent->def->stats.floats.find(key);
    return (dit != ent->def->stats.floats.end()) ? dit->second : defVal;
}

// The stats parser stores string values verbatim: no quote handling, and a
// single embedded space truncates the value (see GameUi::ParseSlots). Strip any
// quotes so an author who writes fire_sound = "x.wav" still resolves, and treat
// a value that is only whitespace/quotes as absent so the fallback applies.
static std::string TrimStatString(const std::string& in) {
    size_t b = in.find_first_not_of(" \t\r\n\"");
    if (b == std::string::npos) return "";
    size_t e = in.find_last_not_of(" \t\r\n\"");
    return in.substr(b, e - b + 1);
}

std::string WeaponBehaviour::SelectedWeaponString(const std::string& key,
                                                 const std::string& def) const {
    EntityInstance* ent = LightningEntityManager::Instance().SelectedEntity();
    if (!ent || !ent->def) return def;
    auto it = ent->def->stats.strings.find(key);
    if (it == ent->def->stats.strings.end()) return def;
    std::string v = TrimStatString(it->second);
    return v.empty() ? def : v;
}

// Play one of the weapon's authored sound stats, falling back to a global
// default when the stat is absent. An authored key wins over the fallback
// even if the asset fails to load -- the author asked for a specific sound,
// so silently playing the generic one would hide a broken path.
void WeaponBehaviour::PlayWeaponSound(const char* pathKey, const char* volKey,
                                      const char* pitchKey,
                                      const std::string& fallbackPath) {
    const std::string authored = SelectedWeaponString(pathKey);
    const std::string path = authored.empty() ? fallbackPath : authored;
    if (path.empty()) return;

    // Defaults are unity rather than 0, so an authored sound is not silent
    // unless the author explicitly asked for silence.
    float volume = SelectedWeaponStat(volKey, 1.0f);
    float pitch  = SelectedWeaponStat(pitchKey, 1.0f);
    SoundManager::Instance().PlayStatSound(path, volume, pitch);
}

// ---------------------------------------------------------------------------
// Fire weapon — delegates to LightningEntityManager
// ---------------------------------------------------------------------------
void WeaponBehaviour::FireWeapon(Camera3D& cam) {
    Vector3 forward = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
    Vector3 origin = Vector3Add(cam.position, Vector3Scale(forward, 2.0f));

    int result = LightningEntityManager::Instance().FireSelectedWeapon(origin, forward);
    if (result < 0) return; // didn't fire

    // Fire/swing audio goes HERE, after the gate above. FireSelectedWeapon
    // returns -1 when it refuses to shoot for reasons that produce no report
    // of their own -- on cooldown, out of stamina, or when it silently
    // auto-reloaded (LightningEntityManager.cpp:269). Playing before this
    // check would fire a gunshot on every click of an empty magazine.
    //
    // result == 0 is a melee swing; > 0 is the projectile count spawned.
    if (result == 0) {
        PlayWeaponSound("swing_sound", "swing_volume", "swing_pitch", "");
    } else {
        PlayWeaponSound("fire_sound", "fire_volume", "fire_pitch",
                        "GameData/Audio/Sounds/Guns/machgf3b.wav");
    }

    oz::ViewModel::Instance().TriggerFire();

    // Muzzle flash, anchored to the weapon rather than the camera. It used to
    // reuse `origin` (camera + 2u forward), which put the glow inside the
    // view-model instead of at the barrel; fall back to that only when no
    // weapon is loaded. The projectile origin is deliberately unchanged.
    //
    // RANGED ONLY. This ran unconditionally, so a melee swing armed the flash
    // too - and the flash is drawn as a glowing sphere at the barrel plus a
    // transient light, so every sword swing looked like it fired a bullet. A
    // sword has no muzzle; `result == 0` is the melee swing.
    if (result > 0) {
        Vector3 muzzle = origin;
        if (!oz::ViewModel::Instance().MuzzleWorldPos(cam, &muzzle))
            muzzle = origin;
        CombatFX::Instance().ArmMuzzleFlash(muzzle, 0.12f);

        // Muzzle smoke. Ranged weapons get a small hot-gas puff; melee swings
        // do not, since a sword has no muzzle. Particles are lit now (see
        // OzParticleSimulationManager::ApplyLighting), so this catches the
        // flash light instead of floating at full brightness.
        OzParticleSimulationManager::Instance().Burst(
            muzzle, forward, 5,
            /*speed*/ 2.2f, /*spread*/ 22.0f,
            /*color*/ {255, 220, 150, 200}, /*colorEnd*/ {120, 110, 100, 0},
            /*sizeStart*/ 0.01f, /*sizeEnd*/ 0.32f,
            /*lifetime*/ 0.28f, /*gravity*/ -0.4f);
    }

    // Apply recoil
    float recoilKick = SelectedWeaponStat("recoil", result > 0 ? 2.0f : 1.0f);
    m_recoilPitch += -recoilKick + (float)(rand() % 100 - 50) / 100.0f * recoilKick * 0.3f;
    m_recoilYaw += (float)(rand() % 100 - 50) / 100.0f * recoilKick * 0.2f;
    m_crosshairBloom += recoilKick * 1.5f;

    bool isMelee = (result == 0);
    float damage = SelectedWeaponStat("damage", 10.0f);

    // Send the trigger to the server. This is the ONLY report the client makes,
    // and the server is authoritative for both weapon kinds.
    //
    // BOTH branches used to run a second client-side NPC raycast here and send
    // NPC_DAMAGE on top of the server-authoritative path -- so every trigger
    // dealt damage TWICE:
    //   * Melee: FireSelectedWeapon already resolved the target and reported it
    //     through ReportMeleeHit -> MELEE_HIT, which the server re-validates for
    //     world match, reach cap, stamina and a damage cap.
    //   * Ranged: the server spawns its own projectile from PLAYER_ACTION and
    //     damages with tick_projectiles; the client projectile is cosmetic.
    // The duplicate NPC_DAMAGE leg skipped every check the first leg has: it
    // validated only distance and rate, never the world, and had no damage cap.
    //
    // A melee swing sends nothing at all here (MELEE_HIT already went out).
    // NPC_DAMAGE is still reachable by any client, so it is hardened server-side
    // on its own rather than trusted not to arrive.
    if (m_client && m_networkEnabled && *m_networkEnabled && m_client->is_connected()) {
        if (!isMelee) {
            m_client->send_weapon_fire(
                origin.x, origin.y, origin.z,
                forward.x, forward.y, forward.z,
                1, (int)damage);
        }
    }
}

void WeaponBehaviour::HandleInput(bool uiBlocking, Camera3D& cam) {
    if (uiBlocking) return;
    if (!Keybinds::Down(GameAction::Fire)) return;
    EntityInstance* wep = LightningEntityManager::Instance().SelectedEntity();
    if (wep && wep->def && wep->def->type == EntityType::WEAPON)
        FireWeapon(cam);
}

// ---------------------------------------------------------------------------
// Recoil recovery + camera application
// ---------------------------------------------------------------------------
void WeaponBehaviour::Update(Camera3D& cam) {
    const float RECOIL_DECAY = 0.82f;
    m_recoilPitch *= RECOIL_DECAY;
    m_recoilYaw *= RECOIL_DECAY;
    m_crosshairBloom *= (m_adsActive ? 0.75f : 0.90f);
    if (fabsf(m_recoilPitch) < 0.01f) m_recoilPitch = 0.0f;
    if (fabsf(m_recoilYaw) < 0.01f) m_recoilYaw = 0.0f;
    if (m_crosshairBloom < 0.1f) m_crosshairBloom = 0.0f;

    // Apply recoil to camera target
    if (m_recoilPitch != 0.0f || m_recoilYaw != 0.0f) {
        Vector3 forward = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
        Vector3 right = Vector3Normalize(Vector3CrossProduct(forward, {0, 1, 0}));
        cam.target = Vector3Add(cam.target, Vector3Scale({0, 1, 0}, m_recoilPitch * 0.1f));
        cam.target = Vector3Add(cam.target, Vector3Scale(right, m_recoilYaw * 0.1f));
    }
}
