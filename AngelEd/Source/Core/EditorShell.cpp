// =============================================================================
// Core/EditorShell.cpp
//
// Camera view presets, the panel toggle shims, world directory and scene reset.
//
// FRAGMENT - not a standalone translation unit. Included by Main.cpp, which is the
// single TU for AngelEd's core layer. See Wiki/Editor-Architecture-Refactor.md.
// =============================================================================

// Make the cwd a usable project root before anything reads a relative path.
//
// Almost EVERY asset path in the editor is relative and unanchored - "GameData",
// "System/AngelEd.ini", "System/Data" - so launching from the wrong directory produces
// a wave of silent failures rather than one clear error. Two were reported from a real
// session and neither said so:
//
//   WARN: GameData/Pawn/PawnDefs not found, using hardcoded defaults
//   Skybox: could not load 'GameData/Tex/Global/Sky/sky_noon.png'
//
// The previous code handled exactly one case: cwd whose final component is literally
// "System". That is the case the run scripts set up and nothing else, so a shortcut with
// a different "Start in", a copy of the tree elsewhere, or a debugger with a different
// working directory all landed in the broken state.
//
// Returns true if the cwd is (or has been made) a project root. Never fails hard - the
// editor can still open an explicit world from a GameData-less layout - but it logs the
// directory it tried, because that is the one fact needed to diagnose what follows.
static bool EnsureProjectRoot() {
    auto looksLikeRoot = [](const fs::path& p) {
        return fs::exists(p / "GameData") && fs::exists(p / "System");
    };

    // 1. Already correct.
    if (looksLikeRoot(fs::current_path()))
        return true;

    // 2. cwd is System/ - the documented way the run scripts launch. Keeps the previous
    //    behaviour, but only when the parent really is a root.
    {
        fs::path cwd = fs::current_path();
        std::string leaf = cwd.filename().string();
        std::transform(leaf.begin(), leaf.end(), leaf.begin(), ::tolower);
        if (leaf == "system" && looksLikeRoot(cwd.parent_path())) {
            fs::current_path(cwd.parent_path());
            EditorLog("Project root: %s (from System/)", fs::current_path().string().c_str());
            return true;
        }
    }

    // 3. Walk up from the executable. Four levels covers bin/<cfg>/, AngelEd/, and a
    //    couple of nesting levels people actually use.
    char buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n && n < MAX_PATH) {
        fs::path dir = fs::path(buf).parent_path();
        for (int up = 0; up < 4 && !dir.empty(); ++up) {
            if (looksLikeRoot(dir)) {
                SetCurrentDirectoryA(dir.string().c_str());
                EditorLog("Project root: %s (from exe)", fs::current_path().string().c_str());
                return true;
            }
            if (dir == dir.parent_path())
                break;                       // reached the drive root
            dir = dir.parent_path();
        }
    }

    EditorLog("WARNING: no project root found. Looked in cwd '%s' and up to 4 levels "
              "above the executable. Every asset path is relative, so expect missing "
              "GameData content - open a world explicitly or run from the repo root.",
              fs::current_path().string().c_str());
    return false;
}

// The first world under GameData/Worlds/ that actually has a World.ozone, or empty.
//
// Replaces a default of "../GameData/World.ozone" that resolved in no layout at all.
// Sorted so the choice is deterministic across machines rather than depending on
// directory-iteration order.
static fs::path FirstAvailableWorld() {
    const fs::path root = "GameData/Worlds";
    std::error_code ec;
    if (!fs::exists(root, ec)) return {};
    std::vector<fs::path> dirs;
    for (auto& d : fs::directory_iterator(root, ec)) {
        if (d.is_directory()) dirs.push_back(d.path());
        if (ec) break;
    }
    std::sort(dirs.begin(), dirs.end());
    for (const auto& d : dirs) {
        if (fs::exists(d / "World.ozone")) return d / "World.ozone";
    }
    return {};
}
static void SetViewPreset(const Vector3& pos, const Vector3& target, const Vector3& up) {
    if (!g_orthoView) {
        g_perspectiveCamState = OTEditor.MainCamera;
        g_orthoView = true;
    }
    OTEditor.MainCamera.position = pos;
    OTEditor.MainCamera.target = target;
    OTEditor.MainCamera.up = up;
    OTEditor.MainCamera.projection = CAMERA_ORTHOGRAPHIC;
}

