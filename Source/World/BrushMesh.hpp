#pragma once
#include <vector>
#include <cmath>

// ============================================================================
// BrushMesh.hpp
//
// CPU-side geometry for the OZONE primitives whose meshes are built by hand
// rather than by raylib's generators (GenMeshCylinder is uniform, so it cannot
// express a taper, and GenMeshPlane is always horizontal).
//
// RAYLIB-FREE ON PURPOSE. This geometry used to be assembled inline in
// OzOzoneLoader next to the UploadMesh call, where nothing could check it:
// building the raylib Mesh needs a GL context, so a headless test could not run
// it. The cylinder then shipped with BOTH caps wound inside-out - the top cap
// faced down, the bottom cap faced up - which is invisible under backface
// culling, so a cylinder simply had no lid: you looked straight into the tube
// and saw the far wall's interior. Nothing about that is detectable by reading
// the loader; it is detectable in one assert. Winding belongs in a test.
//
// CONVENTION: raylib is Y-up and row-vector (v' = v * M). Triangles are wound
// counter-clockwise as seen from OUTSIDE the shape, which is what the renderer's
// `glFrontFace(GL_CCW)` + backface culling expect. A test can verify any builder
// here by taking each triangle's geometric normal (cross(b-a, c-a)) and requiring
// it to point away from the shape's interior - see tests/BrushMesh.test.cpp.
// ============================================================================

// Parallel arrays, ready to be copied into a raylib Mesh and uploaded.
struct BrushMeshData {
    std::vector<float> vertices;           // 3 floats per vertex (x, y, z)
    std::vector<float> normals;            // 3
    std::vector<float> texcoords;          // 2 (u, v)
    std::vector<unsigned short> indices;

    int  VertexCount()   const { return (int)(vertices.size() / 3); }
    int  TriangleCount() const { return (int)(indices.size() / 3); }
    bool Empty()         const { return indices.empty(); }
};

