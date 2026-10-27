#pragma once
#include <string>

// ---------------------------------------------------------------------------
// SpellElement — the IDENTITY of a spell, separate from its colour.
//
// A spell used to carry only `projectile_color` (an RGB triple) and a `spell`
// bool, so "fire vs frost" existed nowhere except as a tint. That is not enough
// for anything that has to REACT to the element: a halo shape, a burn, a chill.
// Two spells can share a colour and must still behave differently (Arc Bolt and
// Frost Lance are both blue), so deriving the element from the RGB hue would be
// wrong by construction.
//
// Authored per spell as `element = fire|frost|shock|psychic|holy` in the .ozls
// stats block. Absent or unrecognised -> Arcane, which is what every existing
// def gets, so nothing changes until an author opts in.
//
// Raylib-free and header-only on purpose: the cast path (LightningEntityManager),
// the projectile (PawnSystem) and the VFX layer (CombatFX) all speak it, and a
// headless test can pin the string mapping.
// ---------------------------------------------------------------------------
enum class SpellElement { Arcane, Fire, Frost, Shock, Psychic, Holy };

inline SpellElement SpellElementFromString(const std::string& s) {
    if (s == "fire" || s == "flame" || s == "burn")          return SpellElement::Fire;
    if (s == "frost" || s == "ice" || s == "cold")           return SpellElement::Frost;
    if (s == "shock" || s == "lightning" || s == "electric") return SpellElement::Shock;
    if (s == "psychic" || s == "void" || s == "mind")        return SpellElement::Psychic;
    if (s == "holy" || s == "radiant" || s == "light")       return SpellElement::Holy;
    return SpellElement::Arcane;
}

inline const char* SpellElementName(SpellElement e) {
    switch (e) {
        case SpellElement::Fire:    return "fire";
        case SpellElement::Frost:   return "frost";
        case SpellElement::Shock:   return "shock";
        case SpellElement::Psychic: return "psychic";
        case SpellElement::Holy:    return "holy";
        case SpellElement::Arcane:  break;
    }
    return "arcane";
}
