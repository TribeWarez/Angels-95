#pragma once
#include <vector>
#include <cstdint>
#include <cmath>

// CSG brush operation — determines how this brush combines with others
enum class CsgOp : uint8_t {
    SOLID = 0,      // ordinary additive brush (no operation)
    ADD,            // additive volume
    SUB,            // subtractive (carves void from existing solids)
    INTERSECT,      // keeps only volume common to existing solids
    DE_RESC         // same as SUB (remove volume)
};

// A single CSG brush: an AABB with a CSG operation
struct CsgBrush {
    CsgOp op = CsgOp::SOLID;
    float minX = 0, minY = 0, minZ = 0;
    float maxX = 0, maxY = 0, maxZ = 0;
};

// A plain axis-aligned box, shared by the CSG and the rotation helper so a
// caller can hand one between them without a conversion.
struct AabbBox {
    float minX = 0, minY = 0, minZ = 0, maxX = 0, maxY = 0, maxZ = 0;
};

// Conservative world-space AABB of `local` scaled, rotated about the Y axis and
// translated - the SAME composition the renderer applies to a brush
// (OzoneLoader's RenderableModelMatrix / DrawModelEx: scale, then rotate about
// the brush's own origin, then translate).
//
// Collision is AABB-only, so a rotated brush cannot be represented exactly.
// This is the tightest axis-aligned box that CONTAINS the rotated shape: it
// never under-covers (under-covering would let the player clip through the ends
// of a rotated beam) and over-covers only the corners.
//
// The Y-rotation signs mirror raylib's MatrixRotateY row-vector form exactly
// (m0=cos, m2=-sin, m8=sin, m10=cos), i.e. x' = x*cos + z*sin, z' = -x*sin + z*cos.
inline AabbBox RotatedAabbY(const AabbBox& local, float rotationY,
                            float px, float py, float pz, float scale) {
    if (rotationY == 0.0f) {
        return AabbBox{ px + local.minX * scale, py + local.minY * scale, pz + local.minZ * scale,
                        px + local.maxX * scale, py + local.maxY * scale, pz + local.maxZ * scale };
    }

    const float c = std::cos(rotationY);
    const float s = std::sin(rotationY);

    AabbBox out;
    bool first = true;
    for (int i = 0; i < 8; i++) {
        const float x = ((i & 1) ? local.maxX : local.minX) * scale;
        const float y = ((i & 2) ? local.maxY : local.minY) * scale;
        const float z = ((i & 4) ? local.maxZ : local.minZ) * scale;
        const float wx = px + x * c + z * s;
        const float wy = py + y;
        const float wz = pz - x * s + z * c;
        if (first) {
            out = AabbBox{wx, wy, wz, wx, wy, wz};
            first = false;
        } else {
            if (wx < out.minX) out.minX = wx;
            if (wy < out.minY) out.minY = wy;
            if (wz < out.minZ) out.minZ = wz;
            if (wx > out.maxX) out.maxX = wx;
            if (wy > out.maxY) out.maxY = wy;
            if (wz > out.maxZ) out.maxZ = wz;
        }
    }
    return out;
}

// CsgProcessor — applies CSG operations to a set of AABB brushes
// producing a final list of solid collision volumes.
//
// Algorithm (simplified AABB CSG):
//   ADD       → push the AABB as a solid volume
//   SUB/DERESC→ for each existing solid overlapping this brush,
//               split the solid into up to 6 sub-AABBs (the parts
//               outside the subtractor)
//   INTERSECT → for each existing solid overlapping this brush,
//               keep only the overlapping portion
class CsgProcessor {
public:
    // Per-BRUSH carve budget: how many volumes ONE sub/intersect brush may add
    // to the solid list before the rest of that brush's carving is abandoned.
    //
    // It was documented as a per-brush cap but implemented against the TOTAL
    // solid count (`size() + 6 > MAX_SPLITS`), so once a world held ~58 solids
    // EVERY later subtraction was skipped and the `sub` carved nothing - an
    // `add`-heavy level could never carve a doorway, and the failure looked like
    // "my brush renders but I fall through it". The budget is now measured
    // against the growth caused by the brush being applied, so a world may hold
    // as many solids as it likes and each sub/intersect still carves.
    //
    // Push() is deliberately NOT capped: an `add` contributes exactly one
    // volume, so capping it would silently drop authored geometry rather than
    // bound pathological growth. Only the splitting operations can multiply.
    static constexpr int MAX_SPLITS = 4096;

    // `maxSplits` overrides the budget. Exists so a test can exercise the guard
    // at a size it can actually reach; production always uses the default.
    explicit CsgProcessor(int maxSplits = MAX_SPLITS) : m_maxSplits(maxSplits) {}

    void Clear();
    void Apply(const CsgBrush& brush);

    int Count() const { return (int)m_solidMinX.size(); }
    const float* GetSolidMinX() const { return m_solidMinX.data(); }
    const float* GetSolidMinY() const { return m_solidMinY.data(); }
    const float* GetSolidMinZ() const { return m_solidMinZ.data(); }
    const float* GetSolidMaxX() const { return m_solidMaxX.data(); }
    const float* GetSolidMaxY() const { return m_solidMaxY.data(); }
    const float* GetSolidMaxZ() const { return m_solidMaxZ.data(); }

    // Convenience: fill a vector of BoundingBox-like structs
    struct Volume { float minX, minY, minZ, maxX, maxY, maxZ; };
    void GetVolumes(std::vector<Volume>& out) const;

    // Overflow protection: merge adjacent coplanar AABBs into larger ones.
    // Returns number of merges performed.
    int MergePass();

    // How many times a brush's carving was abandoned because it would have
    // exceeded the per-brush budget, leaving the remaining solids WHOLE.
    // Non-zero means the world has `sub` brushes that carved only partly (or not
    // at all): geometry the author intended to cut stays solid and blocks the
    // player. Counted because a per-brush stderr line is easy to lose in a load
    // log and impossible to assert on; Clear() resets it with the rest of the
    // state.
    //
    // Not asserted internally: whether a given count is acceptable depends on the
    // level, and CsgProcessor has no opinion about level authoring. The editor
    // surfaces it through CommitBrushRenderable's existing "no solid" warning.
    int overflow_count() const { return m_overflowCount; }

private:
    struct AABB { float minX, minY, minZ, maxX, maxY, maxZ; };

    int m_maxSplits = MAX_SPLITS;

    std::vector<float> m_solidMinX, m_solidMinY, m_solidMinZ;
    std::vector<float> m_solidMaxX, m_solidMaxY, m_solidMaxZ;
    int m_overflowCount = 0;

    bool Overlaps(int idx, const AABB& brush) const;
    // How many solids in [0, uptoInclusive] overlap `brush`. Used only to report
    // how much a budget-exhausted brush left uncut.
    int CountOverlaps(int uptoInclusive, const AABB& brush) const;
    void RemoveAt(int idx);
    void Push(const AABB& aabb);
    // `brushGrowth` is how many volumes the brush being applied has already added.
    // Returns false when the per-brush budget would be exceeded, in which case the
    // solid is left WHOLE (the caller reports and stops carving with this brush).
    bool Subtract(int idx, const AABB& sub, int brushGrowth);
    void Intersect(int idx, const AABB& isect);
};
