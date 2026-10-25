#pragma once
#include "Mesh.hpp"
#include "StaticMesh.hpp"
#include "SkeletalMesh.hpp"
#include "AnimatedMesh.hpp"
#include <string>
#include <unordered_map>

namespace oz {

// Internal mesh asset cache: path -> shared Mesh. Owned/released through
// PawnSystem (the single box for placed entities). Assets are shared across
// instances; per-instance animation state lives on the instance.
class MeshCache {
public:
    static MeshCache& Instance();

    // Returns the cached static mesh, or nullptr when the model cannot load.
    std::shared_ptr<Mesh> GetStatic(const std::string& meshPath, const std::string& texPath,
                                    const std::string& baseDir, bool pointFilter);

    // Returns a skeletal mesh when the asset has clips; otherwise falls back to
    // a StaticMesh so callers can still render it.
    std::shared_ptr<Mesh> GetSkeletal(const std::string& meshPath, const std::string& texPath,
                                      const std::string& baseDir, bool pointFilter);

    // Like GetSkeletal, but a mesh whose vertex bone indices do NOT fit the one
    // skeleton raylib loaded (multi-skin glTF, or a clip from another rig) is
    // downgraded to a StaticMesh instead. raylib's CPU skinning indexes
    // model.boneMatrices with the mesh's bone index, so an unfit asset is an
    // out-of-bounds read that shows up as garbage geometry, not an error.
    // The view-model deliberately uses GetSkeletal so its `viewmodel_skeletal=1`
    // override can still force playback past the scan.
    std::shared_ptr<Mesh> GetSkeletalChecked(const std::string& meshPath, const std::string& texPath,
                                             const std::string& baseDir, bool pointFilter);

    // Returns a vertex-keyframe AnimatedMesh for an external `.ozanim` clip;
    // falls back to a StaticMesh when the clip or mesh cannot be loaded.
    std::shared_ptr<Mesh> GetAnimated(const std::string& meshPath, const std::string& texPath,
                                      const std::string& animFile, const std::string& baseDir,
                                      bool pointFilter);

    // Unload every cached asset (clear PawnSystem nodes first).
    void Clear();

private:
    std::shared_ptr<Mesh> GetInternal(const std::string& key, const std::string& meshPath,
                                      const std::string& texPath, const std::string& baseDir,
                                      bool skeletal, bool pointFilter);

    std::unordered_map<std::string, std::shared_ptr<Mesh>> m_cache;
    // Failure retry gate. A cache MISS only latches a failure for this long.
    // Caching nullptr forever meant one unlucky early load (before a package
    // hot-loaded, or before the .ozls defs registered) pinned that asset to
    // "missing" for the whole session, and made PlayerModel's own retry
    // impossible to succeed. Keyed like m_cache.
    std::unordered_map<std::string, double> m_retryAt;
};

} // namespace oz
