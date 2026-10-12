#pragma once
#include <cstddef>
#include <vector>

// ---------------------------------------------------------------------------
// AutoConvex - voxel-derived convex collision proxies for a mesh or brush.
//
// The engine's collision world is AABB-only: CsgProcessor (see OzBsp.hpp) takes
// axis-aligned boxes and nothing else. A placed Mesh.Static prop therefore has NO
// collision at all - the player walks straight through it - and a brush only gets
// collision from the AABB of its generated primitive, which over-blocks concave
// shapes.
//
// AutoConvex closes that gap the only way an AABB collision world allows: it
// slices the source geometry into a grid of small boxes and keeps the cells a
// triangle actually passes through. The result is a stack of convex boxes whose
// union closely follows the silhouette, which CsgProcessor can then consume
// directly (its MergePass collapses the coplanar ones again).
//
// Deliberately raylib-free: the only input is a bare vertex array, so this is
// unit-testable in the headless harness and addable to the tests/ suite.
// ---------------------------------------------------------------------------

// One generated collision box, world-space.
struct ConvexBox {
    float minX = 0, minY = 0, minZ = 0;
    float maxX = 0, maxY = 0, maxZ = 0;
};

struct AutoConvexParams {
    // Edge length of one voxel. Smaller = tighter fit and far more boxes.
    float cellSize = 0.5f;
    // Hard cap on emitted boxes. Exceeding it stops generation and the caller
    // is expected to report the truncation rather than silently drop geometry:
    // a proxy that is missing parts of a wall is a bug an author cannot see.
    int maxBoxes = 512;
    // Fraction of a cell's edge by which boxes are grown. Collision already
    // inflates sub-0.02-thick brushes, but generated cells sit exactly on the
    // mesh surface, so without a little overlap a player can slip through the
    // seam between two adjacent cells while running along the wall.
    float grow = 0.02f;
};

// Why a build produced nothing. The editor used to attribute EVERY failure to
// the box budget, so a >4096-cell grid or empty geometry sent the author
// hunting for a budget they were nowhere near.
enum class AutoConvexFail {
    NONE = 0,
    BAD_INPUT,        // null / too few vertices, or a non-positive cell size
    DEGENERATE_GRID,  // the source has no extent, so no cell can be generated
    GRID_TOO_BIG,     // more than 4096 cells along an axis
    OVER_BUDGET,      // would emit more than params.maxBoxes
};

const char* AutoConvexFailName(AutoConvexFail f);

// Build convex proxies for the triangle soup in `verts`.
//
// `vertFloatCount` is a count of FLOATS (3 per vertex), not of vertices, and
// the vertices are expected to already be transformed into world space. The
// spelling is deliberate: an earlier "vertexCount" that was really a float
// count made the triangle loop walk off the end of the buffer by 3x and read
// uninitialised memory, which showed up as proxies appearing inside solid
// geometry and as counts that changed between runs.
//
// Returns the number of boxes appended to `out`. Returns 0 and leaves `out`
// untouched when the inputs are unusable or when the grid would exceed
// `params.maxBoxes` - refusing outright is safer than half a collision hull.
// `failOut`, when given, receives the reason, so the caller can say WHICH of
// those it was instead of guessing.
int AutoConvex_Build(const float* verts, int vertFloatCount,
                     const AutoConvexParams& params, std::vector<ConvexBox>& out,
                     AutoConvexFail* failOut = nullptr);

// Count how many boxes the current parameters would emit, without building them.
// Lets the editor tell the author "this mesh needs 4000 proxies" before
// committing to an expensive voxelisation pass.
int AutoConvex_CountCells(const float* verts, int vertFloatCount,
                          const AutoConvexParams& params);