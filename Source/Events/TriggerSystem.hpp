#pragma once
// ---------------------------------------------------------------------------
// TriggerSystem — engine-generic trigger volumes.
//
// SCAFFOLD (Phase 0). Implemented but nothing injects or queries it yet. Phase 1
// adds the OZONE `trigger <def> minX..maxZ [once]` line and calls
// QueryContaining() once per frame, emitting trigger.enter/exit on the EventBus.
//
// Why it exists: the engine's only volumes are zones (ZONE_WATER/LADDER/SKY/
// REVERB/SOUND/PORTAL) with fixed semantics. Gameplay needs arbitrary "when the
// player is here, do this" volumes whose *meaning* is authored in .ozls, not
// compiled into a ZoneType switch.
//
// Raylib-free on purpose: links into headless test targets.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <vector>

#include "EventBus.hpp"

namespace oz {
namespace events {

struct TriggerVolume {
    uint32_t id = 0;
    std::string defName;          // ": trigger" .ozls def to dispatch to
    float minX = 0, minY = 0, minZ = 0;
    float maxX = 0, maxY = 0, maxZ = 0;
    bool once = false;            // fire enter at most once per session
    bool fired = false;
    int  team = 0;                // 0 = any
};

class TriggerSystem {
public:
    static TriggerSystem& Instance();

    int  AddTrigger(const TriggerVolume& t);
    void RemoveTrigger(int id);
    void Clear();

    const std::vector<TriggerVolume>& Triggers() const { return m_triggers; }

    // Append the indices of every trigger whose AABB contains the point.
    void QueryContaining(float x, float y, float z, std::vector<int>& out) const;

    // Advance the player's inside-set for a point and emit trigger.enter /
    // trigger.exit on the EventBus (Event::s0 = the trigger's def name). A
    // `once` trigger emits enter at most once and then stays latched. Call once
    // per simulation step with the player position.
    void UpdateForPoint(float x, float y, float z);

private:
    TriggerSystem() = default;
    std::vector<TriggerVolume> m_triggers;
    std::vector<int> m_inside;   // indices the point is currently inside
    uint32_t m_nextId = 1;
};

} // namespace events
} // namespace oz
