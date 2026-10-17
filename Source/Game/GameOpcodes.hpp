#pragma once
// ---------------------------------------------------------------------------
// GameOpcodes — Angels95-specific LightningScript opcodes.
//
// SCAFFOLD (Phase 0). RegisterGameOpcodes() is a no-op. Phase 1/2 adds the
// generic gameplay opcodes the engine's context should understand
// (emit_event, objective, set_worldflag/get_worldflag, wait/timer, grant_item,
// spawn_projectile, apply_area_damage, give_xp, teleport, change_world,
// play_clip) and any game-specific ones.
//
// The split: generic opcodes live in LightningScriptContext (engine); opcodes
// that only make sense for Angels95 (e.g. `soul_transfer`, `guild_claim`) live
// here and register a handler table with the context.
// ---------------------------------------------------------------------------

namespace oz {
namespace game {

// Idempotent. Called once at startup. Phase 0 does nothing.
void RegisterGameOpcodes();

} // namespace game
} // namespace oz
