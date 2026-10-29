#pragma once
#include "raylib.h"
#include "PhysicsInfo.hpp"
#include <vector>

// PlayerPhysics — the client player/collision/physics module.
//
// Owns the vertical Y integration (gravity/jump/water/terminal velocity), the
// OZONE ground clamp (heightmap first, then brush tops), the OZONE brush
// obstacle test and the collision rollback. Previously split inline between
// Main.cpp (gravity) and Core.hpp DrawWorld() (obstacle/ground/rollback).
//
// World queries are injected through a small adapter interface so this module
// stays independent of OzoneLoader/PawnSystem and stays unit-testable.
namespace oz {
namespace physics {

struct GroundSample {
    float y = 0.0f;      // ground surface Y (world space)
    bool valid = false;  // false when there is no support below/at feet
};

// Shared constants
inline constexpr float kGroundEpsilon = 0.1f;
inline constexpr float kNoGroundY = -99999.0f;

// Step-up height. A surface whose top face is at or below `feetY + kStepHeight`
// is treated as walkable: it is skipped by the obstacle test AND offered to the
// ground clamp, so a run of ≤kStepHeight steps is climbed by simply walking at
// it. This is a hard cliff, not a ramp — anything taller still blocks, which is
// what keeps walls solid. Must stay >= kGroundEpsilon so the surface the player
// is standing on is never mistaken for an obstacle.
inline constexpr float kStepHeight = 1.0f;

class PlayerPhysics {
public:
    // World queries implemented by a thin adapter over OzoneLoader.
    struct CollisionQuery {
        // Highest support surface (heightmap or brush top) at (x, z) that is at
        // or below `feetY + kStepHeight` — the step-up tolerance. Must consider
        // BOTH candidates and return the higher valid one, otherwise a heightmap
        // covering a map hides every raised platform/stair above it.
        // valid=false when unsupported.
        //
        // This is the Y-up COLUMN query (NPCs, scripted ground placement). The
        // player uses SampleGroundAlong, which generalises it to a tilted gravity
        // axis; the two must agree whenever up == +Y.
        virtual GroundSample SampleGround(float x, float z, float feetY) = 0;

        // Support surface below `origin` along -`up`, within `maxDist` of it,
        // returned as `GroundSample::y` = that surface's coordinate ALONG `up`
        // (i.e. dot(hitPoint, up)), not a world Y.
        //
        // Needed because SampleGround indexes an XZ chunk grid and cannot answer a
        // tilted query at all. Default returns invalid: only the engine adapter
        // (OzoneCollisionQuery) can implement it, and a caller that never tilts
        // gravity never reaches it.
        virtual GroundSample SampleGroundAlong(Vector3 origin, const Vector3& up, float maxDist) {
            (void)origin; (void)up; (void)maxDist;
            return GroundSample{0.0f, false};
        }

        // True when the player box overlaps a solid non-floor obstacle.
        //
        // `up` is the gravity up axis and `feetPoint` the body's feet, so the
        // step-up rule ("a surface whose top is within kStepHeight of the feet is
        // walkable, not an obstacle") is measured ALONG the gravity axis. Passing
        // only a world Y - the old signature - silently made every surface above a
        // tilted body an obstacle.
        virtual bool OverlapsObstacle(const BoundingBox& player, Vector3 feetPoint,
                                      const Vector3& up) = 0;
        virtual ~CollisionQuery() = default;
    };

    // Smoothed vertical velocity state normally mirrored on the caller
    // (PlayerMovement). `velocityUp` is the speed ALONG the up axis: positive is
    // jumping/rising, negative is falling. Gravity has no component perpendicular
    // to `up` by definition (up == -normalize(g)), so a scalar is still the whole
    // story even when gravity points sideways.
    struct Motion {
        bool onGround = false;
        float velocityUp = 0.0f;
    };

    // --- One-shot queries (called from Core.hpp draw/sim pass) ---

    // Brush obstacle test against a chunked world query. Sets `ObjectCollision`
    // equivalent result; heightmap volumes must be excluded by the adapter.
    bool TestObstacleOverlap(const BoundingBox& player, Vector3 feetPoint,
                             const Vector3& up, CollisionQuery& q);

    // Ground clamp: picks the highest valid support (heightmap or brush top) at
    // or below feet + kStepHeight, then snaps the camera onto it ALONG `up`.
    // Mutates cam position, motion.velocityUp and motion.onGround. Skipped while
    // flying/noclipping (caller checks flags).
    void ClampToGround(Camera3D& cam, Motion& motion, float eyeHeight,
                       const Vector3& up, CollisionQuery& q);

    // --- Fixed-step vertical integration (called by PlayerController) ---

    // Noclip / fly: direct Space/Ctrl control along `up` at BaseSpeed * flyMult.
    void UpdateFlyVertical(Camera3D& cam, float baseSpeed, const PhysicsInfo& phys,
                           const Vector3& up, float dt, bool allowUp, bool allowDown);

    // Water zones: restored along-up position, reduced gravity + drag, swim-up impulse.
    void UpdateWaterVertical(Camera3D& cam, Motion& motion, Vector3 savedCamPos,
                             float eyeHeight, bool swimBlocked,
                             const PhysicsInfo& phys, const Vector3& up, float dt);

    // Ladder zones: restored position, NO gravity, explicit W/S climb along `up`.
    // Without this branch a ladder zone still runs UpdateGroundVertical, whose
    // gravity (18) dwarfs ladderSpeed (6) and makes the player sink.
    void UpdateLadderVertical(Camera3D& cam, Motion& motion, Vector3 savedCamPos, bool uiBlocked,
                              const PhysicsInfo& phys, const Vector3& up, float dt);

    // Grounded / airborne: restored position, jump impulse, gravity along `up`,
    // terminal clamp.
    void UpdateGroundVertical(Camera3D& cam, Motion& motion, Vector3 savedCamPos,
                              bool jumpBlocked, const PhysicsInfo& phys,
                              const Vector3& up, float dt);

    // Rollback to the pre-step snapshot (full X/Y/Z, noclip exempt).
    void RestorePosition(Camera3D& cam, float oldX, float oldY, float oldZ, bool isNoClip);
};

} // namespace physics
} // namespace oz
