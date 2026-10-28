#include "ViewModel.hpp"
#include "Mesh/MeshCache.hpp"
#include "raymath.h"
#include <rlgl.h>
#include "../Script/LightningEntityManager.hpp"
#include "../Script/LightningEntityDef.hpp"
#include "../Package/PackageAssetLoader.hpp"
#include "../GameplayFlags.hpp"
#include "../Log.hpp"
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <vector>

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
// the target size and let viewmodel_scale act as a RELATIVE HELD LENGTH on top
// of that.
//
// The fit normalises the LONGEST axis, so `viewmodel_scale` reads as "how long
// is this in the hand, where 1.0 is 0.55 units" - it is NOT a uniform size
// multiplier. That is what makes a pistol at 1.0 as long as a rifle at 1.0 and
// therefore absurdly large; the shipped defs use ~0.62 for a handgun, ~0.76 for
// an SMG and ~0.95 for a rifle/shotgun.
//
// The forward offset matters as much as the length: the offset places the
// model's CENTRE, so the near end sits at (offset.z - length/2) from the eye. At
// 0.45 with a 0.55-long gun that is 0.17 - the grip is inside your face and the
// perspective does the rest. Ship offsets that leave the near end >= ~0.5.
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

// Does an asset exist on disk OR in a loaded package? Mesh::Load warns loudly on
// a miss, so probe first rather than logging a warning for every weapon that has
// no attachments.
static bool VmAssetExists(const std::string& resolved) {
    if (resolved.empty()) return false;
    if (IsPathFile(resolved.c_str())) return true;
    std::vector<uint8_t> tmp;
    return PackageAssetLoader::Instance().ReadInto(resolved.c_str(), tmp);
}

// `<dir>/<stem>_AdditionalMeshes.glb` beside the weapon model. Both shipped
// animated-ready weapons (Pistol_01, Rifle_01) put their detachable magazine
// there, which is why the rifle used to fire with no magazine in the view-model:
// only the base model was ever loaded.
static std::string SiblingAdditionalMeshes(const std::string& mesh) {
    const size_t dot = mesh.find_last_of('.');
    const size_t slash = mesh.find_last_of("/\\");
    if (dot == std::string::npos) return "";
    if (slash != std::string::npos && dot < slash) return "";
    std::string ext = mesh.substr(dot);
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    if (ext != ".glb" && ext != ".gltf") return "";
    return mesh.substr(0, dot) + "_AdditionalMeshes" + mesh.substr(dot);
}

