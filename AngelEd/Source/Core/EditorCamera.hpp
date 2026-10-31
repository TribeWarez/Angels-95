#pragma once
// ============================================================================
// Core/EditorCamera.hpp - the ONE definition of the editor's camera gestures.
//
// These used to be hand-copied between Main.cpp's viewport and
// Core/DetachedPane.cpp's pop-out, and BOTH copies panned along WORLD X/Z
// instead of the camera's own axes. That is only correct for the default 3/4
// view: look down +X and dragging the mouse up/down panned along Z, i.e. sideways
// on screen, while dragging left/right panned along X, i.e. toward and away from
// the viewer. Two copies also meant two places to fix, which is the same shape as
// the fogFactorAt drift.
//
// Reference: UnrealEd 1998, Source/Editor/Src/UnEdCam.cpp.
//   * CalcFreeMoveRot (perspective) moves the camera along its YAW basis
//     (forward/right) plus world Z, and ViewportMoveRotWithPhysics applies the
//     position delta unchanged.
//   * Both movement branches scale by MovementSpeed - never by a fixed
//     pixels->units constant.
//   * Ortho pans by `OrthoZoom/30000` and zooms by `OrthoZoom/200`, i.e. both are
//     proportional to the current zoom.
// Translated to this Y-up engine:
//   * pan = camera right/up, scaled by the world size of one pixel at the focus
//     depth;
//   * orbit is angular and therefore already zoom-independent;
//   * dolly is proportional to the focus distance, and in ORTHO it scales the
//     world height instead of translating, because translating an orthographic
//     camera along its view axis has no visible effect at all (which is why the
//     wheel was dead in every Top/Side view).
//
// Header-only on purpose: a .cpp would mean wiring AngelEd/Makefile AND ci.yml's
// inline g++ list.
// ============================================================================

#include <cmath>

#include "raylib.h"
#include "raymath.h"

#include "../Subsystems/GizmoMath.hpp"

namespace edcam {

inline ozg::Vec3 Oz(Vector3 v) { return {v.x, v.y, v.z}; }
inline Vector3   Rl(const ozg::Vec3& v) { return {v.x, v.y, v.z}; }

inline ozg::Vec3 Forward(const Camera3D& cam) {
    return ozg::Normalize(ozg::Sub(Oz(cam.target), Oz(cam.position)));
}

// The depth the gestures scale against: the target is what the user is looking
// at, so it is the natural "thing I am dragging" plane. A degenerate camera falls
// back to one unit so no gesture can divide by zero.
inline float FocusDistance(const Camera3D& cam) {
    const float d = Vector3Distance(cam.position, cam.target);
    return d > 1e-3f ? d : 1.0f;
}

inline float UnitsPerPixel(const Camera3D& cam, float distance, float viewportHeight) {
    if (viewportHeight < 1.0f) viewportHeight = 1.0f;
    if (cam.projection == CAMERA_ORTHOGRAPHIC) {
        // raylib treats `fovy` as the world-space HEIGHT of an ortho view
        // (Core/ViewportRay.hpp builds MatrixOrtho from it the same way), so the
        // world extent of one pixel is simply height / pixels.
        return cam.fovy / viewportHeight;
    }
    return ozg::UnitsPerPixel(cam.fovy, viewportHeight, distance);
}

// Move eye and target together, so the view direction never changes.
inline void Translate(Camera3D& cam, const ozg::Vec3& d) {
    cam.position = Rl(ozg::Add(Oz(cam.position), d));
    cam.target   = Rl(ozg::Add(Oz(cam.target), d));
}

// Middle-drag pan. The content follows the cursor - the convention this viewport
// already had and the one every modern editor uses (UnrealEd moves the CAMERA
// with the mouse instead). Only the AXES and the SCALE change here: screen-right
// and screen-up, one pixel of drag = one pixel of world at the target's depth.
inline void Pan(Camera3D& cam, float dx, float dy, float viewportHeight) {
    ozg::Vec3 right, up;
    if (!ozg::ViewBasis(Forward(cam), Oz(cam.up), right, up)) return;
    const float s = UnitsPerPixel(cam, FocusDistance(cam), viewportHeight);
    Translate(cam, ozg::Add(ozg::Mul(right, -dx * s), ozg::Mul(up, dy * s)));
}

// Shift + middle-drag raises/lowers along WORLD up, so the horizon never tilts.
// UnrealEd's vertical leg is world-Z for the same reason (Delta.Z = -MouseY).
// The sign matches Pan: dragging down moves the content down.
inline void PanVertical(Camera3D& cam, float dy, float viewportHeight) {
    const float s = UnitsPerPixel(cam, FocusDistance(cam), viewportHeight);
    Translate(cam, {0.0f, dy * s, 0.0f});
}

// Alt + middle-drag orbits the target. Angular, so it needs no zoom scaling.
// The pitch clamp keeps the offset off the up axis, where the view basis (and the
// picking ray's `cross(forward, up)`) collapses.
inline void Orbit(Camera3D& cam, float dx, float dy) {
    ozg::Vec3 v = ozg::Sub(Oz(cam.position), Oz(cam.target));
    const float radius = ozg::Length(v);
    if (radius <= 0.01f) return;
    float yaw   = std::atan2(v.x, v.z);
    float pitch = std::asin(v.y / radius);
    const float limit = 1.5707963f - 0.01f;
    yaw   -= dx * 0.01f;
    pitch += dy * 0.01f;
    if (pitch >  limit) pitch =  limit;
    if (pitch < -limit) pitch = -limit;
    v.x = radius * std::cos(pitch) * std::sin(yaw);
    v.y = radius * std::sin(pitch);
    v.z = radius * std::cos(pitch) * std::cos(yaw);
    cam.position = Rl(ozg::Add(Oz(cam.target), v));
}

// Wheel dolly.
//   Perspective: proportional to the focus distance, so one notch is the same
//   FRACTION of the view at every zoom (UnrealEd: OrthoZoom/200 per mouse unit).
//   Ortho: scale the world height, because translating along the view axis has no
//   visible effect without foreshortening. The wheel used to do nothing at all in
//   a Top/Side/Down view.
inline void Dolly(Camera3D& cam, float notches) {
    if (notches == 0.0f) return;
    if (cam.projection == CAMERA_ORTHOGRAPHIC) {
        cam.fovy *= std::pow(0.88f, notches);
        if (cam.fovy < 1.0f)    cam.fovy = 1.0f;
        if (cam.fovy > 4000.0f) cam.fovy = 4000.0f;
        return;
    }
    Translate(cam, ozg::Mul(Forward(cam), FocusDistance(cam) * 0.12f * notches));
}

// PageUp / PageDown. Moves the WHOLE camera: writing position.y alone re-aims the
// view at the old target, so "move up" pitched the camera down instead of
// raising it. UnrealEd's PageUp/PageDown translate the view actor.
inline void MoveVertical(Camera3D& cam, float amount) {
    Translate(cam, {0.0f, amount, 0.0f});
}

} // namespace edcam
