#pragma once
#include "raylib.h"
#include "raymath.h"
#include <cmath>

// PlayerMovement — per-frame physics and movement state
// Stat fields (Health, Mana, PsychicEnergy, Level, XP) are in LightningEntityManager player entity
constexpr float PLAYER_EYE_HEIGHT = 2.0f;

// ---------------------------------------------------------------------------
// Custom first-person controller state.
//
// Movement used to be delegated to raylib's UpdateCamera(); it exposes no
// speed/sensitivity knobs, so look + WASD are integrated here instead. This is
// what lets us honour per-entity `movement_speed` and support sprint/crouch.
// Defaults mirror raylib: CAMERA_MOVE_SPEED (5.4) and mouse sensitivity 0.003.
// ---------------------------------------------------------------------------
class PlayerMovement {
public:
    // Collision box, anchored to the FEET (see UpdateBounds). Height is the
    // full standing body height; PLAYER_EYE_HEIGHT is where the camera sits
    // inside that box. Keeping the two related is what stops walls reading as
    // five-times-taller-than-the-player shafts.
    //
    // Deliberately a little smaller than the visual silhouette: brush collision
    // is AABB-only and over-covers rotated/tapered geometry (RotatedAabbY), so a
    // box the exact size of the visual snags in doorways and tight corners the
    // player can plainly see a gap through. The old 2.0 x 3.0 hull left no
    // clearance in a 2.0-wide opening.
    float Height = 2.8f;
    float Width = 1.5f;

    int HeadBob = 0;
    int HeadBobDirection = 1;

    float velocityY = 0.2f;
    bool onGround = false;
    bool isFlying = false;
    bool isNoClip = false;
    bool inWater = false;
    bool isClimbing = false;   // inside a `zone ladder`; gravity suspended

    // --- Controller tuning ---
    float BaseSpeed = 5.4f;          // units/sec at movement_speed == 1
    float SprintMultiplier = 1.6f;   // hold Shift
    float CrouchMultiplier = 0.5f;   // hold Ctrl
    float MouseSensitivity = 0.003f; // radians per pixel
    float LookDistance = 10.0f;      // camera target distance (matches old cam)

    // --- Stance state ---
    bool isSprinting = false;
    bool isCrouching = false;
    float StandEyeHeight = PLAYER_EYE_HEIGHT;
    float CrouchEyeHeight = 1.2f;
    float EyeHeight = PLAYER_EYE_HEIGHT; // current, smoothed toward the stance

    // --- Authoritative look state ---
    // Owned here (not re-derived from camera.target every frame) so that the
    // vertical physics — which moves position but not target — can never pitch
    // the view. Re-synced from the camera on first use or when something else
    // changes the camera's facing (respawn / portal / SetCamera), detected by a
    // large direction mismatch.
    float Yaw = 0.0f;
    float Pitch = 0.0f;
    bool LookInit = false;

    float OldX = 0.0f, OldY = 0.0f, OldZ = 0.0f;

    BoundingBox PlayerBounds;

