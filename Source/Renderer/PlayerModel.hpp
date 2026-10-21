#pragma once
#include "raylib.h"
#include "Mesh/Mesh.hpp"
#include "Mesh/SkeletalMesh.hpp"
#include "../Menu/CharacterRoster.hpp"
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

struct EntityDef;

namespace oz {

// ---------------------------------------------------------------------------
// PlayerModel — per-character model cache.
//
// `characterId` is a roster id from GameData/Pawn/Player/characters.cfg. An
// empty/unknown id resolves through the original chain:
//   1. the roster entry's mesh/texture/mesh_type
//   2. the "Player" LightningScript def
//   3. GameData/Pawn/Player/Plague_Arcanist.glb
//   4. GameData/Pawn/Player/Character_Killer_01.glb
//   5. nothing (caller draws a placeholder)
//
// Assets are shared through oz::MeshCache; per-instance playback state stays
// with the caller. Client-only.
// ---------------------------------------------------------------------------
class PlayerModel {
public:
    static PlayerModel& Instance();

    Mesh* Get(const std::string& characterId = {});
    SkeletalMesh* Skeletal(const std::string& characterId = {});
    bool Ready(const std::string& characterId = {});

    // Drop every reference and allow the next Get() to resolve again. The
    // assets themselves stay in MeshCache. Called on world load.
    void Invalidate();

    // Scale that renders `characterId` at `targetHeight` engine units tall.
    float NormalisedScale(const std::string& characterId, float targetHeight);

    // Draw one instance with its feet at t.position, facing t.yaw (degrees).
    void DrawInstance(const std::string& characterId, const MeshTransform& t,
                      Shader litShader, int animClip = -1, float animTime = 0.0f);

    // Back-compat wrappers for callers that only need the default model.
    float NormalisedScale(float targetHeight) { return NormalisedScale({}, targetHeight); }
    void DrawInstance(const MeshTransform& t, Shader litShader,
                      int animClip = -1, float animTime = 0.0f) {
        DrawInstance({}, t, litShader, animClip, animTime);
    }

private:
    PlayerModel() = default;

    struct Entry {
        std::shared_ptr<Mesh> mesh;
        SkeletalMesh* skel = nullptr;
        bool tried = false;
        double retryAfter = 0.0;
        std::string sourcePath;
    };

    Entry& EnsureLoaded(const std::string& characterId);
    bool TryResolveAsset(const std::string& characterId, std::string& meshPath,
                         std::string& texPath, bool& skeletal) const;
    static std::string ResolveId(const std::string& characterId);

    std::unordered_map<std::string, Entry> m_entries;
    std::unordered_set<std::string> m_unknownReported;

    static constexpr float kFacingOffsetDeg = 180.0f;
    static constexpr double kRetryInterval = 2.0;
};

} // namespace oz
