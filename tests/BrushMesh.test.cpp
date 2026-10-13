// Brush mesh-winding tests - standalone, no raylib dependency.
//   make test_brushmesh
//   g++ -std=c++20 tests/BrushMesh.test.cpp
//
// These exist because a cylinder shipped with BOTH CAPS INSIDE-OUT and nothing
// could see it: the geometry lives in OzOzoneLoader next to the UploadMesh call,
// which needs a GL context, so no headless test could run the builder. Under
// backface culling an inverted cap is not "a dark surface", it is NO surface -
// you looked straight into the tube. Winding is pure math and belongs here.
//
// The invariant every builder must satisfy:
//   for every triangle (a, b, c), cross(b - a, c - a) points AWAY from the shape.
// For the cylinder that means sides point radially outward (plus the slope term
// for a taper), the top cap +Y and the bottom cap -Y. For the plane it means the
// front face looks along the requested normal.
#include "../Source/World/BrushMesh.hpp"
#include <cstdio>
#include <cmath>
#include <algorithm>

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { g_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// Geometric normal of triangle `t`, normalised. (0,0,0) for a degenerate triangle.
static void TriNormal(const BrushMeshData& m, int t, float out[3]) {
    const float* p = m.vertices.data();
    const unsigned short i0 = m.indices[t * 3 + 0];
    const unsigned short i1 = m.indices[t * 3 + 1];
    const unsigned short i2 = m.indices[t * 3 + 2];
    const float ax = p[i0 * 3], ay = p[i0 * 3 + 1], az = p[i0 * 3 + 2];
    const float bx = p[i1 * 3], by = p[i1 * 3 + 1], bz = p[i1 * 3 + 2];
    const float cx = p[i2 * 3], cy = p[i2 * 3 + 1], cz = p[i2 * 3 + 2];
    const float e1x = bx - ax, e1y = by - ay, e1z = bz - az;
    const float e2x = cx - ax, e2y = cy - ay, e2z = cz - az;
    float nx = e1y * e2z - e1z * e2y;
    float ny = e1z * e2x - e1x * e2z;
    float nz = e1x * e2y - e1y * e2x;
    const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len < 1e-9f) { out[0] = out[1] = out[2] = 0.0f; return; }
    out[0] = nx / len; out[1] = ny / len; out[2] = nz / len;
}

// Centroid of triangle `t`.
static void TriCentroid(const BrushMeshData& m, int t, float out[3]) {
    const float* p = m.vertices.data();
    const unsigned short i0 = m.indices[t * 3 + 0];
    const unsigned short i1 = m.indices[t * 3 + 1];
    const unsigned short i2 = m.indices[t * 3 + 2];
    for (int k = 0; k < 3; k++)
        out[k] = (p[i0 * 3 + k] + p[i1 * 3 + k] + p[i2 * 3 + k]) / 3.0f;
}

static bool Near(float a, float b, float eps = 0.001f) {
    return (a > b ? a - b : b - a) <= eps;
}

// A cap triangle's normal is exactly +/-Y and has no radial component. Classifying
// by NORMAL rather than by the centroid's radius is load-bearing: a fan triangle's
// centroid sits well off the axis, so a radius filter counts caps as sides.
static bool IsCapNormal(const float n[3], bool wantDown = false) {
    if (!Near(n[0], 0.0f, 0.01f) || !Near(n[2], 0.0f, 0.01f)) return false;
    return wantDown ? Near(n[1], -1.0f, 0.01f) : Near(n[1], 1.0f, 0.01f);
}

