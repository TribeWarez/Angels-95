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
    // Ctrl = force 1.0 grid; Shift = fine 0.1; otherwise the toolbar setting
    // (0 = off). Ctrl wins so a user who wants a whole unit always gets one.
    if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) return 1.0f;
    if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) return 0.1f;
    return g_editorPanels.gizmoSnap;
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
        float bestDist = 8.0f;
        const float r = len * 0.85f;
        Vector2 prev = {};
        for (int i = 0; i <= 64; i++) {
            const float a = (float)i / 64.0f * 2.0f * PI;
            Vector3 wp = {pivot.x + cosf(a) * r, pivot.y, pivot.z + sinf(a) * r};
            Vector2 sp = GetWorldToScreen(wp, cam);
            if (i > 0) {
                const float d = DistToSegment2D(mouse, prev, sp);
                if (d < bestDist) { bestDist = d; best = GizmoHandle::RotateY; }
            }
            prev = sp;
        }
        return best;
    }

    const Vector2 sp = GetWorldToScreen(pivot, cam);
    // Centre handle wins ties: it is the smallest target.
    const float dc = sqrtf((mouse.x - sp.x) * (mouse.x - sp.x) + (mouse.y - sp.y) * (mouse.y - sp.y));
    if (dc < 9.0f) return GizmoHandle::Screen;

    // Axis shafts + tips.
    GizmoHandle best = GizmoHandle::None;
    float bestDist = 8.0f;
    const GizmoHandle axes[3] = {GizmoHandle::AxisX, GizmoHandle::AxisY, GizmoHandle::AxisZ};
    for (GizmoHandle h : axes) {
        const Vector3 tip = Vector3Add(pivot, Vector3Scale(AxisVector(h), len));
        const Vector2 tp = GetWorldToScreen(tip, cam);
        const float d = DistToSegment2D(mouse, sp, tp);
        if (d < bestDist) { bestDist = d; best = h; }
    }
    if (best != GizmoHandle::None) return best;

    // Plane quads (move mode only).
    if (toolMode == 1) {
        const GizmoHandle planes[3] = {GizmoHandle::PlaneXY, GizmoHandle::PlaneYZ, GizmoHandle::PlaneXZ};
        for (GizmoHandle h : planes) {
            const Vector2 pc = GetWorldToScreen(PlaneCenter(pivot, h, len), cam);
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
    const Ray ray = GetMouseRay(mouse, cam);
    const ozg::Vec3 ro = ToOz(ray.position);
    const ozg::Vec3 rd = ozg::Normalize(ToOz(ray.direction));
    const ozg::Vec3 p = ToOz(pivot);

    g_gizmoStartPivot = p;
    g_gizmoStartX = AngelTechEditor.X;
    g_gizmoStartY = AngelTechEditor.Y;
    g_gizmoStartZ = AngelTechEditor.Z;
    g_gizmoStartR = AngelTechEditor.R;
    g_gizmoStartS = AngelTechEditor.S;

    if (IsAxis(h)) {
        if (!ozg::RayAxisParameter(ro, rd, p, ToOz(AxisVector(h)), g_gizmoStartParam))
            return false;
    } else if (IsPlane(h)) {
        if (!ozg::RayPlane(ro, rd, p, ToOz(PlaneNormal(h)), g_gizmoStartHit))
            return false;
    } else if (h == GizmoHandle::Screen) {
        const ozg::Vec3 n = ozg::Normalize(ozg::Sub(ToOz(cam.position), ToOz(cam.target)));
        if (!ozg::RayPlane(ro, rd, p, n, g_gizmoStartHit))
            return false;
    } else if (h == GizmoHandle::RotateY) {
        ozg::Vec3 hit;
        if (!ozg::RayPlane(ro, rd, p, {0, 1, 0}, hit))
            return false;
        g_gizmoStartAngle = atan2f(hit.z - p.z, hit.x - p.x);
    }

    g_gizmoDragHandle = h;
    return true;
}

void Gizmo_UpdateDrag(Vector2 mouse, const Camera3D& cam, int toolMode) {
    if (!Gizmo_IsDragging()) return;
    const GizmoHandle h = g_gizmoDragHandle;
    const Vector3 pivot = Gizmo_Pivot();
    const Ray ray = GetMouseRay(mouse, cam);
    const ozg::Vec3 ro = ToOz(ray.position);
    const ozg::Vec3 rd = ozg::Normalize(ToOz(ray.direction));
    const ozg::Vec3 p = g_gizmoStartPivot;
    const float snap = Gizmo_SnapStep();
    const float len = Gizmo_Length(cam, pivot);

    if (IsAxis(h)) {
        float t = 0;
        if (!ozg::RayAxisParameter(ro, rd, p, ToOz(AxisVector(h)), t)) return;

        if (toolMode == 2 && Gizmo_TypeCanScale()) {
            // Scale: one world unit of drag along the axis is roughly one handle
            // length of growth. Uniform because the renderable stores one scale.
            const float factor = 1.0f + (t - g_gizmoStartParam) / (len > 0.001f ? len : 1.0f);
            float s = g_gizmoStartS * factor;
            if (s < 0.05f) s = 0.05f;
            if (s > 100.0f) s = 100.0f;
            if (snap > 0.0f) s = ozg::SnapValue(s, snap);
            AngelTechEditor.S = s;
            return;
        }

        const float delta = t - g_gizmoStartParam;
        if (h == GizmoHandle::AxisX) {
            float v = g_gizmoStartX + delta;
            AngelTechEditor.X = ozg::SnapValue(v, snap);
        } else if (h == GizmoHandle::AxisY) {
            float v = g_gizmoStartY + delta;
            AngelTechEditor.Y = ozg::SnapValue(v, snap);
        } else {
            float v = g_gizmoStartZ + delta;
            AngelTechEditor.Z = ozg::SnapValue(v, snap);
        }
        return;
    }

    if (IsPlane(h) || h == GizmoHandle::Screen) {
        ozg::Vec3 normal;
        if (h == GizmoHandle::Screen)
            normal = ozg::Normalize(ozg::Sub(ToOz(cam.position), ToOz(cam.target)));
        else
            normal = ToOz(PlaneNormal(h));

        ozg::Vec3 hit;
        if (!ozg::RayPlane(ro, rd, p, normal, hit)) return;
        const ozg::Vec3 delta = ozg::Sub(hit, g_gizmoStartHit);
        AngelTechEditor.X = ozg::SnapValue(g_gizmoStartX + delta.x, snap);
        AngelTechEditor.Y = ozg::SnapValue(g_gizmoStartY + delta.y, snap);
        AngelTechEditor.Z = ozg::SnapValue(g_gizmoStartZ + delta.z, snap);
        return;
    }

    if (h == GizmoHandle::RotateY) {
        ozg::Vec3 hit;
        if (!ozg::RayPlane(ro, rd, p, {0, 1, 0}, hit)) return;
        const float ang = atan2f(hit.z - p.z, hit.x - p.x);
        float deltaDeg = (ang - g_gizmoStartAngle) * RAD2DEG;
        deltaDeg = ozg::WrapDeg180(deltaDeg);
        float r = g_gizmoStartR + deltaDeg;
        r = ozg::SnapAngle(r, Gizmo_AngleStep());
        AngelTechEditor.R = r;
        return;
    }
}

void Gizmo_EndDrag() {
    g_gizmoDragHandle = GizmoHandle::None;
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
