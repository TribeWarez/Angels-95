#include "ViewModel.hpp"
#include "Mesh/MeshCache.hpp"
#include "raymath.h"
#include <rlgl.h>
#include "../Script/LightningEntityManager.hpp"
#include "../Script/LightningEntityDef.hpp"
#include "../GameplayFlags.hpp"
#include "../Log.hpp"
#include <cmath>
#include <cstdlib>

namespace oz {

// Target on-screen length for a first-person weapon, in camera-space units.
// A held weapon should read as roughly a forearm-to-torso long prop.
static constexpr float kViewModelTargetSize = 0.55f;

// Source assets disagree wildly on scale because each came out of a different
// DCC tool. Measured from the shipped GLBs:
//
//   pistol_01   bbox 0.223 x 0.032 x 0.141   (already human-scale)
//   etheral_waver bbox ~9.9 x 1.1 x 0.52      (node scale of 100/100/64.8,
//                                               plus a ~19 unit origin offset)
//
// Rendering either at scale 1.0 is wrong: the pistol is a speck and the weaver
// fills the screen. Rather than hand-tuning every def, fit the loaded mesh to
// the target size and let viewmodel_scale act as a relative tuning multiplier on
// top of that.
static float ComputeFitScale(const BoundingBox& b) {
    const float ex = std::fabs(b.max.x - b.min.x);
    const float ey = std::fabs(b.max.y - b.min.y);
    const float ez = std::fabs(b.max.z - b.min.z);
    const float longest = std::fmax(ex, std::fmax(ey, ez));
    if (longest <= 1e-5f) return 1.0f;   // degenerate/empty mesh: leave as-is
    // Guard against absurd input so a bad asset cannot produce an invisible or
    // screen-filling weapon; the real offenders are ~40x, not ~1e6x.
    float s = kViewModelTargetSize / longest;
    if (s < 1e-3f) s = 1e-3f;
    if (s > 100.0f) s = 100.0f;
    return s;
}

// Centre of the mesh in its own space, so rotation spins it about its middle
// instead of about the model's origin.
static Vector3 ComputeCenter(const BoundingBox& b) {
    return { (b.min.x + b.max.x) * 0.5f,
             (b.min.y + b.max.y) * 0.5f,
             (b.min.z + b.max.z) * 0.5f };
}

ViewModel& ViewModel::Instance() {
    // Intentionally leaked: the mesh is owned by MeshCache anyway, and we must
    // not run GL unloads during static destruction (after CloseWindow).
    static ViewModel* instance = new ViewModel();
    return *instance;
}

static std::string VmDefDir(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    return (s == std::string::npos) ? std::string() : p.substr(0, s + 1);
}
static std::string VmStatStr(const EntityDef* d, const char* k, const std::string& def = "") {
    if (!d) return def;
    auto it = d->stats.strings.find(k);
    return (it != d->stats.strings.end()) ? it->second : def;
}
static float VmStatFloat(const EntityDef* d, const char* k, float def) {
    if (!d) return def;
    auto it = d->stats.floats.find(k);
    return (it != d->stats.floats.end()) ? it->second : def;
}
static Vector3 VmStatVec3(const EntityDef* d, const char* k, Vector3 def) {
    if (!d) return def;
    auto it = d->stats.vec3s.find(k);
    if (it != d->stats.vec3s.end()) return {it->second[0], it->second[1], it->second[2]};
    return def;
}

void ViewModel::Clear() {
    m_mesh.reset();
    m_skel = nullptr;
    m_clipIdle = m_clipFire = m_clipReload = m_active = -1;
    m_time = 0.0f;
    m_recoil = m_reloadT = 0.0f;
    m_key.clear();
}

void ViewModel::SetWeapon(const EntityDef* def, int variantIndex) {
    static std::string s_lastDef;
    if (!def || def->type != EntityType::WEAPON) {
        if (!s_lastDef.empty()) {
            OZ_INFO("[CHAIN] VM::SetWeapon def=(none) -> Clear (was '%s')", s_lastDef.c_str());
            s_lastDef.clear();
        }
        Clear();
        return;
    }
    if (def->name != s_lastDef) {
        OZ_INFO("[CHAIN] VM::SetWeapon def='%s' mesh='%s' tex='%s'",
                def->name.c_str(), def->mesh.c_str(), def->texture.c_str());
        s_lastDef = def->name;
    }

    std::string mesh = VmStatStr(def, "viewmodel_mesh", def->mesh);
    std::string tex  = VmStatStr(def, "viewmodel_texture", def->texture);

    // Apply the instance's resolved variant, if the def declares any. Spawn()
    // already picked the tier; without this the view-model kept showing the base
    // mesh while the world copy used the variant.
    if (variantIndex >= 0 && variantIndex < (int)def->variants.size()) {
        const EntityVariant& v = def->variants[(size_t)variantIndex];
        if (!v.meshOverride.empty())    mesh = v.meshOverride;
        if (!v.textureOverride.empty()) tex  = v.textureOverride;
    }

    // Transform params refresh cheaply each call.
    m_offset   = VmStatVec3(def, "viewmodel_offset", m_offset);
    m_rot      = VmStatVec3(def, "viewmodel_rot", m_rot);
    m_scale    = VmStatFloat(def, "viewmodel_scale", m_scale);
    m_recoilMax = VmStatFloat(def, "recoil", m_recoilMax);
    m_reloadSeconds = VmStatFloat(def, "reload_time", 1.2f);

    std::string key = def->name + "|" + mesh + "|" + tex;
    if (key == m_key) return;
    m_key = key;
    m_mesh.reset();
    m_skel = nullptr;
    m_clipIdle = m_clipFire = m_clipReload = m_active = -1;
    m_time = 0.0f;

    if (mesh.empty()) {
        OZ_WARN("[CHAIN] VM::SetWeapon mesh empty for '%s'", def->name.c_str());
        return;
    }
    std::string baseDir = VmDefDir(def->sourcePath);
    OZ_INFO("[CHAIN] VM::SetWeapon loading mesh='%s' tex='%s' baseDir='%s'",
            mesh.c_str(), tex.c_str(), baseDir.c_str());
    auto m = MeshCache::Instance().GetStatic(mesh, tex, baseDir, true);
    if (!m || !m->Valid()) {
        OZ_WARN("[CHAIN] VM::SetWeapon could not load '%s'", mesh.c_str());
        return;
    }
    m_mesh = m;
    m_center = ComputeCenter(m_mesh->Bounds());
    OZ_INFO("ViewModel: '%s' mesh=%s bounds=[%.3f..%.3f, %.3f..%.3f, %.3f..%.3f] "
            "centre=(%.3f, %.3f, %.3f) fitScale=%.4f",
            def->name.c_str(), mesh.c_str(),
            m_mesh->Bounds().min.x, m_mesh->Bounds().max.x,
            m_mesh->Bounds().min.y, m_mesh->Bounds().max.y,
            m_mesh->Bounds().min.z, m_mesh->Bounds().max.z,
            m_center.x, m_center.y, m_center.z,
            ComputeFitScale(m_mesh->Bounds()));
    // Skeletal clip playback is intentionally disabled: the FBX2glTF weapon
    // exports contain MULTIPLE skins (one per mesh part), which raylib loads
    // inconsistently ("can only load one skin") and can crash in
    // UpdateModelAnimation on some builds. The weapon is drawn static with
    // procedural recoil/reload motion instead.
    m_skel = nullptr;
    m_active = -1;
    OZ_INFO("ViewModel: '%s' mesh=%s (static + procedural anim)", def->name.c_str(), mesh.c_str());
}

void ViewModel::TriggerFire() {
    OZ_INFO("[CHAIN] VM::TriggerFire key='%s'", m_key.c_str());
    m_recoil = 1.0f;
    if (m_skel && m_clipFire >= 0) {
        m_active = m_clipFire;
        m_time = 0.0f;
        m_loop = false;
    }
}

void ViewModel::TriggerReload() {
    OZ_INFO("[CHAIN] VM::TriggerReload key='%s'", m_key.c_str());
    m_reloadT = m_reloadSeconds;
    if (m_skel && m_clipReload >= 0) {
        m_active = m_clipReload;
        m_time = 0.0f;
        m_loop = false;
    }
}

void ViewModel::Update(float dt) {
    if (m_recoil > 0.0f) m_recoil = fmaxf(0.0f, m_recoil - dt * 7.0f);
    if (m_reloadT > 0.0f) m_reloadT = fmaxf(0.0f, m_reloadT - dt);

    if (!m_skel || m_active < 0) return;
    m_time += dt;
    if (!m_loop) {
        float dur = m_skel->ClipSeconds(m_active);
        if (dur > 0.0f && m_time >= dur) {
            m_active = m_clipIdle;
            m_time = 0.0f;
            m_loop = true;
        }
    }
}

void ViewModel::Draw(Camera3D& camera, Shader litShader) {
    // Bind to the currently selected weapon (cheap when unchanged).
    EntityInstance* ent = LightningEntityManager::Instance().SelectedEntity();
    SetWeapon(ent ? ent->def : nullptr, ent ? ent->variantIndex : -1);
    if (!m_mesh || !m_mesh->Valid()) return;

    static std::string s_loggedKey;
    static const bool s_trace = (getenv("OZ_VM_TRACE") != nullptr);
    static std::string s_frustumWarned;
    if (m_key != s_loggedKey) {
        OZ_INFO("[CHAIN] VM::Draw begin key='%s' meshes=%d", m_key.c_str(), m_mesh->MeshCount());
        s_loggedKey = m_key;
    }

    // Camera world matrix (view -> world).
    Matrix camWorld = MatrixInvert(GetCameraMatrix(camera));

    // Procedural recoil kick (toward the viewer) + a small pitch pop.
    float kick = m_recoil * 0.05f * (m_recoilMax > 0.0f ? m_recoilMax : 1.0f);
    float rpitch = -m_recoil * 9.0f;

    // Reload dip / tilt.
    float dip = 0.0f, rtilt = 0.0f;
    if (m_reloadT > 0.0f && m_reloadSeconds > 0.0f) {
        float p = fminf(1.0f, m_reloadT / m_reloadSeconds);
        dip = -0.09f * p;
        rtilt = 28.0f * p;
    }

    // Local transform in camera space: x right, y up, z forward (view -Z).
    //
    // raylib is row-vector: v*M, and MatrixMultiply(A,B) == A*B, so in v*(A*B)
    // the LEFTMOST matrix is applied FIRST. The per-vertex order we want is
    //
    //     subtract centre -> rotate -> scale -> translate into camera space
    //
    // so the chain must read left-to-right in that same order:
    //
    //     T(-centre) * R * S * T(offset)
    //
    // This used to be T(off) * R * S, which applied the translate FIRST. That
    // made the 90 deg yaw rotate the offset itself: a weapon authored at
    // offset.x = +0.22 (right) rendered at view-space X = -0.45, roughly 2.3x
    // outside the frustum edge, i.e. off-screen to the left. That is why the
    // pistol was invisible and read as sitting in the left hand. Every shipped
    // weapon offset was tuned against that broken order, so all of them are
    // re-authored below.
    Vector3 off = {m_offset.x, m_offset.y + dip, -m_offset.z + kick};
    Vector3 rotDeg = {m_rot.x + rpitch, m_rot.y, m_rot.z + rtilt};

    // Weapon hand: mirror the view-model across the camera's YZ plane for the
    // left hand. Mirroring a right-handed weapon means offset.x -> -x and the
    // yaw/roll rotations -> -yaw/-roll (pitch is unaffected).
    if (g_weaponHand != 0) {
        off.x = -off.x;
        rotDeg.y = -rotDeg.y;
        rotDeg.z = -rotDeg.z;
    }

    // Head-bob sway: the camera bobs (Core.hpp); the weapon sways on the same
    // phase so it does not stay rigidly locked to a moving eye.
    off.y += sinf(m_bobPhase) * m_bobAmount * 0.6f;
    off.x += cosf(m_bobPhase * 0.5f) * m_bobAmount * 0.4f;
    // Fit normalises wildly different source scales into a usable held size;
    // viewmodel_scale stays a relative tuning multiplier on top of that.
    const float fitScale = m_scale * ComputeFitScale(m_mesh->Bounds());
    Matrix local = MatrixMultiply(
        MatrixMultiply(
            MatrixTranslate(-m_center.x, -m_center.y, -m_center.z),
            MatrixRotateXYZ({rotDeg.x * DEG2RAD, rotDeg.y * DEG2RAD, rotDeg.z * DEG2RAD})),
        MatrixMultiply(
            MatrixScale(fitScale, fitScale, fitScale),
            MatrixTranslate(off.x, off.y, off.z)));
    Matrix world = MatrixMultiply(local, camWorld);

    if (m_skel && m_active >= 0)
        m_skel->ApplyPose(m_active, m_time);

    if (s_trace)
        OZ_INFO("[CHAIN] VM::Draw pre-DrawMatrix key='%s' recoil=%.2f reloadT=%.2f",
                m_key.c_str(), m_recoil, m_reloadT);

    // Fail loudly instead of silently: a view-model off-screen is
    // indistinguishable from a broken one, which is exactly how the
    // multiply-order bug survived. Warn once per weapon key.
    if (m_key != s_frustumWarned) {
        const float fit = ComputeFitScale(m_mesh->Bounds()) * m_scale;
        const Vector3 c = m_center;
        // Nearest point of the fitted box along -Z (forward) in view space.
        const float nearest = -m_offset.z - 0.5f * fit;
        const float halfH = std::fmax(0.5f * fit, 0.5f * (m_mesh->Bounds().max.y -
                                                          m_mesh->Bounds().min.y) * fit);
        const float halfW = halfH;
        const float limH = std::tan(camera.fovy * 0.5f * DEG2RAD) * std::fmax(nearest, 0.01f);
        const float limW = limH * ((float)GetScreenWidth() / (float)GetScreenHeight());
        const float dx = std::fmax(0.0f, std::fabs(off.x + c.x * fit) - limW);
        const float dy = std::fmax(0.0f, std::fabs(off.y + c.y * fit) - limH);
        const float behind = (nearest + 0.5f * fit) <= 0.0f;
        if (dx > 0.0f || dy > 0.0f || behind) {
            OZ_WARN("ViewModel '%s' is off-screen (off=(%.3f, %.3f, %.3f) nearest=%.3f "
                    "limW=%.3f limH=%.3f excessX=%.3f excessY=%.3f behind=%d)",
                    m_key.c_str(), off.x, off.y, off.z, nearest, limW, limH, dx, dy,
                    behind ? 1 : 0);
            s_frustumWarned = m_key;
        }
    }

    // Depth test stays ON: a wall between the camera and the weapon must occlude
    // it. Disabling it (the old behaviour) drew the pistol over every wall in the
    // level, which read as "occlusion missing".
    rlDrawRenderBatchActive();
    m_mesh->DrawMatrix(world, litShader);

    if (s_trace) OZ_INFO("[CHAIN] VM::Draw post-DrawMatrix");
}

bool ViewModel::MuzzleWorldPos(Camera3D camera, Vector3* out) const {
    if (!m_mesh || !m_mesh->Valid() || !out) return false;
    const float fitScale = m_scale * ComputeFitScale(m_mesh->Bounds());
    Matrix camWorld = MatrixInvert(GetCameraMatrix(camera));
    // Mirror with the same rule as Draw() so the muzzle flash stays at the
    // barrel when the weapon is in the left hand.
    const float handSign = (g_weaponHand != 0) ? -1.0f : 1.0f;
    Matrix local = MatrixMultiply(
        MatrixMultiply(
            MatrixTranslate(-m_center.x, -m_center.y, -m_center.z),
            MatrixRotateXYZ({m_rot.x * DEG2RAD, handSign * m_rot.y * DEG2RAD,
                             handSign * m_rot.z * DEG2RAD})),
        MatrixMultiply(
            MatrixScale(fitScale, fitScale, fitScale),
            MatrixTranslate(handSign * m_offset.x, m_offset.y, -m_offset.z)));
    Matrix world = MatrixMultiply(local, camWorld);

    // Muzzle = the far end of the weapon along its own length. Which axis is
    // "length" depends on the asset, so use the largest fitted extent and walk to
    // that end of the bounding box.
    const BoundingBox& b = m_mesh->Bounds();
    const float ex = std::fabs(b.max.x - b.min.x) * fitScale;
    const float ey = std::fabs(b.max.y - b.min.y) * fitScale;
    const float ez = std::fabs(b.max.z - b.min.z) * fitScale;
    Vector3 localMuzzle = m_center;
    if (ex >= ey && ex >= ez)      localMuzzle.x = b.max.x;
    else if (ey >= ez)             localMuzzle.y = b.max.y;
    else                           localMuzzle.z = b.max.z;

    // Row-vector convention: result = v * world.
    const float v[4] = { localMuzzle.x, localMuzzle.y, localMuzzle.z, 1.0f };
    out->x = v[0]*world.m0 + v[1]*world.m4 + v[2]*world.m8  + v[3]*world.m12;
    out->y = v[0]*world.m1 + v[1]*world.m5 + v[2]*world.m9  + v[3]*world.m13;
    out->z = v[0]*world.m2 + v[1]*world.m6 + v[2]*world.m10 + v[3]*world.m14;
    return true;
}

} // namespace oz