void ViewModel::Clear() {
    m_mesh.reset();
    m_skel = nullptr;
    m_clipIdle = m_clipFire = m_clipReload = m_active = -1;
    m_time = 0.0f;
    m_clipRate = 1.0f;
    m_recoil = m_reloadT = 0.0f;
    m_attach.reset();
    m_magMesh = -1;
    m_magDrop = 0.0f;
    m_attachCenter = {0.0f, 0.0f, 0.0f};
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
    //
    // The fallbacks are the DEFAULTS, not the current members. They used to be
    // `m_offset`/`m_rot`/`m_scale`/`m_recoilMax`, so a weapon whose def does not
    // author a key silently inherited whatever the PREVIOUS weapon had set: pick
    // up the rifle (rot.y = 90, scale 1.3) and then a weapon with no
    // viewmodel_rot rendered sideways at the rifle's offset. A transform is a
    // property of the weapon, exactly like its mesh; it must not carry over.
    m_offset   = VmStatVec3(def, "viewmodel_offset", {0.22f, -0.18f, 0.45f});
    m_rot      = VmStatVec3(def, "viewmodel_rot", {0.0f, 0.0f, 0.0f});
    m_scale    = VmStatFloat(def, "viewmodel_scale", 1.0f);
    m_recoilMax = VmStatFloat(def, "recoil", 1.0f);
    m_reloadSeconds = VmStatFloat(def, "reload_time", 1.2f);
    // raylib resamples glTF clips at 60 fps, so that is the default conversion
    // rate; 30 halved the Fire/Idle playback rate of every gun that did not
    // author viewmodel_anim_fps.
    m_clipFps = VmStatFloat(def, "viewmodel_anim_fps", kSkeletalClipFps);
    if (m_clipFps <= 0.0f) m_clipFps = kSkeletalClipFps;

    std::string key = def->name + "|" + mesh + "|" + tex + "|" +
                      VmStatStr(def, "viewmodel_attachment", SiblingAdditionalMeshes(mesh));
    if (key == m_key) return;
    m_key = key;
    m_mesh.reset();
    m_skel = nullptr;
    m_clipIdle = m_clipFire = m_clipReload = m_active = -1;
    m_time = 0.0f;
    m_clipRate = 1.0f;
    m_attach.reset();
    m_magMesh = -1;
    m_magDrop = 0.0f;
    m_attachCenter = {0.0f, 0.0f, 0.0f};

    if (mesh.empty()) {
        OZ_WARN("[CHAIN] VM::SetWeapon mesh empty for '%s'", def->name.c_str());
        return;
    }
    std::string baseDir = VmDefDir(def->sourcePath);
    // Skeletal playback (Idle/Fire/Reload clips + any play_clip request) is
    // AUTO-ENABLED when the asset carries clips and its skinning data fits the
    // one skeleton raylib loaded. The FBX2glTF weapon exports contain MULTIPLE
    // skins (one per mesh part); raylib keeps only the first ("can only load
    // one skin") and animating such a model can index past the pose array -
    // the crash this gate exists for, and the reason playback used to be
    // hard-disabled for every weapon. Stats override:
    //   viewmodel_skeletal =  1  force skeletal (author asserts the asset is safe)
    //   viewmodel_skeletal = -1  force static (procedural recoil/reload only)
    //   absent / 0               auto (clips + single-skeleton scan)
    const float skelMode = VmStatFloat(def, "viewmodel_skeletal", 0.0f);
    OZ_INFO("[CHAIN] VM::SetWeapon loading mesh='%s' tex='%s' baseDir='%s' skelMode=%.0f",
            mesh.c_str(), tex.c_str(), baseDir.c_str(), skelMode);
    auto m = (skelMode < 0.0f)
                 ? MeshCache::Instance().GetStatic(mesh, tex, baseDir, true)
                 : MeshCache::Instance().GetSkeletal(mesh, tex, baseDir, true);
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
    auto* sk = dynamic_cast<SkeletalMesh*>(m_mesh.get());
    const bool hazard = (sk != nullptr) && !BoneIdsFitSkeleton(sk->GetModel());

    // Rigid attachment (detachable magazine). The shipped weapon GLBs are
    // flattened to single-skin static meshes, so a reload CLIP cannot play on
    // them; the magazine is a separate model (`_AdditionalMeshes.glb`) and is
    // animated by moving it during the reload below. Loading only the base model
    // is why the rifle fired with no magazine attached.
    {
        std::string attachPath = VmStatStr(def, "viewmodel_attachment", SiblingAdditionalMeshes(mesh));
        if (!attachPath.empty() &&
            VmAssetExists(ResolveMeshAsset(baseDir, attachPath))) {
            // Give the attachment the WEAPON's texture. It used to load with an
            // empty texPath, and the additional-mesh GLB carries no material
            // texture of its own, so the magazine rendered as an untextured white
            // slab tinted by the nearest light - a bright yellow-green block
            // hanging off the pistol grip and off the SMG's stock.
            std::string attachTex = VmStatStr(def, "texture", "");
            auto a = MeshCache::Instance().GetStatic(attachPath, attachTex, baseDir, true);
            if (a && a->Valid() && a->MeshCount() > 0) {
                m_attach = a;
                m_attachCenter = ComputeCenter(a->Bounds());
                int idx = (int)VmStatFloat(def, "viewmodel_attachment_mesh", -1.0f);
                if (idx < 0 || idx >= a->MeshCount())
                    idx = (a->MeshCount() > 1) ? 1 : 0; // [Separated, Full/2+Bullets, OneBullet]
                m_magMesh = idx;
                OZ_INFO("ViewModel: '%s' attachment='%s' mesh %d/%d", def->name.c_str(),
                        attachPath.c_str(), idx, a->MeshCount());
            }
        }
    }

    if (sk && (skelMode > 0.0f || !hazard)) {
        m_skel = sk;
        m_clipIdle   = sk->FindClip("Idle");
        m_clipFire   = sk->FindClip("Fire");
        m_clipReload = sk->FindClip("Reload");
        // Start on the Idle loop so the held weapon animates from the first
        // frame; TriggerFire/TriggerReload swap to their clip and Update()
        // returns to Idle when a one-shot ends.
        m_active = m_clipIdle;
        m_loop = true;
        m_time = 0.0f;
        OZ_INFO("ViewModel: '%s' skeletal playback ON (clips=%d idle=%d fire=%d reload=%d%s)",
                def->name.c_str(), sk->ClipCount(), m_clipIdle, m_clipFire,
                m_clipReload, hazard ? " FORCED past skeleton scan" : "");
    } else {
        m_skel = nullptr;
        m_clipIdle = m_clipFire = m_clipReload = m_active = -1;
        if (sk && hazard && skelMode <= 0.0f)
            OZ_INFO("ViewModel: '%s' static + procedural anim (multi-skin/mismatched "
                    "skeleton; set viewmodel_skeletal=1 to force)",
                    def->name.c_str());
        else
            OZ_INFO("ViewModel: '%s' mesh=%s (static + procedural anim)",
                    def->name.c_str(), mesh.c_str());
    }
}

