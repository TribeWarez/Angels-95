// ============================================================================
// Subsystems/Gizmo.cpp
//
// FRAGMENT - included by Main.cpp. Implements the viewport transform gizmo:
// axis/plane/centre move, Y-rotation ring, uniform scale, snapping, hover and
// drag-cancel.
//
// Why this exists: the old Move tool added the raw mouse delta to the gizmo
// (X += dx / 8, Y += dy / 8, Z from the wheel). That is screen-space, not
// world-space, so the entity did not follow the cursor; rotation and scale were
// wheel-only; and there were no handles to grab, so the only feedback was the
// red selection box. The frame loop already publishes the gizmo transform into
// the live selection every frame (Main.cpp), so this file only ever writes
// AngelTechEditor - never the entity - which keeps ONE write path.
// ============================================================================

#include "GizmoMath.hpp"
#include "../Core/EditorCamera.hpp"

enum class GizmoHandle {
    None = 0,
    AxisX, AxisY, AxisZ,
    PlaneXY, PlaneYZ, PlaneXZ,
    Screen,
    RotateY,
};

static GizmoHandle g_gizmoDragHandle = GizmoHandle::None;
static GizmoHandle g_gizmoHoverHandle = GizmoHandle::None;

static ozg::Vec3 g_gizmoStartPivot;
static float g_gizmoStartX = 0, g_gizmoStartY = 0, g_gizmoStartZ = 0;
static float g_gizmoStartR = 0, g_gizmoStartS = 1;
static float g_gizmoStartParam = 0;
// Screen-space fallback accumulators. An axis that points (nearly) at the camera
// has no closest-point parameter, and an edge-on plane has no ray hit; both used to
// make the handle silently do nothing. These accumulate the pixel delta projected
// onto the handle's screen direction instead.
static float  g_gizmoScreenAccum = 0.0f;
static ozg::Vec3 g_gizmoScreenAccumVec;
static ozg::Vec3 g_gizmoStartHit;
static float g_gizmoStartAngle = 0;

static ozg::Vec3 ToOz(Vector3 v) { return {v.x, v.y, v.z}; }
static Vector3 ToRl(ozg::Vec3 v) { return {v.x, v.y, v.z}; }

static bool Gizmo_SelectionExists() {
    return g_sel.type != SelType::NONE && g_sel.type != SelType::MAP;
}

static bool Gizmo_TypeCanScale() {
    return g_sel.type == SelType::BRUSH || g_sel.type == SelType::MESH ||
           g_sel.type == SelType::MODEL;
}

static bool Gizmo_TypeCanRotate() {
    // Yaw is the only rotation OZONE stores, and only these types write it back
    // in the frame sync. Hiding the ring elsewhere is honest: dragging a ring that
    // changes nothing reads as a broken tool.
    return g_sel.type == SelType::BRUSH || g_sel.type == SelType::MESH ||
           g_sel.type == SelType::MODEL || g_sel.type == SelType::PARTICLE;
}

static Vector3 Gizmo_Pivot() { return {AngelTechEditor.X, AngelTechEditor.Y, AngelTechEditor.Z}; }

// Screen-constant handle length: world size grows with camera distance so the
// gizmo stays the same apparent size while zooming.
static float Gizmo_Length(const Camera3D& cam, Vector3 pivot) {
    const float d = Vector3Distance(cam.position, pivot);
    float len = d * 0.16f;
    if (len < 2.0f) len = 2.0f;
    if (len > 24.0f) len = 24.0f;
    return len;
}

static float Gizmo_SnapStep() {
    // The toolbar step is the authority; held keys MODIFY it for the duration of
    // the gesture instead of replacing it. Ctrl used to force 1.0 and the toolbar
    // defaulted to Off, so a configured 0.25 became 1.0 while held and nothing at
    // all otherwise - which read as "snap only works occasionally".
    //   Shift = fine  (a tenth of the step, or 0.1 when snapping is off)
    //   Ctrl  = hold-to-snap at the configured step (0.5 when the toolbar is Off)
    const bool ctrl  = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    const bool shift = IsKeyDown(KEY_LEFT_SHIFT)   || IsKeyDown(KEY_RIGHT_SHIFT);
    const float base = g_editorPanels.gizmoSnap;
    if (shift) return base > 0.0f ? base * 0.1f : 0.1f;
    if (ctrl)  return base > 0.0f ? base : 0.5f;
    return base;
}

