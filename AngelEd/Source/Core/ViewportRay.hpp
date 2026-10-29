#pragma once
// ============================================================================
// Core/ViewportRay.hpp — pane-aware picking math (raylib side)
//
// Phase 2 of multiple viewports. raylib's GetMouseRay/GetWorldToScreen map the
// mouse over the WHOLE window, so a pane that is a sub-rectangle gets the wrong
// ray (and the gizmo handles project to the wrong screen position). These helpers
// apply the same projection SetPaneProjection uses - fovy + the PANE aspect - and
// map NDC into the pane's screen rect.
//
// Viewport.hpp itself stays raylib-free so the layout math remains headless-testable;
// this header is the raylib bridge and is included only by the editor.
// ============================================================================

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include "Viewport.hpp"

// The pane that currently owns input, set once per frame in Main.cpp. The gizmo
// (a fragment) reads it so its screen-space hit tests match the active pane.
inline ed::viewport::VpRect g_activeViewportRect;

inline Matrix ViewportProjMatrix(const Camera3D& cam, const ed::viewport::VpRect& r) {
    const double aspect = (r.h > 0.0f) ? (double)r.w / (double)r.h : 1.0;
    if (cam.projection == CAMERA_PERSPECTIVE) {
        return MatrixPerspective((double)cam.fovy * DEG2RAD, aspect,
                                 RL_CULL_DISTANCE_NEAR, RL_CULL_DISTANCE_FAR);
    }
    const double top = (double)cam.fovy * 0.5;
    const double right = top * aspect;
    return MatrixOrtho(-right, right, -top, top, RL_CULL_DISTANCE_NEAR, RL_CULL_DISTANCE_FAR);
}

// A ray through the mouse inside `r`, matching SetPaneProjection's frustum.
inline Ray RayFromPane(Vector2 mp, const Camera3D& cam, const ed::viewport::VpRect& r) {
    const float nx = (r.w > 0.0f) ? ((mp.x - r.x) / r.w) * 2.0f - 1.0f : 0.0f;
    const float ny = (r.h > 0.0f) ? 1.0f - ((mp.y - r.y) / r.h) * 2.0f : 0.0f;
    const float aspect = (r.h > 0.0f) ? r.w / r.h : 1.0f;

    const Vector3 forward = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
    const Vector3 right = Vector3Normalize(Vector3CrossProduct(forward, cam.up));
    const Vector3 up = Vector3Normalize(Vector3CrossProduct(right, forward));

    if (cam.projection == CAMERA_PERSPECTIVE) {
        const float halfV = tanf(cam.fovy * 0.5f * DEG2RAD);
        const float halfH = halfV * aspect;
        const Vector3 dir = Vector3Add(forward,
            Vector3Add(Vector3Scale(right, nx * halfH), Vector3Scale(up, ny * halfV)));
        return Ray{ cam.position, Vector3Normalize(dir) };
    }
    const float halfV = cam.fovy * 0.5f;
    const float halfH = halfV * aspect;
    const Vector3 origin = Vector3Add(cam.position,
        Vector3Add(Vector3Scale(right, nx * halfH), Vector3Scale(up, ny * halfV)));
    return Ray{ origin, forward };
}

// World -> screen INSIDE the pane, so a gizmo handle projects where it is drawn.
inline Vector2 WorldToScreenInRect(Vector3 world, const Camera3D& cam, const ed::viewport::VpRect& r) {
    const Matrix view = MatrixLookAt(cam.position, cam.target, cam.up);
    const Matrix proj = ViewportProjMatrix(cam, r);
    const Vector3 ndc = Vector3Transform(world, MatrixMultiply(view, proj));
    return { r.x + (ndc.x + 1.0f) * 0.5f * r.w, r.y + (1.0f - ndc.y) * 0.5f * r.h };
}
