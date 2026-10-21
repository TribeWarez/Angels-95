#pragma once
// ---------------------------------------------------------------------------
// Keybinds.hpp - one remappable action table for gameplay input.
//
// Before this, every gameplay site polled a hardcoded raylib key:
//   PlayerMovement (WASD/shift/ctrl), PlayerPhysics (space/ctrl), PlayerController
//   (K/Q/Tab), InventoryBehaviour (Q drop), LightningEntityManager (1-8/R/E),
//   Main.cpp (E pickup, grave console, Esc pause), WeaponBehaviour (LMB).
// There was no way to change any of it, and Drop used the SAME KEY as the
// spellbook (Q), so dropping while the inventory was open also toggled the
// spellbook and closed the overlay.
//
// Header-only with `inline` state so it needs no build-list entries; every TU
// links the same instance. Raylib types are used for the key constants, so a
// test that includes this needs raylib (tests/Keybinds.test.cpp does).
// ---------------------------------------------------------------------------

#include "raylib.h"
#include "../IniConfig.hpp"
#include <string>
#include <cstdio>

enum class GameAction : int {
    MoveForward = 0,
    MoveBack,
    MoveLeft,
    MoveRight,
    Jump,
    Sprint,
    Crouch,
    Interact,
    Inventory,
    Spellbook,
    Skills,
    DropItem,
    Console,
    Pause,
    Reload,
    Fire,
    AltFire,
    Hotbar1,
    Hotbar2,
    Hotbar3,
    Hotbar4,
    Hotbar5,
    Hotbar6,
    Hotbar7,
    Hotbar8,
    Count
};