static float Gizmo_AngleStep() {
    if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) return 1.0f;
    return g_editorPanels.gizmoAngleSnap > 0.0f ? g_editorPanels.gizmoAngleSnap : 0.0f;
}

static float DistToSegment2D(Vector2 p, Vector2 a, Vector2 b) {
    const float vx = b.x - a.x, vy = b.y - a.y;
    const float wx = p.x - a.x, wy = p.y - a.y;
    const float len2 = vx * vx + vy * vy;
    float t = len2 > 1e-6f ? (wx * vx + wy * vy) / len2 : 0.0f;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    const float dx = p.x - (a.x + vx * t);
    const float dy = p.y - (a.y + vy * t);
    return sqrtf(dx * dx + dy * dy);
}

static Vector3 AxisVector(GizmoHandle h) {
    switch (h) {
        case GizmoHandle::AxisX: return {1, 0, 0};
        case GizmoHandle::AxisY: return {0, 1, 0};
        case GizmoHandle::AxisZ: return {0, 0, 1};
        default: return {0, 0, 0};
    }
}

static bool IsAxis(GizmoHandle h) {
    return h == GizmoHandle::AxisX || h == GizmoHandle::AxisY || h == GizmoHandle::AxisZ;
}
static bool IsPlane(GizmoHandle h) {
    return h == GizmoHandle::PlaneXY || h == GizmoHandle::PlaneYZ || h == GizmoHandle::PlaneXZ;
}

static Vector3 PlaneNormal(GizmoHandle h) {
    switch (h) {
        case GizmoHandle::PlaneXY: return {0, 0, 1};
        case GizmoHandle::PlaneYZ: return {1, 0, 0};
        case GizmoHandle::PlaneXZ: return {0, 1, 0};
        default: return {0, 0, 0};
    }
}

static Vector3 PlaneCenter(Vector3 pivot, GizmoHandle h, float len) {
    switch (h) {
        case GizmoHandle::PlaneXY: return {pivot.x + len * 0.30f, pivot.y + len * 0.30f, pivot.z};
        case GizmoHandle::PlaneYZ: return {pivot.x, pivot.y + len * 0.30f, pivot.z + len * 0.30f};
        case GizmoHandle::PlaneXZ: return {pivot.x + len * 0.30f, pivot.y, pivot.z + len * 0.30f};
        default: return pivot;
    }
}

