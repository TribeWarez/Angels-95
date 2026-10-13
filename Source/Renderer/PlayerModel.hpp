#pragma once
#include "raylib.h"
#include "Mesh/Mesh.hpp"
#include "Mesh/SkeletalMesh.hpp"
#include <memory>
#include <string>

struct EntityDef;

namespace oz {

// ---------------------------------------------------------------------------
// PlayerModel — shared player-character model.
//
// Every other player you see online is drawn from ONE shared asset resolved
// here, so "can remote players be seen at all?" reduces to a single question:
// did EnsureLoaded() succeed? It is written so the answer is always yes unless
// every rung of the fallback chain failed, and so a failure is temporary rather
// than permanent.
//
// RESOLUTION ORDER (first that loads wins, and the winner is logged)
//   1. `mesh` / `texture` on the "Player" LightningScript def, when it names a
//      mesh. This is the authoring hook — point it anywhere.
//   2. GameData/Pawn/Player/Plague_Arcanist.glb — the guaranteed default.
//      Rigged, embedded textures, no external file to lose.
//   3. GameData/Pawn/Player/Character_Killer_01.glb + .png — the historical
//      default. Static, so it can never animate, but it is tiny.
//   4. Nothing loaded: the caller draws a primitive placeholder.
//
// WHY THE RETRY
// -------------
// The previous version latched `m_tried = true` on its FIRST call regardless
// of the outcome, so a single unlucky early call — before the .ozls defs were
// registered, or before a package was hot-loaded — pinned the whole session to
// the placeholder capsule. Load failure now only rate-limits: we retry on a
// later frame, and a late-arriving def heals itself. Invalidate() resets it on
// a world load, where the def map is re-pointed anyway.
//
// Assets with clips load as a SkeletalMesh; without clips they load as a static
// mesh. Playback state stays with the caller (see DrawRemotePlayers3D).
// Client-only.
// ---------------------------------------------------------------------------
class PlayerModel {
public:
    static PlayerModel& Instance();

    // Lazily resolve + load the shared asset. Never returns a dangling pointer:
    // callers fall back to a primitive placeholder when Get() is null.
    Mesh* Get();
    SkeletalMesh* Skeletal(); // non-null only when the asset actually has clips
    bool Ready();

    // Drop our reference and allow the next Get() to resolve again. The asset
    // itself stays in MeshCache. Called from LoadWorld() and after the def map
    // is re-pointed at a new world.
    void Invalidate();

    // Scale that renders the asset at `targetHeight` engine units tall. The
    // character GLBs are authored in metres while PlayerMovement::Height is 3.0,
    // so an un-normalised draw leaves remote players at roughly half the
    // collision silhouette. Returns 1.0f when the bounds are unusable.
    float NormalisedScale(float targetHeight) const;

    // Draw one instance with its feet at t.position, facing t.yaw (degrees).
    // `animClip` < 0 draws the bind pose. Applies the model's facing correction.
    void DrawInstance(const MeshTransform& t, Shader litShader,
                      int animClip = -1, float animTime = 0.0f);

private:
    PlayerModel() = default;
    void EnsureLoaded();
    bool TryResolveAsset(std::string& meshPath, std::string& texPath, bool& skeletal) const;

    std::shared_ptr<Mesh> m_mesh;
    SkeletalMesh* m_skel = nullptr;
    bool m_tried = false;        // a successful resolve happened (never retried)
    double m_retryAfter = 0.0;   // monotonic-ish stamp; failure backs off
    std::string m_sourcePath;

    // FBX/mixamo characters end up facing -Z after FBX2glTF bakes the Z-up ->
    // Y-up rotation; the engine's yaw 0 faces +Z, so correct by 180 degrees.
    static constexpr float kFacingOffsetDeg = 180.0f;

    // How long to wait before retrying a failed resolve. Long enough that a
    // missing asset costs one probe per interval rather than one per frame.
    static constexpr double kRetryInterval = 2.0;
};

} // namespace oz
