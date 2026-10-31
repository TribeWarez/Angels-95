// EditorSnap test - standalone, no raylib dependency.
//   make test_editorsnap
//   g++ -std=c++20 tests/EditorSnap.test.cpp -o test_editorsnap
//
// Pins the two "Snap to Ground / Side" queries. The editor glue around them
// (Subsystems/Selection.cpp) needs a live world, but the MATHS is what decides
// whether an object lands on the floor or on the roof above it, so it lives here.
#include "../AngelEd/Source/Subsystems/SnapMath.hpp"
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

using namespace ozsnap;

static Box B(float minX, float minY, float minZ, float maxX, float maxY, float maxZ) {
    Box b; b.minX = minX; b.minY = minY; b.minZ = minZ;
    b.maxX = maxX; b.maxY = maxY; b.maxZ = maxZ;
    return b;
}

// A 4x4 floor slab whose top is at y = 0, and a roof slab 8 units up.
static void TestGroundPicksTheFloorNotTheRoof() {
    Box solids[2] = { B(-50, -4, -50, 50, 0, 50),      // floor, top y = 0
                      B(-50, 8, -50, 50, 10, 50) };    // ceiling, bottom y = 8
    Box mover = B(-1, 3, -1, 1, 5, 1);                 // floating 3 above the floor
    CHECK_NEAR(GroundBelow(mover, solids, 2, -100.0f), 0.0f, 1e-4f);
}

static void TestGroundUsesFallbackWhenNothingIsUnder() {
    Box solids[1] = { B(20, -4, 20, 40, 0, 40) };      // far away in XZ
    Box mover = B(-1, 3, -1, 1, 5, 1);
    CHECK_NEAR(GroundBelow(mover, solids, 1, -7.5f), -7.5f, 1e-4f);
}

static void TestGroundNeedsFootprintOverlapNotJustTheCentre() {
    // The ledge only reaches x = 0, and the mover straddles that edge: its centre is
    // at x = +0.5 (off the ledge) but part of its footprint is on it. The centre
    // column alone would miss the ledge and fall through to the fallback.
    Box solids[1] = { B(-20, -4, -20, 0, 2, 20) };
    Box mover = B(-0.5f, 6, -1, 1.5f, 8, 1);
    CHECK_NEAR(GroundBelow(mover, solids, 1, -100.0f), 2.0f, 1e-4f);
}

static void TestGroundNeverSnapsUpwards() {
    // A box sitting ON the floor must not be pulled up onto the ceiling, and a box
    // above the ceiling must not be pulled DOWN through it.
    Box solids[1] = { B(-20, -4, -20, 20, 0, 20) };
    Box mover = B(-1, 0, -1, 1, 2, 1);
    CHECK_NEAR(GroundBelow(mover, solids, 1, -100.0f), 0.0f, 1e-4f);
}

// --- Side -------------------------------------------------------------------
static void TestPushAgainstWallAhead() {
    Box solids[1] = { B(10, -4, -20, 14, 10, 20) };    // wall starting at x = 10
    Box mover = B(0, 0, -1, 4, 3, 1);
    float d = 0.0f;
    CHECK(PushAgainstSide(mover, solids, 1, 1.0f, 0.0f, d));
    CHECK_NEAR(d, 6.0f, 1e-4f);                        // mover.maxX (4) -> wall.minX (10)
}

static void TestPushIgnoresWallsBehindAndOnOtherStoreys() {
    Box solids[2] = { B(-10, -4, -20, -6, 10, 20),     // behind us
                      B(10, 20, -20, 14, 30, 20) };    // ahead, but two storeys up
    Box mover = B(0, 0, -1, 4, 3, 1);
    float d = 0.0f;
    CHECK(!PushAgainstSide(mover, solids, 2, 1.0f, 0.0f, d));
}