// ---------------------------------------------------------------- hit testing
static GizmoHandle Gizmo_Pick(Vector2 mouse, const Camera3D& cam, int toolMode) {
    if (!Gizmo_SelectionExists() || toolMode == 0) return GizmoHandle::None;
    if (toolMode == 2 && !Gizmo_TypeCanScale()) return GizmoHandle::None;
    if (toolMode == 3 && !Gizmo_TypeCanRotate()) return GizmoHandle::None;
    const Vector3 pivot = Gizmo_Pivot();
    const float len = Gizmo_Length(cam, pivot);

    if (toolMode == 3) {
        // Rotation ring in the XZ plane. Screen-space distance to the projected
        // polyline is far more forgiving than a 3D circle test.
        GizmoHandle best = GizmoHandle::None;
        // 10px, not 8: the ring radius is clamped to a 2-unit minimum, so at a
        // distance (or in an ortho side view, where it projects to a line) an 8px
        // band is a very small target.
        float bestDist = 10.0f;
        const float r = len * 0.85f;
        Vector2 prev = {};
        for (int i = 0; i <= 64; i++) {
            const float a = (float)i / 64.0f * 2.0f * PI;
            Vector3 wp = {pivot.x + cosf(a) * r, pivot.y, pivot.z + sinf(a) * r};
            Vector2 sp = WorldToScreenInRect(wp, cam, g_activeViewportRect);
            if (i > 0) {
                const float d = DistToSegment2D(mouse, prev, sp);
                if (d < bestDist) { bestDist = d; best = GizmoHandle::RotateY; }
            }
            prev = sp;
        }
        if (best != GizmoHandle::None) return best;

        // Inner disc: Rotate must never be a handle you cannot find. The ring is
        // the precise target; anything inside it starts a yaw too (nothing else in
        // Rotate mode is pickable, so this steals no other handle).
        const Vector2 c = WorldToScreenInRect(pivot, cam, g_activeViewportRect);
        const float rc = sqrtf((mouse.x - c.x) * (mouse.x - c.x) +
                               (mouse.y - c.y) * (mouse.y - c.y));
        const Vector2 edge = WorldToScreenInRect(
            {pivot.x + r, pivot.y, pivot.z}, cam, g_activeViewportRect);
        const float rPix = sqrtf((edge.x - c.x) * (edge.x - c.x) +
                                 (edge.y - c.y) * (edge.y - c.y));
        if (rPix >= 8.0f && rc <= rPix) return GizmoHandle::RotateY;
        return GizmoHandle::None;
    }

    const Vector2 sp = WorldToScreenInRect(pivot, cam, g_activeViewportRect);
    // Centre handle wins ties: it is the smallest target.
    const float dc = sqrtf((mouse.x - sp.x) * (mouse.x - sp.x) + (mouse.y - sp.y) * (mouse.y - sp.y));
    if (dc < 9.0f) return GizmoHandle::Screen;

    // Axis shafts + tips.
    GizmoHandle best = GizmoHandle::None;
    float bestDist = 8.0f;
    const GizmoHandle axes[3] = {GizmoHandle::AxisX, GizmoHandle::AxisY, GizmoHandle::AxisZ};
    for (GizmoHandle h : axes) {
        const Vector3 tip = Vector3Add(pivot, Vector3Scale(AxisVector(h), len));
        const Vector2 tp = WorldToScreenInRect(tip, cam, g_activeViewportRect);
        // Skip an axis whose shaft is (nearly) parallel to the view direction:
        // it projects to a few pixels at the pivot, so it is both unaimable and
        // wins the distance test against the axis the user is actually pointing
        // at. That was the "moving only pulls one axis, weirdly" report.
        const float sdx = tp.x - sp.x, sdy = tp.y - sp.y;
        if (sqrtf(sdx * sdx + sdy * sdy) < 12.0f) continue;
        const float d = DistToSegment2D(mouse, sp, tp);
        if (d < bestDist) { bestDist = d; best = h; }
    }
    if (best != GizmoHandle::None) return best;

    // Plane quads (move mode only).
    if (toolMode == 1) {
        const GizmoHandle planes[3] = {GizmoHandle::PlaneXY, GizmoHandle::PlaneYZ, GizmoHandle::PlaneXZ};
        for (GizmoHandle h : planes) {
            const Vector2 pc = WorldToScreenInRect(PlaneCenter(pivot, h, len), cam, g_activeViewportRect);
            const float d = sqrtf((mouse.x - pc.x) * (mouse.x - pc.x) + (mouse.y - pc.y) * (mouse.y - pc.y));
            if (d < 9.0f) return h;
        }
    }
    return GizmoHandle::None;
}

void Gizmo_UpdateHover(Vector2 mouse, const Camera3D& cam, int toolMode) {
    g_gizmoHoverHandle = Gizmo_Pick(mouse, cam, toolMode);
}

bool Gizmo_IsDragging() { return g_gizmoDragHandle != GizmoHandle::None; }
bool Gizmo_HasHover() { return g_gizmoHoverHandle != GizmoHandle::None; }

