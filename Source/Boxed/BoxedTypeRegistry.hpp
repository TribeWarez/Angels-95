#pragma once
// ---------------------------------------------------------------------------
// BoxedTypeRegistry — engine-generic world-entity type registry.
//
// SCAFFOLD (Phase 0). Implemented but nothing registers or iterates it yet.
// Phase 1 converts OzOzoneLoader::InjectOzoneEntities' hardcoded switch into an
// iteration over registered types, and Phase 2 registers Angels95-specific types
// (faction_terminal, soul_receptacle, guild_node, ...) from Source/Game.
//
// Why it exists: every world entity type (mesh, light, emitter, pickup, npc,
// trigger, zone, portal, pathnode, windzone) is currently a branch inside one
// engine function. A registry lets the engine iterate descriptors and lets the
// game add types without editing engine code.
//
// The hook signatures are deliberately OPAQUE (void*) at scaffold time: the
// parse/inject hooks touch OZONE types and the draw/update hooks touch raylib,
// and pinning those signatures before Phase 1 would drag raylib into this
// header. Phase 1 replaces the void* trampolines with typed function pointers.
// ---------------------------------------------------------------------------

#include <string>
#include <vector>

namespace oz {
namespace boxed {

struct BoxedTypeDesc {
    std::string name;            // OZONE token / .ozls type, e.g. "trigger"
    std::string source;          // "engine" or "game" (provenance for diagnostics)
    void* parse  = nullptr;      // Phase 1: parse an OZONE line -> node
    void* inject = nullptr;      // Phase 1: push node into the runtime system
    void* update = nullptr;      // Phase 1: per-frame update
    void* draw   = nullptr;      // Phase 1: per-frame draw
    void* serialize = nullptr;   // Phase 1: persist node state
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
