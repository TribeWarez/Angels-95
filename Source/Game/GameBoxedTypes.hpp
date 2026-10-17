#pragma once
// ---------------------------------------------------------------------------
// GameBoxedTypes — Angels95-specific boxed world-entity types.
//
// SCAFFOLD (Phase 0). RegisterGameBoxedTypes() is a no-op. Phase 2/3 registers
// game types (faction_terminal, soul_receptacle, guild_node, ascension_altar,
// ...) with the engine BoxedTypeRegistry so they parse/inject/draw without any
// engine edit.
// ---------------------------------------------------------------------------

namespace oz {
namespace game {

// Idempotent. Called once at startup after the engine's own boxed types are
// registered. Phase 0 does nothing.
void RegisterGameBoxedTypes();

} // namespace game
} // namespace oz