// ---------------------------------------------------------------- drag control
bool Gizmo_BeginDrag(Vector2 mouse, const Camera3D& cam, int toolMode) {
    if (Gizmo_IsDragging()) return true;
    const GizmoHandle h = Gizmo_Pick(mouse, cam, toolMode);
    if (h == GizmoHandle::None) return false;

    const Vector3 pivot = Gizmo_Pivot();
    const Ray ray = RayFromPane(mouse, cam, g_activeViewportRect);
    const ozg::Vec3 ro = ToOz(ray.position);
    const ozg::Vec3 rd = ozg::Normalize(ToOz(ray.direction));
    const ozg::Vec3 p = ToOz(pivot);

    g_gizmoStartPivot = p;
    g_gizmoStartX = AngelTechEditor.X;
    g_gizmoStartY = AngelTechEditor.Y;
    g_gizmoStartZ = AngelTechEditor.Z;
    g_gizmoStartR = AngelTechEditor.R;
    g_gizmoStartS = AngelTechEditor.S;
    g_gizmoScreenAccum = 0.0f;
    g_gizmoScreenAccumVec = {};

    if (IsAxis(h)) {
        // A failure here is NOT fatal any more: Gizmo_UpdateDrag falls back to the
        // handle's screen direction when the axis points at the camera.
        if (!ozg::RayAxisParameter(ro, rd, p, ToOz(AxisVector(h)), g_gizmoStartParam))
            g_gizmoStartParam = 0.0f;
    } else if (IsPlane(h)) {
        if (!ozg::RayPlane(ro, rd, p, ToOz(PlaneNormal(h)), g_gizmoStartHit))
            g_gizmoStartHit = p;
    } else if (h == GizmoHandle::Screen) {
        const ozg::Vec3 n = ozg::Normalize(ozg::Sub(ToOz(cam.position), ToOz(cam.target)));
        if (!ozg::RayPlane(ro, rd, p, n, g_gizmoStartHit))
            g_gizmoStartHit = p;
    } else if (h == GizmoHandle::RotateY) {
        ozg::Vec3 hit;
        if (ozg::RayPlane(ro, rd, p, {0, 1, 0}, hit)) {
            g_gizmoStartAngle = atan2f(hit.z - p.z, hit.x - p.x);
        } else {
            // Looking level: the ring's plane is edge-on, so take the cursor's
            // angle around the projected pivot instead of refusing to drag.
            const Vector2 c = WorldToScreenInRect(pivot, cam, g_activeViewportRect);
            g_gizmoStartAngle = atan2f(-(mouse.y - c.y), mouse.x - c.x);
        }
    }

    g_gizmoDragHandle = h;
    return true;
}

