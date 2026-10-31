#pragma once
// ---------------------------------------------------------------------------
// EditorKeys.hpp - remappable AngelEd shortcuts.
//
// The editor's shortcuts were scattered `IsKeyPressed(KEY_*)` calls in Main.cpp,
// and the File menu advertised bare N/O/S/P/Q accelerators that no code handled.
// They are now one table, persisted in System/AngelEd.ini [Keys], and editable
// from Help -> Keyboard Shortcuts.
//
// This is a top-level AngelEd/Source header (not Core/ or UI/) because both the
// frame loop (Core) and the keymap panel (UI) use it; keeping it out of both
// directories avoids an upward include from UI/.
//
// Modifiers are part of the action (Save is always Ctrl+S); rebinding changes the
// KEY and keeps the modifier, which is what the shortcut text shows.
// ---------------------------------------------------------------------------

#include "../Source/Input/Keybinds.hpp"   // raylib keys + KeyName
#include "../Source/IniConfig.hpp"
#include <windows.h>
#include <string>

enum class EdAction : int {
    New = 0,
    Open,
    Save,
    SaveAs,
    Play,
    Exit,
    Undo,
    Redo,
    Duplicate,
    Delete,
    Hierarchy,
    ModelBrowser,
    SoundManager,
    TextureManager,
    PawnManager,
    ScriptManager,
    PickupPanel,
    Heightmap,
    ModeModel,
    ModePickup,
    ModeNode,
    ModeZone,
    ModeTerrain,
    CsgCycle,
    Count
};

