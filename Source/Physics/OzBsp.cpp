#include "OzBsp.hpp"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdio>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
static inline bool AABB_Overlap(
    float aminX, float aminY, float aminZ, float amaxX, float amaxY, float amaxZ,
    float bminX, float bminY, float bminZ, float bmaxX, float bmaxY, float bmaxZ)
{
    return !(amaxX <= bminX || aminX >= bmaxX ||
             amaxY <= bminY || aminY >= bmaxY ||
             amaxZ <= bminZ || aminZ >= bmaxZ);
}

static inline bool AABB_Contains(
    float aminX, float aminY, float aminZ, float amaxX, float amaxY, float amaxZ,
    float bminX, float bminY, float bminZ, float bmaxX, float bmaxY, float bmaxZ)
{
    return aminX <= bminX && aminY <= bminY && aminZ <= bminZ &&
           amaxX >= bmaxX && amaxY >= bmaxY && amaxZ >= bmaxZ;
}

// ---------------------------------------------------------------------------
// CsgProcessor implementation
// ---------------------------------------------------------------------------
void CsgProcessor::Clear() {
    m_solidMinX.clear(); m_solidMinY.clear(); m_solidMinZ.clear();
    m_solidMaxX.clear(); m_solidMaxY.clear(); m_solidMaxZ.clear();
    m_overflowCount = 0;
}

bool CsgProcessor::Overlaps(int idx, const AABB& brush) const {
    return AABB_Overlap(
        m_solidMinX[idx], m_solidMinY[idx], m_solidMinZ[idx],
        m_solidMaxX[idx], m_solidMaxY[idx], m_solidMaxZ[idx],
        brush.minX, brush.minY, brush.minZ,
        brush.maxX, brush.maxY, brush.maxZ);
}

void CsgProcessor::RemoveAt(int idx) {
    int last = (int)m_solidMinX.size() - 1;
    if (idx < last) {
        m_solidMinX[idx] = m_solidMinX[last]; m_solidMinY[idx] = m_solidMinY[last]; m_solidMinZ[idx] = m_solidMinZ[last];
        m_solidMaxX[idx] = m_solidMaxX[last]; m_solidMaxY[idx] = m_solidMaxY[last]; m_solidMaxZ[idx] = m_solidMaxZ[last];
    }
    m_solidMinX.pop_back(); m_solidMinY.pop_back(); m_solidMinZ.pop_back();
    m_solidMaxX.pop_back(); m_solidMaxY.pop_back(); m_solidMaxZ.pop_back();
}

void CsgProcessor::Push(const AABB& aabb) {
    m_solidMinX.push_back(aabb.minX); m_solidMinY.push_back(aabb.minY); m_solidMinZ.push_back(aabb.minZ);
    m_solidMaxX.push_back(aabb.maxX); m_solidMaxY.push_back(aabb.maxY); m_solidMaxZ.push_back(aabb.maxZ);
}

