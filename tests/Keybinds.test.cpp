// Keybinds test - links raylib for the KEY_* constants, but does not open a window.
//   make test_keybinds
#include "../Source/Input/Keybinds.hpp"
#include <cstdio>

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { g_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

int main() {
    using namespace Keybinds;

    // Defaults: Drop is G, not Q (Q is the spellbook).
    ResetDefaults();
    CHECK(Key(GameAction::DropItem) == KEY_G);
    CHECK(Key(GameAction::Spellbook) == KEY_Q);
    CHECK(Key(GameAction::MoveForward) == KEY_W);
    CHECK(Key(GameAction::Fire) == kMouseBase + MOUSE_BUTTON_LEFT);

    // Round-trip through the INI object.
    {
        IniConfig cfg;
        cfg.SetInt("Keybinds", "drop_item", KEY_X);
        cfg.SetInt("Keybinds", "move_forward", KEY_I);
        Load(cfg);
        CHECK(Key(GameAction::DropItem) == KEY_X);
        CHECK(Key(GameAction::MoveForward) == KEY_I);
        // A key absent from the file keeps its current value.
        CHECK(Key(GameAction::MoveBack) == KEY_S);
    }

    // Invalid key codes are ignored rather than stored.
    {
        IniConfig cfg;
        cfg.SetInt("Keybinds", "jump", 3);          // below the valid range
        cfg.SetInt("Keybinds", "crouch", 99999);
        Load(cfg);
        CHECK(Key(GameAction::Jump) == KEY_SPACE);
        CHECK(Key(GameAction::Crouch) == KEY_LEFT_CONTROL);
    }

    // Save writes every action under its stable id.
    {
        IniConfig cfg;
        ResetDefaults();
        Assign(GameAction::DropItem, KEY_X);
        Save(cfg);
        CHECK(cfg.GetInt("Keybinds", "drop_item", -1) == KEY_X);
        CHECK(cfg.GetInt("Keybinds", "spellbook", -1) == KEY_Q);
        CHECK(cfg.GetInt("Keybinds", "fire", -1) == kMouseBase + MOUSE_BUTTON_LEFT);
    }

    // A conflicting assignment swaps, so neither action ends up unbound.
    {
        ResetDefaults();
        const int oldDrop = Key(GameAction::DropItem);
        const int swapped = Assign(GameAction::DropItem, KEY_Q);   // Q = Spellbook
        CHECK(swapped == (int)GameAction::Spellbook);
        CHECK(Key(GameAction::DropItem) == KEY_Q);
        CHECK(Key(GameAction::Spellbook) == oldDrop);
        // After the swap nothing else owns Q, and Drop's old key now belongs to
        // the spellbook.
        CHECK(Conflict(KEY_Q, GameAction::DropItem) == -1);
        CHECK(Conflict(oldDrop, GameAction::Spellbook) == -1);
    }

    // Names are stable for the UI.
    CHECK(KeyName(KEY_G) == "G");
    CHECK(KeyName(KEY_SPACE) == "Space");
    CHECK(KeyName(KEY_LEFT_SHIFT) == "Left Shift");
    CHECK(KeyName(kMouseBase + MOUSE_BUTTON_LEFT) == "Mouse Left");
    CHECK(KeyName(KEY_F1) == "F1");

    ResetDefaults();

    if (g_fail == 0) {
        printf("Keybinds: %d/%d checks passed\n", g_checks, g_checks);
        return 0;
    }
    printf("Keybinds: %d/%d checks FAILED\n", g_fail, g_checks);
    return 1;
}