void Gizmo_UpdateDrag(Vector2 mouse, const Camera3D& cam, int toolMode) {
    if (!Gizmo_IsDragging()) return;
    const GizmoHandle h = g_gizmoDragHandle;
    const Vector3 pivot = Gizmo_Pivot();
    const Ray ray = RayFromPane(mouse, cam, g_activeViewportRect);
    const ozg::Vec3 ro = ToOz(ray.position);
    const ozg::Vec3 rd = ozg::Normalize(ToOz(ray.direction));
    const ozg::Vec3 p = g_gizmoStartPivot;
    const float snap = Gizmo_SnapStep();
    const float len = Gizmo_Length(cam, pivot);

    if (IsAxis(h)) {
        // World distance moved along the axis since the drag started. When the axis
        // is (nearly) parallel to the view ray the closest-point parameter is
        // undefined, and `return`ing here made the handle silently dead (the Y shaft
        // in a Top view could never be dragged; in any view the axis aimed at the
        // camera was dead). Fall back to projecting the pixel delta onto the axis's
        // SCREEN direction - which is what UnrealEd's axial move does (Delta.X =
        // +MouseX, no ray at all).
        float delta = 0.0f;
        float t = 0.0f;
        if (ozg::RayAxisParameter(ro, rd, p, ToOz(AxisVector(h)), t)) {
            delta = t - g_gizmoStartParam;
        } else {
            const Vector2 a = WorldToScreenInRect(pivot, cam, g_activeViewportRect);
            const Vector2 b = WorldToScreenInRect(
                Vector3Add(pivot, Vector3Scale(AxisVector(h), len)), cam, g_activeViewportRect);
            const float sx = b.x - a.x, sy = b.y - a.y;
            const float len2 = sx * sx + sy * sy;
            const Vector2 md = GetMouseDelta();
            // `len` world units span |(sx,sy)| pixels along this direction, so one
            // pixel of movement along it is len/sqrt(len2) world units.
            if (len2 >= 4.0f)
                g_gizmoScreenAccum += (md.x * sx + md.y * sy) / len2 * len;
            delta = g_gizmoScreenAccum;
        }

        if (toolMode == 2 && Gizmo_TypeCanScale()) {
            // Scale: one handle length of drag along the axis is 2x growth. Uniform
            // because the renderable stores one scale.
            const float factor = 1.0f + delta / (len > 0.001f ? len : 1.0f);
            float s = g_gizmoStartS * factor;
            if (s < 0.05f) s = 0.05f;
            if (s > 100.0f) s = 100.0f;
            if (snap > 0.0f) s = ozg::SnapValue(s, snap);
            AngelTechEditor.S = s;
            return;
        }

        // SnapAround, not SnapValue: the grid is anchored at the drag's start
        // position (UnrealEd's GGridBase), so touching a handle never teleports an
        // off-grid object onto the grid before the user has moved a full step.
        if (h == GizmoHandle::AxisX) {
            AngelTechEditor.X = ozg::SnapAround(g_gizmoStartX + delta, g_gizmoStartX, snap);
        } else if (h == GizmoHandle::AxisY) {
            AngelTechEditor.Y = ozg::SnapAround(g_gizmoStartY + delta, g_gizmoStartY, snap);
        } else {
            AngelTechEditor.Z = ozg::SnapAround(g_gizmoStartZ + delta, g_gizmoStartZ, snap);
        }
        return;
    }

    if (IsPlane(h) || h == GizmoHandle::Screen) {
        ozg::Vec3 normal;
        if (h == GizmoHandle::Screen)
            normal = ozg::Normalize(ozg::Sub(ToOz(cam.position), ToOz(cam.target)));
        else
            normal = ToOz(PlaneNormal(h));

        ozg::Vec3 delta;
        ozg::Vec3 hit;
        if (ozg::RayPlane(ro, rd, p, normal, hit)) {
            delta = ozg::Sub(hit, g_gizmoStartHit);
        } else {
            // Plane seen edge-on: same screen-space fallback, projected back INTO
            // the plane so the handle only ever moves along the two axes it draws.
            ozg::Vec3 right, up;
            if (!ozg::ViewBasis(ozg::Normalize(ozg::Sub(ToOz(cam.target), ToOz(cam.position))),
                                {0.0f, 1.0f, 0.0f}, right, up))
                return;
            const float paneH = g_activeViewportRect.h > 0.0f
                                    ? g_activeViewportRect.h : (float)GetScreenHeight();
            const float k = edcam::UnitsPerPixel(
                cam, Vector3Distance(cam.position, pivot), paneH);
            const Vector2 md = GetMouseDelta();
            ozg::Vec3 d = ozg::Add(ozg::Mul(right, md.x * k), ozg::Mul(up, -md.y * k));
            d = ozg::Sub(d, ozg::Mul(normal, ozg::Dot(normal, d)));
            g_gizmoScreenAccumVec = ozg::Add(g_gizmoScreenAccumVec, d);
            delta = g_gizmoScreenAccumVec;
        }
        AngelTechEditor.X = ozg::SnapAround(g_gizmoStartX + delta.x, g_gizmoStartX, snap);
        AngelTechEditor.Y = ozg::SnapAround(g_gizmoStartY + delta.y, g_gizmoStartY, snap);
        AngelTechEditor.Z = ozg::SnapAround(g_gizmoStartZ + delta.z, g_gizmoStartZ, snap);
        return;
    }

    if (h == GizmoHandle::RotateY) {
        ozg::Vec3 hit;
        float ang;
        if (ozg::RayPlane(ro, rd, p, {0, 1, 0}, hit)) {
            ang = atan2f(hit.z - p.z, hit.x - p.x);
        } else {
            // Level view: the ring's plane is edge-on. Use the cursor's angle
            // around the projected pivot so Rotate still works instead of dying.
            const Vector2 c = WorldToScreenInRect(pivot, cam, g_activeViewportRect);
            ang = atan2f(-(mouse.y - c.y), mouse.x - c.x);
        }
        float deltaDeg = (ang - g_gizmoStartAngle) * RAD2DEG;
        deltaDeg = ozg::WrapDeg180(deltaDeg);
        float r = g_gizmoStartR + deltaDeg;
        r = ozg::SnapAngle(r, Gizmo_AngleStep());
        AngelTechEditor.R = r;
        return;
    }
}

