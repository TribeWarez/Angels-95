#include "OzoneFrustum.hpp"

#include "raymath.h"
#include <cmath>

// Build the 6 frustum planes from camera view geometry. Matches raylib's
// BeginMode3D defaults (near 0.01, far 1000.0). Normals point inward so
// interior points satisfy dot(normal, p) + d > 0.
void BuildFrustum(const Camera3D& camera, FrustumPlane out[6], float aspectOverride) {
    Vector3 pos = camera.position;
    Vector3 forward = Vector3Normalize(Vector3Subtract(camera.target, pos));
    Vector3 right = Vector3Normalize(Vector3CrossProduct(forward, camera.up));
    Vector3 up = Vector3CrossProduct(right, forward);

    float halfV = tanf(camera.fovy * 0.5f * DEG2RAD);
    float aspect = (aspectOverride > 0.0f)
                       ? aspectOverride
                       : (float)GetScreenWidth() /
                             (float)(GetScreenHeight() > 0 ? GetScreenHeight() : 1);
    float halfH = halfV * aspect;

    const float nearPlane = 0.01f;
    const float farPlane = 1000.0f;

    const Vector3 nc = Vector3Add(pos, Vector3Scale(forward, nearPlane));
    const Vector3 fc = Vector3Add(pos, Vector3Scale(forward, farPlane));
    const Vector3 inner = Vector3Scale(Vector3Add(nc, fc), 0.5f); // strictly-inside point

    // FOUR corners per face. The old code built every side plane from the two
    // DIAGONAL corners (`nc - right - up` and `nc + right + up`). Those points
    // are collinear with the face centre, so the near/far plane normals came out
    // zero, and the left/right and top/bottom planes were duplicates of one
    // diagonal edge rather than the four real sides. Culling was dead for years
    // (hasBounds was never set), so the broken planes were invisible; the moment
    // culling started working, whole visible walls were rejected by one of the
    // two bogus planes. Keep all eight corners.
    Vector3 nBL = Vector3Add(nc, Vector3Add(Vector3Scale(right, -halfH * nearPlane),
                                           Vector3Scale(up, -halfV * nearPlane)));
    Vector3 nBR = Vector3Add(nc, Vector3Add(Vector3Scale(right,  halfH * nearPlane),
                                           Vector3Scale(up, -halfV * nearPlane)));
    Vector3 nTL = Vector3Add(nc, Vector3Add(Vector3Scale(right, -halfH * nearPlane),
                                           Vector3Scale(up,  halfV * nearPlane)));
    Vector3 nTR = Vector3Add(nc, Vector3Add(Vector3Scale(right,  halfH * nearPlane),
                                           Vector3Scale(up,  halfV * nearPlane)));
    Vector3 fBL = Vector3Add(fc, Vector3Add(Vector3Scale(right, -halfH * farPlane),
                                           Vector3Scale(up, -halfV * farPlane)));
    Vector3 fBR = Vector3Add(fc, Vector3Add(Vector3Scale(right,  halfH * farPlane),
                                           Vector3Scale(up, -halfV * farPlane)));
    Vector3 fTL = Vector3Add(fc, Vector3Add(Vector3Scale(right, -halfH * farPlane),
                                           Vector3Scale(up,  halfV * farPlane)));
    Vector3 fTR = Vector3Add(fc, Vector3Add(Vector3Scale(right,  halfH * farPlane),
                                           Vector3Scale(up,  halfV * farPlane)));

    // Plane from three points, oriented toward `inner`.
    auto plane_from_points = [inner](Vector3 a, Vector3 b_, Vector3 c) {
        FrustumPlane p;
        p.normal = Vector3Normalize(Vector3CrossProduct(
            Vector3Subtract(b_, a), Vector3Subtract(c, a)));
        p.d = -Vector3DotProduct(p.normal, a);
        if (Vector3DotProduct(p.normal, inner) + p.d < 0.0f) {
            p.normal = Vector3Negate(p.normal);
            p.d = -p.d;
        }
        return p;
    };
    out[0] = plane_from_points(nBL, fBL, fTL); // left
    out[1] = plane_from_points(nBR, fBR, fTR); // right
    out[2] = plane_from_points(nBL, fBL, fBR); // bottom
    out[3] = plane_from_points(nTL, fTL, fTR); // top
    out[4] = plane_from_points(nBL, nBR, nTR); // near
    out[5] = plane_from_points(fBL, fBR, fTR); // far
}

// AABB vs plane: false when the whole box sits behind the plane.
bool BoxVsPlane(const FrustumPlane& p, const BoundingBox& b) {
    Vector3 corner = {
        (p.normal.x >= 0.0f) ? b.max.x : b.min.x,
        (p.normal.y >= 0.0f) ? b.max.y : b.min.y,
        (p.normal.z >= 0.0f) ? b.max.z : b.min.z
    };
    return Vector3DotProduct(p.normal, corner) + p.d >= 0.0f;
}

bool AabbInFrustum(const FrustumPlane planes[6], const BoundingBox& b) {
    for (int i = 0; i < 6; ++i)
        if (!BoxVsPlane(planes[i], b)) return false;
    return true;
}