namespace EdKeys {

struct EdKeyDef {
    EdAction action;
    const char* id;       // INI key
    const char* label;    // dialog label
    int key;              // raylib key
    bool ctrl;
    bool shift;
};

inline const EdKeyDef kDefs[] = {
    {EdAction::New,           "new",            "File: New",              KEY_N, true,  false},
    {EdAction::Open,          "open",           "File: Open",             KEY_O, true,  false},
    {EdAction::Save,          "save",           "File: Save",             KEY_S, true,  false},
    {EdAction::SaveAs,        "save_as",        "File: Save As",          KEY_S, true,  true},
    {EdAction::Play,          "play",           "File: Play Test",        KEY_P, true,  false},
    {EdAction::Exit,          "exit",           "File: Exit",             KEY_Q, true,  false},
    {EdAction::Undo,          "undo",           "Edit: Undo",             KEY_Z, true,  false},
    {EdAction::Redo,          "redo",           "Edit: Redo",             KEY_Y, true,  false},
    {EdAction::Duplicate,     "duplicate",      "Edit: Duplicate",        KEY_D, true,  false},
    {EdAction::Delete,        "delete",         "Edit: Delete",           KEY_DELETE, false, false},
    {EdAction::Hierarchy,     "hierarchy",      "View: World Hierarchy",  KEY_F3, false, false},
    {EdAction::ModelBrowser,  "model_browser",  "View: Model Browser",    KEY_F5, false, false},
    {EdAction::SoundManager,  "sound_manager",  "View: Sound Manager",    KEY_F6, false, false},
    {EdAction::TextureManager,"texture_manager","View: Texture Manager",  KEY_F7, false, false},
    {EdAction::PawnManager,   "pawn_manager",   "View: Pawn Manager",     KEY_F8, false, false},
    {EdAction::ScriptManager, "script_manager", "View: Script Manager",   KEY_F9, false, false},
    {EdAction::PickupPanel,   "pickup_panel",   "View: Pickups",          KEY_F10, false, false},
    {EdAction::Heightmap,     "heightmap",      "View: Heightmap",        KEY_H, false, false},
    {EdAction::ModeModel,     "mode_model",     "Mode: Model",            KEY_ONE, false, false},
    {EdAction::ModePickup,    "mode_pickup",    "Mode: Pickup",           KEY_TWO, false, false},
    {EdAction::ModeNode,      "mode_node",      "Mode: Node",             KEY_THREE, false, false},
    {EdAction::ModeZone,      "mode_zone",      "Mode: Zone",             KEY_FOUR, false, false},
    {EdAction::ModeTerrain,   "mode_terrain",   "Mode: Terrain",          KEY_FIVE, false, false},
    {EdAction::CsgCycle,      "csg_cycle",      "Mode: Cycle CSG op",     KEY_G, false, false},
};
inline constexpr int kCount = (int)(sizeof(kDefs) / sizeof(kDefs[0]));
static_assert(kCount == (int)EdAction::Count, "Edit kDefs when adding EdAction");

inline int g_keys[kCount] = {};
inline bool g_loaded = false;

inline void EnsureDefaults() {
    if (g_loaded) return;
    for (int i = 0; i < kCount; i++) g_keys[i] = kDefs[i].key;
    g_loaded = true;
}

inline bool CtrlDown() {
    return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
}
inline bool ShiftDown() {
    return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
}
inline bool AltDown() {
    return IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
}

// --- Keyboard layout ---------------------------------------------------------
//
// raylib/GLFW key codes are POSITIONAL: they are the US virtual-key code of a
// SCANCODE, so they follow the US physical grid and not the label on the keycap.
// On German QWERTZ the two letters that swap are exactly the two that matter
// here: the key labelled Z sits at the US-Y scancode, so pressing it reports
// KEY_Y. Ctrl+Z therefore matched nothing, Edit: Undo never fired, and the Y
// nudge stepped the selection down instead. AZERTY swaps A/Q and Z/W; Dvorak
// moves nearly everything.
//
// A stored binding is therefore LOGICAL ("the letter Z") and is resolved to the
// physical key that produces that letter on the ACTIVE layout. Windows does the
// layout work for us in both directions once we convert through the scancode:
//
//   logical letter --VK_TO_VSC(active)--> scancode --VSC_TO_VK(US)--> raylib key
//
// Verified against a real German layout: 'Z' -> scan 0x15 -> VK 0x59 = KEY_Y,
// and raylib KEY_Y -> scan 0x15 -> VK 0x5A = 'Z'.
//
// Note MAPVK_VK_TO_CHAR is NOT usable for this: for A-Z it returns the VK itself
// regardless of layout, so it looks correct on a US keyboard and does nothing on
// a QWERTZ one.
inline HKL UsLayout() {
    static HKL s_us = nullptr;
    if (!s_us) {
        s_us = LoadKeyboardLayoutW(L"00000409", KLF_NOTELLSHELL);
        if (!s_us) s_us = (HKL)(ULONG_PTR)0x04090409;   // en-US is always present
    }
    return s_us;
}

inline int PhysicalKey(int logical) {
    if (logical < 'A' || logical > 'Z') return logical;   // only letters move
    const HKL lay = GetKeyboardLayout(0);
    const UINT sc = MapVirtualKeyExW((UINT)logical, MAPVK_VK_TO_VSC, lay);
    if (!sc) return logical;
    const UINT us = MapVirtualKeyExW(sc, MAPVK_VSC_TO_VK, UsLayout());
    return us ? (int)us : logical;
}

// Inverse of PhysicalKey: a key CAPTURED in the keymap dialog is stored as the
// letter it produces, so the INI stays portable across layouts and machines.
inline int LogicalKey(int physical) {
    if (physical < 'A' || physical > 'Z') return physical;
    const HKL lay = GetKeyboardLayout(0);
    const UINT sc = MapVirtualKeyExW((UINT)physical, MAPVK_VK_TO_VSC, UsLayout());
    if (!sc) return physical;
    const UINT vk = MapVirtualKeyExW(sc, MAPVK_VSC_TO_VK_EX, lay);
    return (vk >= 'A' && vk <= 'Z') ? (int)vk : physical;
}

inline int Index(EdAction a) {
    const int i = (int)a;
    return (i >= 0 && i < kCount) ? i : -1;
}

inline int Key(EdAction a) {
    EnsureDefaults();
    const int i = Index(a);
    // PHYSICAL key, so `IsKeyPressed(EdKeys::Key(...))` sites follow the user's
    // layout exactly like Pressed() does.
    return i >= 0 ? PhysicalKey(g_keys[i]) : 0;
}

// The LOGICAL key of an action (what the shortcut is NAMED after). Shortcut text
// must use this, not Key(), or a German user reads "Ctrl+Y" for Undo.
inline int LogicalBinding(EdAction a) {
    EnsureDefaults();
    const int i = Index(a);
    return i >= 0 ? g_keys[i] : 0;
}

inline bool Pressed(EdAction a) {
    EnsureDefaults();
    const int i = Index(a);
    if (i < 0) return false;
    const auto& d = kDefs[i];
    if (!IsKeyPressed(PhysicalKey(g_keys[i]))) return false;
    if (d.ctrl != CtrlDown()) return false;
    if (d.shift != ShiftDown()) return false;
    return true;
}

inline void Load(IniConfig& cfg) {
    EnsureDefaults();
    for (int i = 0; i < kCount; i++) {
        const int v = cfg.GetInt("Keys", kDefs[i].id, g_keys[i]);
        if (v >= 32 && v <= 348) g_keys[i] = v;
    }
}

inline void Load() {
    IniConfig cfg;
    if (cfg.Load("System/AngelEd.ini")) Load(cfg);
    else EnsureDefaults();
}

inline void Save(IniConfig& cfg) {
    EnsureDefaults();
    for (int i = 0; i < kCount; i++)
        cfg.SetInt("Keys", kDefs[i].id, g_keys[i]);
}

inline bool Save() {
    IniConfig cfg;
    cfg.Load("System/AngelEd.ini");
    Save(cfg);
    return cfg.Save("System/AngelEd.ini");
}

inline void ResetDefaults() {
    for (int i = 0; i < kCount; i++) g_keys[i] = kDefs[i].key;
    g_loaded = true;
}

// The action already using `key` with the same modifier requirements, or -1.
// Different modifiers mean no conflict (Ctrl+S and Ctrl+Shift+S coexist).
inline int Conflict(EdAction a, int key) {
    EnsureDefaults();
    const int self = Index(a);
    if (self < 0) return -1;
    for (int i = 0; i < kCount; i++) {
        if (i == self) continue;
        if (g_keys[i] != key) continue;
        if (kDefs[i].ctrl != kDefs[self].ctrl) continue;
        if (kDefs[i].shift != kDefs[self].shift) continue;
        return i;
    }
    return -1;
}

// Assign a key. `key` is a PHYSICAL key (what the capture dialog saw) and is
// stored LOGICALLY, so rebinding on one layout does not produce a binding that
// means a different letter on another. Returns the conflicting action index (it
// is swapped, so no action is left unbound), or -1.
inline int Assign(EdAction a, int key) {
    EnsureDefaults();
    const int self = Index(a);
    key = LogicalKey(key);
    if (self < 0 || key < 32 || key > 348) return -1;
    const int other = Conflict(a, key);
    if (other >= 0) g_keys[other] = g_keys[self];
    g_keys[self] = key;
    return other;
}

inline std::string ShortcutText(EdAction a) {
    const int i = Index(a);
    if (i < 0) return "";
    std::string s;
    if (kDefs[i].ctrl) s += "Ctrl+";
    if (kDefs[i].shift) s += "Shift+";
    // The LOGICAL binding, so the hint names the letter on the keycap.
    s += Keybinds::KeyName(LogicalBinding(a));
    return s;
}

// Map a Win32 virtual-key code to a raylib key code. Used by the keymap dialog's
// synchronous capture; letters/digits already share values.
inline int FromVirtualKey(int vk) {
    if (vk >= 'A' && vk <= 'Z') return vk;
    if (vk >= '0' && vk <= '9') return vk;
    switch (vk) {
        case VK_SPACE: return KEY_SPACE;
        case VK_TAB: return KEY_TAB;
        case VK_ESCAPE: return KEY_ESCAPE;
        case VK_RETURN: return KEY_ENTER;
        case VK_BACK: return KEY_BACKSPACE;
        case VK_LEFT: return KEY_LEFT;
        case VK_RIGHT: return KEY_RIGHT;
        case VK_UP: return KEY_UP;
        case VK_DOWN: return KEY_DOWN;
        case VK_HOME: return KEY_HOME;
        case VK_END: return KEY_END;
        case VK_PRIOR: return KEY_PAGE_UP;
        case VK_NEXT: return KEY_PAGE_DOWN;
        case VK_INSERT: return KEY_INSERT;
        case VK_DELETE: return KEY_DELETE;
        case VK_F1:  return KEY_F1;
        case VK_F2:  return KEY_F2;
        case VK_F3:  return KEY_F3;
        case VK_F4:  return KEY_F4;
        case VK_F5:  return KEY_F5;
        case VK_F6:  return KEY_F6;
        case VK_F7:  return KEY_F7;
        case VK_F8:  return KEY_F8;
        case VK_F9:  return KEY_F9;
        case VK_F10: return KEY_F10;
        case VK_F11: return KEY_F11;
        case VK_F12: return KEY_F12;
        case VK_OEM_3: return KEY_GRAVE;
        case VK_OEM_MINUS: return KEY_MINUS;
        case VK_OEM_PLUS: return KEY_EQUAL;
        case VK_OEM_4: return KEY_LEFT_BRACKET;
        case VK_OEM_6: return KEY_RIGHT_BRACKET;
        case VK_OEM_5: return KEY_BACKSLASH;
        case VK_OEM_1: return KEY_SEMICOLON;
        case VK_OEM_7: return KEY_APOSTROPHE;
        case VK_OEM_COMMA: return KEY_COMMA;
        case VK_OEM_PERIOD: return KEY_PERIOD;
        case VK_OEM_2: return KEY_SLASH;
        case VK_NUMPAD0: return KEY_KP_0;
        case VK_NUMPAD1: return KEY_KP_1;
        case VK_NUMPAD2: return KEY_KP_2;
        case VK_NUMPAD3: return KEY_KP_3;
        case VK_NUMPAD4: return KEY_KP_4;
        case VK_NUMPAD5: return KEY_KP_5;
        case VK_NUMPAD6: return KEY_KP_6;
        case VK_NUMPAD7: return KEY_KP_7;
        case VK_NUMPAD8: return KEY_KP_8;
        case VK_NUMPAD9: return KEY_KP_9;
        default: return -1;
    }
}

} // namespace EdKeys
