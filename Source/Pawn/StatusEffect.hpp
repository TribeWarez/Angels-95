#pragma once
#include "../Renderer/SpellElement.hpp"

// ---------------------------------------------------------------------------
// StatusEffect — a timed condition a spell leaves on a pawn.
//
// Deliberately TWO effects, not a general buff/debuff framework:
//   Burn  — damage over time (fire)
//   Chill — movement slow (frost)
//
// Single-status, refresh-on-reapply: a pawn carries at most one, and a new
// element replaces the old one (clearing its movement change first, or a chill
// replaced by a burn would leave the pawn permanently slowed). Stacking needs a
// design decision about whether two fires double the DoT, which is not made yet.
//
// Single-player only for now: ApplyPawnDamage returns early for networkControlled
// pawns, and the server owns MP NPC health. A networked status would need a tail
// on NPC_DAMAGE/NPC_STATE plus server-side ticking; do not pretend otherwise by
// letting the client apply it locally.
//
// Raylib-free so a headless test can pin the element->status mapping and the
// tick/expiry rules.
// ---------------------------------------------------------------------------
enum class StatusEffect { None, Burn, Chill };

inline StatusEffect StatusForElement(SpellElement e) {
    switch (e) {
        case SpellElement::Fire:  return StatusEffect::Burn;
        case SpellElement::Frost: return StatusEffect::Chill;
        default:                  return StatusEffect::None;
    }
}

inline const char* StatusEffectName(StatusEffect s) {
    switch (s) {
        case StatusEffect::Burn:  return "burn";
        case StatusEffect::Chill: return "chill";
        case StatusEffect::None:  break;
    }
    return "none";
}
