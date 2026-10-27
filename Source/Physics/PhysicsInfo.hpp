#pragma once
#include <cstdint>

// PhysicsInfo — per-zone authorable physics parameters (called "collision
// physics" in the OZONE authoring surface). Plain data, raylib-free, so the
// shared OzoneParser, the dedicated server and the unit tests can include it.
//
// Defaults reproduce the previously hardcoded client constants:
//   gravity 20, jump 8, water gravity 8 / drag 0.95 / swim-up 5,
//   ladder speed 6, fly/noclip multiplier 1.5.
namespace oz {
namespace physics {

// Raylib-free 3-vector, so PhysicsInfo stays linkable from tools/tests that do
// not pull in raylib's Vector3.
struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct PhysicsInfo {
    // Normal (grounded/airborne) integration
    float gravity = 18.0f;            // positive magnitude; applied as -gravity
    // Vector form of gravity, in ENGINE space (Y-up; -Y is down). The scalar
    // `gravity` above is its magnitude for the plain vertical path; gravity
    // zones author the full vector so the player can fall along any axis.
    // Default {0,-18,0} == gravity 18 straight down. When `gravity=` is authored
    // as a scalar, both are written; `gravity_vector=` writes only the vector.
    Vec3 gravityVector{0.0f, -18.0f, 0.0f};
    float jumpSpeed = 9.0f;           // upward impulse on Space
    float terminalVelocity = 60.0f;   // max fall speed (new; was unbounded)

    // Water zones
    float waterGravity = 8.0f;        // positive magnitude; applied as -waterGravity
    float waterDrag = 0.95f;          // per-step velocity multiplier
    float swimUpSpeed = 5.0f;         // upward impulse on Space

    // Ladder zones
    float ladderSpeed = 6.0f;         // W/S climb speed

    // Share speed multiplier used by noclip/fly vertical control
    float flySpeedMult = 1.5f;
};

// Zones that do not override anything embed an untouched PhysicsInfo.
inline const PhysicsInfo& DefaultPhysics() {
    static PhysicsInfo def;
    return def;
}

} // namespace physics
} // namespace oz
