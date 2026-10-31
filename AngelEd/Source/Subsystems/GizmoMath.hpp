#pragma once
// ---------------------------------------------------------------------------
// GizmoMath.hpp - pure geometry for the editor transform gizmo.
//
// Raylib-free and Win32-free on purpose, so tests/GizmoMath.test.cpp can pin the
// parts that are easy to get subtly wrong:
//   * closest parameter on an axis line to a mouse ray,
//   * ray/plane intersection,
//   * grid and angle snapping.
//
// These were previously inline in Main.cpp's frame loop in screen-space form
// (X += dx/8), which is why there was no axis constraint and no test.
// ---------------------------------------------------------------------------

#include <cmath>

namespace ozg {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

inline Vec3 MakeVec3(float x, float y, float z) { return Vec3{x, y, z}; }
inline Vec3 Add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 Mul(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float LengthSq(const Vec3& a) { return Dot(a, a); }
inline float Length(const Vec3& a) { return std::sqrt(LengthSq(a)); }
inline Vec3 Normalize(const Vec3& a) {
    const float l = Length(a);
    return l > 1e-8f ? Mul(a, 1.0f / l) : Vec3{0, 0, 0};
}

// Parameter t on the infinite line P + t*axis that is closest to the ray
// R + s*dir. Returns false when the line and the ray are (nearly) parallel,
// where "closest point" is undefined and the caller should fall back to a
// screen-plane drag instead of producing a wild jump.
inline bool RayAxisParameter(const Vec3& rayOrigin, const Vec3& rayDir,
                             const Vec3& p, const Vec3& axis, float& outT) {
    const float a = Dot(axis, axis);
    const float b = Dot(axis, rayDir);
    const float c = Dot(rayDir, rayDir);
    const float denom = a * c - b * b;
    if (std::fabs(denom) < 1e-6f) return false;
    const Vec3 w = Sub(rayOrigin, p);
    const float d = Dot(axis, w);
    const float e = Dot(rayDir, w);
    outT = (c * d - b * e) / denom;
    return true;
}

// Intersection of the ray with the infinite plane through `point` with normal
// `normal`. Returns false when the ray is parallel to the plane.
inline bool RayPlane(const Vec3& rayOrigin, const Vec3& rayDir,
                     const Vec3& point, const Vec3& normal, Vec3& outHit) {
    const float denom = Dot(normal, rayDir);
    if (std::fabs(denom) < 1e-6f) return false;
    const float t = Dot(normal, Sub(point, rayOrigin)) / denom;
    if (t < 0) return false;
    outHit = Add(rayOrigin, Mul(rayDir, t));
    return true;
}

// Round `v` to the nearest multiple of `step`. step <= 0 disables snapping.
inline float SnapValue(float v, float step) {
    if (step <= 0.0f) return v;
    return std::round(v / step) * step;
}

// Round an angle in degrees. Keeps the sign (a negative rotation stays negative)
// so cancelling a drag back to zero is exact.
inline float SnapAngle(float deg, float step) {
    if (step <= 0.0f) return deg;
    return std::round(deg / step) * step;
}

// Wrap to (-180, 180] for display/HUD use.
inline float WrapDeg180(float deg) {
    while (deg > 180.0f) deg -= 360.0f;
    while (deg <= -180.0f) deg += 360.0f;
    return deg;
}

// ---------------------------------------------------------------------------
// Camera basis and pixels-to-world scale
// ---------------------------------------------------------------------------

// Orthonormal basis from a forward vector and a world up:
//   right = normalize(cross(forward, up)), up' = normalize(cross(right, forward))
// - the same composition Core/ViewportRay.hpp builds its picking ray from, so a
// pan moves the view along exactly the axes the pointer ray is defined in.
// Returns false when forward and up are (nearly) parallel: `right` is undefined
// there, and it is the same degeneracy that makes a top-down camera with up = +Y
// render black.
inline bool ViewBasis(const Vec3& forward, const Vec3& worldUp, Vec3& outRight, Vec3& outUp) {
    const Vec3 r = Cross(forward, worldUp);
    if (LengthSq(r) < 1e-8f) { outRight = Vec3{}; outUp = Vec3{}; return false; }
    outRight = Normalize(r);
    outUp = Normalize(Cross(outRight, forward));
    return true;
}

// World units covered by one vertical screen pixel at `distance` from the eye,
// for a vertical FOV of `fovyDeg` rendered over `viewportHeight` pixels.
//
// This is the factor that makes a drag 1:1 with the cursor at any zoom. A fixed
// pixels->units constant (which both the camera pan and the Unreal drag used) is
// unusable far out and violent up close, and UnrealEd scaled its ortho pan and
// zoom by OrthoZoom for exactly the same reason.
inline float UnitsPerPixel(float fovyDeg, float viewportHeight, float distance) {
    if (viewportHeight < 1.0f) viewportHeight = 1.0f;
    const float halfV = std::tan(fovyDeg * 0.5f * 0.017453292519943295f);
    return 2.0f * halfV * distance / viewportHeight;
}

// ---------------------------------------------------------------------------
// Snapping that keeps the selection on its own grid phase
// ---------------------------------------------------------------------------

// Round `v` to a `step` grid ANCHORED at `anchor` rather than at zero.
//
// SnapValue() rounds to absolute multiples, so an object sitting at 0.37 jumped
// to 0.5 the instant a 0.5-grid drag began - the first pixel of a drag teleported
// it, and with snapping on by default that read as "the move tool is broken".
// UnrealEd anchors snapping to the pivot captured when the drag starts:
// `GGridBase = GPivotLocation - GSnappedLocation` in SetPivot (UnEdCam.cpp) is
// reused by every MoveActors call, so the object never jumps and each increment
// is a whole grid step. That is what this reproduces.
inline float SnapAround(float v, float anchor, float step) {
    if (step <= 0.0f) return v;
    return anchor + std::round((v - anchor) / step) * step;
}

// ---------------------------------------------------------------------------
// UnrealEd (1998) movement mappings - Source/Editor/Src/UnEdCam.cpp
// ---------------------------------------------------------------------------

struct Move3 {
    float x = 0, y = 0, z = 0;
};

// CalcAxialMoveRot - the perspective ACTOR-move gesture. Unreal's exact mapping:
//   LMB          -> +X
//   RMB          -> +Y
//   LMB + RMB    -> -MouseY on Z   (screen up is negative, so dragging up rises)
// This engine is Y-up while Unreal (and OZONE) are Z-up, so Unreal's Y becomes Z
// and Unreal's Z becomes Y. The result is a world-space delta: multiply it by the
// pixels-to-world factor yourself. Unreal's if-chain puts the both-buttons case
// last, but with both down the first two tests fail, so "both wins" is exact.
inline Move3 UnrealAxialMove(float mouseDx, float mouseDy, bool lmb, bool rmb) {
    Move3 d;
    if (lmb && rmb)  d.y = -mouseDy;   // Unreal's Z: up/down
    else if (lmb)    d.x = mouseDx;    // Unreal's X
    else if (rmb)    d.z = mouseDx;    // Unreal's Y
    return d;
}

// CalcAxialRot's yaw leg (RMB -> +MouseX). Only yaw is storable here: OZONE keeps
// one rotation per renderable/mesh, so pitch and roll would be controls that write
// nothing. Unreal's literal `*4` is in its 65536-units-per-turn space; degrees per
// pixel is the equivalent that survives the translation.
inline float UnrealAxialYaw(float mouseDx) { return mouseDx * 0.4f; }

// UnrealEd's MovementSpeed (keys 1/2/3 set 1/4/16, and the actor branch multiplies
// by 0.25). Keeping the same 0.25x / 1x / 4x ratio is what makes the speed keys
// mean the same thing they did there.
inline float UnrealSpeedScale(int speedIndex) {
    switch (speedIndex) {
        case 0:  return 0.25f;
        case 2:  return 4.0f;
        default: return 1.0f;
    }
}

} // namespace ozg
