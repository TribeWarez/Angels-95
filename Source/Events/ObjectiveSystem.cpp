#include "ObjectiveSystem.hpp"
#include "EventBus.hpp"

#include <sstream>

namespace oz {
namespace events {

ObjectiveSystem& ObjectiveSystem::Instance() {
    static ObjectiveSystem* instance = new ObjectiveSystem();
    return *instance;
}

ObjectiveState& ObjectiveSystem::Ensure(const std::string& id) {
    auto it = m_objectives.find(id);
    if (it != m_objectives.end()) return it->second;
    ObjectiveState s;
    s.id = id;
    return m_objectives.emplace(id, s).first->second;
}

void ObjectiveSystem::Define(const std::string& id, int target) {
    ObjectiveState& s = Ensure(id);
    s.progress = 0;
    s.target = target;
    s.complete = false;
    s.failed = false;
}

bool ObjectiveSystem::Add(const std::string& id, int delta) {
    ObjectiveState& s = Ensure(id);
    if (s.complete || s.failed) return false;
    s.progress += delta;
    Event ev;
    ev.topic = "objective.progress";
    ev.s0 = id;
    ev.i0 = s.progress;
    ev.i1 = s.target;
    EventBus::Instance().Emit(ev);
    if (s.target > 0 && s.progress >= s.target) {
        s.complete = true;
        Event c;
        c.topic = "objective.complete";
        c.s0 = id;
        EventBus::Instance().Emit(c);
        return true;
    }
    return false;
}

bool ObjectiveSystem::Set(const std::string& id, int progress) {
    ObjectiveState& s = Ensure(id);
    return Add(id, progress - s.progress);
}

void ObjectiveSystem::Complete(const std::string& id) {
    ObjectiveState& s = Ensure(id);
    if (s.complete) return;
    s.complete = true;
    s.progress = (s.target > 0) ? s.target : s.progress;
    Event c;
    c.topic = "objective.complete";
    c.s0 = id;
    EventBus::Instance().Emit(c);
}

void ObjectiveSystem::Fail(const std::string& id) {
    ObjectiveState& s = Ensure(id);
    if (s.failed || s.complete) return;
    s.failed = true;
    Event f;
    f.topic = "objective.fail";
    f.s0 = id;
    EventBus::Instance().Emit(f);
}

bool ObjectiveSystem::IsComplete(const std::string& id) const {
    auto it = m_objectives.find(id);
    return it != m_objectives.end() && it->second.complete;
}

const ObjectiveState* ObjectiveSystem::Get(const std::string& id) const {
    auto it = m_objectives.find(id);
    return it != m_objectives.end() ? &it->second : nullptr;
}

void ObjectiveSystem::Reset() {
    m_objectives.clear();
    m_flags.clear();
}

void ObjectiveSystem::SetWorldFlag(const std::string& name, int value) {
    if (name.empty()) return;
    m_flags[name] = value;
}

int ObjectiveSystem::GetWorldFlag(const std::string& name, int def) const {
    auto it = m_flags.find(name);
    return it != m_flags.end() ? it->second : def;
}

std::string ObjectiveSystem::Serialize() const {
    std::ostringstream os;
    for (const auto& kv : m_objectives) {
        const ObjectiveState& s = kv.second;
        os << "obj " << s.id << '=' << s.progress << ',' << s.target << ','
           << (s.complete ? 1 : 0) << ',' << (s.failed ? 1 : 0) << '\n';
    }
    for (const auto& kv : m_flags) {
        os << "flag " << kv.first << '=' << kv.second << '\n';
    }
    return os.str();
}

void ObjectiveSystem::Deserialize(const std::string& text) {
    m_objectives.clear();
    m_flags.clear();
    std::istringstream is(text);
    std::string line;
    while (std::getline(is, line)) {
        if (line.empty()) continue;
        size_t sp = line.find(' ');
        if (sp == std::string::npos) continue;
        std::string kind = line.substr(0, sp);
        std::string rest = line.substr(sp + 1);
        size_t eq = rest.find('=');
        if (eq == std::string::npos) continue;
        std::string key = rest.substr(0, eq);
        std::string val = rest.substr(eq + 1);
        if (kind == "flag") {
            try { m_flags[key] = std::stoi(val); } catch (...) {}
        } else if (kind == "obj") {
            ObjectiveState s;
            s.id = key;
            for (char& c : val) if (c == ',') c = ' ';
            std::istringstream rs(val);
            rs >> s.progress >> s.target >> s.complete >> s.failed;
            m_objectives[s.id] = s;
        }
    }
}

} // namespace events
} // namespace oz
