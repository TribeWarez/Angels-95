// ============================================================================
// LevelState.cpp
//
// FRAGMENT - not a standalone translation unit. Included by ../UiShell.cpp,
// which is the single TU for the whole UI layer. That is deliberate: see
// UiShell.cpp for why the layer is built as a unity build rather than as 16
// separate objects.
// ============================================================================

// =====================================================================
// Level state (was: Environment Settings / Zone Properties)
// =====================================================================
// =====================================================================
// The standalone Zone Properties window is gone. Its fog/ambient rows are now
// the Environment section of the Entity Properties panel (per-zone, written to
// ZoneVolumeNode::envOverrides) and its GameType/skybox/particle rows are the
// "Map (level)" row of the WorldGraph (LevelMetadata). g_zoneTab, g_zoneProps,
// GetZoneProperties, ClearZoneApplyFlags, ShowEnvPanel, ZonePropertiesProc and
// SetLevelMetadata's 17-field mirror into g_zoneProps all went with it.

// --- Available worlds scan (for portal targets + LevelList) ---
static std::vector<std::string> g_availableWorlds;

static void ScanAvailableWorlds() {
    g_availableWorlds.clear();
    const char* roots[] = {"GameData/Worlds", "../GameData/Worlds"};
    for (const char* root : roots) {
        fs::path base(root);
        if (!fs::exists(base)) continue;
        for (auto& entry : fs::directory_iterator(base)) {
            if (!entry.is_directory()) continue;
            std::string name = entry.path().filename().string();
            if (name == "Legacy") continue; // nested legacy world dump
            if (fs::exists(entry.path() / "World.ozone")) {
                // avoid duplicates when both roots resolve to the same tree
                bool dup = false;
                for (const auto& w : g_availableWorlds) if (w == name) { dup = true; break; }
                if (!dup) g_availableWorlds.push_back(name);
            }
        }
    }
    std::sort(g_availableWorlds.begin(), g_availableWorlds.end());
}

// Tab control groups, the tab IDs and the Fog/Ambient/GameType/Particles/Portal
// control IDs of the removed Zone Properties window went with it.

// Portal editing state (shared with Main.cpp via accessors)
// --- Portal data plumbing ---
void RefreshPortalList() {
    // Intentionally inert. This used to rebuild the removed Zone window's portal
    // combo, so its whole body was already a no-op behind a `hEnvPanel` guard.
    // Portals are edited in the Entity Properties PORTAL section now, which reads
    // ZoneManager::GetPortals() directly, so there is no list left to refresh.
    // Kept as a function because three Main.cpp call sites (portal create/delete)
    // treat it as the "portals changed" notification.
    //
    // GetPortalCount / GetPortalTargetWorld used to sit here too. They had zero
    // callers and were deleted.
}

// Generic image picker. Prefers a repo-relative GameData/ path so saved worlds
// stay portable. Used by the entity/model properties texture fields and the
// Zone Properties skybox.
static bool ChooseImageFile(std::string& outPath) {
    wchar_t path[MAX_PATH] = {};
    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_hRaylibWnd;
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrFilter = L"Images (*.png;*.jpg;*.jpeg;*.bmp;*.tga)\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0All Files (*.*)\0*.*\0\0";
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameW(&dialog)) return false;

    int size = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return false;
    std::vector<char> utf8((size_t)size);
    WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8.data(), size, nullptr, nullptr);
    outPath.assign(utf8.data());

    std::string s = outPath;
    for (auto& c : s) if (c == '\\') c = '/';
    size_t gd = s.find("GameData/");
    if (gd != std::string::npos) outPath = s.substr(gd);
    return true;
}

// Skybox texture picker for the Zone Properties Fog tab.
static bool ChooseSkyboxFile(std::string& outPath) { return ChooseImageFile(outPath); }

