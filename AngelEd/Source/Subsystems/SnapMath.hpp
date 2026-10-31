#pragma once
// ---------------------------------------------------------------------------
// Subsystems/SnapMath.hpp - "drop this box onto the floor" and "push this box
// against the nearest wall", as pure maths over axis-aligned boxes.
//
// Raylib-free and Win32-free, like GizmoMath.hpp, so tests/EditorSnap.test.cpp can
// pin the two queries without a GL context. The editor glue
// (Subsystems/Selection.cpp) converts a selection to a box, calls these, and moves
// the entity through MoveSelectionCentre - so a snap obeys the same
// centre-vs-origin rules as every other move, and a multi-selection snaps as a set.
//
// The engine's collision world is AABB-only (CsgProcessor consumes nothing else),
// so this is exact for what the player will actually collide with; it deliberately
// does not try to be cleverer than that.
// ---------------------------------------------------------------------------
#include <cmath>

namespace ozsnap {

struct Box {
    float minX = 0, minY = 0, minZ = 0;
    float maxX = 0, maxY = 0, maxZ = 0;
};

inline float WidthX(const Box& b)  { return b.maxX - b.minX; }
inline float WidthY(const Box& b)  { return b.maxY - b.minY; }
inline float WidthZ(const Box& b)  { return b.maxZ - b.minZ; }

inline bool OverlapsXZ(const Box& a, const Box& b) {
    return a.minX < b.maxX && a.maxX > b.minX && a.minZ < b.maxZ && a.maxZ > b.minZ;
}

// Highest solid surface that is under `mover`'s footprint and at or below its feet.
//
// `fallbackY` is the world floor where there is no solid (the heightmap, or a
// caller-chosen datum); it is returned when nothing qualifies, so the answer is
// always a real altitude. A surface ABOVE the mover is skipped, so a box standing
// under a roof is not snapped upwards onto it. Footprint overlap is required, not
// just a vertical column through the centre - that is what makes a crate resting on
// the edge of a ledge still find the ledge.
inline float GroundBelow(const Box& mover, const Box* solids, int count, float fallbackY) {
    float best = fallbackY;
    const float eps = 1e-4f;
    for (int i = 0; i < count; i++) {
        const Box& s = solids[i];
        if (!OverlapsXZ(mover, s)) continue;
        if (s.maxY > mover.minY + eps) continue;      // above us, or we are inside it
        if (s.maxY > best) best = s.maxY;
    }
    return best;
}

// Signed distance to slide `mover` along (dx, dz) - a unit cardinal direction -
// until it touches the nearest solid ahead. False when nothing is in the way.
//
// "Ahead" is exclusive: a solid the mover already overlaps (or has passed) is
// ignored, so pressing the button twice does not drift the object into/through the
// wall. The mover's vertical AND lateral cross-sections must overlap the solid, so
// a wall beside the object is found but a ceiling above it is not.
inline bool PushAgainstSide(const Box& mover, const Box* solids, int count,
                            float dx, float dz, float& outDelta) {
    const bool posX = dx > 0.5f, negX = dx < -0.5f;
    const bool posZ = dz > 0.5f, negZ = dz < -0.5f;
    if (!posX && !negX && !posZ && !negZ) return false;
    const float eps = 1e-4f;

    bool found = false;
    float best = 0.0f;
    for (int i = 0; i < count; i++) {
        const Box& s = solids[i];
        if (mover.minY >= s.maxY || mover.maxY <= s.minY) continue;   // different storey

        float d = 0.0f;
        if (posX) {
            if (!(mover.minZ < s.maxZ && mover.maxZ > s.minZ)) continue;
            if (s.minX < mover.maxX - eps) continue;      // behind or overlapping
            d = s.minX - mover.maxX;
        } else if (negX) {
            if (!(mover.minZ < s.maxZ && mover.maxZ > s.minZ)) continue;
            if (s.maxX > mover.minX + eps) continue;
            d = s.maxX - mover.minX;                      // negative
        } else if (posZ) {
            if (!(mover.minX < s.maxX && mover.maxX > s.minX)) continue;
            if (s.minZ < mover.maxZ - eps) continue;
            d = s.minZ - mover.maxZ;
        } else {
            if (!(mover.minX < s.maxX && mover.maxX > s.minX)) continue;
            if (s.maxZ > mover.minZ + eps) continue;
            d = s.maxZ - mover.minZ;                      // negative
        }
        if (!found || std::fabs(d) < std::fabs(best)) { best = d; found = true; }
    }
    outDelta = best;
    return found;
}

// PushAgainstSide, but refusing a move longer than `maxReach` world units. Without
// this a snap from an object in open ground would leap to a wall on the far side of
// the level - which reads as teleporting, not as aligning.
inline bool PushAgainstSideLimited(const Box& mover, const Box* solids, int count,
                                   float dx, float dz, float maxReach, float& outDelta) {
    if (!PushAgainstSide(mover, solids, count, dx, dz, outDelta)) return false;
    return std::fabs(outDelta) <= maxReach;
}

// The cardinal horizontal direction closest to `v` (the view's right vector),
// with a deterministic fallback for the degenerate straight-down view.
inline void CardinalFrom(float vx, float vz, float& dx, float& dz) {
    if (std::fabs(vx) < 1e-4f && std::fabs(vz) < 1e-4f) { dx = 1.0f; dz = 0.0f; return; }
    if (std::fabs(vx) >= std::fabs(vz)) { dx = (vx >= 0.0f) ? 1.0f : -1.0f; dz = 0.0f; }
    else                                { dx = 0.0f; dz = (vz >= 0.0f) ? 1.0f : -1.0f; }
}

} // namespace ozsnap
