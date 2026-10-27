#pragma once
// ---------------------------------------------------------------------------
// Physics/DynamicCollision.hpp — the dynamic-collision side-list.
//
// Doors (Phase 1.3) and quantum obstacles (Phase 7.2) need their collision to
// be TOGGLED at runtime, but the static collision world is a CSG output whose
// volumes carry no brush identity (Subtract replaces one solid with up to six,
// MergePass swap-removes). There is therefore no reliable "remove the volume(s)
// that came from brush N" operation.
//
// The side-list sidesteps that entirely: a door or quantum brush is authored
// with SURF_NO_BSP_CUTS (so it never enters CSG) and its AABB is registered
// here instead. The runtime collision query checks the static CSG volumes AND
// this list (skipping `enabled == false`), and a door/quantum toggle flips one
// bool rather than rebuilding the world.
//
// RAYLIB-FREE on purpose: plain floats, no BoundingBox, so it links from the
// headless test harness (same discipline as WorldChunkManager and PhysicsInfo).
// The renderer maps a raylib BoundingBox onto these floats at the call site.
// ---------------------------------------------------------------------------

#include <vector>
#include <cstdint>

namespace oz {
namespace physics {

struct DynamicBlocker {
    float minX, minY, minZ, maxX, maxY, maxZ;
    bool enabled = true;
    int  ownerId = -1;   // caller-assigned (door id / quantum renderable index)
};

class DynamicCollision {
public:
    int Add(const DynamicBlocker& b) {
        m_blocks.push_back(b);
        return (int)m_blocks.size() - 1;
    }
    void Clear() { m_blocks.clear(); }
    size_t Count() const { return m_blocks.size(); }
    bool AnyEnabled() const {
        for (const auto& b : m_blocks) if (b.enabled) return true;
        return false;
    }
    void SetEnabled(int index, bool on) {
        if (index >= 0 && index < (int)m_blocks.size()) m_blocks[index].enabled = on;
    }
    void SetAllEnabled(bool on) {
        for (auto& b : m_blocks) b.enabled = on;
    }
    const DynamicBlocker* At(int index) const {
        return (index >= 0 && index < (int)m_blocks.size()) ? &m_blocks[index] : nullptr;
    }

    // AABB overlap against any ENABLED blocker.
    bool OverlapsEnabled(float minX, float minY, float minZ,
                         float maxX, float maxY, float maxZ) const {
        for (const auto& b : m_blocks) {
            if (!b.enabled) continue;
            if (minX < b.maxX && maxX > b.minX &&
                minY < b.maxY && maxY > b.minY &&
                minZ < b.maxZ && maxZ > b.minZ)
                return true;
        }
        return false;
    }

    // Highest ENABLED blocker top that contains (x, z), i.e. the floor the
    // side-list would support a point with. Returns `none` when nothing is under
    // the point (same convention as the ground sampler's kNoGroundY).
    float TopUnder(float x, float z, float none) const {
        float best = none;
        for (const auto& b : m_blocks) {
            if (!b.enabled) continue;
            if (x >= b.minX && x <= b.maxX && z >= b.minZ && z <= b.maxZ)
                if (b.maxY > best) best = b.maxY;
        }
        return best;
    }

private:
    std::vector<DynamicBlocker> m_blocks;
};

} // namespace physics
} // namespace oz
