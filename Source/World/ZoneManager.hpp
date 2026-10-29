#pragma once
// ---------------------------------------------------------------------------
// World/ZoneManager.hpp — zone volume ownership + runtime queries
//
// Extracted from Source/Pawn/OzPawnSystem.hpp. Zone volumes, level portals and
// the per-frame point-region tracker used to live inside the pawn system even
// though they are world-space volume data, not entities. They now live here so
// that:
//
//   * world loading (OzOzoneLoader) can produce a parsed entity set without
//     touching the entity system,
//   * runtime zone entry checks, volume detection and environment override
//     merging are queried through a single module instead of four scattered
//     PawnSystem methods,
//   * the zone taxonomy stays raylib-light (only BoundingBox is needed).
//
// Storage note: zone ids come from a ZoneManager-local counter. They only need
// to be unique inside a zone list (region tracking, light zone binding), so
// they no longer have to share the entity id space.
// ---------------------------------------------------------------------------

#include "ZoneTypes.hpp"
#include "raylib.h"
#include "../Physics/PhysicsInfo.hpp"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

// Zone volume node - AABB volumes with behavior flags
struct ZoneVolumeNode {
    uint32_t id = 0;
    BoundingBox bounds;
    ZoneType zoneType = ZoneType::ZONE_WATER;
    float intensity = 1.0f;  // e.g., water density, ladder speed
    int priority = 0;         // higher = wins when overlapping
    std::string name;        // logical name for LightningScript zone lookups
    GameplaySoundProfile soundProfile; // game-type-specific audio profile
    ZoneEnvOverrides envOverrides; // environment overrides (fog, ambient, reverb)
    oz::physics::PhysicsInfo physics; // per-zone physics overrides (named kwargs)
};

// ZonePortal — connects two zones / two LEVELS (enable zone transitions + campaigns)
struct ZonePortal {
    BoundingBox bounds;           // trigger volume
    std::string targetWorld;      // destination level folder name in GameData/Worlds/ ("" = unassigned)
    Vector3 targetSpawn{0, 20, 0}; // player position on arrival in targetWorld
    bool bidirectional = true;
    bool enabled = true;
};

// PointRegion — per-entity zone tracking with stacking support
struct PointRegion {
    int primaryZoneId = -1;        // current frame's primary zone
    ZoneType primaryZoneType = ZoneType::ZONE_WATER; // type of primary zone
    std::unordered_set<int> activeZoneIds;   // all overlapping zones this frame
    ZoneEnvOverrides combinedEnv;            // merged from all active zones
    // Bitmask of EVERY active zone type, not just the primary one. A CRT or
    // gravity pocket authored INSIDE a water/sky volume is not the primary zone
    // (priority/volume decides that), so `HasZoneType()` - which is primary-only -
    // would never see it. Zone effects that should fire from anywhere inside the
    // volume read this instead. One bit per ZoneType; ZoneType is uint8_t and the
    // enum has 8 values, so a uint32_t is the whole space.
    uint32_t activeTypeMask = 0;

    // `activeZones` must be sorted highest-priority-first (see GetActiveZones).
    void Rebuild(const std::vector<ZoneVolumeNode*>& activeZones);
    // Primary-zone test (the zone that owns the player's physics/env).
    bool HasZoneType(ZoneType type) const { return primaryZoneId >= 0 && primaryZoneType == type; }
    // Any-active-zone test (an effect pocket anywhere inside the player's volume).
    bool HasZoneTypeIn(ZoneType type) const {
        return (activeTypeMask & (1u << (int)type)) != 0;
    }
    bool HasZoneId(int id) const;
};

class ZoneManager {
public:
    // --- Zone volumes -------------------------------------------------------
    int AddZone(const ZoneVolumeNode& node);
    void RemoveZone(int id);
    void ClearZones();
    std::vector<ZoneVolumeNode>& GetZones() { return m_zones; }
    const std::vector<ZoneVolumeNode>& GetZones() const { return m_zones; }
    ZoneVolumeNode* GetZone(int id);
    // First zone whose volume contains the point (or bounds, as a fallback).
    ZoneVolumeNode* CheckZoneCollision(Vector3 pos, BoundingBox bounds);

    // --- Level portals (campaign links) -------------------------------------
    // Portal handles are indices into GetPortals() (they are edited by index
    // from the editor UI, so indices must stay dense).
    int AddPortal(const ZonePortal& node);
    void RemovePortal(int id);
    void ClearPortals();
    std::vector<ZonePortal>& GetPortals() { return m_portals; }
    const std::vector<ZonePortal>& GetPortals() const { return m_portals; }
    ZonePortal* GetPortal(int id);
    // Returns the first enabled portal whose volume contains pos/bounds, nullptr if none
    ZonePortal* CheckPortalCollision(Vector3 pos, BoundingBox bounds);

    // --- Player region tracking --------------------------------------------
    // Single-pass scan: collects every overlapping zone, merges env overrides
    // and refreshes the enter/exit sets. Call once per frame with the player AABB.
    void UpdatePlayerRegion(Vector3 playerPos, BoundingBox playerBounds);
    PointRegion& GetPlayerRegion() { return m_playerRegion; }
    const PointRegion& GetPlayerRegion() const { return m_playerRegion; }

    // All overlapping zones sorted by priority (highest first), then volume
    // (smallest first) so later layers win when overrides are merged.
    std::vector<ZoneVolumeNode*> GetActiveZones(Vector3 pos, BoundingBox bounds);

    static ZoneManager& Instance();

private:
    std::vector<ZoneVolumeNode> m_zones;
    std::vector<ZonePortal> m_portals;
    PointRegion m_playerRegion;
    uint32_t m_nextZoneId = 1;
};