namespace Keybinds {

// Mouse bindings use 1000 + raylib button index so a binding is one int.
inline constexpr int kMouseBase = 1000;

struct ActionDef {
    GameAction action;
    const char* id;       // INI key
    const char* label;    // UI label
    int def;              // default binding
};

inline const ActionDef kDefs[] = {
    {GameAction::MoveForward, "move_forward", "Move Forward",  KEY_W},
    {GameAction::MoveBack,    "move_back",    "Move Back",     KEY_S},
    {GameAction::MoveLeft,    "move_left",    "Move Left",     KEY_A},
    {GameAction::MoveRight,   "move_right",   "Move Right",    KEY_D},
    {GameAction::Jump,        "jump",         "Jump",          KEY_SPACE},
    {GameAction::Sprint,      "sprint",       "Sprint",        KEY_LEFT_SHIFT},
    {GameAction::Crouch,      "crouch",       "Crouch",        KEY_LEFT_CONTROL},
    {GameAction::Interact,    "interact",     "Interact / Pick up", KEY_E},
    {GameAction::Inventory,   "inventory",    "Inventory",     KEY_TAB},
    {GameAction::Spellbook,   "spellbook",    "Spellbook",     KEY_Q},
    {GameAction::Skills,      "skills",       "Skill Tree",    KEY_K},
    {GameAction::DropItem,    "drop_item",    "Drop Item",     KEY_G},
    {GameAction::Console,     "console",      "Console",       KEY_GRAVE},
    {GameAction::Pause,       "pause",        "Pause / Back",  KEY_ESCAPE},
    {GameAction::Reload,      "reload",       "Reload",        KEY_R},
    {GameAction::Fire,        "fire",         "Fire",          kMouseBase + MOUSE_BUTTON_LEFT},
    {GameAction::AltFire,     "alt_fire",     "Aim / Alt Fire", kMouseBase + MOUSE_BUTTON_RIGHT},
    {GameAction::Hotbar1,     "hotbar_1",     "Hotbar 1",      KEY_ONE},
    {GameAction::Hotbar2,     "hotbar_2",     "Hotbar 2",      KEY_TWO},
    {GameAction::Hotbar3,     "hotbar_3",     "Hotbar 3",      KEY_THREE},
    {GameAction::Hotbar4,     "hotbar_4",     "Hotbar 4",      KEY_FOUR},
    {GameAction::Hotbar5,     "hotbar_5",     "Hotbar 5",      KEY_FIVE},
    {GameAction::Hotbar6,     "hotbar_6",     "Hotbar 6",      KEY_SIX},
    {GameAction::Hotbar7,     "hotbar_7",     "Hotbar 7",      KEY_SEVEN},
    {GameAction::Hotbar8,     "hotbar_8",     "Hotbar 8",      KEY_EIGHT},
};
inline constexpr int kActionCount = (int)(sizeof(kDefs) / sizeof(kDefs[0]));
static_assert(kActionCount == (int)GameAction::Count,
              "kDefs must cover every GameAction");

inline int g_keys[kActionCount] = {};
inline bool g_loaded = false;

inline bool IsValidKey(int key) {
    if (key >= kMouseBase && key <= kMouseBase + 2) return true;   // L/M/R
    return key >= 32 && key <= 348;                                 // raylib keys
}

inline void EnsureDefaults() {
    if (g_loaded) return;
    for (int i = 0; i < kActionCount; i++) g_keys[i] = kDefs[i].def;
    g_loaded = true;
}

inline int ActionIndex(GameAction a) {
    const int i = (int)a;
    return (i >= 0 && i < kActionCount) ? i : -1;
}

inline int Key(GameAction a) {
    EnsureDefaults();
    const int i = ActionIndex(a);
    return (i >= 0) ? g_keys[i] : 0;
}

inline bool Down(GameAction a) {
    const int k = Key(a);
    if (k >= kMouseBase) return IsMouseButtonDown(k - kMouseBase);
    return k > 0 && IsKeyDown(k);
}

inline bool Pressed(GameAction a) {
    const int k = Key(a);
    if (k >= kMouseBase) return IsMouseButtonPressed(k - kMouseBase);
    return k > 0 && IsKeyPressed(k);
}

inline void Load(IniConfig& cfg) {
    EnsureDefaults();
    for (int i = 0; i < kActionCount; i++) {
        const int v = cfg.GetInt("Keybinds", kDefs[i].id, g_keys[i]);
        if (IsValidKey(v)) g_keys[i] = v;
    }
}

inline void Save(IniConfig& cfg) {
    EnsureDefaults();
    for (int i = 0; i < kActionCount; i++)
        cfg.SetInt("Keybinds", kDefs[i].id, g_keys[i]);
}

inline void ResetDefaults() {
    for (int i = 0; i < kActionCount; i++) g_keys[i] = kDefs[i].def;
    g_loaded = true;
}

// The action already bound to `key`, or -1. Used by the Controls UI to warn.
inline int Conflict(int key, GameAction ignore) {
    EnsureDefaults();
    for (int i = 0; i < kActionCount; i++) {
        if (i == (int)ignore) continue;
        if (g_keys[i] == key) return i;
    }
    return -1;
}

// Assign a binding. On a conflict the two actions SWAP, so a rebind can never
// leave an action silently unbound (the usual outcome of "clear the old one").
// Returns the action it swapped with, or -1.
inline int Assign(GameAction a, int key) {
    EnsureDefaults();
    const int i = ActionIndex(a);
    if (i < 0 || !IsValidKey(key)) return -1;
    const int other = Conflict(key, a);
    if (other >= 0) g_keys[other] = g_keys[i];
    g_keys[i] = key;
    return other;
}

inline std::string KeyName(int key) {
    if (key >= kMouseBase) {
        switch (key - kMouseBase) {
            case MOUSE_BUTTON_LEFT:   return "Mouse Left";
            case MOUSE_BUTTON_RIGHT:  return "Mouse Right";
            case MOUSE_BUTTON_MIDDLE: return "Mouse Middle";
            default: return "Mouse " + std::to_string(key - kMouseBase);
        }
    }
    if (key >= 'A' && key <= 'Z') return std::string(1, (char)key);
    if (key >= '0' && key <= '9') return std::string(1, (char)key);
    switch (key) {
        case KEY_SPACE: return "Space";
        case KEY_TAB: return "Tab";
        case KEY_ESCAPE: return "Escape";
        case KEY_GRAVE: return "`";
        case KEY_ENTER: return "Enter";
        case KEY_BACKSPACE: return "Backspace";
        case KEY_LEFT_SHIFT: return "Left Shift";
        case KEY_RIGHT_SHIFT: return "Right Shift";
        case KEY_LEFT_CONTROL: return "Left Ctrl";
        case KEY_RIGHT_CONTROL: return "Right Ctrl";
        case KEY_LEFT_ALT: return "Left Alt";
        case KEY_RIGHT_ALT: return "Right Alt";
        case KEY_UP: return "Up";
        case KEY_DOWN: return "Down";
        case KEY_LEFT: return "Left";
        case KEY_RIGHT: return "Right";
        case KEY_PAGE_UP: return "Page Up";
        case KEY_PAGE_DOWN: return "Page Down";
        case KEY_HOME: return "Home";
        case KEY_END: return "End";
        case KEY_INSERT: return "Insert";
        case KEY_DELETE: return "Delete";
        default: break;
    }
    if (key >= KEY_F1 && key <= KEY_F12)
        return "F" + std::to_string(key - KEY_F1 + 1);
    if (key == KEY_APOSTROPHE) return "'";
    if (key == KEY_COMMA) return ",";
    if (key == KEY_PERIOD) return ".";
    if (key == KEY_SLASH) return "/";
    if (key == KEY_SEMICOLON) return ";";
    if (key == KEY_EQUAL) return "=";
    if (key == KEY_MINUS) return "-";
    if (key == KEY_LEFT_BRACKET) return "[";
    if (key == KEY_RIGHT_BRACKET) return "]";
    if (key == KEY_BACKSLASH) return "\\";
    return "Key " + std::to_string(key);
}

// The first key or mouse button pressed this frame, for the rebind capture.
// Returns -1 when nothing was pressed. Escape is returned normally; the caller
// treats it as "cancel" rather than as a binding.
inline int CapturePressedKey() {
    for (int k = 32; k <= 348; k++)
        if (IsKeyPressed(k)) return k;
    for (int b = 0; b <= 2; b++)
        if (IsMouseButtonPressed(b)) return kMouseBase + b;
    return -1;
}

} // namespace Keybinds