// ---------------------------------------------------------------------------
// The reported bug: a cylinder had no lid. The top cap must face +Y and the
// bottom cap -Y, and BOTH must be present.
// ---------------------------------------------------------------------------
static void test_cylinder_caps_face_outward() {
    printf("cylinder caps\n");

    BrushMeshData m;
    const float rTop = 1.0f, rBot = 1.0f, h = 2.0f;
    oz::brushmesh::BuildCylinderMesh(m, rTop, rBot, h, 16);
    CHECK(!m.Empty());
    CHECK(m.VertexCount() == m.TriangleCount() * 3);

    // The mesh spans y in [-h, 0]: the authored cz is the TOP.
    float minY = 1e9f, maxY = -1e9f;
    for (int i = 0; i < m.VertexCount(); i++) {
        minY = std::min(minY, m.vertices[i * 3 + 1]);
        maxY = std::max(maxY, m.vertices[i * 3 + 1]);
    }
    CHECK(Near(minY, -h));
    CHECK(Near(maxY, 0.0f));

    int topTris = 0, bottomTris = 0, sideTris = 0;
    for (int t = 0; t < m.TriangleCount(); t++) {
        float n[3]; TriNormal(m, t, n);
        if (IsCapNormal(n))                  topTris++;     // +Y
        else if (IsCapNormal(n, true))       bottomTris++;  // -Y
        else                                 sideTris++;
    }

    // A closed 16-slice cylinder: 16 top fan tris, 16 bottom, 32 side.
    CHECK(topTris == 16);
    CHECK(bottomTris == 16);
    CHECK(sideTris == 32);

    // EVERY triangle must face outward. This is the assertion that fails when a
    // cap is wound inside-out: its geometric normal points the other way.
    int inverted = 0;
    for (int t = 0; t < m.TriangleCount(); t++) {
        float n[3];   TriNormal(m, t, n);
        float c[3];   TriCentroid(m, t, c);
        if (IsCapNormal(n) || IsCapNormal(n, true)) {
            // Cap: its Y component must agree with which end it is at.
            const float wantSign = (c[1] > -h * 0.5f) ? 1.0f : -1.0f;
            if (n[1] * wantSign < 0.9f) inverted++;
            continue;
        }
        // Side: outward = the radial direction at the centroid.
        float rad[3] = {c[0], 0.0f, c[2]};
        const float rl = std::sqrt(rad[0] * rad[0] + rad[2] * rad[2]);
        if (rl < 1e-6f) continue;
        rad[0] /= rl; rad[2] /= rl;
        if (n[0] * rad[0] + n[2] * rad[2] < 0.5f) inverted++;
    }
    CHECK(inverted == 0);
}

// A cone: rTop = 0 has no top cap, and the side normals tilt outward+upward.
static void test_cylinder_taper() {
    printf("cylinder taper\n");

    BrushMeshData cone;
    oz::brushmesh::BuildCylinderMesh(cone, 0.0f, 1.0f, 1.0f, 12);
    CHECK(!cone.Empty());

    int caps = 0, sides = 0, tiltedUp = 0;
    for (int t = 0; t < cone.TriangleCount(); t++) {
        float n[3]; TriNormal(cone, t, n);
        if (IsCapNormal(n) || IsCapNormal(n, true)) { caps++; continue; }
        sides++;
        // Side normal for a cone from rBot=1 at y=-1 to rTop=0 at y=0:
        // normalize(radial*h, rBot - rTop) = normalize(radial, 1) -> +Y component.
        if (n[1] > 0.1f) tiltedUp++;
    }
    CHECK(caps == 12);          // bottom only: a cone needs no top lid
    CHECK(sides == 12);         // one triangle per slice: the apex triangle collapses
    CHECK(tiltedUp == sides);   // every side triangle leans outward-and-up

    // A straight cylinder's sides must have NO vertical component. Classified by
    // NORMAL, not by centroid radius - a cap triangle's centroid is well off the
    // axis (the fan's centre vertex does not pull it in), so a radius filter counts
    // caps as sides and this reads as a builder bug.
    BrushMeshData straight;
    oz::brushmesh::BuildCylinderMesh(straight, 2.0f, 2.0f, 3.0f, 12);
    int straightSides = 0;
    for (int t = 0; t < straight.TriangleCount(); t++) {
        float n[3]; TriNormal(straight, t, n);
        if (IsCapNormal(n) || IsCapNormal(n, true)) continue;
        straightSides++;
        CHECK(Near(n[1], 0.0f, 0.01f));
    }
    CHECK(straightSides == 24);
}

