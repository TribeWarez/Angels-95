// CSG + collision-query tests — standalone, no raylib dependency.
//   make test_csg
//   g++ -std=c++20 tests/Csg.test.cpp Source/Physics/OzBsp.cpp Source/Physics/WorldChunk.cpp
//
// This suite exists because the collision pipeline had NO coverage at all, and
// three of its bugs were only found by reading it:
//   * MAX_SPLITS was checked against the TOTAL solid count, so any world with
//     more than ~58 solids silently stopped carving (the 500-add test below).
//   * The obstacle query passed the player's X max to a `worldZ` parameter, so
//     walls only collided when |x - z| happened to be small (the box tests).
//   * Rotated brushes collided as their UNROTATED box (the RotatedAabbY tests).
#include "../Source/Physics/OzBsp.hpp"
#include "../Source/Physics/WorldChunk.hpp"
#include "../Source/Physics/DynamicCollision.hpp"
#include <cstdio>
#include <cmath>
#include <vector>

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { g_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_NEAR(a, b, eps) do { \
    g_checks++; \
    float _d = (a) - (b); if (_d < 0) _d = -_d; \
    if (_d > (eps)) { g_fail++; printf("  FAIL %s:%d  %s ~= %s (%.4f vs %.4f)\n", \
        __FILE__, __LINE__, #a, #b, (float)(a), (float)(b)); } \
} while (0)

static constexpr float kDeg2Rad = 3.14159265358979323846f / 180.0f;

static CsgBrush Box(float minX, float minY, float minZ,
                    float maxX, float maxY, float maxZ, CsgOp op) {
    CsgBrush b;
    b.minX = minX; b.minY = minY; b.minZ = minZ;
    b.maxX = maxX; b.maxY = maxY; b.maxZ = maxZ;
    b.op = op;
    return b;
}

// Does any solid exactly equal the given box?
static bool HasSolid(const CsgProcessor& c, float minX, float minY, float minZ,
                     float maxX, float maxY, float maxZ) {
    for (int i = 0; i < c.Count(); i++) {
        if (std::fabs(c.GetSolidMinX()[i] - minX) < 0.001f &&
            std::fabs(c.GetSolidMinY()[i] - minY) < 0.001f &&
            std::fabs(c.GetSolidMinZ()[i] - minZ) < 0.001f &&
            std::fabs(c.GetSolidMaxX()[i] - maxX) < 0.001f &&
            std::fabs(c.GetSolidMaxY()[i] - maxY) < 0.001f &&
            std::fabs(c.GetSolidMaxZ()[i] - maxZ) < 0.001f)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// CSG semantics
// ---------------------------------------------------------------------------
static int test_add_and_subtract() {
    printf("add / subtract\n");

    CsgProcessor c;
    c.Apply(Box(-5, -5, -5, 5, 5, 5, CsgOp::ADD));
    CHECK(c.Count() == 1);

    // Cut a slab right through the middle on X. That leaves two solids.
    c.Apply(Box(-1, -6, -6, 1, 6, 6, CsgOp::SUB));
    CHECK(c.Count() == 2);
    CHECK(HasSolid(c, -5, -5, -5, -1, 5, 5));
    CHECK(HasSolid(c, 1, -5, -5, 5, 5, 5));
    CHECK(c.overflow_count() == 0);

    // A subtractor that fully contains a solid removes it outright.
    CsgProcessor c2;
    c2.Apply(Box(0, 0, 0, 1, 1, 1, CsgOp::ADD));
    c2.Apply(Box(-10, -10, -10, 10, 10, 10, CsgOp::SUB));
    CHECK(c2.Count() == 0);

    // A subtractor that overlaps nothing leaves everything alone.
    CsgProcessor c3;
    c3.Apply(Box(0, 0, 0, 1, 1, 1, CsgOp::ADD));
    c3.Apply(Box(50, 50, 50, 60, 60, 60, CsgOp::SUB));
    CHECK(c3.Count() == 1);
    return 0;
}

static int test_intersect() {
    printf("intersect\n");

    CsgProcessor c;
    c.Apply(Box(0, 0, 0, 10, 10, 10, CsgOp::ADD));
    // Deliberately CLEAR of the intersect box below, so it must be dropped
    // rather than clipped: intersecting clips what overlaps and removes the rest.
    c.Apply(Box(20, 20, 20, 30, 30, 30, CsgOp::ADD));
    CHECK(c.Count() == 2);

    c.Apply(Box(2, 2, 2, 6, 6, 6, CsgOp::INTERSECT));
    CHECK(c.Count() == 1);
    CHECK(HasSolid(c, 2, 2, 2, 6, 6, 6));

    // A partially-overlapping solid must be CLIPPED to the overlap, not dropped.
    CsgProcessor c2;
    c2.Apply(Box(0, 0, 0, 10, 10, 10, CsgOp::ADD));
    c2.Apply(Box(5, 5, 5, 15, 15, 15, CsgOp::ADD));
    c2.Apply(Box(2, 2, 2, 6, 6, 6, CsgOp::INTERSECT));
    CHECK(c2.Count() == 2);
    CHECK(HasSolid(c2, 2, 2, 2, 6, 6, 6));   // first clipped to the overlap
    CHECK(HasSolid(c2, 5, 5, 5, 6, 6, 6));   // second clipped to ITS overlap
    return 0;
}

// ---------------------------------------------------------------------------
// The regression this suite was written for.
//
// MAX_SPLITS used to be compared against the TOTAL solid count
// (`m_solidMinX.size() + 6 > MAX_SPLITS`), so once a world held ~58 solids every
// later subtraction was skipped and the `sub` carved nothing at all - while its
// render mesh still drew, so the author saw a hole and fell through a wall. Any
// world built mostly from `add` brushes was affected, which is every shipped
// world (none of them author a single `sub`).
// ---------------------------------------------------------------------------
static int test_many_solids_still_carve() {
    printf("per-brush budget (500 adds, then a sub)\n");

    CsgProcessor c;
    // 500 separate solids, deliberately clear of the one we will carve.
    for (int i = 0; i < 500; i++) {
        float x = 100.0f + (float)i * 3.0f;
        c.Apply(Box(x, 0, 0, x + 2, 2, 2, CsgOp::ADD));
    }
    CHECK(c.Count() == 500);

    // Now carve a slab through a fresh solid. On the old code this was skipped
    // because 500 + 6 > 64.
    c.Apply(Box(0, 0, 0, 10, 10, 10, CsgOp::ADD));
    CHECK(c.Count() == 501);
    c.Apply(Box(4, -1, -1, 6, 11, 11, CsgOp::SUB));

    CHECK(c.overflow_count() == 0);
    CHECK(c.Count() == 502);                     // one solid became two
    CHECK(HasSolid(c, 0, 0, 0, 4, 10, 10));
    CHECK(HasSolid(c, 6, 0, 0, 10, 10, 10));
    return 0;
}

// The budget is per BRUSH: a second sub gets a fresh budget even after the first
// one used its own.
static int test_budget_is_per_brush() {
    printf("budget is per brush\n");

    CsgProcessor c(8);   // tiny budget so the guard is reachable in a test

    // Ten solids in a row, then a slab that PARTIALLY cuts through all of them.
    // A cutter that fully contains a solid takes the removal path (no split, no
    // growth), so it has to cut through the middle for the budget to matter.
    for (int i = 0; i < 10; i++)
        c.Apply(Box((float)i, 0, 0, (float)i + 1, 10, 10, CsgOp::ADD));
    const int before = c.Count();
    CHECK(before == 10);

    c.Apply(Box(0.5f, 4, -1, 9.5f, 6, 11, CsgOp::SUB));

    // The brush stopped early rather than growing without bound...
    CHECK(c.overflow_count() == 1);
    CHECK(c.Count() <= before + 8 + 6);

    // ...and the visible consequence is a cut the author drew that is NOT there:
    // at least one overlapping solid is still whole.
    CHECK(HasSolid(c, 0, 0, 0, 1, 10, 10));

    // A LATER brush is unaffected by the exhausted budget, because the budget is
    // per brush rather than a running total.
    CsgProcessor c2(8);
    c2.Apply(Box(0, 0, 0, 10, 10, 10, CsgOp::ADD));
    c2.Apply(Box(0.5f, -1, -1, 9.5f, 11, 11, CsgOp::SUB));
    CHECK(c2.overflow_count() == 0);
    CHECK(c2.Count() == 2);
    return 0;
}

// ---------------------------------------------------------------------------
// Rotation-aware conservative AABB.
//
// The signs must mirror raylib's MatrixRotateY row-vector form. A 90-degree
// rotation is used because its result is unambiguous: x' = z, z' = -x.
// ---------------------------------------------------------------------------
static int test_rotated_aabb() {
    printf("RotatedAabbY\n");

    // Identity rotation is a plain scale + translate.
    AabbBox local{-1, 0, -2, 1, 3, 2};
    AabbBox r0 = RotatedAabbY(local, 0.0f, 10, 20, 30, 2.0f);
    CHECK_NEAR(r0.minX,  8.0f, 0.001f);
    CHECK_NEAR(r0.maxX, 12.0f, 0.001f);
    CHECK_NEAR(r0.minY, 20.0f, 0.001f);
    CHECK_NEAR(r0.maxY, 26.0f, 0.001f);
    CHECK_NEAR(r0.minZ, 26.0f, 0.001f);
    CHECK_NEAR(r0.maxZ, 34.0f, 0.001f);

    // 90 degrees about Y: x' = z, z' = -x.
    // Local x in [-1,1], z in [-2,2] -> world x from z = [-2,2],
    //                                    world z from -x = [-1,1].
    AabbBox r90 = RotatedAabbY(local, 90.0f * kDeg2Rad, 0, 0, 0, 1.0f);
    CHECK_NEAR(r90.minX, -2.0f, 0.001f);
    CHECK_NEAR(r90.maxX,  2.0f, 0.001f);
    CHECK_NEAR(r90.minZ, -1.0f, 0.001f);
    CHECK_NEAR(r90.maxZ,  1.0f, 0.001f);

    // A long thin beam rotated 90 degrees must still CONTAIN its own ends: an
    // unrotated box would report length 20 on X while the beam now runs along Z.
    AabbBox beam{-10, -0.5f, -0.5f, 10, 0.5f, 0.5f};
    AabbBox rb = RotatedAabbY(beam, 90.0f * kDeg2Rad, 0, 0, 0, 1.0f);
    CHECK_NEAR(rb.minX, -0.5f, 0.001f);
    CHECK_NEAR(rb.maxX,  0.5f, 0.001f);
    CHECK_NEAR(rb.minZ, -10.0f, 0.001f);
    CHECK_NEAR(rb.maxZ,  10.0f, 0.001f);

    // The conservative box must never be smaller than the shape's true extent:
    // for 45 degrees the X half-extent is (10*cos45 + 0.5*sin45) ~= 7.42.
    AabbBox r45 = RotatedAabbY(beam, 45.0f * kDeg2Rad, 0, 0, 0, 1.0f);
    const float expected = 10.0f * std::cos(45.0f * kDeg2Rad) + 0.5f * std::sin(45.0f * kDeg2Rad);
    CHECK(r45.maxX >= expected - 0.001f);
    return 0;
}

// ---------------------------------------------------------------------------
// The obstacle query must answer for the body's (x,z) BOX.
//
// The original call passed the player's X max into the `worldZ` parameter, so
// the query covered the cells around (x, x) instead of (x, z). Walls were found
// only when |x - z| fell inside the 3x3 cell window. These tests pin the
// contract that made that impossible to express.
// ---------------------------------------------------------------------------
static int test_volumes_near_box_uses_both_axes() {
    printf("GetVolumesNearBox\n");

    // One volume far off the x == z diagonal: World_endless_snow's spawn is
    // (x=0, z=-410), Dust_Ravine (-70, 0), Dessert_Dreams (-72, 0).
    std::vector<WorldChunkManager::Volume> vols = {
        {-2.0f, 0.0f, -412.0f, 2.0f, 10.0f, -408.0f},   // wall at z ~ -410
        {-72.0f, 0.0f, -1.0f, -68.0f, 10.0f, 1.0f},     // wall near Dust_Ravine spawn
    };
    WorldChunkManager mgr;
    mgr.Build(vols);

    std::vector<int> out;

    // A body standing at (0, -410): the wall must be found.
    mgr.GetVolumesNearBox(-0.5f, -410.5f, 0.5f, -409.5f, out);
    CHECK(out.size() == 1);
    CHECK(out.size() == 1 && out[0] == 0);

    // The OLD point query at (x, x) finds nothing - this is the bug, pinned.
    std::vector<int> oldWay;
    mgr.GetVolumesNear(0.0f, 0.0f, oldWay);
    CHECK(oldWay.empty());

    // And a body at Dust_Ravine's spawn finds its own wall, not the snow one.
    mgr.GetVolumesNearBox(-70.5f, -0.5f, -69.5f, 0.5f, out);
    CHECK(out.size() == 1);
    CHECK(out.size() == 1 && out[0] == 1);

    // A wide box spanning both must return both (the query covers the whole box,
    // not just its first corner).
    mgr.GetVolumesNearBox(-80.0f, -420.0f, 10.0f, 10.0f, out);
    CHECK(out.size() == 2);

    // Corner order must not matter.
    mgr.GetVolumesNearBox(0.5f, -409.5f, -0.5f, -410.5f, out);
    CHECK(out.size() == 1);
    return 0;
}

// ---------------------------------------------------------------------------
// Dynamic-collision side-list (doors / quantum obstacles).
//
// Toggling a door or quantum brush must not rebuild the CSG world: the side-list
// holds those AABBs separately and the collision query skips `enabled == false`.
// ---------------------------------------------------------------------------
static int test_dynamic_collision() {
    printf("DynamicCollision\n");
    using oz::physics::DynamicCollision;
    using oz::physics::DynamicBlocker;
    DynamicCollision dc;
    int door = dc.Add(DynamicBlocker{0, 0, 0, 4, 10, 4, true, 1});
    int plat = dc.Add(DynamicBlocker{10, 5, 10, 14, 6, 14, true, 2});
    CHECK(dc.Count() == 2);
    CHECK(dc.AnyEnabled());

    // Both enabled: both block and both support.
    CHECK(dc.OverlapsEnabled(1, 1, 1, 3, 3, 3));
    CHECK(dc.OverlapsEnabled(11, 5, 11, 13, 6, 13));
    CHECK_NEAR(dc.TopUnder(2, 2, -999.0f), 10.0f, 0.001f);   // door top
    CHECK_NEAR(dc.TopUnder(12, 12, -999.0f), 6.0f, 0.001f);  // platform top

    // Disable the door: it no longer blocks or supports, the platform still does.
    dc.SetEnabled(door, false);
    CHECK(!dc.OverlapsEnabled(1, 1, 1, 3, 3, 3));
    CHECK_NEAR(dc.TopUnder(2, 2, -999.0f), -999.0f, 0.001f);
    CHECK(dc.OverlapsEnabled(11, 5, 11, 13, 6, 13));

    // Re-enable: back to solid.
    dc.SetEnabled(door, true);
    CHECK(dc.OverlapsEnabled(1, 1, 1, 3, 3, 3));
    CHECK(dc.AnyEnabled());

    // SetAllEnabled(false) clears everything (e.g. world unload).
    dc.SetAllEnabled(false);
    CHECK(!dc.AnyEnabled());
    CHECK(!dc.OverlapsEnabled(11, 5, 11, 13, 6, 13));

    dc.Clear();
    CHECK(dc.Count() == 0);
    return 0;
}

int main() {
    printf("CSG / collision query tests\n");
    printf("===========================\n");

    int failures = 0;
    failures += test_add_and_subtract();
    failures += test_intersect();
    failures += test_many_solids_still_carve();
    failures += test_budget_is_per_brush();
    failures += test_rotated_aabb();
    failures += test_volumes_near_box_uses_both_axes();
    failures += test_dynamic_collision();

    printf("===========================\n");
    printf("%d/%d checks passed, %d failed\n",
           g_checks - g_fail, g_checks, g_fail);
    return failures ? 1 : 0;
}
