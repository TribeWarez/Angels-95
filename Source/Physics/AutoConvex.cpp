#include "AutoConvex.hpp"
#include <cmath>
#include <algorithm>

// ---------------------------------------------------------------------------
// AutoConvex - triangle/AABB voxelisation
//
// Raylib-free on purpose: the only input is a bare float array, so this file
// compiles into the headless test harness with no engine dependency.
// ---------------------------------------------------------------------------
namespace {

struct Box {
    float minX, minY, minZ;
    float maxX, maxY, maxZ;
};

float FloorTo(float v, float step) {
    return (float)std::floor((double)(v / step)) * step;
}

float CeilTo(float v, float step) {
    return (float)std::ceil((double)(v / step)) * step;
}

// Triangle/box overlap via the separating axis theorem.
//
// The cheap alternative - "does this cell overlap the triangle's AABB" - would
// fill in every hollow in the model (the inside of a mug, the gap between two
// legs) and silently make those gaps solid. Testing the real triangle is what
// keeps the proxy set following the actual silhouette.
//
// SAT for triangle vs AABB needs 12 axes: the 3 box face normals (X, Y, Z) and
// the 9 edge-edge crosses cross(triangleEdge, boxAxis). The Akenine-Moller
// voxelisation paper folds these down to 3+3 by reordering the triangle into
// the positive octant, but that optimisation is easy to get subtly wrong (a
// mis-applied vertex swap makes the whole test reject everything), so the plain
// 12-axis form is used here: a few more dots per cell, no ordering assumptions.
bool TriangleOverlapsBox(const float* v, const Box& b) {
    // Translate so the box centre is the origin; every projection test then
    // reduces to "does the triangle span zero".
    const float cx = (b.minX + b.maxX) * 0.5f;
    const float cy = (b.minY + b.maxY) * 0.5f;
    const float cz = (b.minZ + b.maxZ) * 0.5f;
    const float h[3] = {(b.maxX - b.minX) * 0.5f,
                        (b.maxY - b.minY) * 0.5f,
                        (b.maxZ - b.minZ) * 0.5f};

    float p[3][3];
    for (int i = 0; i < 3; i++) {
        p[i][0] = v[i * 3 + 0] - cx;
        p[i][1] = v[i * 3 + 1] - cy;
        p[i][2] = v[i * 3 + 2] - cz;
    }

    // Axis 1-3: the box faces.
    for (int a = 0; a < 3; a++) {
        const float lo = std::min(p[0][a], std::min(p[1][a], p[2][a]));
        const float hi = std::max(p[0][a], std::max(p[1][a], p[2][a]));
        if (lo > h[a] || hi < -h[a]) return false;
    }

    // Axis 4-12: cross(triangleEdge, boxAxis). The box edges are the unit
    // vectors, so each cross product has one term dropped.
    for (int e = 0; e < 3; e++) {
        const int a = e;
        const int o = (e + 1) % 3;
        const float dx = p[o][0] - p[a][0];
        const float dy = p[o][1] - p[a][1];
        const float dz = p[o][2] - p[a][2];

        // cross((dx,dy,dz), (1,0,0)) = (0, dz, -dy)
        // cross((dx,dy,dz), (0,1,0)) = (-dz, 0, dx)
        // cross((dx,dy,dz), (0,0,1)) = (dy, -dx, 0)
        const float ax[3][3] = {
            {   0.0f,   dz,  -dy},
            {  -dz,   0.0f,  dx},
            {   dy,   -dx, 0.0f},
        };
        for (int k = 0; k < 3; k++) {
            const float* n = ax[k];
            const float t0 = n[0] * p[0][0] + n[1] * p[0][1] + n[2] * p[0][2];
            const float t1 = n[0] * p[1][0] + n[1] * p[1][1] + n[2] * p[1][2];
            const float t2 = n[0] * p[2][0] + n[1] * p[2][1] + n[2] * p[2][2];
            const float lo = std::min(t0, std::min(t1, t2));
            const float hi = std::max(t0, std::max(t1, t2));
            // The box's projection onto this axis, centred on the origin.
            const float rad = std::fabs(n[0]) * h[0] +
                              std::fabs(n[1]) * h[1] +
                              std::fabs(n[2]) * h[2];
            if (lo > rad || hi < -rad) return false;
        }
    }

    return true;
}

bool BuildFromCells(const float* verts, int vertFloatCount, const AutoConvexParams& params,
                    std::vector<ConvexBox>& out, bool countOnly,
                    AutoConvexFail* failOut) {
    if (failOut) *failOut = AutoConvexFail::NONE;
    if (!verts || vertFloatCount < 9 || params.cellSize <= 0.0f) {
        if (failOut) *failOut = AutoConvexFail::BAD_INPUT;
        return false;
    }

    const float s = params.cellSize;

    // World AABB of the source geometry. `vertFloatCount` counts FLOATS, so
    // step by 3 components per vertex - indexing verts[i * 3] here walked 3x
    // past the end of the buffer and inflated the grid with garbage.
    Box bounds = {verts[0], verts[1], verts[2], verts[0], verts[1], verts[2]};
    for (int i = 0; i + 2 < vertFloatCount; i += 3) {
        bounds.minX = std::min(bounds.minX, verts[i + 0]);
        bounds.minY = std::min(bounds.minY, verts[i + 1]);
        bounds.minZ = std::min(bounds.minZ, verts[i + 2]);
        bounds.maxX = std::max(bounds.maxX, verts[i + 0]);
        bounds.maxY = std::max(bounds.maxY, verts[i + 1]);
        bounds.maxZ = std::max(bounds.maxZ, verts[i + 2]);
    }

    // Snap outward to the grid so a cell boundary never lands inside the mesh.
    //
    // A zero-thickness axis gets exactly one cell CENTRED on the geometry
    // instead: floor/ceil of an equal min and max is zero cells, which silently
    // returned nothing and so left planes, card meshes and flat brushes with no
    // collision at all - the player falls straight through a floor decal.
    float lo[3] = {bounds.minX, bounds.minY, bounds.minZ};
    float hi[3] = {bounds.maxX, bounds.maxY, bounds.maxZ};
    for (int a = 0; a < 3; a++) {
        if (hi[a] - lo[a] < s) {
            float mid = (lo[a] + hi[a]) * 0.5f;
            lo[a] = mid - s * 0.5f;
            hi[a] = mid + s * 0.5f;
        } else {
            lo[a] = FloorTo(lo[a], s);
            hi[a] = CeilTo(hi[a], s);
        }
    }
    bounds.minX = lo[0]; bounds.maxX = hi[0];
    bounds.minY = lo[1]; bounds.maxY = hi[1];
    bounds.minZ = lo[2]; bounds.maxZ = hi[2];

    // Refuse an absurd grid before touching the triangle loop: a 128-unit floor
    // at 0.5 cells is 256^3 = 16M cells, and the loop below is O(cells *
    // triangles).
    const long long nx = (long long)std::llround((bounds.maxX - bounds.minX) / s);
    const long long ny = (long long)std::llround((bounds.maxY - bounds.minY) / s);
    const long long nz = (long long)std::llround((bounds.maxZ - bounds.minZ) / s);
    if (nx <= 0 || ny <= 0 || nz <= 0) {
        if (failOut) *failOut = AutoConvexFail::DEGENERATE_GRID;
        return false;
    }
    if (nx > 4096 || ny > 4096 || nz > 4096) {
        if (failOut) *failOut = AutoConvexFail::GRID_TOO_BIG;
        return false;
    }

    // 9 floats per triangle (3 vertices x 3 components).
    const int triCount = vertFloatCount / 9;
    const float grow = (params.grow > 0.0f) ? params.grow : 0.0f;

    for (long long ix = 0; ix < nx; ix++) {
        for (long long iy = 0; iy < ny; iy++) {
            for (long long iz = 0; iz < nz; iz++) {
                Box cell;
                cell.minX = bounds.minX + (float)ix * s;
                cell.minY = bounds.minY + (float)iy * s;
                cell.minZ = bounds.minZ + (float)iz * s;
                cell.maxX = cell.minX + s;
                cell.maxY = cell.minY + s;
                cell.maxZ = cell.minZ + s;

                bool hit = false;
                for (int t = 0; t < triCount && !hit; t++)
                    hit = TriangleOverlapsBox(verts + t * 9, cell);
                if (!hit) continue;

                if (countOnly) { out.push_back(ConvexBox{}); continue; }
                if ((int)out.size() >= params.maxBoxes) {
                    if (failOut) *failOut = AutoConvexFail::OVER_BUDGET;
                    return false;
                }
                ConvexBox cb;
                cb.minX = cell.minX - grow; cb.minY = cell.minY - grow; cb.minZ = cell.minZ - grow;
                cb.maxX = cell.maxX + grow; cb.maxY = cell.maxY + grow; cb.maxZ = cell.maxZ + grow;
                out.push_back(cb);
            }
        }
    }
    return true;
}

}  // namespace