// ---------------------------------------------------------------------------
// The plane must face along its authored normal and lie in n . x = dist.
// ---------------------------------------------------------------------------
static void test_plane_orientation() {
    printf("plane orientation\n");

    const float normals[3][3] = {
        {0.0f, 1.0f, 0.0f},   // horizontal, up (the common case)
        {0.0f, 0.0f, 1.0f},   // vertical, facing +Z
        {1.0f, 1.0f, 0.0f},   // sloped; the builder normalises it
    };
    for (int k = 0; k < 3; k++) {
        BrushMeshData m;
        oz::brushmesh::BuildPlaneMesh(m, normals[k][0], normals[k][1], normals[k][2], 4.0f);
        CHECK(m.TriangleCount() == 2);

        float len = std::sqrt(normals[k][0] * normals[k][0] +
                              normals[k][1] * normals[k][1] +
                              normals[k][2] * normals[k][2]);
        float want[3] = {normals[k][0] / len, normals[k][1] / len, normals[k][2] / len};

        // Both triangles face along +n.
        for (int t = 0; t < 2; t++) {
            float n[3]; TriNormal(m, t, n);
            CHECK(Near(n[0], want[0]) && Near(n[1], want[1]) && Near(n[2], want[2]));
        }
        // Every vertex lies in the plane n . x = dist, and the quad is centred
        // on n * dist, so a plane authored with dist 0 sits where it was placed.
        for (int i = 0; i < m.VertexCount(); i++) {
            const float d = m.vertices[i * 3] * want[0] +
                            m.vertices[i * 3 + 1] * want[1] +
                            m.vertices[i * 3 + 2] * want[2];
            CHECK(Near(d, 4.0f));
        }
    }

    // A zero normal must not produce a degenerate quad.
    BrushMeshData fallback;
    oz::brushmesh::BuildPlaneMesh(fallback, 0.0f, 0.0f, 0.0f, 0.0f);
    CHECK(fallback.TriangleCount() == 2);
    float n[3]; TriNormal(fallback, 0, n);
    CHECK(Near(n[1], 1.0f));   // falls back to a horizontal plane
}

// ---------------------------------------------------------------------------
// A pyramid: four sides leaning up-and-out, one base facing -Y, and every face
// non-degenerate. Its six faces were all wound inside-out (see-through shape) with
// hard-coded (0, 0.5, 0) normals, which no shipped world exercised.
// ---------------------------------------------------------------------------
static void test_pyramid() {
    printf("pyramid\n");

    const float w = 2.0f, d = 2.0f, h = 3.0f;
    BrushMeshData m;
    oz::brushmesh::BuildPyramidMesh(m, w, d, h);
    CHECK(m.TriangleCount() == 6);          // 4 sides + 2 base
    CHECK(m.VertexCount() == 18);

    // Spans y in [-h, 0]: the authored cz is the top, same as the cylinder.
    float minY = 1e9f, maxY = -1e9f;
    for (int i = 0; i < m.VertexCount(); i++) {
        minY = std::min(minY, m.vertices[i * 3 + 1]);
        maxY = std::max(maxY, m.vertices[i * 3 + 1]);
    }
    CHECK(Near(minY, -h));
    CHECK(Near(maxY, 0.0f));

    int sides = 0, baseTris = 0;
    for (int t = 0; t < m.TriangleCount(); t++) {
        float n[3];   TriNormal(m, t, n);
        float c[3];   TriCentroid(m, t, c);

        // No triangle may be degenerate: a zero-area face has no normal, so it can
        // neither shade nor cull correctly.
        const float nlen = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        CHECK(Near(nlen, 1.0f, 0.01f));

        // The vertex normals must equal the face normals - the old builder's were
        // (0, 0.5, 0) regardless of the face.
        const float* vn = &m.normals[m.indices[t * 3] * 3];
        CHECK(Near(vn[0], n[0]) && Near(vn[1], n[1]) && Near(vn[2], n[2]));

        if (Near(n[1], -1.0f, 0.01f)) {
            baseTris++;
            CHECK(Near(c[1], -h));                       // it IS the base
            continue;
        }
        sides++;
        // A side face's normal must agree with the outward radial direction...
        float rad[3] = {c[0], 0.0f, c[2]};
        const float rl = std::sqrt(rad[0] * rad[0] + rad[2] * rad[2]);
        CHECK(rl > 1e-4f);
        rad[0] /= rl; rad[2] /= rl;
        CHECK(n[0] * rad[0] + n[2] * rad[2] > 0.3f);
        // ...and lean upward, because the face narrows toward the apex.
        CHECK(n[1] > 0.1f);
    }
    CHECK(sides == 4);
    CHECK(baseTris == 2);
}

int main() {
    printf("Brush mesh winding tests\n");
    printf("========================\n");

    test_cylinder_caps_face_outward();
    test_cylinder_taper();
    test_pyramid();
    test_plane_orientation();

    printf("========================\n");
    printf("%d/%d checks passed, %d failed\n", g_checks - g_fail, g_checks, g_fail);
    return g_fail ? 1 : 0;
}