    // Read mouse + WASD/gamepad, apply the per-entity speed multiplier plus
    // sprint/crouch, and move the camera horizontally. Y is owned by the
    // caller's jump/gravity code. When `inputBlocked` (inventory/skill/pause/
    // console) look and move are skipped, but the stance still settles.
    void UpdateLookAndMove(Camera3D& cam, float dt, float speedScalar, bool inputBlocked) {
        if (dt <= 0.0f) dt = 1.0f / 60.0f;

        if (!inputBlocked) {
            isCrouching = IsKeyDown(KEY_LEFT_CONTROL);
            isSprinting = IsKeyDown(KEY_LEFT_SHIFT) && !isCrouching;
        } else {
            isSprinting = false;
        }

        float targetEye = isCrouching ? CrouchEyeHeight : StandEyeHeight;
        EyeHeight += (targetEye - EyeHeight) * fminf(1.0f, 10.0f * dt);

        if (inputBlocked) return;

        // Re-sync the authoritative look when the camera's facing changed
        // externally (first frame, teleport, respawn). Vertical drift from the
        // jump/gravity code is tiny, so it stays below the mismatch threshold.
        Vector3 camDir = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
        Vector3 lookDir = {cosf(Pitch) * sinf(Yaw), sinf(Pitch), cosf(Pitch) * cosf(Yaw)};
        if (!LookInit || Vector3DotProduct(camDir, lookDir) < 0.9f) {
            Yaw = atan2f(camDir.x, camDir.z);
            Pitch = asinf(fmaxf(-1.0f, fminf(1.0f, camDir.y)));
            LookInit = true;
        }

        Vector2 md = GetMouseDelta();
        Yaw -= md.x * MouseSensitivity;
        Pitch -= md.y * MouseSensitivity;

        // Gamepad right stick look (raylib's first-person camera provided this)
        float lookX = GetGamepadAxisMovement(0, GAMEPAD_AXIS_RIGHT_X);
        float lookY = GetGamepadAxisMovement(0, GAMEPAD_AXIS_RIGHT_Y);
        if (fabsf(lookX) > 0.15f) Yaw -= lookX * 2.4f * dt;
        if (fabsf(lookY) > 0.15f) Pitch -= lookY * 1.8f * dt;

        if (Pitch > 1.5533f) Pitch = 1.5533f;   // clamp ~89 degrees
        if (Pitch < -1.5533f) Pitch = -1.5533f;

        // --- Move input relative to yaw ---
        float f = 0.0f, r = 0.0f;
        if (IsKeyDown(KEY_W)) f += 1.0f;
        if (IsKeyDown(KEY_S)) f -= 1.0f;
        if (IsKeyDown(KEY_D)) r += 1.0f;
        if (IsKeyDown(KEY_A)) r -= 1.0f;
        float gx = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_X);
        float gy = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_Y);
        if (fabsf(gx) > 0.15f) r += gx;
        if (fabsf(gy) > 0.15f) f -= gy;

        float len = sqrtf(f * f + r * r);
        if (len > 1.0f) { f /= len; r /= len; }

        float speed = BaseSpeed * (speedScalar > 0.0f ? speedScalar : 1.0f);
        if (isSprinting) speed *= SprintMultiplier;
        if (isCrouching) speed *= CrouchMultiplier;

        Vector3 forward = {sinf(Yaw), 0.0f, cosf(Yaw)};
        // Right-hand strafe: cross(forward, up) for a Y-up camera.
        Vector3 right   = {-cosf(Yaw), 0.0f, sinf(Yaw)};
        Vector3 move = {forward.x * f + right.x * r, 0.0f, forward.z * f + right.z * r};
        float dist = speed * dt;

        // Split into sub-steps so a sprint step cannot tunnel through a thin
        // brush AABB between the per-frame collision tests.
        int steps = (int)(dist / 0.5f) + 1;
        for (int i = 0; i < steps; i++) {
            float s = dist / (float)steps;
            cam.position.x += move.x * s;
            cam.position.z += move.z * s;
        }

        // Rebuild the look target from the owned yaw/pitch.
        Vector3 dir = {cosf(Pitch) * sinf(Yaw), sinf(Pitch), cosf(Pitch) * cosf(Yaw)};
        cam.target = Vector3Add(cam.position, Vector3Scale(dir, LookDistance));
    }

    // Collision box in world space, measured from the FEET upward.
    //
    // The box deliberately spans [feet, feet + Height] rather than
    // [cam.y - Height, cam.y]: the old anchoring hung most of the box below the
    // feet, where OverlapsObstacle's "top at/below the feet is a floor, not an
    // obstacle" rule discarded it, so the effective body was only EyeHeight tall
    // and Height did nothing. Anchoring to the feet makes Height meaningful and
    // makes ceilings below feet+Height solid.
    //
    // Uses the smoothed EyeHeight so crouching (1.2) ducks under low ceilings.
    void UpdateBounds(Camera3D& cam) {
        const float feet = cam.position.y - EyeHeight;
        PlayerBounds = (BoundingBox){
            (Vector3){cam.position.x - Width / 2,
                       feet,
                       cam.position.z - Width / 2},
            (Vector3){cam.position.x + Width / 2,
                       feet + Height,
                       cam.position.z + Width / 2}};
    }
};
