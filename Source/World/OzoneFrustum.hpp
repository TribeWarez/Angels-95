#pragma once

// Frustum culling helpers (public raylib API only - no rlgl internals).
// Extracted from OzOzoneLoader.cpp so the world loader stays focused on IO.

#include "raylib.h"

struct FrustumPlane {
    Vector3 normal;
    float d;
};

// Build the 6 frustum planes from camera view geometry. Matches raylib's
// BeginMode3D defaults (near 0.01, far 1000.0). Normals point inward so
// interior points satisfy dot(normal, p) + d > 0.
//
// `aspectOverride` exists for the editor's split panes: each pane is rendered
// with its own projection aspect, while GetScreenWidth/Height describes the
// whole window. Using the window aspect for a 2x1 pane builds a frustum that is
// too NARROW and culls geometry the pane can actually see.
void BuildFrustum(const Camera3D& camera, FrustumPlane out[6],
                  float aspectOverride = 0.0f);

// AABB vs plane: false when the whole box sits behind the plane.
bool BoxVsPlane(const FrustumPlane& p, const BoundingBox& b);

bool AabbInFrustum(const FrustumPlane planes[6], const BoundingBox& b);