// ---------------------------------------------------------------- Unreal-style
//
// UnrealEd's transforms are mouse + modifier gestures rather than handle drags.
// The mapping here is translated to THIS engine's axes, which are Y-up while
// Unreal (and OZONE) are Z-up:
//
//   Ctrl + LMB            -> world X        (Unreal's X)
//   Ctrl + RMB            -> world Z        (Unreal's Y)
//   Ctrl + LMB + RMB      -> world Y        (Unreal's Z, up/down)
//   Ctrl + RMB in Rotate  -> yaw (degrees, gizmoAngleSnap/Shift-1)
//   Shift + LMB           -> the same axial move, AND the camera rides along
//
// Those are UnrealEd's own mappings (UnEdCam.cpp, CalcAxialMoveRot): LMB is +X,
// RMB is +Y, both buttons are -MouseY on Z. Only one of the three is ever live,
// and ozg::UnrealAxialMove() is the single definition of which.
//
// Shift is NOT a different transform. In UnrealEd the modifier only decides which
// branch runs:
//     if(!(Buttons & (Ctrl|Shift))) { camera } else { actors }
//     MoveActors(..., (Buttons & MOUSE_Shift) ? Viewport->Actor : NULL);
// so Shift moves the actor on the SAME axis and additionally translates the view
// actor by the same delta. That is what g_unrealFollowCamera reproduces. (It used
// to be an unconstrained screen-plane move, which is neither what the modifier
// means nor axis-constrained at all.)
//
// Rotation is yaw-only on purpose: OZONE stores one rotation value per
// renderable/mesh, so mapping pitch/roll here would be a control that changes
// nothing - the failure mode the gizmo avoids by hiding its ring for types that
// cannot rotate.
//
// All of this writes AngelTechEditor only. Main.cpp's per-frame sync publishes
// that into the selected entity, so there is still exactly ONE write path.

enum class UnrealDragMode {
    None = 0,
    TranslateX,
    TranslateZ,
    TranslateY,
    Yaw,
};

static UnrealDragMode g_unrealDrag = UnrealDragMode::None;
static bool  g_unrealFollowCamera = false;   // Shift: translate the view too
static float g_unrealStartX = 0, g_unrealStartY = 0, g_unrealStartZ = 0;
static float g_unrealStartR = 0;
// Captured because a Shift gesture moves the camera too: cancelling must put the
// VIEW back as well, or Escape reverted the selection and left the camera
// displaced by the whole drag.
static Vector3 g_unrealStartCamPos = {0, 0, 0};
static Vector3 g_unrealStartCamTarget = {0, 0, 0};
// Pixels->world is captured ONCE per gesture, from the distance at the start.
// Re-deriving it every frame from the (moving) selection would make the drag
// accelerate as the object approaches the camera.
static float g_unrealDist = 0.0f;

bool Gizmo_IsUnrealDragging() { return g_unrealDrag != UnrealDragMode::None; }

void Gizmo_BeginUnrealDrag(UnrealDragMode mode, bool followCamera) {
    g_unrealDrag = mode;
    g_unrealFollowCamera = followCamera;
    g_unrealStartX = AngelTechEditor.X;
    g_unrealStartY = AngelTechEditor.Y;
    g_unrealStartZ = AngelTechEditor.Z;
    g_unrealStartR = AngelTechEditor.R;
    g_unrealStartCamPos = OTEditor.MainCamera.position;
    g_unrealStartCamTarget = OTEditor.MainCamera.target;
    g_unrealDist = 0.0f;
}

