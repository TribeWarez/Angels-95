// SelectionSet test - standalone, no raylib dependency.
//   make test_selectionset
//
// Pins the click -> selection-set rule (AngelEd/Source/Core/SelectionSet.hpp).
//
// The regression it exists for: picking moved to PRESS (UnEdClick.cpp parity), so
// the same press that resolves the hit also starts a Move/Scale/Rotate gesture.
// The old rule "click the already-selected entity again to drop it" therefore
// CLEARED the selection on the very press that began the transform, the drag
// moved nothing, and the object looked like it deselected itself the moment the
// user grabbed it. UE1 only toggles with MOUSE_Ctrl; a plain click is idempotent.
#include "../AngelEd/Source/Core/SelectionSet.hpp"
#include <cstdio>

using edsel::PickAction;

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { g_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static const char* Name(PickAction a) {
    switch (a) {
        case PickAction::Replace: return "Replace";
        case PickAction::Keep:    return "Keep";
        case PickAction::Toggle:  return "Toggle";
    }
    return "?";
}

static void Expect(PickAction want, PickAction got, const char* what) {
    g_checks++;
    if (want != got) {
        g_fail++;
        printf("  FAIL %s: expected %s, got %s\n", what, Name(want), Name(got));
    }
}

// A plain left click on an entity that is already selected must NOT deselect it:
// it keeps the set and makes that entity the primary. This is the bug report.
static void TestPlainClickOnSelectedKeepsIt() {
    Expect(PickAction::Keep, edsel::ResolvePickAction(true, false, true),
           "plain click on a selected entity");
    // ...and it must never be the toggle that removes it.
    CHECK(edsel::ResolvePickAction(true, false, true) != PickAction::Toggle);
}

// A plain click on something else replaces the set (single-select).
static void TestPlainClickOnOtherReplaces() {
    Expect(PickAction::Replace, edsel::ResolvePickAction(true, false, false),
           "plain click on a different entity");
}

// Ctrl/Shift toggles membership, both ways.
static void TestModifierToggles() {
    Expect(PickAction::Toggle, edsel::ResolvePickAction(true, true, false),
           "modifier click adds");
    Expect(PickAction::Toggle, edsel::ResolvePickAction(true, true, true),
           "modifier click removes");
}

// The context menu must never change membership: it acts on what is selected,
// and a toggle here would drop the entity the menu is about to operate on.
static void TestContextMenuNeverToggles() {
    Expect(PickAction::Keep,    edsel::ResolvePickAction(false, true, true),
           "right-click on a selected entity");
    Expect(PickAction::Replace, edsel::ResolvePickAction(false, true, false),
           "right-click on a different entity");
    Expect(PickAction::Keep,    edsel::ResolvePickAction(false, false, true),
           "right-click, no modifier");
}

// Exhaustive: the whole table, so a future edit cannot reintroduce either half.
static void TestTable() {
    for (int allowToggle = 0; allowToggle <= 1; ++allowToggle) {
        for (int modifier = 0; modifier <= 1; ++modifier) {
            for (int inSet = 0; inSet <= 1; ++inSet) {
                const PickAction a = edsel::ResolvePickAction(
                    allowToggle != 0, modifier != 0, inSet != 0);
                // A member is never dropped by a non-toggling click...
                if (!(allowToggle && modifier) && inSet) {
                    CHECK(a == PickAction::Keep);
                }
                // ...and a non-member is always selected.
                if (!(allowToggle && modifier) && !inSet) {
                    CHECK(a == PickAction::Replace);
                }
                // Toggle is reachable ONLY with a modifier on a toggling path.
                if (a == PickAction::Toggle) {
                    CHECK(allowToggle && modifier);
                }
            }
        }
    }
}

int main() {
    printf("SelectionSet\n============\n");
    TestPlainClickOnSelectedKeepsIt();
    TestPlainClickOnOtherReplaces();
    TestModifierToggles();
    TestContextMenuNeverToggles();
    TestTable();

    printf("============\n%d/%d checks passed, %d failed\n",
           g_checks - g_fail, g_checks, g_fail);
    return g_fail;
}
