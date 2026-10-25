#include "MeshCache.hpp"
#include "../../Log.hpp"
#include <chrono>

namespace oz {

namespace {

constexpr double kRetrySeconds = 2.0;   // same backoff PlayerModel already used

double now_seconds() {
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

MeshCache& MeshCache::Instance() {
    // Intentionally leaked: destroying the cache at static-destruction time
    // would run UnloadModel after CloseWindow() has torn down the GL context.
    // Explicit Clear() (called from PawnSystem while the context is alive) is
    // the only place assets are released.
    static MeshCache* instance = new MeshCache();
    return *instance;
}

std::shared_ptr<Mesh> MeshCache::GetStatic(const std::string& meshPath, const std::string& texPath,
                                           const std::string& baseDir, bool pointFilter) {
    std::string key = "S|" + meshPath + "|" + texPath;
    return GetInternal(key, meshPath, texPath, baseDir, false, pointFilter);
}

std::shared_ptr<Mesh> MeshCache::GetSkeletal(const std::string& meshPath, const std::string& texPath,
                                             const std::string& baseDir, bool pointFilter) {
    std::string key = "K|" + meshPath + "|" + texPath;
    return GetInternal(key, meshPath, texPath, baseDir, true, pointFilter);
}

std::shared_ptr<Mesh> MeshCache::GetSkeletalChecked(const std::string& meshPath,
                                                    const std::string& texPath,
                                                    const std::string& baseDir,
                                                    bool pointFilter) {
    std::shared_ptr<Mesh> mesh = GetSkeletal(meshPath, texPath, baseDir, pointFilter);
    auto* skel = dynamic_cast<SkeletalMesh*>(mesh.get());
    if (skel && !skel->SkeletonFits()) {
        // GetSkeletal already warned at load; downgrade so raylib never indexes
        // model.boneMatrices out of range, and so the caller takes its static
        // path (bind pose) instead of drawing garbage.
        return GetStatic(meshPath, texPath, baseDir, pointFilter);
    }
    return mesh;
}

std::shared_ptr<Mesh> MeshCache::GetAnimated(const std::string& meshPath, const std::string& texPath,
                                             const std::string& animFile, const std::string& baseDir,
                                             bool pointFilter) {
    std::string key = "A|" + meshPath + "|" + texPath + "|" + animFile;
    auto it = m_cache.find(key);
    if (it != m_cache.end()) return it->second;

    const double now = now_seconds();
    auto retry = m_retryAt.find(key);
    if (retry != m_retryAt.end() && now < retry->second) return nullptr;

    std::shared_ptr<Mesh> mesh;
    auto am = std::make_shared<AnimatedMesh>();
    if (am->LoadAnimated(meshPath, texPath, animFile, baseDir, pointFilter)) {
        if (am->ClipCount() > 0) mesh = am;
    }
    if (!mesh) {
        auto st = std::make_shared<StaticMesh>();
        if (st->Load(meshPath, texPath, baseDir, pointFilter)) mesh = st;
    }

    if (mesh) {
        m_cache[key] = mesh;
        m_retryAt.erase(key);
    } else {
        m_retryAt[key] = now + kRetrySeconds;
    }
    return mesh;
}

std::shared_ptr<Mesh> MeshCache::GetInternal(const std::string& key, const std::string& meshPath,
                                             const std::string& texPath, const std::string& baseDir,
                                             bool skeletal, bool pointFilter) {
    auto it = m_cache.find(key);
    if (it != m_cache.end()) return it->second;

    // A failed load is retried after a backoff instead of being latched forever.
    // Callers already handle nullptr (they fall back to a billboard/gizmo), and
    // this runs once per frame for a pawn with no model, so the gate matters.
    const double now = now_seconds();
    auto retry = m_retryAt.find(key);
    const bool retrying = (retry != m_retryAt.end());
    if (retrying && now < retry->second) return nullptr;

    // Log only the FIRST attempt: a permanently missing path would otherwise print
    // a miss line every kRetrySeconds for the life of the session.
    if (!retrying) OZ_INFO("[CHAIN] MeshCache miss key='%s' (creating)", key.c_str());

    std::shared_ptr<Mesh> mesh;
    if (skeletal) {
        auto sm = std::make_shared<SkeletalMesh>();
        if (sm->LoadSkeletal(meshPath, texPath, baseDir, pointFilter)) {
            // Only advertise as skeletal when clips actually exist; otherwise
            // fall through to a plain static mesh ("static unless animated").
            if (sm->ClipCount() > 0) {
                mesh = sm;
            }
        }
    }
    if (!mesh) {
        auto st = std::make_shared<StaticMesh>();
        if (st->Load(meshPath, texPath, baseDir, pointFilter)) mesh = st;
    }

    if (mesh) {
        m_cache[key] = mesh;
        m_retryAt.erase(key);
    } else {
        m_retryAt[key] = now + kRetrySeconds;
    }
    return mesh;
}

void MeshCache::Clear() {
    size_t n = m_cache.size();
    m_cache.clear();
    m_retryAt.clear();
    if (n > 0) OZ_INFO("MeshCache: cleared %zu cached mesh asset(s)", n);
}

} // namespace oz