// `delta` is the per-frame mouse delta, so the axis can change mid-gesture
// (adding the second button switches to the vertical axis) without the object
// jumping: total movement is the sum of the deltas, not a projection from the
// gesture's start.
void Gizmo_UpdateUnrealDrag(Vector2 delta, const Camera3D& cam, int toolMode,
                            bool ctrl, bool shift, bool lmb, bool rmb) {
    if (g_unrealDrag == UnrealDragMode::None) return;

    // Re-derive the axis from the LIVE button state every frame. UnrealEd does the
    // same (CalcAxialMoveRot dispatches on Buttons), which is what lets LMB -> X
    // become LMB+RMB -> Y mid-drag.
    UnrealDragMode want = UnrealDragMode::None;
    if (ctrl || shift) {
        if (lmb && rmb) want = UnrealDragMode::TranslateY;
        else if (lmb)   want = UnrealDragMode::TranslateX;
        else if (rmb)   want = (toolMode == 3) ? UnrealDragMode::Yaw
                                               : UnrealDragMode::TranslateZ;
    }
    // Modifier released before the button: freeze rather than latch. Main.cpp's
    // release path still ends the gesture, so nothing stays stuck.
    if (want == UnrealDragMode::None) return;
    g_unrealDrag = want;

    // Ctrl and Shift own the modifiers, so snapping reads the toolbar grid
    // directly instead of Gizmo_SnapStep() (which treats Ctrl as "snap to 1").
    const float snap = g_editorPanels.gizmoSnap;
    const float speed = ozg::UnrealSpeedScale(g_editorPanels.unrealMoveSpeed);

    if (g_unrealDrag == UnrealDragMode::Yaw) {
        AngelTechEditor.R = ozg::SnapAngle(AngelTechEditor.R + ozg::UnrealAxialYaw(delta.x) * speed,
                                           Gizmo_AngleStep());
        return;
    }

    const Vector3 pivot = Gizmo_Pivot();
    if (g_unrealDist <= 0.0f) g_unrealDist = Vector3Distance(cam.position, pivot);
    const float paneH = g_activeViewportRect.h > 0.0f ? g_activeViewportRect.h
                                                      : (float)GetScreenHeight();
    // One pixel of drag = one pixel's worth of world at the selection's depth, so
    // the gesture covers the same screen distance at any zoom. UnrealEd multiplied
    // by a fixed 0.25*MovementSpeed instead, which is only equivalent at one
    // distance - that is why the old dist*0.002 factor felt violent far out and
    // dead up close, and why the speed keys did not match UnrealEd's ratio.
    const float k = edcam::UnitsPerPixel(cam, g_unrealDist, paneH) * speed;

    const ozg::Move3 d = ozg::UnrealAxialMove(delta.x, delta.y, lmb, rmb);

    // Applying all three components is safe and deliberate: UnrealAxialMove zeroes
    // the two that are not live, and SnapAround(v, vStart, step) returns vStart
    // exactly when the delta is zero. One definition of the axis mapping, no second
    // switch here that could drift from it.
    const float x0 = AngelTechEditor.X, y0 = AngelTechEditor.Y, z0 = AngelTechEditor.Z;
    AngelTechEditor.X = ozg::SnapAround(x0 + d.x * k, g_unrealStartX, snap);
    AngelTechEditor.Y = ozg::SnapAround(y0 + d.y * k, g_unrealStartY, snap);
    AngelTechEditor.Z = ozg::SnapAround(z0 + d.z * k, g_unrealStartZ, snap);

    if (g_unrealFollowCamera) {
        // UnrealEd passes the view actor to MoveActors, so the camera moves by the
        // SAME delta and the selection stays put on screen. The SNAPPED delta keeps
        // the pair rigid.
        const Vector3 step = {AngelTechEditor.X - x0,
                              AngelTechEditor.Y - y0,
                              AngelTechEditor.Z - z0};
        OTEditor.MainCamera.position = Vector3Add(OTEditor.MainCamera.position, step);
        OTEditor.MainCamera.target   = Vector3Add(OTEditor.MainCamera.target,   step);
    }
}

void Gizmo_EndUnrealDrag() {
    g_unrealDrag = UnrealDragMode::None;
    g_unrealFollowCamera = false;
}

void Gizmo_EndDrag() {
    g_gizmoDragHandle = GizmoHandle::None;
}

void Gizmo_CancelUnrealDrag() {
    if (g_unrealDrag == UnrealDragMode::None) return;
    AngelTechEditor.X = g_unrealStartX;
    AngelTechEditor.Y = g_unrealStartY;
    AngelTechEditor.Z = g_unrealStartZ;
    AngelTechEditor.R = g_unrealStartR;
    if (g_unrealFollowCamera) {
        OTEditor.MainCamera.position = g_unrealStartCamPos;
        OTEditor.MainCamera.target   = g_unrealStartCamTarget;
    }
    g_unrealDrag = UnrealDragMode::None;
    g_unrealFollowCamera = false;
}


