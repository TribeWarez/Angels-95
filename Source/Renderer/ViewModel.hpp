#pragma once
#include "raylib.h"
#include "Mesh/Mesh.hpp"
#include "Mesh/SkeletalMesh.hpp"
#include <memory>
#include <string>

struct EntityDef;

namespace oz {

// ---------------------------------------------------------------------------
// ViewModel — first-person weapon model drawn attached to the camera.
//
// The weapon mesh/clip files and the camera-space transform come from the
// selected weapon EntityDef's `stats`:
//   viewmodel_mesh / viewmodel_texture        (strings, default to mesh/texture)
//   viewmodel_offset / viewmodel_rot          (vec3, camera space: x right, y up, z forward)
//   viewmodel_scale / recip                   (floats)
//   recoil                                     (float, kick amount)
//   viewmodel_skeletal                         (float: absent/0 = auto,
//                                               1 = force skeletal playback,
//                                               -1 = force static + procedural)
// Clips are named Idle/Fire/Reload inside the (merged) model GLB; play_clip
// may request any clip by name (PlayClip, routed from Core.hpp).
//
// Skeletal playback is AUTO-ENABLED when the asset carries clips and its
// skinning data fits the one skeleton raylib loaded (BoneIdsFitSkeleton).
// The multi-skin FBX2glTF weapon exports fail that scan — raylib keeps only
// the first skin and animating them can index past the pose array — which is
// why playback used to be hard-disabled for every weapon.
//
// Client-only. Draw() refreshes the weapon from the currently selected entity.
// ---------------------------------------------------------------------------
class ViewModel {
public:
    static ViewModel& Instance();

    void Update(float dt);
    void Draw(Camera3D& camera, Shader litShader);

    void TriggerFire();
    void TriggerReload();
    void Clear();

    // Play a named clip on the CURRENT weapon (one-shot; returns to Idle when
    // it ends; "Idle" itself loops). Driven by the `play_clip` opcode via
    // Core.hpp — the same opcode reaches world pawns through
    // PawnSystem::PlayPawnClip, so one script call animates both copies.
    // Returns false when the held weapon has no skeletal mesh or no clip
    // matches; the caller decides whether that is worth warning about.
    bool PlayClip(const std::string& name);

    // Head-bob sync. The camera bob is computed in Core.hpp; the view-model
    // reads the same phase/amount so the weapon sways with the walk instead of
    // staying rigidly locked to a bobbing camera.
    void SetViewBob(float phase, float amount) { m_bobPhase = phase; m_bobAmount = amount; }

    // World-space point at the weapon's muzzle, derived from the same transform
    // the model is drawn with. Used to anchor the muzzle flash / light to the
    // gun rather than to the camera, which previously put the flash inside the
    // view-model. Returns false when no weapon is loaded.
    bool MuzzleWorldPos(Camera3D camera, Vector3* out) const;

private:
    ViewModel() = default;

    void SetWeapon(const EntityDef* def, int variantIndex = -1);

    std::shared_ptr<Mesh> m_mesh;
    SkeletalMesh* m_skel = nullptr; // alias into m_mesh when it has clips
    int m_clipIdle = -1, m_clipFire = -1, m_clipReload = -1, m_active = -1;
    float m_time = 0.0f;
    float m_loop = true;
    // One-shot playback rate. A reload clip is stretched/squeezed to the
    // authored `reload_time`, so the animation ends exactly when ammo returns.
    float m_clipRate = 1.0f;
    // Sample rate for the clips. raylib has no fps field, so this is the only
    // way to turn keyframe counts into seconds; `viewmodel_anim_fps` overrides
    // the 30 default for assets exported at another rate.
    float m_clipFps = 30.0f;

    // Rigid attachment (the detachable magazine). The shipped weapon GLBs are
    // flattened to single-skin static meshes, so real skinned clips cannot play
    // on them; the magazine lives in a sibling `_AdditionalMeshes.glb` and is
    // drawn with the weapon transform, then moved for the reload.
    std::shared_ptr<Mesh> m_attach;
    int m_magMesh = -1;
    Vector3 m_attachCenter{0.0f, 0.0f, 0.0f};
    float m_magDrop = 0.0f;          // 0..1 drop/reinsert phase from the reload timer

    float m_recoil = 0.0f;       // 0..1 impulse
    float m_recoilMax = 1.0f;
    float m_reloadT = 0.0f;      // reload motion timer (s)
    float m_reloadSeconds = 1.2f;

    Vector3 m_offset{0.22f, -0.18f, 0.45f};
    Vector3 m_rot{0.0f, 0.0f, 0.0f};
    float m_scale = 1.0f;

    // Head-bob phase/amount fed from Core.hpp. Sway is a fraction of the camera
    // bob so the weapon lags the view rather than sliding in lockstep.
    float m_bobPhase = 0.0f;
    float m_bobAmount = 0.0f;
    // Midpoint of the loaded mesh's bounds, subtracted before rotation so the
    // weapon spins about its middle. Recomputed on every (re)load.
    Vector3 m_center{0.0f, 0.0f, 0.0f};

    std::string m_key; // def+mesh+tex, to avoid reloading each frame
};

} // namespace oz
