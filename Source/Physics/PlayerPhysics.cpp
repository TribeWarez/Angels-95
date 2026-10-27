#include "PlayerPhysics.hpp"
#include "../Input/Keybinds.hpp"

namespace oz {
namespace physics {

bool PlayerPhysics::TestObstacleOverlap(const BoundingBox& player, float feetY,
                                        CollisionQuery& q) {
    return q.OverlapsObstacle(player, feetY);
}

void PlayerPhysics::ClampToGround(Camera3D& cam, Motion& motion, float eyeHeight,
                                  CollisionQuery& q) {
    GroundSample ground = q.SampleGround(cam.position.x, cam.position.z, cam.position.y - eyeHeight);
    if (!ground.valid) {
        motion.onGround = false;
        return;
    }
    if (cam.position.y <= ground.y + eyeHeight + kGroundEpsilon) {
        cam.position.y = ground.y + eyeHeight;
        motion.velocityY = 0.0f;
        motion.onGround = true;
    } else {
        motion.onGround = false;
    }
}

void PlayerPhysics::UpdateFlyVertical(Camera3D& cam, float baseSpeed, const PhysicsInfo& phys,
                                      float dt, bool allowUp, bool allowDown) {
    // Honour the gates. They were both `(void)`-discarded, so the signature
    // modelled a vertical-flight restriction that was never wired: any caller
    // could pass false and still fly. The current caller (noclip / flying) passes
    // (true, true), so this is behaviour-preserving today and gives a future
    // restricted-flight caller (spectate, dead, a zone that disables ascent)
    // something that actually works.
    float vy = 0.0f;
    if (allowUp && Keybinds::Down(GameAction::Jump)) vy += 1.0f;
    if (allowDown && Keybinds::Down(GameAction::Crouch)) vy -= 1.0f;
    cam.position.y += vy * baseSpeed * phys.flySpeedMult * dt;
}

void PlayerPhysics::UpdateWaterVertical(Camera3D& cam, Motion& motion, float savedCamY,
                                        float eyeHeight, bool swimBlocked,
                                        const PhysicsInfo& phys, float dt) {
    (void)eyeHeight;
    cam.position.y = savedCamY;

    if (Keybinds::Pressed(GameAction::Jump) && !swimBlocked)
        motion.velocityY = phys.swimUpSpeed;

    if (!motion.onGround) {
        motion.velocityY += -phys.waterGravity * dt; // reduced gravity
        motion.velocityY *= phys.waterDrag;          // water drag
        cam.position.y += motion.velocityY * dt;
    }
}

void PlayerPhysics::UpdateLadderVertical(Camera3D& cam, Motion& motion, float savedCamY,
                                        bool uiBlocked, const PhysicsInfo& phys, float dt) {
    // Neutralise gravity first: the ladder is the only thing that moves you.
    cam.position.y = savedCamY;
    motion.velocityY = 0.0f;
    motion.onGround = false;

    if (uiBlocked)
        return;

    if (Keybinds::Down(GameAction::MoveForward))
        cam.position.y += phys.ladderSpeed * dt;
    if (Keybinds::Down(GameAction::MoveBack))
        cam.position.y -= phys.ladderSpeed * dt;
}

void PlayerPhysics::UpdateGroundVertical(Camera3D& cam, Motion& motion, float savedCamY,
                                         bool jumpBlocked, const PhysicsInfo& phys, float dt) {
    cam.position.y = savedCamY;

    if (Keybinds::Pressed(GameAction::Jump) && motion.onGround && !jumpBlocked) {
        motion.velocityY = phys.jumpSpeed;
        motion.onGround = false;
    }

    if (!motion.onGround) {
        // Gravity. Uses the vector form so a gravity zone can retune the fall
        // (including upward). The default {0,-18,0} reproduces -gravity.
        motion.velocityY += phys.gravityVector.y * dt;
        // Terminal velocity clamp (bounds the free-fall speed)
        if (motion.velocityY < -phys.terminalVelocity)
            motion.velocityY = -phys.terminalVelocity;
        cam.position.y += motion.velocityY * dt;
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