// Subtract sub-volume from solid at idx, replacing it with up to 6 sub-AABBs.
// The sub-AABBs are the portions of the original solid that lie outside 'sub'.
//
// Returns false when this brush has already grown the solid list by as much as
// its budget allows, leaving the solid WHOLE. See the note on MAX_SPLITS.
bool CsgProcessor::Subtract(int idx, const AABB& sub, int brushGrowth) {
    float sx = m_solidMinX[idx], sy = m_solidMinY[idx], sz = m_solidMinZ[idx];
    float ex = m_solidMaxX[idx], ey = m_solidMaxY[idx], ez = m_solidMaxZ[idx];

    // If the subtractor completely contains the solid, remove it entirely
    if (AABB_Contains(sub.minX, sub.minY, sub.minZ, sub.maxX, sub.maxY, sub.maxZ,
                      sx, sy, sz, ex, ey, ez)) {
        RemoveAt(idx);
        return true;
    }

    // Per-brush budget. A split adds at most 5 net volumes (6 parts replace 1),
    // so refuse when that would exceed what THIS brush may add.
    //
    // This failure has to stay visible: the split not happening means the `sub`
    // carved less than the author intended, so a wall meant to have a doorway
    // stays solid and the player is blocked by geometry that looks open. The
    // caller logs each occurrence with the brush's bounds and how many
    // overlapping solids were left uncut.
    if (brushGrowth + 6 > m_maxSplits) return false;

    // Clip along each axis where the subtractor partially overlaps
    // We build up to 6 sub-AABBs (left/right on X, bottom/top on Y, back/front on Z)
    struct { float minX, minY, minZ, maxX, maxY, maxZ; } parts[6];
    int partCount = 0;

    // Left of sub on X: [sx, ex] × [sy, ey] × [sz, ez] ∩ X < sub.minX
    if (sx < sub.minX && ex > sub.minX) {
        parts[partCount++] = {sx, sy, sz, sub.minX, ey, ez};
    }
    // Right of sub on X: [sx, ex] × [sy, ey] × [sz, ez] ∩ X > sub.maxX
    if (ex > sub.maxX && sx < sub.maxX) {
        parts[partCount++] = {sub.maxX, sy, sz, ex, ey, ez};
    }
    // Below sub on Y: [sx, ex] × [sy, ey] × [sz, ez] ∩ Y < sub.minY
    if (sy < sub.minY && ey > sub.minY) {
        parts[partCount++] = {sx, sy, sz, ex, sub.minY, ez};
    }
    // Above sub on Y: [sx, ex] × [sy, ey] × [sz, ez] ∩ Y > sub.maxY
    if (ey > sub.maxY && sy < sub.maxY) {
        parts[partCount++] = {sx, sub.maxY, sz, ex, ey, ez};
    }
    // Behind sub on Z: [sx, ex] × [sy, ey] × [sz, ez] ∩ Z < sub.minZ
    if (sz < sub.minZ && ez > sub.minZ) {
        parts[partCount++] = {sx, sy, sz, ex, ey, sub.minZ};
    }
    // Front of sub on Z: [sx, ex] × [sy, ey] × [sz, ez] ∩ Z > sub.maxZ
    if (ez > sub.maxZ && sz < sub.maxZ) {
        parts[partCount++] = {sx, sy, sub.maxZ, ex, ey, ez};
    }

    // Replace the original solid with the first part, push the rest
    if (partCount > 0) {
        m_solidMinX[idx] = parts[0].minX; m_solidMinY[idx] = parts[0].minY; m_solidMinZ[idx] = parts[0].minZ;
        m_solidMaxX[idx] = parts[0].maxX; m_solidMaxY[idx] = parts[0].maxY; m_solidMaxZ[idx] = parts[0].maxZ;
        for (int i = 1; i < partCount; i++)
            Push({parts[i].minX, parts[i].minY, parts[i].minZ,
                  parts[i].maxX, parts[i].maxY, parts[i].maxZ});
    }
    return true;
}

int CsgProcessor::CountOverlaps(int uptoInclusive, const AABB& brush) const {
    if (uptoInclusive >= (int)m_solidMinX.size())
        uptoInclusive = (int)m_solidMinX.size() - 1;
    int n = 0;
    for (int i = 0; i <= uptoInclusive; i++)
        if (Overlaps(i, brush)) n++;
    return n;
}

// Intersect solid at idx with isect — keep only the overlapping portion
void CsgProcessor::Intersect(int idx, const AABB& isect) {
    float sx = m_solidMinX[idx], sy = m_solidMinY[idx], sz = m_solidMinZ[idx];
    float ex = m_solidMaxX[idx], ey = m_solidMaxY[idx], ez = m_solidMaxZ[idx];

    float newMinX = std::max(sx, isect.minX);
    float newMinY = std::max(sy, isect.minY);
    float newMinZ = std::max(sz, isect.minZ);
    float newMaxX = std::min(ex, isect.maxX);
    float newMaxY = std::min(ey, isect.maxY);
    float newMaxZ = std::min(ez, isect.maxZ);

    if (newMinX >= newMaxX || newMinY >= newMaxY || newMinZ >= newMaxZ) {
        RemoveAt(idx);  // no overlap → solid removed
    } else {
        m_solidMinX[idx] = newMinX; m_solidMinY[idx] = newMinY; m_solidMinZ[idx] = newMinZ;
        m_solidMaxX[idx] = newMaxX; m_solidMaxY[idx] = newMaxY; m_solidMaxZ[idx] = newMaxZ;
    }
}

void CsgProcessor::Apply(const CsgBrush& brush) {
    AABB b = {brush.minX, brush.minY, brush.minZ, brush.maxX, brush.maxY, brush.maxZ};

    switch (brush.op) {
        case CsgOp::ADD:
        case CsgOp::SOLID:
            Push(b);
            break;

        case CsgOp::SUB:
        case CsgOp::DE_RESC: {
            // The budget is per BRUSH, not per world: it is measured against how
            // much THIS brush has grown the solid list, so a level with hundreds
            // of solids still carves. See the note on MAX_SPLITS.
            const int brushStart = Count();
            bool exhausted = false;
            int leftUncut = 0;
            // Iterate in reverse so removal doesn't affect indices
            for (int i = (int)m_solidMinX.size() - 1; i >= 0; i--) {
                if (!Overlaps(i, b)) continue;
                if (!Subtract(i, b, Count() - brushStart)) {
                    exhausted = true;
                    leftUncut = CountOverlaps(i, b);
                    break;
                }
            }
            if (exhausted) {
                fprintf(stderr,
                        "[CSG] Per-brush budget reached (%d volumes): the sub/intersect at "
                        "(%.1f %.1f %.1f)-(%.1f %.1f %.1f) stopped carving with %d "
                        "overlapping solid(s) left UNCUT — those stay whole, so a cut the "
                        "author drew may still block the player.\n",
                        m_maxSplits, b.minX, b.minY, b.minZ, b.maxX, b.maxY, b.maxZ,
                        leftUncut);
                m_overflowCount++;
            }
            break;
        }

        case CsgOp::INTERSECT:
            for (int i = (int)m_solidMinX.size() - 1; i >= 0; i--) {
                if (Overlaps(i, b))
                    Intersect(i, b);
                else
                    RemoveAt(i);  // no overlap → outside intersection
            }
            break;
    }
}

