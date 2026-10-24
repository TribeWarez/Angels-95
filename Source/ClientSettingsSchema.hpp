#pragma once

// Schema for System/Angels95.ini: the section names and the legacy-migration
// rule, split out from ClientSettings.hpp so they can be tested headlessly.
//
// ClientSettings.hpp pulls in Settings.hpp, which pulls in raygui/raylib, so a
// test that includes it needs a GL-less raylib build and cannot touch the
// statics it persists. The two things that actually go wrong here are testable
// on their own and raylib-free:
//
//   1. Load and Save disagreeing about a section name. The symptom is silent:
//      the load reads defaults, the save writes them back over the player's
//      file, and the settings appear to reset at random.
//   2. A legacy flat [Settings] file being ignored on upgrade, which loses
//      every option once and then looks like a regression in the new layout.
//
// tests/ClientSettings.test.cpp covers both, so it owns no runtime state.

#include "IniConfig.hpp"
#include <string>
#include <vector>

namespace clientcfg {

inline constexpr const char* kWindow   = "AngelTech.Window";
inline constexpr const char* kRender   = "AngelTech.Render";
inline constexpr const char* kAudio    = "AngelTech.Audio";
inline constexpr const char* kGameplay = "AngelTech.Gameplay";
inline constexpr const char* kInput    = "AngelTech.Input";
inline constexpr const char* kHud      = "AngelTech.HUD";
inline constexpr const char* kDebug    = "AngelTech.Debug";

// Pre-scoping layout: every client setting lived in one flat [Settings] block.
// Still read, per key, so an existing install migrates on its first save.
inline constexpr const char* kLegacy   = "Settings";

// Sections the client owns, in the order they are written. A section missing
// from this list is not dropped (SaveOrdered appends it alphabetically), but it
// does not get its intended position.
inline const std::vector<std::string> kSectionOrder = {
    kWindow, kRender, kAudio, kGameplay, kInput, kHud, kDebug,
    "Keybinds", "MasterServers",
};

// Read `section`, falling back to the legacy block. The fallback is per KEY, not
// per section, so a half-migrated or hand-edited file still loads.
inline int Int(IniConfig& cfg, const char* section, const char* key, int def) {
    if (cfg.Has(section, key)) return cfg.GetInt(section, key, def);
    return cfg.GetInt(kLegacy, key, def);
}

inline float Float(IniConfig& cfg, const char* section, const char* key, float def) {
    if (cfg.Has(section, key)) return cfg.GetFloat(section, key, def);
    return cfg.GetFloat(kLegacy, key, def);
}

inline bool Bool(IniConfig& cfg, const char* section, const char* key, bool def) {
    if (cfg.Has(section, key)) return cfg.GetBool(section, key, def);
    return cfg.GetBool(kLegacy, key, def);
}

} // namespace clientcfg
