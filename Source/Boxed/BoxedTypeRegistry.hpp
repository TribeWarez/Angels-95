#pragma once
// ---------------------------------------------------------------------------
// BoxedTypeRegistry — engine-generic world-entity type registry.
//
// The engine registers its own world-entity types here (playerstart, pickup,
// zone, skyzone, portal, light, mesh, pathnode, windzone, particle emitter,
// emitter, trigger, npc spawn). InjectOzoneEntities iterates the registry and
// calls each type's inject hook, instead of a hardcoded switch — so a game can
// add a type (Source/Game/GameBoxedTypes.cpp) without editing engine code.
//
// The inject hook receives a BoxedInjectContext with typed pointers. The
// concrete types are forward-declared to keep this header raylib-free.
// ---------------------------------------------------------------------------

#include <string>
#include <vector>

class OzoneEntitySet;
class PawnSystem;
class ZoneManager;
namespace oz {
namespace events { class TriggerSystem; }
namespace boxed {

struct BoxedInjectContext {
    const OzoneEntitySet* entities = nullptr;
    PawnSystem*           pawns    = nullptr;
    ZoneManager*          zones    = nullptr;
    oz::events::TriggerSystem* triggers = nullptr;
};

using BoxedInjectFn = void (*)(const BoxedInjectContext& ctx);

struct BoxedTypeDesc {
    std::string  name;            // OZONE token / .ozls type, e.g. "trigger"
    std::string  source;          // "engine" or "game" (provenance/diagnostics)
    BoxedInjectFn inject = nullptr;
};

class BoxedTypeRegistry {
public:
    static BoxedTypeRegistry& Instance();

    void Register(const BoxedTypeDesc& desc);
    const BoxedTypeDesc* Find(const std::string& name) const;
    const std::vector<BoxedTypeDesc>& All() const { return m_types; }
    void Clear();

private:
    BoxedTypeRegistry() = default;
    std::vector<BoxedTypeDesc> m_types;
};

} // namespace boxed
} // namespace oz