void CsgProcessor::GetVolumes(std::vector<Volume>& out) const {
    out.clear();
    int n = Count();
    out.reserve(n);
    for (int i = 0; i < n; i++)
        out.push_back({m_solidMinX[i], m_solidMinY[i], m_solidMinZ[i],
                       m_solidMaxX[i], m_solidMaxY[i], m_solidMaxZ[i]});
}

// ---------------------------------------------------------------------------
// MergePass — combine adjacent coplanar AABBs into larger volumes
// Returns the number of merges performed.
//
// Two AABBs can merge if they are adjacent on exactly one axis and have
// identical min/max on the other two (i.e., they share a complete face).
// ---------------------------------------------------------------------------
int CsgProcessor::MergePass() {
    int totalMerges = 0;
    const float EPS = 0.001f;  // small tolerance for floating-point comparison

    for (int iter = 0; iter < 10; iter++) {  // max 10 passes
        bool merged = false;
        int n = Count();
        for (int i = n - 1; i >= 0 && !merged; i--) {
            for (int j = i - 1; j >= 0 && !merged; j--) {
                // Check adjacency on X axis
                if (std::fabs(m_solidMaxX[i] - m_solidMinX[j]) < EPS ||
                    std::fabs(m_solidMaxX[j] - m_solidMinX[i]) < EPS) {
                    // Same Y and Z extent?
                    if (std::fabs(m_solidMinY[i] - m_solidMinY[j]) < EPS &&
                        std::fabs(m_solidMaxY[i] - m_solidMaxY[j]) < EPS &&
                        std::fabs(m_solidMinZ[i] - m_solidMinZ[j]) < EPS &&
                        std::fabs(m_solidMaxZ[i] - m_solidMaxZ[j]) < EPS) {
                        // Merge along X: combine the X ranges
                        float newMinX = std::min(m_solidMinX[i], m_solidMinX[j]);
                        float newMaxX = std::max(m_solidMaxX[i], m_solidMaxX[j]);
                        m_solidMinX[j] = newMinX; m_solidMaxX[j] = newMaxX;
                        RemoveAt(i);
                        merged = true;
                        totalMerges++;
                    }
                }
                // Check adjacency on Z axis
                else if (std::fabs(m_solidMaxZ[i] - m_solidMinZ[j]) < EPS ||
                         std::fabs(m_solidMaxZ[j] - m_solidMinZ[i]) < EPS) {
                    // Same X and Y extent?
                    if (std::fabs(m_solidMinX[i] - m_solidMinX[j]) < EPS &&
                        std::fabs(m_solidMaxX[i] - m_solidMaxX[j]) < EPS &&
                        std::fabs(m_solidMinY[i] - m_solidMinY[j]) < EPS &&
                        std::fabs(m_solidMaxY[i] - m_solidMaxY[j]) < EPS) {
                        float newMinZ = std::min(m_solidMinZ[i], m_solidMinZ[j]);
                        float newMaxZ = std::max(m_solidMaxZ[i], m_solidMaxZ[j]);
                        m_solidMinZ[j] = newMinZ; m_solidMaxZ[j] = newMaxZ;
                        RemoveAt(i);
                        merged = true;
                        totalMerges++;
                    }
                }
                // Check adjacency on Y axis
                else if (std::fabs(m_solidMaxY[i] - m_solidMinY[j]) < EPS ||
                         std::fabs(m_solidMaxY[j] - m_solidMinY[i]) < EPS) {
                    // Same X and Z extent?
                    if (std::fabs(m_solidMinX[i] - m_solidMinX[j]) < EPS &&
                        std::fabs(m_solidMaxX[i] - m_solidMaxX[j]) < EPS &&
                        std::fabs(m_solidMinZ[i] - m_solidMinZ[j]) < EPS &&
                        std::fabs(m_solidMaxZ[i] - m_solidMaxZ[j]) < EPS) {
                        float newMinY = std::min(m_solidMinY[i], m_solidMinY[j]);
                        float newMaxY = std::max(m_solidMaxY[i], m_solidMaxY[j]);
                        m_solidMinY[j] = newMinY; m_solidMaxY[j] = newMaxY;
                        RemoveAt(i);
                        merged = true;
                        totalMerges++;
                    }
                }
            }
        }
        if (!merged) break;
    }
    return totalMerges;
}
