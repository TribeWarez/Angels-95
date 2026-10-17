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

} // namespace events
} // namespace oz
