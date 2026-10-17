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
// The inject hook receives an opaque BoxedInjectContext: the concrete pointer
// types (OzoneEntitySet / PawnSystem / ZoneManager / TriggerSystem) are known
// only to the engine TU that registers the hook, so this header stays
// raylib-free and links into headless tests.
// ---------------------------------------------------------------------------

#include <string>
#include <vector>

namespace oz {
namespace boxed {

// Opaque handle bundle for a boxed type's inject hook.
struct BoxedInjectContext {
    const void* entities = nullptr;   // const OzoneEntitySet*
    void*       pawns    = nullptr;   // PawnSystem*
    void*       zones    = nullptr;   // ZoneManager*
    void*       triggers = nullptr;   // oz::events::TriggerSystem*
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
