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

} // namespace ozg