bool ViewModel::PlayClip(const std::string& name) {
    if (!m_skel || name.empty()) return false;
    const int idx = m_skel->FindClip(name);
    if (idx < 0) return false;
    m_active = idx;
    m_time = 0.0f;
    m_clipRate = 1.0f;
    // An explicit Idle request just keeps looping; every other clip is
    // one-shot and Update() returns to the Idle loop when it ends.
    m_loop = (idx == m_clipIdle);
    return true;
}

void ViewModel::TriggerFire() {
    OZ_INFO("[CHAIN] VM::TriggerFire key='%s'", m_key.c_str());
    m_recoil = 1.0f;
    if (m_skel && m_clipFire >= 0) {
        m_active = m_clipFire;
        m_time = 0.0f;
        m_clipRate = 1.0f;
        m_loop = false;
    }
}

void ViewModel::TriggerReload() {
    OZ_INFO("[CHAIN] VM::TriggerReload key='%s'", m_key.c_str());
    m_reloadT = m_reloadSeconds;
    m_magDrop = 0.0f;
    if (m_skel && m_clipReload >= 0) {
        m_active = m_clipReload;
        m_time = 0.0f;
        m_loop = false;
        // Stretch/squeeze the clip so it finishes when the magazine is actually
        // back in (reload_time), instead of running at its authored rate.
        const float dur = m_skel->ClipSeconds(m_clipReload, m_clipFps);
        m_clipRate = (dur > 0.0f && m_reloadSeconds > 0.0f) ? (dur / m_reloadSeconds) : 1.0f;
    }
}