const char* AutoConvexFailName(AutoConvexFail f) {
    switch (f) {
        case AutoConvexFail::NONE:           return "no failure";
        case AutoConvexFail::BAD_INPUT:      return "no usable geometry (or a non-positive cell size)";
        case AutoConvexFail::DEGENERATE_GRID:return "the geometry has no extent";
        case AutoConvexFail::GRID_TOO_BIG:   return "the voxel grid exceeds 4096 cells on an axis";
        case AutoConvexFail::OVER_BUDGET:    return "the mesh needs more proxies than the box budget";
    }
    return "unknown";
}

int AutoConvex_Build(const float* verts, int vertFloatCount,
                     const AutoConvexParams& params, std::vector<ConvexBox>& out,
                     AutoConvexFail* failOut) {
    std::vector<ConvexBox> built;
    if (!BuildFromCells(verts, vertFloatCount, params, built, /*countOnly=*/false, failOut))
        return 0;
    out.insert(out.end(), built.begin(), built.end());
    return (int)built.size();
}

int AutoConvex_CountCells(const float* verts, int vertFloatCount,
                          const AutoConvexParams& params) {
    // Same traversal, results discarded: the count is what the editor shows
    // before committing, so it must agree with what Build would emit.
    std::vector<ConvexBox> counted;
    if (!BuildFromCells(verts, vertFloatCount, params, counted, /*countOnly=*/true, nullptr)) return 0;
    return (int)counted.size();
}