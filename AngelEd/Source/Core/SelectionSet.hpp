#pragma once
// ---------------------------------------------------------------------------
// Core/SelectionSet.hpp - what a resolved viewport click does to the selection
// SET (not to the primary).
//
// Header-only and raylib-free on purpose: the decision is pure, and pinning it in
// tests/SelectionSet.test.cpp is the only thing that keeps it from drifting back
// to "clicking the selected actor deselects it".
//
// WHY THIS EXISTS
//   Picking moved to PRESS (UnEdClick.cpp parity: UEditorEngine::Click resolves
//   the hit on button-down and the drag then moves whatever is selected). That
//   made two old rules actively wrong:
//
//     * "click the already-selected entity again to drop it" -> the press that
//       STARTS a Move/Scale/Rotate gesture cleared the selection first, so the
//       gesture moved nothing and the object looked like it deselected itself
//       the instant the user grabbed it;
//     * "a click on any entity replaces the set" -> grabbing one member of a
//       multi-selection collapsed it to that member, so the rest silently
//       stopped moving (MoveActors walks the whole set in UE1).
//
//   UE1's Click(HActor) writes only the clicked actor's `bSelected`, and toggles
//   it ONLY when MOUSE_Ctrl is down. Everything else is idempotent; HBackdrop
//   (empty space) is what clears the selection, and that stays in the caller.
// ---------------------------------------------------------------------------

namespace edsel {

// What the click does to the set.
enum class PickAction {
    Replace,   // set := { clicked }
    Keep,      // set unchanged; clicked becomes the primary
    Toggle,    // add clicked if absent, remove it if present
};

// `allowToggle`  - false for paths that must never change membership (the
//                  context menu; it acts on whatever is already selected).
// `modifierHeld` - Ctrl or Shift (see SelectionModifierHeld in Selection.cpp).
// `alreadyInSet` - the clicked entity is a MEMBER of the set, not necessarily
//                  the primary. Grabbing any member drags the whole selection.
inline PickAction ResolvePickAction(bool allowToggle, bool modifierHeld,
                                    bool alreadyInSet) {
    if (allowToggle && modifierHeld) return PickAction::Toggle;
    if (alreadyInSet) return PickAction::Keep;
    return PickAction::Replace;
}

} // namespace edsel
