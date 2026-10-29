#include "ZoneManager.hpp"

#include <algorithm>

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------
ZoneManager& ZoneManager::Instance() {
    static ZoneManager instance;
    return instance;
}

// ---------------------------------------------------------------------------
// Zone volumes
// ---------------------------------------------------------------------------
int ZoneManager::AddZone(const ZoneVolumeNode& node) {
    ZoneVolumeNode n = node;
    if (n.id == 0) n.id = m_nextZoneId++;
    m_zones.push_back(n);
    return (int)n.id;
}

void ZoneManager::RemoveZone(int id) {
    m_zones.erase(std::remove_if(m_zones.begin(), m_zones.end(),
                                 [id](const ZoneVolumeNode& n) { return n.id == (uint32_t)id; }),
                  m_zones.end());
}

void ZoneManager::ClearZones() {
    m_zones.clear();
}

ZoneVolumeNode* ZoneManager::GetZone(int id) {
    for (auto& n : m_zones) {
        if (n.id == (uint32_t)id) return &n;
    }
    return nullptr;
}

ZoneVolumeNode* ZoneManager::CheckZoneCollision(Vector3 pos, BoundingBox bounds) {
    for (auto& n : m_zones) {
        if (pos.x >= n.bounds.min.x && pos.x <= n.bounds.max.x &&
            pos.y >= n.bounds.min.y && pos.y <= n.bounds.max.y &&
            pos.z >= n.bounds.min.z && pos.z <= n.bounds.max.z) {
            return &n;
        }
    }
    // Fall back to a box overlap test for entities whose origin sits just
    // outside the volume but whose body still reaches into it.
    for (auto& n : m_zones) {
        if (CheckCollisionBoxes(bounds, n.bounds))
            return &n;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Level portals
// ---------------------------------------------------------------------------
int ZoneManager::AddPortal(const ZonePortal& node) {
    m_portals.push_back(node);
    return (int)m_portals.size() - 1;
}

void ZoneManager::RemovePortal(int id) {
    if (id < 0 || id >= (int)m_portals.size()) return;
    m_portals.erase(m_portals.begin() + id);
}

void ZoneManager::ClearPortals() {
    m_portals.clear();
}

ZonePortal* ZoneManager::GetPortal(int id) {
    if (id < 0 || id >= (int)m_portals.size()) return nullptr;
    return &m_portals[id];
}

ZonePortal* ZoneManager::CheckPortalCollision(Vector3 pos, BoundingBox bounds) {
    for (auto& p : m_portals) {
        if (!p.enabled || p.targetWorld.empty()) continue;
        if (pos.x >= p.bounds.min.x && pos.x <= p.bounds.max.x &&
            pos.y >= p.bounds.min.y && pos.y <= p.bounds.max.y &&
            pos.z >= p.bounds.min.z && pos.z <= p.bounds.max.z) {
            return &p;
        }
        if (CheckCollisionBoxes(bounds, p.bounds))
            return &p;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Active zone query — highest priority first, then smallest volume
// ---------------------------------------------------------------------------
std::vector<ZoneVolumeNode*> ZoneManager::GetActiveZones(Vector3 pos, BoundingBox bounds) {
    std::vector<ZoneVolumeNode*> result;
    for (auto& z : m_zones) {
        bool inside = (pos.x >= z.bounds.min.x && pos.x <= z.bounds.max.x &&
                       pos.y >= z.bounds.min.y && pos.y <= z.bounds.max.y &&
                       pos.z >= z.bounds.min.z && pos.z <= z.bounds.max.z);
        if (!inside)
            inside = CheckCollisionBoxes(bounds, z.bounds);
        if (inside)
            result.push_back(&z);
    }
    std::sort(result.begin(), result.end(), [](const ZoneVolumeNode* a, const ZoneVolumeNode* b) {
        if (a->priority != b->priority) return a->priority > b->priority;
        float va = (a->bounds.max.x - a->bounds.min.x) *
                   (a->bounds.max.y - a->bounds.min.y) *
                   (a->bounds.max.z - a->bounds.min.z);
        float vb = (b->bounds.max.x - b->bounds.min.x) *
                   (b->bounds.max.y - b->bounds.min.y) *
                   (b->bounds.max.z - b->bounds.min.z);
        return va < vb;
    });
    return result;
}

// ---------------------------------------------------------------------------
// UpdatePlayerRegion — single-pass zone scan for the player
// ---------------------------------------------------------------------------
void ZoneManager::UpdatePlayerRegion(Vector3 playerPos, BoundingBox playerBounds) {
    m_playerRegion.Rebuild(GetActiveZones(playerPos, playerBounds));
}

// ---------------------------------------------------------------------------
// PointRegion
// ---------------------------------------------------------------------------
void PointRegion::Rebuild(const std::vector<ZoneVolumeNode*>& activeZones) {
    std::unordered_set<int> newIds;
    ZoneEnvOverrides merged;
    uint32_t typeMask = 0;

    // Layers are applied lowest-priority first so the highest-priority zone
    // has the final word on fog/ambient/reverb.
    for (auto it = activeZones.rbegin(); it != activeZones.rend(); ++it) {
        if (!*it) continue;
        newIds.insert((int)(*it)->id);
        merged.Merge((*it)->envOverrides);
        // Every active type, independent of priority. `primaryZoneType` below is
        // the single winner; this is the "is the player inside ANY volume of type
        // X" question, which per-effect systems (CRT warp, gravity audio) ask.
        typeMask |= (1u << (int)(*it)->zoneType);
    }

    // enteredZoneIds/exitedZoneIds were computed here and never read by anything
    // (PointRegion::CommitFrame, which consumed them, had zero callers). The
    // per-zone enter/exit hooks are driven from the lightning scripts in Main.cpp
    // via lastZoneName, not from this set. Removed rather than kept as dead state.
    activeZoneIds = std::move(newIds);
    activeTypeMask = typeMask;
    combinedEnv = merged;
    if (activeZoneIds.empty()) {
        primaryZoneId = -1;
        primaryZoneType = ZoneType::ZONE_WATER;
    } else {
        // Take the id from the SAME zone the type comes from.
        //
        // These were two different lookups of one concept: the type read
        // activeZones[0], which GetActiveZones sorted highest-priority-first
        // (priority desc, then smaller volume wins), while the id read
        // `*activeZoneIds.begin()` — the first element of an unordered_set, i.e.
        // whichever bucket the hash landed in. So with two overlapping zones the id
        // and the type could name DIFFERENT zones: the player got water physics from
        // the ladder volume, or `GetZone(primaryZoneId)` handed back a zone whose
        // envOverrides were never the ones consulted.
        //
        // GetActiveZones computes a full priority sort and this was throwing it away
        // at the one place it mattered.
        const ZoneVolumeNode* primary = activeZones.empty() ? nullptr : activeZones[0];
        primaryZoneId    = primary ? (int)primary->id : -1;
        primaryZoneType  = primary ? primary->zoneType : ZoneType::ZONE_WATER;
    }
}

bool PointRegion::HasZoneId(int id) const {
    return activeZoneIds.find(id) != activeZoneIds.end();
}