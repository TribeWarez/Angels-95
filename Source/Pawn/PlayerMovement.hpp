#pragma once
#include "raylib.h"
#include "raymath.h"
#include "../Input/Keybinds.hpp"
#include "../GameplayFlags.hpp"
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

    // Speed ALONG the gravity axis (`up`): positive is jumping/rising, negative is
    // falling. Was `velocityY` when the world was Y-up only; gravity has no
    // component perpendicular to `up`, so a scalar is still the whole story in a
    // tilted / inverted zone - only the axis it is measured on changed.
    float velocityUp = 0.2f;
    bool onGround = false;
    bool isFlying = false;
    bool isNoClip = false;
    bool inWater = false;
    bool isClimbing = false;   // inside a `zone ladder`; gravity suspended

    // --- Gravity-zone smoothing (Phase 7.1) ---
    // The vertical integrator falls along the active zone's gravity VECTOR, but
    // a hard swap at a zone boundary teleports the fall. These three fields lerp
    // the effective gravity from the old vector to the new one over ~0.3 s so a
    // gravity-zone entry reads as a push, not a snap. All in ENGINE space (Y-up).
    Vector3 gravityFrom{0.0f, -18.0f, 0.0f};
    Vector3 gravityTarget{0.0f, -18.0f, 0.0f};
    float   gravityBlend = 1.0f;   // 0..1; 1 = fully settled at target
    static constexpr float kGravityLerpSeconds = 0.3f;

    // Effective gravity, smoothstep-eased between gravityFrom and gravityTarget.
    Vector3 EffectiveGravity() const {
        float s = gravityBlend;
        s = s * s * (3.0f - 2.0f * s); // smoothstep
        return { gravityFrom.x + (gravityTarget.x - gravityFrom.x) * s,
                 gravityFrom.y + (gravityTarget.y - gravityFrom.y) * s,
                 gravityFrom.z + (gravityTarget.z - gravityFrom.z) * s };
    }
    // Point the target at a new gravity vector. No-op when it is (practically)
    // the same as the current target, so a stable zone costs nothing per step.
    void RetargetGravity(const Vector3& target) {
        if (fabsf(target.x - gravityTarget.x) < 0.01f &&
            fabsf(target.y - gravityTarget.y) < 0.01f &&
            fabsf(target.z - gravityTarget.z) < 0.01f) return;
        gravityFrom = EffectiveGravity();
        gravityTarget = target;
        gravityBlend = 0.0f;
    }
    void AdvanceGravity(float dt) {
        if (gravityBlend < 1.0f)
            gravityBlend = fminf(1.0f, gravityBlend + dt / kGravityLerpSeconds);
    }

    // --- Gravity ORIENTATION (Phase 7.1b) ---
    // `up` is the axis the player's body, movement basis, ground clamp and camera
    // are built from: the unit opposite of the effective gravity. It is eased on
    // the same 0.3 s budget as the gravity vector so entering an inverted or
    // wall-walking zone reads as a camera roll rather than a snap.
    //
    // In +Y gravity this is exactly {0,1,0}, which is why every consumer below can
    // keep a Y-up fast path and ordinary worlds behave identically.
    Vector3 up{0.0f, 1.0f, 0.0f};

    Vector3 TargetUp() const {
        Vector3 g = EffectiveGravity();
        const float m = sqrtf(g.x * g.x + g.y * g.y + g.z * g.z);
        if (m < 1e-4f) return up;      // zero-g: keep the orientation we have
        return { -g.x / m, -g.y / m, -g.z / m };
    }

    // Normalize-lerp toward TargetUp(). Slerp would need an axis/angle; for unit
    // vectors nlerp traces the same arc to within float error and cannot produce
    // a NaN when the two are antiparallel (slerp's sin(theta) does).
    void AdvanceUp(float dt) {
        const Vector3 target = TargetUp();
        const float k = fminf(1.0f, dt / kGravityLerpSeconds);
        Vector3 n = { up.x + (target.x - up.x) * k,
                      up.y + (target.y - up.y) * k,
                      up.z + (target.z - up.z) * k };
        const float m = Vector3Length(n);
        if (m > 1e-5f) up = { n.x / m, n.y / m, n.z / m };
    }

    // Some unit vector perpendicular to `v`, for the degenerate basis case.
    static Vector3 AnyPerpendicular(const Vector3& v) {
        // Cross with whichever world axis is least parallel, so the result is never
        // near-zero (a zero basis makes BuildFrustum's right vector NaN and the
        // viewport renders BLACK - the documented ResetCamera failure class).
        Vector3 axis = (fabsf(v.y) < 0.9f) ? Vector3{0, 1, 0} : Vector3{1, 0, 0};
        Vector3 p = Vector3CrossProduct(v, axis);
        return Vector3Normalize(p);
    }

    // Yaw-relative forward, projected onto the plane perpendicular to `up`.
    // At +Y gravity this is exactly {sin yaw, 0, cos yaw} - the old basis.
    Vector3 ForwardOnPlane() const {
        Vector3 f = { sinf(Yaw), 0.0f, cosf(Yaw) };
        f = Vector3Subtract(f, Vector3Scale(up, Vector3DotProduct(f, up)));
        const float m = Vector3Length(f);
        if (m < 1e-4f) return AnyPerpendicular(up);   // yaw aimed along up
        return Vector3Scale(f, 1.0f / m);
    }
    // right = cross(forward, up); at +Y this is {-cos yaw, 0, sin yaw}.
    Vector3 RightOnPlane() const {
        return Vector3Normalize(Vector3CrossProduct(ForwardOnPlane(), up));
    }

    // Where the camera looks: yaw on the `up` plane, then pitch about its right
    // axis. At +Y this reproduces the old {cos p sin y, sin p, cos p cos y}.
    Vector3 LookDir() const {
        return Vector3Normalize(Vector3Add(
            Vector3Scale(ForwardOnPlane(), cosf(Pitch)),
            Vector3Scale(up, sinf(Pitch))));
    }

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
            isCrouching = Keybinds::Down(GameAction::Crouch);
            isSprinting = Keybinds::Down(GameAction::Sprint) && !isCrouching;
        } else {
            isSprinting = false;
        }

        float targetEye = isCrouching ? CrouchEyeHeight : StandEyeHeight;
        EyeHeight += (targetEye - EyeHeight) * fminf(1.0f, 10.0f * dt);

        if (inputBlocked) return;

        // Re-sync the authoritative look when the camera's facing changed
        // externally (first frame, teleport, respawn). Vertical drift from the
        // jump/gravity code is tiny, so it stays below the mismatch threshold.
        //
        // Basis-aware: the old comparison built a WORLD-Y look direction, so in a
        // tilted-gravity zone (where the camera is rolled) it never matched and this
        // block re-initialised yaw from the camera EVERY frame - the view fought the
        // mouse. Pitch is measured against `up` and yaw from the projected XZ
        // direction, which keeps the same yaw convention at any orientation.
        cam.up = up;
        Vector3 camDir = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
        if (!LookInit || Vector3DotProduct(camDir, LookDir()) < 0.9f) {
            Vector3 flat = Vector3Subtract(camDir, Vector3Scale(up, Vector3DotProduct(camDir, up)));
            if (Vector3Length(flat) > 1e-4f)
                Yaw = atan2f(flat.x, flat.z);
            Pitch = asinf(fmaxf(-1.0f, fminf(1.0f, Vector3DotProduct(camDir, up))));
            LookInit = true;
        }

        Vector2 md = GetMouseDelta();
        Yaw -= md.x * MouseSensitivity * g_mouseSensitivity;
        Pitch -= md.y * MouseSensitivity * g_mouseSensitivity * (g_invertY ? -1.0f : 1.0f);

        // Gamepad right stick look (raylib's first-person camera provided this)
        float lookX = GetGamepadAxisMovement(0, GAMEPAD_AXIS_RIGHT_X);
        float lookY = GetGamepadAxisMovement(0, GAMEPAD_AXIS_RIGHT_Y);
        if (fabsf(lookX) > 0.15f) Yaw -= lookX * 2.4f * dt;
        if (fabsf(lookY) > 0.15f) Pitch -= lookY * 1.8f * dt;

        if (Pitch > 1.5533f) Pitch = 1.5533f;   // clamp ~89 degrees
        if (Pitch < -1.5533f) Pitch = -1.5533f;

        // --- Move input relative to yaw ---
        float f = 0.0f, r = 0.0f;
        if (Keybinds::Down(GameAction::MoveForward)) f += 1.0f;
        if (Keybinds::Down(GameAction::MoveBack)) f -= 1.0f;
        if (Keybinds::Down(GameAction::MoveRight)) r += 1.0f;
        if (Keybinds::Down(GameAction::MoveLeft)) r -= 1.0f;
        float gx = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_X);
        float gy = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_Y);
        if (fabsf(gx) > 0.15f) r += gx;
        if (fabsf(gy) > 0.15f) f -= gy;

        float len = sqrtf(f * f + r * r);
        if (len > 1.0f) { f /= len; r /= len; }

        float speed = BaseSpeed * (speedScalar > 0.0f ? speedScalar : 1.0f);
        if (isSprinting) speed *= SprintMultiplier;
        if (isCrouching) speed *= CrouchMultiplier;

        // Movement in the plane perpendicular to `up`. Both basis vectors are unit
        // and lie in that plane, so the sum needs no further projection - and at
        // +Y gravity this is bit-for-bit the old {forward*f + right*r, y=0}.
        //
        // Do NOT remap the input axes: yaw still means the same thing to the
        // player, the plane just tilts under them.
        Vector3 move = Vector3Add(Vector3Scale(ForwardOnPlane(), f),
                                  Vector3Scale(RightOnPlane(), r));
        float dist = speed * dt;

        // Split into sub-steps so a sprint step cannot tunnel through a thin
        // brush AABB between the per-frame collision tests.
        int steps = (int)(dist / 0.5f) + 1;
        for (int i = 0; i < steps; i++) {
            float s = dist / (float)steps;
            cam.position = Vector3Add(cam.position, Vector3Scale(move, s));
        }

        // Rebuild the look target from the owned yaw/pitch.
        cam.target = Vector3Add(cam.position, Vector3Scale(LookDir(), LookDistance));
        cam.up = up;
    }

    // Collision box in world space, measured from the FEET upward along `up`.
    //
    // The box deliberately spans [feet, feet + Height] rather than
    // [cam.y - Height, cam.y]: the old anchoring hung most of the box below the
    // feet, where OverlapsObstacle's "top at/below the feet is a floor, not an
    // obstacle" rule discarded it, so the effective body was only EyeHeight tall
    // and Height did nothing. Anchoring to the feet makes Height meaningful and
    // makes ceilings below feet+Height solid. Crouching works because EyeHeight is
    // the SMOOTHED value, so the box ducks as the stance settles.
    //
    // The body is Width across and Height tall, oriented along the gravity up axis,
    // and the query world is AABB-only - so the box is the tightest world-AABB that
    // CONTAINS that oriented body (it over-covers near 90 degrees, never under-covers).
    // Per axis the perpendicular half-extent of a disc of radius w whose normal is
    // `up` is w*sqrt(1 - up_i^2); at +Y gravity that collapses to exactly the old
    // {x +- Width/2, feet..feet+Height, z +- Width/2} box.
    void UpdateBounds(Camera3D& cam) {
        const float w = Width * 0.5f;
        const Vector3 feet = Vector3Subtract(cam.position, Vector3Scale(up, EyeHeight));
        const Vector3 head = Vector3Add(feet, Vector3Scale(up, Height));
        const Vector3 half = {
            w * sqrtf(fmaxf(0.0f, 1.0f - up.x * up.x)),
            w * sqrtf(fmaxf(0.0f, 1.0f - up.y * up.y)),
            w * sqrtf(fmaxf(0.0f, 1.0f - up.z * up.z)) };
        PlayerBounds = (BoundingBox){
            (Vector3){fminf(feet.x, head.x) - half.x,
                      fminf(feet.y, head.y) - half.y,
                      fminf(feet.z, head.z) - half.z},
            (Vector3){fmaxf(feet.x, head.x) + half.x,
                      fmaxf(feet.y, head.y) + half.y,
                      fmaxf(feet.z, head.z) + half.z}};
    }
};
