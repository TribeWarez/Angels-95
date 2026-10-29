#include "PlayerPhysics.hpp"
#include "../Input/Keybinds.hpp"

namespace oz {
namespace physics {

// ---------------------------------------------------------------------------
// Along-up helpers (Phase 7.1b)
//
// Every integrator used to read/write cam.position.y and restore `savedCamY`.
// The general form of "restore the vertical coordinate, keep the horizontal one"
// is to restore the coordinate ALONG `up` and leave the tangential part alone.
// At up == +Y these are bit-for-bit the old operations, which is what lets the
// ordinary Y-up path stay untouched.
// ---------------------------------------------------------------------------
static inline float AlongUp(const Vector3& p, const Vector3& up) {
    return p.x * up.x + p.y * up.y + p.z * up.z;
}

// Reset the position's along-up coordinate to `saved`'s, keeping the tangential
// part. The caller moves the body tangentially BEFORE this (walk input), so this
// is "undo last step's vertical integration", exactly what `cam.position.y =
// savedCamY` meant in a Y-up world.
static inline void RestoreAlongUp(Camera3D& cam, const Vector3& saved, const Vector3& up) {
    const float delta = AlongUp(saved, up) - AlongUp(cam.position, up);
    cam.position.x += up.x * delta;
    cam.position.y += up.y * delta;
    cam.position.z += up.z * delta;
}

bool PlayerPhysics::TestObstacleOverlap(const BoundingBox& player, Vector3 feetPoint,
                                        const Vector3& up, CollisionQuery& q) {
    return q.OverlapsObstacle(player, feetPoint, up);
}

void PlayerPhysics::ClampToGround(Camera3D& cam, Motion& motion, float eyeHeight,
                                  const Vector3& up, CollisionQuery& q) {
    // Two paths on purpose. With up == +Y the column query is used verbatim, so
    // every ordinary world keeps the exact behaviour and cost it had; only a
    // tilted gravity zone pays for the (unaccelerated) along-up ray.
    const float eyeUp = AlongUp(cam.position, up);
    float supportUp = 0.0f;
    bool valid = false;

    if (up.y > 0.999f) {
        GroundSample ground = q.SampleGround(cam.position.x, cam.position.z,
                                             cam.position.y - eyeHeight);
        valid = ground.valid;
        supportUp = ground.y;
    } else {
        // Probe far enough to reach the ground from standing height plus a jump.
        GroundSample ground = q.SampleGroundAlong(cam.position, up, eyeHeight + 6.0f);
        valid = ground.valid;
        supportUp = ground.y;   // already a coordinate along `up`
    }

    if (!valid) {
        motion.onGround = false;
        return;
    }
    if (eyeUp <= supportUp + eyeHeight + kGroundEpsilon) {
        const float snap = (supportUp + eyeHeight) - eyeUp;
        cam.position.x += up.x * snap;
        cam.position.y += up.y * snap;
        cam.position.z += up.z * snap;
        motion.velocityUp = 0.0f;
        motion.onGround = true;
    } else {
        motion.onGround = false;
    }
}

void PlayerPhysics::UpdateFlyVertical(Camera3D& cam, float baseSpeed, const PhysicsInfo& phys,
                                      const Vector3& up, float dt, bool allowUp, bool allowDown) {
    // Honour the gates. They were both `(void)`-discarded, so the signature
    // modelled a vertical-flight restriction that was never wired: any caller
    // could pass false and still fly. The current caller (noclip / flying) passes
    // (true, true), so this is behaviour-preserving today and gives a future
    // restricted-flight caller (spectate, dead, a zone that disables ascent)
    // something that actually works.
    float vu = 0.0f;
    if (allowUp && Keybinds::Down(GameAction::Jump)) vu += 1.0f;
    if (allowDown && Keybinds::Down(GameAction::Crouch)) vu -= 1.0f;
    const float step = vu * baseSpeed * phys.flySpeedMult * dt;
    cam.position.x += up.x * step;
    cam.position.y += up.y * step;
    cam.position.z += up.z * step;
}

void PlayerPhysics::UpdateWaterVertical(Camera3D& cam, Motion& motion, Vector3 savedCamPos,
                                        float eyeHeight, bool swimBlocked,
                                        const PhysicsInfo& phys, const Vector3& up, float dt) {
    (void)eyeHeight;
    RestoreAlongUp(cam, savedCamPos, up);

    if (Keybinds::Pressed(GameAction::Jump) && !swimBlocked)
        motion.velocityUp = phys.swimUpSpeed;

    if (!motion.onGround) {
        motion.velocityUp += -phys.waterGravity * dt; // reduced gravity, along up
        motion.velocityUp *= phys.waterDrag;          // water drag
        const float step = motion.velocityUp * dt;
        cam.position.x += up.x * step;
        cam.position.y += up.y * step;
        cam.position.z += up.z * step;
    }
}

void PlayerPhysics::UpdateLadderVertical(Camera3D& cam, Motion& motion, Vector3 savedCamPos,
                                         bool uiBlocked, const PhysicsInfo& phys,
                                         const Vector3& up, float dt) {
    // Neutralise gravity first: the ladder is the only thing that moves you.
    RestoreAlongUp(cam, savedCamPos, up);
    motion.velocityUp = 0.0f;
    motion.onGround = false;

    if (uiBlocked)
        return;

    float dir = 0.0f;
    if (Keybinds::Down(GameAction::MoveForward)) dir += 1.0f;
    if (Keybinds::Down(GameAction::MoveBack))    dir -= 1.0f;
    const float step = dir * phys.ladderSpeed * dt;
    cam.position.x += up.x * step;
    cam.position.y += up.y * step;
    cam.position.z += up.z * step;
}

void PlayerPhysics::UpdateGroundVertical(Camera3D& cam, Motion& motion, Vector3 savedCamPos,
                                         bool jumpBlocked, const PhysicsInfo& phys,
                                         const Vector3& up, float dt) {
    RestoreAlongUp(cam, savedCamPos, up);

    if (Keybinds::Pressed(GameAction::Jump) && motion.onGround && !jumpBlocked) {
        motion.velocityUp = phys.jumpSpeed;
        motion.onGround = false;
    }

    if (!motion.onGround) {
        // Gravity, projected onto the up axis. For an aligned zone (up ==
        // -normalize(gravityVector)) this is exactly -|g|; during a blend it is
        // the component that still points down the current up, which is what keeps
        // the transition smooth instead of popping when the axis rotates.
        //
        // Gravity has no component perpendicular to `up` by construction, so a
        // scalar along-up velocity remains the whole story even sideways.
        const float gUp = phys.gravityVector.x * up.x +
                          phys.gravityVector.y * up.y +
                          phys.gravityVector.z * up.z;
        motion.velocityUp += gUp * dt;
        // Terminal velocity clamp (bounds the free-fall speed)
        if (motion.velocityUp < -phys.terminalVelocity)
            motion.velocityUp = -phys.terminalVelocity;
        const float step = motion.velocityUp * dt;
        cam.position.x += up.x * step;
        cam.position.y += up.y * step;
        cam.position.z += up.z * step;
    }
}

void PlayerPhysics::RestorePosition(Camera3D& cam, float oldX, float oldY, float oldZ,
                                    bool isNoClip) {
    if (!isNoClip) {
        cam.position.x = oldX;
        cam.position.y = oldY;
        cam.position.z = oldZ;
    }
}

} // namespace physics
} // namespace oz