static void SetViewPerspective() {
    if (g_orthoView) {
        // Restore saved state
        OTEditor.MainCamera = g_perspectiveCamState;
        g_orthoView = false;
    }
    OTEditor.MainCamera.projection = CAMERA_PERSPECTIVE;
}

// Panel toggle helpers (keyboard-driven, no top menu bar)
static void ToggleSoundMgr()    { ShowSoundManager(!g_editorPanels.showSoundMgr); }
static void ToggleTextureMgr()  { ShowTextureManager(!g_editorPanels.showTextureMgr); }
static void TogglePawnMgr()     { ShowPawnManager(!g_editorPanels.showPawnMgr); }
static void ToggleScriptMgr()   { ShowScriptManager(!g_editorPanels.showScriptMgr); }
static void ToggleModelBrowser(){ ShowModelBrowser(!g_editorPanels.showModelBrowser); }
static void TogglePickupPanel() { ShowPickupPanel(!g_editorPanels.showPickupPanel); }
static void ToggleNodePanel()   { ShowNodePanel(!g_editorPanels.showNodePanel); }
// Zone placement mode. Used to be ToggleEnvPanel(), which toggled the standalone
// Zone Properties window AND set placement mode — two unrelated things behind one
// key. The window is gone (its per-zone rows live in Entity Properties and its
// level-state rows in the Map row), so this is only the placement half.
static void ToggleZonePlacement() { g_placeMode = PlaceMode::ENV; }
static void ToggleHeightmapEditor() { ShowHeightmapEditor(!g_editorPanels.showHeightmapEditor); }
static void ToggleWorldGraph() { ShowWorldGraph(!g_editorPanels.showWorldGraph); }
static void ToggleAnimPanel()   { ShowAnimPanel(!g_editorPanels.showAnimPanel); }
static void ResetCamera()       { OTEditor.MainCamera.position = {0, 10, 0}; OTEditor.MainCamera.target = {0, 0, 0}; OTEditor.MainCamera.up = {0, 1, 0}; }
static void CamUp()             { OTEditor.MainCamera.position.y += 2; }
static void CamDown()           { OTEditor.MainCamera.position.y -= 2; }

// Model preview for Win32 dialog (render-to-texture)
static RenderTexture2D g_previewRT = {0};
static int g_lastPreviewSel = -1;
static bool g_previewNeedsUpdate = false;

static void SetWorldDirectory(const fs::path& directory) {
    std::string path = directory.string();
    if (!path.empty() && path.back() != '/' && path.back() != '\\') path += fs::path::preferred_separator;
    std::strncpy(OTEditor.Path, path.c_str(), sizeof(OTEditor.Path) - 1);
    OTEditor.Path[sizeof(OTEditor.Path) - 1] = '\0';
}

static void StopSoundPreview() {
    if (!g_previewSoundLoaded) return;
    StopSound(g_previewSound);
    UnloadSound(g_previewSound);
    g_previewSound = {0};
    g_previewSoundLoaded = false;
}

// NOT static: Subsystems/History.cpp is a real translation unit and calls this from
// HistoryRestore(). Same reason as ExportToOzone / g_editorLog - a `static` in a unity
// fragment has internal linkage and cannot satisfy an external reference.
void ClearScene() {
    StopSoundPreview();
    OzoneLoader::Instance().Unload();
    auto& pawns = PawnSystem::Instance();
    auto& zones = ZoneManager::Instance();
    pawns.DespawnAll();
    pawns.ClearPlayerStarts();
    pawns.ClearPickups();
    zones.ClearZones();
    pawns.ClearLights();     // lights otherwise accumulate across loads
    zones.ClearPortals();
    pawns.ClearEmitters();
    pawns.ClearParticleEmitters();
    pawns.ClearPathNodes();
    pawns.ClearWindZones();
    pawns.ClearMeshObjects();
    pawns.ClearSkyZones();
    OmegaTechEditor.DrawModel = false;
}
