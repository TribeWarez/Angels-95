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

int main() {
    TestRayAxisParameter();
    TestRayPlane();
    TestSnapping();
    TestVectorHelpers();

    if (g_fail == 0) {
        printf("GizmoMath: %d/%d checks passed\n", g_checks, g_checks);
        return 0;
    }
    printf("GizmoMath: %d/%d checks FAILED\n", g_fail, g_checks);
    return 1;
}