static void TestPushIgnoresSideNeighboursOffAxis() {
    // A wall at +Z must not stop a push along +X.
    Box solids[1] = { B(10, -4, 5, 14, 10, 9) };
    Box mover = B(0, 0, -1, 4, 3, 1);
    float d = 0.0f;
    CHECK(!PushAgainstSide(mover, solids, 1, 1.0f, 0.0f, d));
}

static void TestPushPicksTheNearestWall() {
    Box solids[2] = { B(20, -4, -20, 24, 10, 20),
                      B(10, -4, -20, 14, 10, 20) };
    Box mover = B(0, 0, -1, 4, 3, 1);
    float d = 0.0f;
    CHECK(PushAgainstSide(mover, solids, 2, 1.0f, 0.0f, d));
    CHECK_NEAR(d, 6.0f, 1e-4f);
}

static void TestPushBackwards() {
    Box solids[1] = { B(-10, -4, -20, -6, 10, 20) };
    Box mover = B(0, 0, -1, 4, 3, 1);
    float d = 0.0f;
    CHECK(PushAgainstSide(mover, solids, 1, -1.0f, 0.0f, d));
    CHECK_NEAR(d, -6.0f, 1e-4f);                       // mover.minX (0) -> wall.maxX (-6)
}

static void TestPushAlongZ() {
    Box solids[1] = { B(-20, -4, 10, 20, 10, 14) };
    Box mover = B(-1, 0, 0, 1, 3, 4);
    float d = 0.0f;
    CHECK(PushAgainstSide(mover, solids, 1, 0.0f, 1.0f, d));
    CHECK_NEAR(d, 6.0f, 1e-4f);
}

static void TestPushIsLimitedByReach() {
    Box solids[1] = { B(500, -4, -20, 504, 10, 20) };
    Box mover = B(0, 0, -1, 4, 3, 1);
    float d = 0.0f;
    CHECK(PushAgainstSideLimited(mover, solids, 1, 1.0f, 0.0f, 200.0f, d) == false);
    CHECK(PushAgainstSideLimited(mover, solids, 1, 1.0f, 0.0f, 1000.0f, d) == true);
    CHECK_NEAR(d, 496.0f, 1e-4f);
}

static void TestCardinalFrom() {
    float dx = 0, dz = 0;
    CardinalFrom(0.9f, 0.1f, dx, dz);  CHECK_NEAR(dx, 1.0f, 1e-6f);  CHECK_NEAR(dz, 0.0f, 1e-6f);
    CardinalFrom(-0.2f, -0.95f, dx, dz); CHECK_NEAR(dx, 0.0f, 1e-6f); CHECK_NEAR(dz, -1.0f, 1e-6f);
    CardinalFrom(-0.6f, 0.6f, dx, dz); CHECK_NEAR(dx, -1.0f, 1e-6f); CHECK_NEAR(dz, 0.0f, 1e-6f);
    // Straight-down view: right is degenerate, so pick a deterministic axis rather
    // than emitting NaN.
    CardinalFrom(0.0f, 0.0f, dx, dz);  CHECK_NEAR(dx, 1.0f, 1e-6f);  CHECK_NEAR(dz, 0.0f, 1e-6f);
}

int main() {
    TestGroundPicksTheFloorNotTheRoof();
    TestGroundUsesFallbackWhenNothingIsUnder();
    TestGroundNeedsFootprintOverlapNotJustTheCentre();
    TestGroundNeverSnapsUpwards();
    TestPushAgainstWallAhead();
    TestPushIgnoresWallsBehindAndOnOtherStoreys();
    TestPushIgnoresSideNeighboursOffAxis();
    TestPushPicksTheNearestWall();
    TestPushBackwards();
    TestPushAlongZ();
    TestPushIsLimitedByReach();
    TestCardinalFrom();

    if (g_fail == 0) {
        printf("EditorSnap: %d/%d checks passed\n", g_checks, g_checks);
        return 0;
    }
    printf("EditorSnap: %d/%d checks FAILED\n", g_fail, g_checks);
    return 1;
}
