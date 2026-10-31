// GizmoMath test - standalone, no raylib dependency.
//   make test_gizmomath
//   g++ -std=c++20 tests/GizmoMath.test.cpp -o test_gizmomath
#include "../AngelEd/Source/Subsystems/GizmoMath.hpp"
#include <cstdio>
#include <cmath>

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { g_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_NEAR(a, b, eps) do { \
    g_checks++; \
    if (std::fabs((a) - (b)) > (eps)) { \
        g_fail++; \
        printf("  FAIL %s:%d  %s=%.6f expected %.6f\n", __FILE__, __LINE__, #a, (double)(a), (double)(b)); \
    } \
} while (0)

using namespace ozg;

// A ray from above, straight down, crossing the X axis at x = 3.
static void TestRayAxisParameter() {
    const Vec3 origin{0, 10, 0};
    const Vec3 dir{0, -1, 0};
    const Vec3 pivot{0, 0, 0};
    float t = -999.0f;

    // X axis: the perpendicular foot is (3,0,0) only if the ray is offset in X.
    const Vec3 rayX{3, 10, 0};
    CHECK(RayAxisParameter(rayX, dir, pivot, {1, 0, 0}, t));
    CHECK_NEAR(t, 3.0f, 1e-4f);

    // Y axis: a ray straight down the Y axis is parallel to it and must be
    // rejected rather than returning a garbage parameter.
    CHECK(!RayAxisParameter(origin, dir, pivot, {0, 1, 0}, t));

    // Z axis at an angle.
    const Vec3 rayZ{0, 10, 4};
    CHECK(RayAxisParameter(rayZ, dir, pivot, {0, 0, 1}, t));
    CHECK_NEAR(t, 4.0f, 1e-4f);
}

static void TestRayPlane() {
    Vec3 hit;
    // Straight down onto the XZ plane at y = 2.
    CHECK(RayPlane({1, 10, 3}, {0, -1, 0}, {0, 2, 0}, {0, 1, 0}, hit));
    CHECK_NEAR(hit.x, 1.0f, 1e-4f);
    CHECK_NEAR(hit.y, 2.0f, 1e-4f);
    CHECK_NEAR(hit.z, 3.0f, 1e-4f);

    // Parallel to the plane -> no hit.
    CHECK(!RayPlane({0, 0, 0}, {1, 0, 0}, {0, 2, 0}, {0, 1, 0}, hit));

    // Plane behind the ray -> no hit (t < 0).
    CHECK(!RayPlane({0, 0, 0}, {0, 1, 0}, {0, -2, 0}, {0, 1, 0}, hit));
}

static void TestSnapping() {
    CHECK_NEAR(SnapValue(0.0f, 1.0f), 0.0f, 1e-6f);
    CHECK_NEAR(SnapValue(0.49f, 1.0f), 0.0f, 1e-6f);
    CHECK_NEAR(SnapValue(0.51f, 1.0f), 1.0f, 1e-6f);
    CHECK_NEAR(SnapValue(-0.51f, 1.0f), -1.0f, 1e-6f);
    CHECK_NEAR(SnapValue(-2.2f, 0.25f), -2.25f, 1e-6f);
    // step 0 = off.
    CHECK_NEAR(SnapValue(1.234f, 0.0f), 1.234f, 1e-6f);

    CHECK_NEAR(SnapAngle(7.0f, 15.0f), 0.0f, 1e-6f);
    CHECK_NEAR(SnapAngle(8.0f, 15.0f), 15.0f, 1e-6f);
    CHECK_NEAR(SnapAngle(-22.0f, 15.0f), -15.0f, 1e-6f);
    CHECK_NEAR(SnapAngle(-23.0f, 15.0f), -30.0f, 1e-6f);
    CHECK_NEAR(SnapAngle(12.0f, 0.0f), 12.0f, 1e-6f);

    CHECK_NEAR(WrapDeg180(190.0f), -170.0f, 1e-6f);
    CHECK_NEAR(WrapDeg180(-190.0f), 170.0f, 1e-6f);
    CHECK_NEAR(WrapDeg180(180.0f), 180.0f, 1e-6f);
}

static void TestVectorHelpers() {
    CHECK_NEAR(Length({3, 4, 0}), 5.0f, 1e-6f);
    const Vec3 n = Normalize({0, 0, 5});
    CHECK_NEAR(n.z, 1.0f, 1e-6f);
    CHECK_NEAR(Length(Normalize({0, 0, 0})), 0.0f, 1e-6f);
    const Vec3 c = Cross({1, 0, 0}, {0, 1, 0});
    CHECK_NEAR(c.z, 1.0f, 1e-6f);
}

// ---------------------------------------------------------------------------
// UnrealEd axis mapping (Source/Editor/Src/UnEdCam.cpp, CalcAxialMoveRot).
// The whole point of these is that the mapping is stated ONCE and in one place,
// so a translation error is caught here instead of by a user dragging a wall.
// ---------------------------------------------------------------------------
static void TestUnrealAxialMove() {
    // LMB: +X from the horizontal mouse delta, and nothing else.
    Move3 d = UnrealAxialMove(10.0f, -7.0f, true, false);
    CHECK_NEAR(d.x, 10.0f, 1e-6f);
    CHECK_NEAR(d.y, 0.0f, 1e-6f);
    CHECK_NEAR(d.z, 0.0f, 1e-6f);

    // RMB: Unreal's Y becomes this engine's Z.
    d = UnrealAxialMove(10.0f, -7.0f, false, true);
    CHECK_NEAR(d.x, 0.0f, 1e-6f);
    CHECK_NEAR(d.y, 0.0f, 1e-6f);
    CHECK_NEAR(d.z, 10.0f, 1e-6f);

    // Both: Unreal's Z becomes this engine's Y, and screen-up is a NEGATIVE mouse
    // delta, so delta.y = -7 (cursor moved up) must RAISE the selection.
    d = UnrealAxialMove(10.0f, -7.0f, true, true);
    CHECK_NEAR(d.x, 0.0f, 1e-6f);
    CHECK_NEAR(d.y, 7.0f, 1e-6f);
    CHECK_NEAR(d.z, 0.0f, 1e-6f);
    d = UnrealAxialMove(0.0f, 7.0f, true, true);
    CHECK_NEAR(d.y, -7.0f, 1e-6f);

    // No buttons: no movement.
    d = UnrealAxialMove(10.0f, -7.0f, false, false);
    CHECK_NEAR(d.x + d.y + d.z, 0.0f, 1e-6f);

    // Applying the result is what Main/Gizmo rely on: exactly one axis is live, so
    // a SnapAround(v, v, step) on the other two cannot disturb them.
    d = UnrealAxialMove(3.0f, 3.0f, true, false);
    CHECK_NEAR(SnapAround(0.37f + d.y, 0.37f, 0.5f), 0.37f, 1e-6f);
    CHECK_NEAR(SnapAround(0.37f + d.z, 0.37f, 0.5f), 0.37f, 1e-6f);
}

static void TestUnrealSpeedScale() {
    CHECK_NEAR(UnrealSpeedScale(0), 0.25f, 1e-6f);
    CHECK_NEAR(UnrealSpeedScale(1), 1.0f, 1e-6f);
    CHECK_NEAR(UnrealSpeedScale(2), 4.0f, 1e-6f);
    // UnrealEd's MovementSpeed keys are 1/4/16 with a 0.25 multiplier, so the
    // ratio between neighbours is always 4x. That ratio is the property the speed
    // keys must keep, not three independent magic numbers.
    CHECK_NEAR(UnrealSpeedScale(1) / UnrealSpeedScale(0), 4.0f, 1e-6f);
    CHECK_NEAR(UnrealSpeedScale(2) / UnrealSpeedScale(1), 4.0f, 1e-6f);
}

static void TestUnrealAxialYaw() {
    CHECK_NEAR(UnrealAxialYaw(10.0f), 4.0f, 1e-6f);    // same sign as MouseX
    CHECK_NEAR(UnrealAxialYaw(-10.0f), -4.0f, 1e-6f);
    CHECK_NEAR(UnrealAxialYaw(0.0f), 0.0f, 1e-6f);
}

// ---------------------------------------------------------------------------
// SnapAround: a grid ANCHORED at the drag start, never at zero. SnapValue is
// still right for absolute values; these pin the difference.
// ---------------------------------------------------------------------------
static void TestSnapAround() {
    // The first pixel of a drag must not teleport an off-grid object.
    CHECK_NEAR(SnapAround(0.37f, 0.37f, 0.5f), 0.37f, 1e-6f);
    // Less than half a step still lands on the anchor.
    CHECK_NEAR(SnapAround(0.37f + 0.24f, 0.37f, 0.5f), 0.37f, 1e-6f);
    // More than half a step moves exactly one step FROM THE ANCHOR (0.87), not to
    // the absolute grid (1.0). That difference is the whole point.
    CHECK_NEAR(SnapAround(0.37f + 0.26f, 0.37f, 0.5f), 0.87f, 1e-6f);
    CHECK_NEAR(SnapAround(0.37f - 0.26f, 0.37f, 0.5f), -0.13f, 1e-6f);
    // Negative anchors behave the same way.
    CHECK_NEAR(SnapAround(-4.1f, -4.1f, 2.0f), -4.1f, 1e-6f);
    CHECK_NEAR(SnapAround(-4.1f + 1.1f, -4.1f, 2.0f), -2.1f, 1e-6f);
    // step 0 = off.
    CHECK_NEAR(SnapAround(1.234f, 0.0f, 0.0f), 1.234f, 1e-6f);
    // An on-grid anchor is identical to absolute snapping (so no behaviour change
    // for content that already sits on the grid).
    CHECK_NEAR(SnapAround(2.6f, 0.0f, 1.0f), 3.0f, 1e-6f);
    CHECK_NEAR(SnapAround(2.4f, 0.0f, 1.0f), 2.0f, 1e-6f);
}

// ---------------------------------------------------------------------------
// Camera scale. This is what replaced both copies' fixed 0.1 units/pixel pan and
// the Unreal drag's dist*0.002 - the reason a drag was violent far out and dead
// up close.
// ---------------------------------------------------------------------------
static void TestUnitsPerPixel() {
    const float kDeg = 0.017453292519943295f;
    const float expect = 2.0f * std::tan(30.0f * kDeg) * 20.0f / 1080.0f;
    CHECK_NEAR(UnitsPerPixel(60.0f, 1080.0f, 20.0f), expect, 1e-6f);

    // Linear in distance: twice as far away = twice the world per pixel.
    CHECK_NEAR(UnitsPerPixel(60.0f, 1080.0f, 40.0f),
               2.0f * UnitsPerPixel(60.0f, 1080.0f, 20.0f), 1e-5f);
    // Inversely proportional to height: half the pixels = twice the world each.
    CHECK_NEAR(UnitsPerPixel(60.0f, 540.0f, 20.0f),
               2.0f * UnitsPerPixel(60.0f, 1080.0f, 20.0f), 1e-5f);
    // A zero/negative height must not divide by zero or flip the sign.
    CHECK(UnitsPerPixel(60.0f, 0.0f, 20.0f) > 0.0f);
}

// ---------------------------------------------------------------------------
// View basis: the axes a screen-plane pan moves along.
// ---------------------------------------------------------------------------
static void TestViewBasis() {
    Vec3 right, up;
    // Looking down +Z with up +Y.
    CHECK(ViewBasis({0, 0, 1}, {0, 1, 0}, right, up));
    CHECK_NEAR(right.x, -1.0f, 1e-6f);   // cross(forward, up)
    CHECK_NEAR(right.y, 0.0f, 1e-6f);
    CHECK_NEAR(right.z, 0.0f, 1e-6f);
    CHECK_NEAR(up.x, 0.0f, 1e-6f);
    CHECK_NEAR(up.y, 1.0f, 1e-6f);
    CHECK_NEAR(up.z, 0.0f, 1e-6f);

    // Top view: looking straight down with up = -Z (the ViewTop preset), which is
    // the case the old world-X/Z pan got most visibly wrong.
    CHECK(ViewBasis({0, -1, 0}, {0, 0, -1}, right, up));
    CHECK_NEAR(right.x, 1.0f, 1e-6f);
    CHECK_NEAR(up.z, -1.0f, 1e-6f);
    CHECK_NEAR(Dot(right, up), 0.0f, 1e-6f);

    // Forward parallel to up: undefined. Must be REPORTED rather than yielding a
    // zero basis, which would silently make pan do nothing.
    CHECK(!ViewBasis({0, 1, 0}, {0, 1, 0}, right, up));
    CHECK(!ViewBasis({0, -1, 0}, {0, 1, 0}, right, up));
}

int main() {
    TestRayAxisParameter();
    TestRayPlane();
    TestSnapping();
    TestVectorHelpers();
    TestUnrealAxialMove();
    TestUnrealSpeedScale();
    TestUnrealAxialYaw();
    TestSnapAround();
    TestUnitsPerPixel();
    TestViewBasis();

    if (g_fail == 0) {
        printf("GizmoMath: %d/%d checks passed\n", g_checks, g_checks);
        return 0;
    }
    printf("GizmoMath: %d/%d checks FAILED\n", g_fail, g_checks);
    return 1;
}
