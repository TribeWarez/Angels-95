#include "EventBus.hpp"

namespace oz {
namespace events {

EventBus& EventBus::Instance() {
    // Intentionally leaked, matching the other engine singletons: the bus may be
    // emitted into from static destructors otherwise.
    static EventBus* instance = new EventBus();
    return *instance;
}

void EventBus::Subscribe(const std::string& topic, EventHandler fn, void* user) {
    if (topic.empty() || fn == nullptr) return;
    m_subs.push_back(Sub{topic, fn, user});
}

void EventBus::Unsubscribe(void* user) {
    if (user == nullptr) return;
    for (size_t i = 0; i < m_subs.size();) {
        if (m_subs[i].user == user) m_subs.erase(m_subs.begin() + (long)i);
        else ++i;
    }
}

void EventBus::Emit(const Event& ev) {
    // Copy the subscription list header out before dispatch: a handler is
    // allowed to Subscribe/Unsubscribe (an entity dying can remove its own
    // handlers), and mutating m_subs mid-iteration would invalidate it.
    // A subscription whose topic is "*" receives every event (the script bridge
    // in LightningEntityManager uses exactly one of these).
    const size_t n = m_subs.size();
    for (size_t i = 0; i < n && i < m_subs.size(); ++i) {
        const Sub& s = m_subs[i];
        if (s.fn && (s.topic == ev.topic || s.topic == "*")) s.fn(ev, s.user);
    }
}

void EventBus::Clear() {
    m_subs.clear();
}

} // namespace events
} // namespace oz
