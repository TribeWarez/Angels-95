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

// The guaranteed default. Rigged and carries its own embedded textures.
constexpr const char* kDefaultMesh = "GameData/Pawn/Player/Plague_Arcanist.glb";
constexpr const char* kLegacyMesh  = "GameData/Pawn/Player/Character_Killer_01.glb";
constexpr const char* kLegacyTex   = "GameData/Pawn/Player/Character_Killer_01.png";
constexpr const char* kBaseDir = "GameData/";

} // namespace

PlayerModel& PlayerModel::Instance() {
    // Intentionally leaked: the mesh is owned by MeshCache anyway, and GL
    // unloads must not run during static destruction (after CloseWindow).
    static PlayerModel* instance = new PlayerModel();
    return *instance;
}

std::string PlayerModel::ResolveId(const std::string& characterId) {
    // Legacy profiles stored a mesh path; CharacterRoster maps that back to the
    // entry id. Unknown ids resolve to "", which means "the Player def".
    return angelsmenu::CharacterRoster::Instance().ResolveSelection(characterId);
}

bool PlayerModel::TryResolveAsset(const std::string& characterId,
                                  std::string& meshPath, std::string& texPath,
                                  bool& skeletal) const {
    // Rung 1: the character roster entry.
    if (const angelsmenu::CharacterDef* c =
            angelsmenu::CharacterRoster::Instance().Find(characterId)) {
        meshPath = c->mesh;
        texPath = c->texture;
        skeletal = (c->meshType == "skeletal");
        return true;
    }
    // Rung 2: the authoring hook on the Player def.
    if (const EntityDef* def = LightningEntityRegistry::Instance().Find("Player")) {
        if (!def->mesh.empty()) {
            meshPath = def->mesh;
            texPath  = def->texture;
            skeletal = (def->meshType == "skeletal");
            return true;
        }
    }
    // Rung 3: the guaranteed default.
    meshPath = kDefaultMesh;
    texPath.clear();
    skeletal = false;
    return true;
}

PlayerModel::Entry& PlayerModel::EnsureLoaded(const std::string& characterId) {
    const std::string id = ResolveId(characterId);

    auto found = m_entries.find(id);
    if (found != m_entries.end()) {
        Entry& existing = found->second;
        if (existing.tried) return existing;
        if (existing.retryAfter > 0.0 && now_seconds() < existing.retryAfter)
            return existing;
    }

    Entry e;
    std::string meshPath, texPath;
    bool skeletal = false;
    TryResolveAsset(id, meshPath, texPath, skeletal);

    e.mesh = skeletal
        ? MeshCache::Instance().GetSkeletal(meshPath, texPath, kBaseDir, true)
        : MeshCache::Instance().GetStatic(meshPath, texPath, kBaseDir, true);

    if (e.mesh && e.mesh->Valid()) {
        e.skel = dynamic_cast<SkeletalMesh*>(e.mesh.get());
        e.tried = true;
        e.sourcePath = meshPath;
        OZ_INFO("PlayerModel[%s]: using '%s' (%s)",
                id.empty() ? "default" : id.c_str(), meshPath.c_str(),
                (e.skel && e.skel->ClipCount() > 0) ? "skeletal" : "static");
        m_entries[id] = std::move(e);
        return m_entries[id];
    }

    // A named character that fails to load must not become a capsule. Fall back
    // to the default asset, but keep the failure rate-limited so a missing file
    // costs one probe per retry interval rather than one per frame.
    if (!id.empty()) {
        if (m_unknownReported.insert(id).second)
            OZ_WARN("PlayerModel: character '%s' failed to load '%s' — using the default model",
                    id.c_str(), meshPath.c_str());
        // IMPORTANT: this may insert into m_entries and rehash it, so do not
        // hold a reference to any map element across the call.
        Entry fallback = EnsureLoaded({});
        e.mesh = fallback.mesh;
        e.skel = fallback.skel;
        e.tried = (fallback.mesh && fallback.mesh->Valid());
        e.sourcePath = fallback.sourcePath;
        if (!e.tried) e.retryAfter = now_seconds() + kRetryInterval;
        m_entries[id] = std::move(e);
        return m_entries[id];
    }

    // Default failed: walk down to the legacy asset.
    e.mesh = MeshCache::Instance().GetStatic(kLegacyMesh, kLegacyTex, kBaseDir, true);
    if (e.mesh && e.mesh->Valid()) {
        e.skel = dynamic_cast<SkeletalMesh*>(e.mesh.get());
        e.tried = true;
        e.sourcePath = kLegacyMesh;
        OZ_WARN("PlayerModel: '%s' unavailable — fell back to '%s'",
                meshPath.c_str(), kLegacyMesh);
        m_entries[id] = std::move(e);
        return m_entries[id];
    }
    e.mesh.reset();
    e.skel = nullptr;
    e.retryAfter = now_seconds() + kRetryInterval;
    OZ_WARN("PlayerModel: no player model (tried '%s' then '%s') — placeholder in use; retrying in %.1fs",
            meshPath.c_str(), kLegacyMesh, kRetryInterval);
    m_entries[id] = std::move(e);
    return m_entries[id];
}

Mesh* PlayerModel::Get(const std::string& characterId) {
    Entry& e = EnsureLoaded(characterId);
    return (e.mesh && e.mesh->Valid()) ? e.mesh.get() : nullptr;
}

SkeletalMesh* PlayerModel::Skeletal(const std::string& characterId) {
    Entry& e = EnsureLoaded(characterId);
    return (e.skel && e.skel->ClipCount() > 0) ? e.skel : nullptr;
}

bool PlayerModel::Ready(const std::string& characterId) {
    return Get(characterId) != nullptr;
}

float PlayerModel::NormalisedScale(const std::string& characterId, float targetHeight) {
    Entry& e = EnsureLoaded(characterId);
    if (!e.mesh || !e.mesh->Valid()) return 1.0f;
    const BoundingBox& b = e.mesh->Bounds();
    const float h = b.max.y - b.min.y;
    if (h <= 0.0001f) return 1.0f;
    return targetHeight / h;
}

void PlayerModel::DrawInstance(const std::string& characterId, const MeshTransform& t,
                               Shader litShader, int animClip, float animTime) {
    Entry& e = EnsureLoaded(characterId);
    if (!e.mesh || !e.mesh->Valid()) return;
    if (animClip >= 0 && e.skel && e.skel->ClipCount() > 0)
        e.skel->ApplyPose(animClip, animTime);
    MeshTransform mt = t;
    mt.yaw += kFacingOffsetDeg;
    e.mesh->Draw(mt, litShader);
}

void PlayerModel::Invalidate() {
    m_entries.clear();
    m_unknownReported.clear();
}

} // namespace oz