// RGB colour picker (CHOOSECOLORW). Used by the Map and Zone "Environment"
// sections, which carry fog / ambient / particle colour channels as text fields.
//
// The legacy Zone Properties dialog used three HSCROLL sliders per colour, which
// cannot express an exact value and gave no way to *type* one — so a round-tripped
// value could only be nudged. Text fields plus a picker can do both.
static bool ChooseColorRGB(int& r, int& g, int& b) {
    // Clamped inline rather than via Main.cpp's file-static ClampPropInt: this TU
    // must not take a dependency on Main.cpp, and a colour channel is the one
    // value that genuinely cannot be left out of range (BYTE truncation).
    auto clamp = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
    CHOOSECOLORW cc = {};
    static COLORREF cust[16] = {0};   // must outlive the dialog
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = g_hRaylibWnd;
    cc.rgbResult = RGB((BYTE)clamp(r), (BYTE)clamp(g), (BYTE)clamp(b));
    cc.lpCustColors = cust;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc)) return false;
    r = GetRValue(cc.rgbResult);
    g = GetGValue(cc.rgbResult);
    b = GetBValue(cc.rgbResult);
    return true;
}

// Sound picker for the editable `.ozls` stat rows (fire_sound, jump_sound, ...).
// Normalised to a repo-relative GameData/ path like ChooseImageFile, so a
// committed .ozls stays portable.
//
// The value is written bare into the file, and the .ozls stats parser truncates
// a string at the first space with no quoting, so a filename containing a space
// would silently resolve to the wrong asset. Reject it here rather than let the
// author save something that cannot work.
static bool ChooseSoundFile(std::string& outPath) {
    wchar_t path[MAX_PATH] = {};
    OPENFILENAMEW d = {};
    d.lStructSize = sizeof(d);
    d.hwndOwner = g_hRaylibWnd;
    d.lpstrFile = path;
    d.nMaxFile = MAX_PATH;
    d.lpstrFilter = L"Sounds (*.wav;*.mp3;*.ogg;*.flac)\0*.wav;*.mp3;*.ogg;*.flac\0All Files (*.*)\0*.*\0\0";
    d.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameW(&d)) return false;

    int size = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return false;
    std::vector<char> utf8((size_t)size);
    WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8.data(), size, nullptr, nullptr);
    std::string s(utf8.data());

    for (auto& c : s) if (c == '\\') c = '/';
    size_t gd = s.find("GameData/");
    outPath = (gd != std::string::npos) ? s.substr(gd) : s;

    if (outPath.find(' ') != std::string::npos || outPath.find('\t') != std::string::npos) {
        MessageBoxA(g_hRaylibWnd,
            "This filename contains a space.\n\n"
            "A .ozls stats value is stored verbatim and stops at the first\n"
            "space, so the sound would never resolve. Rename the file.",
            "Cannot use this sound", MB_OK | MB_ICONWARNING);
        outPath.clear();
        return false;
    }
    return true;
}

// `.ozanim` vertex-keyframe picker (normalized to a GameData-relative path).
static bool ChooseAnimFile(std::string& outPath) {
    wchar_t path[MAX_PATH] = {};
    OPENFILENAMEW d = {};
    d.lStructSize = sizeof(d);
    d.hwndOwner = g_hRaylibWnd;
    d.lpstrFile = path;
    d.nMaxFile = MAX_PATH;
    d.lpstrFilter = L"OzAnim (*.ozanim)\0*.ozanim\0All Files (*.*)\0*.*\0\0";
    d.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameW(&d)) return false;

    int size = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return false;
    std::vector<char> utf8((size_t)size);
    WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8.data(), size, nullptr, nullptr);
    outPath.assign(utf8.data());

    std::string s = outPath;
    for (auto& c : s) if (c == '\\') c = '/';
    size_t gd = s.find("GameData/");
    if (gd != std::string::npos) outPath = s.substr(gd);
    return true;
}

// The Zone Properties window procedure is gone. Its controls, its five tabs and
// SetSkyboxField (which existed only to feed the window's skybox text field)
// all went with it.