void ViewModel::Update(float dt) {
    if (m_recoil > 0.0f) m_recoil = fmaxf(0.0f, m_recoil - dt * 7.0f);
    if (m_reloadT > 0.0f) m_reloadT = fmaxf(0.0f, m_reloadT - dt);

    // Magazine motion for the reload. Phases: drop (0-30%), out (30-45%),
    // insert (45-80%), seated. Driven by the same timer as the dip, so the
    // magazine is home exactly when ammo returns. The attachment is a SEPARATE
    // unskinned model, so it is animated even when the gun body has a Reload
    // clip (the clip's Magazine joint has no geometry bound to it).
    if (m_attach && m_magMesh >= 0 && m_reloadSeconds > 0.0f) {
        const float p = 1.0f - (m_reloadT / m_reloadSeconds);   // 0..1
        if (p <= 0.0f) m_magDrop = 0.0f;
        else if (p < 0.30f) m_magDrop = p / 0.30f;
        else if (p < 0.45f) m_magDrop = 1.0f;
        else if (p < 0.80f) m_magDrop = 1.0f - (p - 0.45f) / 0.35f;
        else m_magDrop = 0.0f;
    } else {
        m_magDrop = 0.0f;
    }

    if (!m_skel || m_active < 0) return;
    m_time += dt * m_clipRate;
    if (!m_loop) {
        float dur = m_skel->ClipSeconds(m_active, m_clipFps);
        if (dur > 0.0f && m_time >= dur) {
            m_active = m_clipIdle;
            m_time = 0.0f;
            m_clipRate = 1.0f;
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

    // Reload dip / tilt. A skeletal Reload clip owns the motion when present, so
    // the procedural dip is skipped then (it used to run on top of the clip).
    float dip = 0.0f, rtilt = 0.0f;
    const bool reloadClipOwnsMotion = (m_skel && m_clipReload >= 0);
    if (m_reloadT > 0.0f && m_reloadSeconds > 0.0f && !reloadClipOwnsMotion) {
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
    // `center` is the model's own bounds midpoint (base and attachment differ),
    // so both copies of the weapon spin/drop about their own centre while
    // sharing the camera-space offset, rotation and scale.
    auto buildLocal = [&](const Vector3& center, const Vector3& pivotOff, float scaleMul) {
        return MatrixMultiply(
            MatrixMultiply(
                MatrixTranslate(-center.x, -center.y, -center.z),
                MatrixRotateXYZ({rotDeg.x * DEG2RAD, rotDeg.y * DEG2RAD, rotDeg.z * DEG2RAD})),
            MatrixMultiply(
                MatrixScale(fitScale * scaleMul, fitScale * scaleMul, fitScale * scaleMul),
                MatrixTranslate(pivotOff.x, pivotOff.y, pivotOff.z)));
    };
    Matrix local = buildLocal(m_center, off, 1.0f);
    Matrix world = MatrixMultiply(local, camWorld);

    if (m_skel && m_active >= 0)
        m_skel->ApplyPose(m_active, m_time, m_clipFps);

    if (s_trace)
        OZ_INFO("[CHAIN] VM::Draw pre-DrawMatrix key='%s' recoil=%.2f reloadT=%.2f",
                m_key.c_str(), m_recoil, m_reloadT);

    // Fail loudly instead of silently: a view-model that is not on screen at all
    // is indistinguishable from a broken one, which is exactly how the
    // multiply-order bug survived. Warn once per weapon key.
    //
    // This used to derive the near depth from `-m_offset.z`, but the camera-space
    // z of the model is NEGATIVE for anything in front (`off.z = -m_offset.z`),
    // so every weapon computed a negative depth and reported "behind=1" - a false
    // alarm on all of them, which is how a real one would be missed. Transform the
    // fitted box's corners by the SAME local matrix the draw uses and count how
    // many land inside the frustum: no sign guessing, and it accounts for the
    // 90-degree yaw that swaps the box's extents.
    //
    // The test is "NO corner visible", not "any corner outside". A shipped
    // view-model deliberately runs its grip off the bottom-right edge (that is
    // what makes a gun with no hand model read as held), so a per-corner
    // out-of-frame test warned on every weapon and would have drowned a real
    // one. Behind-the-eye is still checked separately.
    if (m_key != s_frustumWarned) {
        const BoundingBox& b = m_mesh->Bounds();
        const float tanHalf = std::tan(camera.fovy * 0.5f * DEG2RAD);
        const float aspect = (float)GetScreenWidth() /
                             (float)std::fmax(1.0f, (float)GetScreenHeight());
        float nearestDepth = 1e9f;
        int visibleCorners = 0;
        bool behind = false;
        for (int corner = 0; corner < 8; ++corner) {
            const Vector3 p = {
                (corner & 1) ? b.max.x : b.min.x,
                (corner & 2) ? b.max.y : b.min.y,
                (corner & 4) ? b.max.z : b.min.z,
            };
            const Vector3 v = Vector3Transform(p, local);
            // Forward is -Z in view space, so this corner's depth is -v.z.
            const float depth = -v.z;
            if (depth <= 0.0f) behind = true;
            const float d = std::fmax(depth, 0.01f);
            nearestDepth = std::fmin(nearestDepth, depth);
            if (depth > 0.0f && std::fabs(v.x) <= tanHalf * aspect * d &&
                std::fabs(v.y) <= tanHalf * d) {
                visibleCorners++;
            }
        }
        const bool fullyOutside = (visibleCorners == 0);
        if (behind || fullyOutside) {
            OZ_WARN("ViewModel '%s' is off-screen (nearest=%.3f visibleCorners=%d "
                    "behind=%d) - check viewmodel_offset in the weapon def",
                    m_key.c_str(), nearestDepth, visibleCorners, behind ? 1 : 0);
            s_frustumWarned = m_key;
        }
        // A weapon whose near end is close to the eye is not "off-screen", so the
        // check above misses it: it is on screen and enormous, with the grip
        // smeared by a near-field perspective blowup. That is what the shipped
        // pistol did (0.17 units from the eye) and it read as a broken asset
        // rather than as a bad offset. Keep the near end >= ~0.4.
        if (!behind && nearestDepth < 0.4f) {
            OZ_WARN("ViewModel '%s' sits too close to the eye (nearest=%.3f) - the "
                    "grip will fill the frame and stretch; push viewmodel_offset.z "
                    "out so (z - length/2) >= 0.5", m_key.c_str(), nearestDepth);
            s_frustumWarned = m_key;
        }
    }

    // Depth test stays ON: a wall between the camera and the weapon must occlude
    // it. Disabling it (the old behaviour) drew the pistol over every wall in the
    // level, which read as "occlusion missing".
    rlDrawRenderBatchActive();
    m_mesh->DrawMatrix(world, litShader);

    // Detachable magazine (sibling `_AdditionalMeshes.glb`). Drawn with the same
    // transform; during a reload it drops out of the well and is reinserted, so
    // the gun is never seen firing with an empty mag well.
    //
    // Drawn ONLY while a reload is in flight. The attachment is a separate model
    // authored at its own origin, and the automatic probe cannot know where the
    // well is, so drawing it permanently put a stray magazine slab next to the
    // grip (visible as a bright block on the pistol and the SMG). During the
    // reload it is the thing the animation is about; the rest of the time the
    // weapon model reads as complete.
    if (m_attach && m_attach->Valid() && m_magMesh >= 0 && m_reloadT > 0.0f) {
        // Camera-space nudge/scale so an attachment authored in a different
        // pivot can be corrected without re-exporting the asset.
        const EntityDef* wdef = ent ? ent->def : nullptr;
        const Vector3 adj = VmStatVec3(wdef, "viewmodel_attachment_offset", {0.0f, 0.0f, 0.0f});
        const float magScale = VmStatFloat(wdef, "viewmodel_attachment_scale", 1.0f);
        const float drop = 0.12f * m_magDrop;
        Vector3 magOff = {off.x + adj.x, off.y + adj.y - drop, off.z + adj.z};
        Matrix magWorld = MatrixMultiply(buildLocal(m_attachCenter, magOff, magScale), camWorld);
        m_attach->DrawSubmeshMatrix(m_magMesh, magWorld, litShader);
    }

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
