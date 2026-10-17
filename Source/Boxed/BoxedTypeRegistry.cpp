#include "BoxedTypeRegistry.hpp"

namespace oz {
namespace boxed {

BoxedTypeRegistry& BoxedTypeRegistry::Instance() {
    static BoxedTypeRegistry* instance = new BoxedTypeRegistry();
    return *instance;
}

void BoxedTypeRegistry::Register(const BoxedTypeDesc& desc) {
    if (desc.name.empty()) return;
    for (auto& t : m_types) {
        if (t.name == desc.name) { t = desc; return; }   // last registration wins
    }
    m_types.push_back(desc);
}

const BoxedTypeDesc* BoxedTypeRegistry::Find(const std::string& name) const {
    for (const auto& t : m_types)
        if (t.name == name) return &t;
    return nullptr;
}

void BoxedTypeRegistry::Clear() {
    m_types.clear();
}

} // namespace boxed
} // namespace oz
