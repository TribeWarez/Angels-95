#include "TriggerSystem.hpp"

#include <algorithm>

namespace oz {
namespace events {

TriggerSystem& TriggerSystem::Instance() {
    static TriggerSystem* instance = new TriggerSystem();
    return *instance;
}

int TriggerSystem::AddTrigger(const TriggerVolume& t) {
    TriggerVolume v = t;
    v.id = m_nextId++;
    m_triggers.push_back(v);
    return (int)m_triggers.size() - 1;
}

void TriggerSystem::RemoveTrigger(int id) {
    if (id < 0 || id >= (int)m_triggers.size()) return;
    m_triggers.erase(m_triggers.begin() + id);
}

void TriggerSystem::Clear() {
    m_triggers.clear();
    m_inside.clear();
    m_nextId = 1;
}

void TriggerSystem::QueryContaining(float x, float y, float z, std::vector<int>& out) const {
    out.clear();
    for (size_t i = 0; i < m_triggers.size(); ++i) {
        const TriggerVolume& t = m_triggers[i];
        if (x >= t.minX && x <= t.maxX &&
            y >= t.minY && y <= t.maxY &&
            z >= t.minZ && z <= t.maxZ) {
            out.push_back((int)i);
        }
    }
}

void TriggerSystem::UpdateForPoint(float x, float y, float z) {
    std::vector<int> now;
    QueryContaining(x, y, z, now);
    auto contains = [](const std::vector<int>& v, int i) {
        return std::find(v.begin(), v.end(), i) != v.end();
    };
    for (int i : now) {
        if (contains(m_inside, i)) continue;
        TriggerVolume& t = m_triggers[(size_t)i];
        if (t.once && t.fired) continue;
        t.fired = true;
        Event ev;
        ev.topic = "trigger.enter";
        ev.s0 = t.defName;
        ev.x = x; ev.y = y; ev.z = z;
        EventBus::Instance().Emit(ev);
    }
    for (int i : m_inside) {
        if (contains(now, i)) continue;
        const TriggerVolume& t = m_triggers[(size_t)i];
        Event ev;
        ev.topic = "trigger.exit";
        ev.s0 = t.defName;
        ev.x = x; ev.y = y; ev.z = z;
        EventBus::Instance().Emit(ev);
    }
    m_inside.swap(now);
}

} // namespace events
} // namespace oz
