#include "PlayerModel.hpp"
#include "Mesh/MeshCache.hpp"
#include "raymath.h"
#include "../Script/LightningEntityRegistry.hpp"
#include "../Script/LightningEntityDef.hpp"
#include "../Log.hpp"
#include <chrono>

namespace oz {

namespace {

double now_seconds() {
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

// The guaranteed default. Chosen because it is rigged and carries its own
// embedded textures, so it cannot be broken by losing a sibling PNG.
constexpr const char* kDefaultMesh = "GameData/Pawn/Player/Plague_Arcanist.glb";
// Last rung before the placeholder. Static and tiny; kept because a 61 KB file
// is worth having when the 4 MB default is missing.
constexpr const char* kLegacyMesh  = "GameData/Pawn/Player/Character_Killer_01.glb";
constexpr const char* kLegacyTex   = "GameData/Pawn/Player/Character_Killer_01.png";

// Directory handed to MeshCache for relative-path resolution. Note this differs
// from the historical `GameData/Pawn/Player/`, because the def's `mesh` is a
// repo-root path and `ResolveMeshAsset` already passes GameData-rooted and
// absolute paths through verbatim — pointing baseDir at the Player folder only
// helped one of the two naming styles.
constexpr const char* kBaseDir = "GameData/";

} // namespace

PlayerModel& PlayerModel::Instance() {
    // Intentionally leaked: the mesh is owned by MeshCache anyway, and GL
    // unloads must not run during static destruction (after CloseWindow).
    static PlayerModel* instance = new PlayerModel();
    return *instance;
}

bool PlayerModel::TryResolveAsset(std::string& meshPath, std::string& texPath,
                                  bool& skeletal) const {
    // Rung 1: the authoring hook. "Player" is looked up the same way a zone's
    // name= finds its skyzone def, so no new plumbing is needed for it.
    if (const EntityDef* def = LightningEntityRegistry::Instance().Find("Player")) {
        if (!def->mesh.empty()) {
            meshPath = def->mesh;
            texPath  = def->texture;
            skeletal = (def->meshType == "skeletal");
            return true;
        }
    }
    // Rungs 2 and 3. No texture is forced onto them: both carry their own
    // materials, and a def that names a mesh without a texture would otherwise
    // get Character_Killer_01.png smeared across every material by Mesh::Load.
    meshPath = kDefaultMesh;
    texPath.clear();
    skeletal = false;
    return true;
}

void PlayerModel::EnsureLoaded() {
    if (m_tried) return;                        // resolved once; never redo it
    if (m_retryAfter > 0.0 && now_seconds() < m_retryAfter) return;

    std::string meshPath, texPath;
    bool skeletal = false;
    TryResolveAsset(meshPath, texPath, skeletal);

    m_mesh = skeletal
        ? MeshCache::Instance().GetSkeletal(meshPath, texPath, kBaseDir, true)
        : MeshCache::Instance().GetStatic(meshPath, texPath, kBaseDir, true);

    if (m_mesh && m_mesh->Valid()) {
        m_skel = dynamic_cast<SkeletalMesh*>(m_mesh.get());
        m_tried = true;
        m_sourcePath = meshPath;
        OZ_INFO("PlayerModel: using '%s' (%s)", meshPath.c_str(),
                (m_skel && m_skel->ClipCount() > 0) ? "skeletal" : "static");
        return;
    }

    // Rung 2 failed. Walk down to the legacy asset rather than giving up: the
    // point of this class is that a remote player is always a model, not a
    // capsule, and a 61 KB fallback costs nothing.
    m_mesh.reset();
    m_skel = nullptr;
    m_mesh = MeshCache::Instance().GetStatic(kLegacyMesh, kLegacyTex, kBaseDir, true);
    if (m_mesh && m_mesh->Valid()) {
        m_skel = dynamic_cast<SkeletalMesh*>(m_mesh.get());
        m_tried = true;
        m_sourcePath = kLegacyMesh;
        OZ_WARN("PlayerModel: '%s' unavailable — fell back to '%s'",
                meshPath.c_str(), kLegacyMesh);
        return;
    }
    m_mesh.reset();
    m_skel = nullptr;

    // Nothing loaded. Do NOT latch: back off and try again, so a def that
    // registers after this frame heals on its own (it produces a different
    // MeshCache key, so it is not shadowed by the negative entry cached above).
    // A file that was merely missing from disk stays missing until the path
    // changes — MeshCache caches negative results, and clearing that cache
    // mid-session is not worth it for an asset the build ships.
    m_retryAfter = now_seconds() + kRetryInterval;
    OZ_WARN("PlayerModel: no player model (tried '%s' then '%s') — placeholder in use; retrying in %.1fs",
            meshPath.c_str(), kLegacyMesh, kRetryInterval);
}

Mesh* PlayerModel::Get() {
    EnsureLoaded();
    return (m_mesh && m_mesh->Valid()) ? m_mesh.get() : nullptr;
}

SkeletalMesh* PlayerModel::Skeletal() {
    Get();
    return (m_skel && m_skel->ClipCount() > 0) ? m_skel : nullptr;
}

bool PlayerModel::Ready() {
    return Get() != nullptr;
}

float PlayerModel::NormalisedScale(float targetHeight) const {
    if (!m_mesh || !m_mesh->Valid()) return 1.0f;
    const BoundingBox& b = m_mesh->Bounds();
    const float h = b.max.y - b.min.y;
    // A zero/negative extent means the mesh has no usable vertical span (a flat
    // card, or bounds we never measured). Fall back to unscaled rather than
    // dividing by something near zero and launching the player into orbit.
    if (h <= 0.0001f) return 1.0f;
    return targetHeight / h;
}

void PlayerModel::DrawInstance(const MeshTransform& t, Shader litShader,
                               int animClip, float animTime) {
    Mesh* mesh = Get();
    if (!mesh) return;

    if (animClip >= 0 && m_skel && m_skel->ClipCount() > 0)
        m_skel->ApplyPose(animClip, animTime);

    MeshTransform mt = t;
    mt.yaw += kFacingOffsetDeg;
    mesh->Draw(mt, litShader);
}

void PlayerModel::Invalidate() {
    m_mesh.reset();
    m_skel = nullptr;
    m_tried = false;
    m_retryAfter = 0.0;
    m_sourcePath.clear();
}

} // namespace oz