namespace oz::brushmesh {

inline constexpr float kPi = 3.14159265358979323846f;

inline void Emit(BrushMeshData& m, float px, float py, float pz,
                 float nx, float ny, float nz, float u, float v) {
    m.vertices.push_back(px); m.vertices.push_back(py); m.vertices.push_back(pz);
    m.normals.push_back(nx);  m.normals.push_back(ny);  m.normals.push_back(nz);
    m.texcoords.push_back(u); m.texcoords.push_back(v);
}

// ---------------------------------------------------------------------------
// Tapered cylinder / cone frustum.
//
// AXIS: spans y in [-h, 0], so the LOCAL ORIGIN IS ON THE TOP CAP. That is the
// OZONE convention - the authored `cz` is a `cyl`'s top, not its centre or its
// base (see Wiki/World-Format-OZONE.md and the level comments it cites).
//
// SIDE NORMALS follow the frustum slope: the surface tangent in the (radial, y)
// plane is (1, dr/dy) with dr/dy = (rTop - rBot)/h, so the outward normal is
// normalize(radial * h, rBot - rTop). A straight cylinder reduces to the radial
// direction, which is what the uniform mesh used to give.
//
// CAPS are triangle fans with normals exactly +/-Y, and are omitted when the
// matching radius is 0 (a cone needs no top lid).
// ---------------------------------------------------------------------------
inline void BuildCylinderMesh(BrushMeshData& out, float rTop, float rBot,
                              float h, int slices) {
    if (slices < 3) slices = 16;
    if (h <= 0.0f) h = 0.001f;
    if (rTop < 0.0f) rTop = 0.0f;
    if (rBot < 0.0f) rBot = 0.0f;

    const float yTop = 0.0f;
    const float yBot = -h;

    // --- Sides ---
    // U runs once around the circumference, V runs 0..1 down the height, so a
    // brush-wide texScaleU/V tiles the tube the way it did before.
    const float slopeScale = 1.0f / std::sqrt(h * h + (rTop - rBot) * (rTop - rBot));
    for (int i = 0; i < slices; i++) {
        const float a0 = (float)i / (float)slices * 2.0f * kPi;
        const float a1 = (float)(i + 1) / (float)slices * 2.0f * kPi;
        const float c0 = std::cos(a0), s0 = std::sin(a0);
        const float c1 = std::cos(a1), s1 = std::sin(a1);

        // Outward normal for this angular segment.
        const float n0x = c0 * h * slopeScale;
        const float n0y = (rBot - rTop) * slopeScale;
        const float n0z = s0 * h * slopeScale;
        const float n1x = c1 * h * slopeScale;
        const float n1y = n0y;
        const float n1z = s1 * h * slopeScale;

        const float u0 = (float)i / (float)slices;
        const float u1 = (float)(i + 1) / (float)slices;

        // Wound so the outward face is front-facing. Verified by
        // tests/BrushMesh.test.cpp: for the segment a0=0..90 on a unit cylinder
        // the first triangle's normal comes out ~(0.707, 0, 0.707), i.e. the
        // outward direction at the segment's midpoint.
        //
        // A cone closes to a POINT at one end, so one of the quad's two triangles
        // collapses there (its two vertices are the same apex) and is skipped. It
        // would rasterise nothing either way, but a zero-area triangle has no
        // defined normal, which makes "every side triangle faces outward" false
        // for a reason that has nothing to do with winding.
        const bool botIsPoint = (rBot <= 1e-6f);
        const bool topIsPoint = (rTop <= 1e-6f);

        if (!botIsPoint) {
            Emit(out, c1 * rBot, yBot, s1 * rBot, n1x, n1y, n1z, u1, 1.0f);
            Emit(out, c0 * rBot, yBot, s0 * rBot, n0x, n0y, n0z, u0, 1.0f);
            Emit(out, c0 * rTop, yTop, s0 * rTop, n0x, n0y, n0z, u0, 0.0f);
        }
        if (!topIsPoint) {
            Emit(out, c1 * rBot, yBot, s1 * rBot, n1x, n1y, n1z, u1, 1.0f);
            Emit(out, c0 * rTop, yTop, s0 * rTop, n0x, n0y, n0z, u0, 0.0f);
            Emit(out, c1 * rTop, yTop, s1 * rTop, n1x, n1y, n1z, u1, 0.0f);
        }
    }

    // --- Caps ---
    // The fan order is NOT symmetric between the two caps, and that asymmetry is
    // the whole bug this file exists to prevent. For a triangle
    // (centre, p0, p1) with p_i = (r*cos a_i, y, r*sin a_i), the geometric normal
    // cross(p0-c, p1-c) is -Y * r^2 * sin(a1 - a0), i.e. it points DOWN for
    // a1 > a0. So:
    //   top cap    (+Y) needs (centre, p1, p0)
    //   bottom cap (-Y) needs (centre, p0, p1)
    // Writing them the same way round - which reads as the "tidy" thing to do -
    // turns one of them inside out.
    auto emitCap = [&](float r, float y, float ny) {
        if (r <= 0.0f) return;
        const bool up = (ny > 0.0f);
        for (int i = 0; i < slices; i++) {
            const float a0 = (float)i / (float)slices * 2.0f * kPi;
            const float a1 = (float)(i + 1) / (float)slices * 2.0f * kPi;
            const float c0 = std::cos(a0), s0 = std::sin(a0);
            const float c1 = std::cos(a1), s1 = std::sin(a1);
            // Disc UVs, so a cap samples the texture instead of one texel.
            const float u0 = c0 * 0.5f + 0.5f, v0 = s0 * 0.5f + 0.5f;
            const float u1 = c1 * 0.5f + 0.5f, v1 = s1 * 0.5f + 0.5f;

            Emit(out, 0.0f, y, 0.0f, 0.0f, ny, 0.0f, 0.5f, 0.5f);
            if (up) {
                Emit(out, c1 * r, y, s1 * r, 0.0f, ny, 0.0f, u1, v1);
                Emit(out, c0 * r, y, s0 * r, 0.0f, ny, 0.0f, u0, v0);
            } else {
                Emit(out, c0 * r, y, s0 * r, 0.0f, ny, 0.0f, u0, v0);
                Emit(out, c1 * r, y, s1 * r, 0.0f, ny, 0.0f, u1, v1);
            }
        }
    };
    emitCap(rTop, yTop, +1.0f);
    emitCap(rBot, yBot, -1.0f);

    out.indices.reserve(out.VertexCount());
    for (int i = 0; i < out.VertexCount(); i++)
        out.indices.push_back((unsigned short)i);
}

// ---------------------------------------------------------------------------
// Oriented quad in the plane `n . x = dist`.
//
// The authored normal is ENGINE-space (the loader converts from OZONE's Z-up),
// and `halfSize` is half the quad's edge - 5 for the 10x10 patch the loader has
// always produced. The quad is centred on `n * dist`, so a plane authored with
// dist 0 sits on the brush's own position.
//
// Winding: with u = normalize(cross(helper, n)) and v = cross(n, u), the corner
// order below gives cross(b-a, c-a) = 4*halfSize^2 * (u x v) = ... * n, i.e. the
// front face looks along +n. Verified in tests/BrushMesh.test.cpp.
// ---------------------------------------------------------------------------
inline void BuildPlaneMesh(BrushMeshData& out, float nx, float ny, float nz,
                           float dist, float halfSize = 5.0f) {
    float len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len < 1e-6f) { nx = 0.0f; ny = 1.0f; nz = 0.0f; len = 1.0f; }
    nx /= len; ny /= len; nz /= len;