void Gizmo_CancelDrag() {
    if (!Gizmo_IsDragging()) return;
    // The frame loop publishes the gizmo into the entity, so restoring the gizmo
    // restores the entity on the next sync. No second write path.
    AngelTechEditor.X = g_gizmoStartX;
    AngelTechEditor.Y = g_gizmoStartY;
    AngelTechEditor.Z = g_gizmoStartZ;
    AngelTechEditor.R = g_gizmoStartR;
    AngelTechEditor.S = g_gizmoStartS;
    g_gizmoDragHandle = GizmoHandle::None;
}

// ---------------------------------------------------------------- drawing
void DrawTransformGizmo(const Camera3D& cam, int toolMode) {
    if (!Gizmo_SelectionExists() || g_editorPanels.animEditVerts) return;
    if (toolMode == 0) return;
    if (toolMode == 2 && !Gizmo_TypeCanScale()) return;
    if (toolMode == 3 && !Gizmo_TypeCanRotate()) return;

    const Vector3 pivot = Gizmo_Pivot();
    const float len = Gizmo_Length(cam, pivot);
    const float tip = len * 0.10f;

    const Color cX = (g_gizmoHoverHandle == GizmoHandle::AxisX) ? YELLOW : RED;
    const Color cY = (g_gizmoHoverHandle == GizmoHandle::AxisY) ? YELLOW : BLUE;
    const Color cZ = (g_gizmoHoverHandle == GizmoHandle::AxisZ) ? YELLOW : GREEN;

    if (toolMode == 3) {
        // Y rotation ring.
        const Color ring = (g_gizmoHoverHandle == GizmoHandle::RotateY) ? YELLOW : (Color){255, 200, 0, 255};
        const float r = len * 0.85f;
        for (int i = 0; i < 64; i++) {
            const float a0 = (float)i / 64.0f * 2.0f * PI;
            const float a1 = (float)(i + 1) / 64.0f * 2.0f * PI;
            DrawLine3D({pivot.x + cosf(a0) * r, pivot.y, pivot.z + sinf(a0) * r},
                       {pivot.x + cosf(a1) * r, pivot.y, pivot.z + sinf(a1) * r}, ring);
        }
        DrawSphere(pivot, tip * 0.45f, (Color){180, 180, 180, 220});
        return;
    }

    // Axis shafts + tips. Scale mode draws cubes, move mode arrow spheres.
    const GizmoHandle axes[3] = {GizmoHandle::AxisX, GizmoHandle::AxisY, GizmoHandle::AxisZ};
    const Color colors[3] = {cX, cY, cZ};
    for (int i = 0; i < 3; i++) {
        const Vector3 dir = AxisVector(axes[i]);
        const Vector3 end = Vector3Add(pivot, Vector3Scale(dir, len));
        DrawLine3D(pivot, end, colors[i]);
        if (toolMode == 2) {
            DrawCube(end, tip * 1.2f, tip * 1.2f, tip * 1.2f, colors[i]);
            DrawCubeWires(end, tip * 1.2f, tip * 1.2f, tip * 1.2f, DARKGRAY);
        } else {
            DrawSphere(end, tip, colors[i]);
        }
    }

    if (toolMode == 1) {
        // Plane handles: thin slabs, not triangles, so they read as grabs.
        const GizmoHandle planes[3] = {GizmoHandle::PlaneXY, GizmoHandle::PlaneYZ, GizmoHandle::PlaneXZ};
        for (GizmoHandle h : planes) {
            const bool hot = (g_gizmoHoverHandle == h);
            const Color c = hot ? YELLOW : (Color){140, 140, 160, 200};
            const Vector3 pc = PlaneCenter(pivot, h, len);
            const float s = len * 0.16f;
            if (h == GizmoHandle::PlaneXY)      DrawCube(pc, s, s, s * 0.06f, c);
            else if (h == GizmoHandle::PlaneYZ) DrawCube(pc, s * 0.06f, s, s, c);
            else                                DrawCube(pc, s, s * 0.06f, s, c);
        }
    }

    const bool hotCentre = (g_gizmoHoverHandle == GizmoHandle::Screen);
    DrawSphere(pivot, tip * 0.55f, hotCentre ? YELLOW : (Color){220, 220, 220, 230});
}