    // Tangent basis: a helper axis that is not parallel to the normal.
    float ux, uy, uz;
    if (std::fabs(ny) > 0.99f) { ux = 1.0f; uy = 0.0f; uz = 0.0f; }
    else                       { ux = 0.0f; uy = 1.0f; uz = 0.0f; }
    float ax = uy * nz - uz * ny;
    float ay = uz * nx - ux * nz;
    float az = ux * ny - uy * nx;
    const float al = std::sqrt(ax * ax + ay * ay + az * az);
    if (al < 1e-6f) { ax = 1.0f; ay = 0.0f; az = 0.0f; }
    else { ax /= al; ay /= al; az /= al; }
    const float vx = ny * az - nz * ay;
    const float vy = nz * ax - nx * az;
    const float vz = nx * ay - ny * ax;

    const float h = halfSize;
    const float cx = nx * dist, cy = ny * dist, cz = nz * dist;
    const float corners[4][3] = {
        {cx - ax * h - vx * h, cy - ay * h - vy * h, cz - az * h - vz * h},
        {cx + ax * h - vx * h, cy + ay * h - vy * h, cz + az * h - vz * h},
        {cx + ax * h + vx * h, cy + ay * h + vy * h, cz + az * h + vz * h},
        {cx - ax * h + vx * h, cy - ay * h + vy * h, cz - az * h + vz * h},
    };
    const float uvs[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    for (int i = 0; i < 4; i++)
        Emit(out, corners[i][0], corners[i][1], corners[i][2], nx, ny, nz,
             uvs[i][0], uvs[i][1]);
    const unsigned short idx[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; i++) out.indices.push_back(idx[i]);
}

// ---------------------------------------------------------------------------
// Square pyramid, apex up. Spans y in [-h, 0] like the cylinder, so the authored
// `cz` is the top.
//
// Normals are DERIVED FROM EACH TRIANGLE'S OWN WINDING. The loader hard-coded
// (0, 0.5, 0) for all four sides and (0, -1, 0) for the base, so a pyramid shaded
// from a direction that never existed - and the winding it shipped with was
// inside-out on all six faces, which is invisible under culling: the whole shape
// was see-through. Nothing in the shipped worlds uses `pyr`, which is the only
// reason that never got reported. AngelEd's toolbox can still place one.
// ---------------------------------------------------------------------------
inline void BuildPyramidMesh(BrushMeshData& out, float w, float d, float h) {
    if (h <= 0.0f) h = 0.001f;
    const float hw = w * 0.5f, hd = d * 0.5f;

    const float base[4][3] = {
        {-hw, -h, -hd},
        { hw, -h, -hd},
        { hw, -h,  hd},
        {-hw, -h,  hd},
    };
    const float apex[3] = {0.0f, 0.0f, 0.0f};

    // Emit a triangle whose normal comes from its own vertices, so the shading and
    // the culling can never disagree about which way the face points.
    auto tri = [&](const float a[3], const float b[3], const float c[3],
                   const float uva[2], const float uvb[2], const float uvc[2]) {
        const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        float n[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                      e1[2] * e2[0] - e1[0] * e2[2],
                      e1[0] * e2[1] - e1[1] * e2[0]};
        const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l > 1e-9f) { n[0] /= l; n[1] /= l; n[2] /= l; }
        Emit(out, a[0], a[1], a[2], n[0], n[1], n[2], uva[0], uva[1]);
        Emit(out, b[0], b[1], b[2], n[0], n[1], n[2], uvb[0], uvb[1]);
        Emit(out, c[0], c[1], c[2], n[0], n[1], n[2], uvc[0], uvc[1]);
    };

    // Sides. The order is (base[i+1], base[i], apex); the "tidy" (base[i],
    // base[i+1], apex) points INTO the solid, which is the bug being fixed here.
    for (int i = 0; i < 4; i++) {
        const int j = (i + 1) % 4;
        const float uvA[2] = {0.0f, 1.0f};   // base edge, start
        const float uvB[2] = {1.0f, 1.0f};   // base edge, end
        const float uvC[2] = {0.5f, 0.0f};   // apex
        tri(base[j], base[i], apex, uvA, uvB, uvC);
    }

    // Base, wound to face -Y: cross(b1-b0, b2-b0) is -Y for both triangles.
    const float uvs[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    tri(base[0], base[1], base[2], uvs[0], uvs[1], uvs[2]);
    tri(base[0], base[2], base[3], uvs[0], uvs[2], uvs[3]);

    out.indices.reserve(out.VertexCount());
    for (int i = 0; i < out.VertexCount(); i++)
        out.indices.push_back((unsigned short)i);
}

}  // namespace oz::brushmesh
