// =============================================================================
// Subsystems/WorldGraphBridge.cpp
//
// The accessor surface the UI layer calls into, and the Win32 menu dispatch.
//
// FRAGMENT - not a standalone translation unit. Included by Main.cpp, which is the
// single TU for AngelEd's core layer. See Wiki/Editor-Architecture-Refactor.md.
// =============================================================================

#include "../Resources/MaterialBake.hpp"
#include "../../../Source/Material/MaterialRegistry.hpp"

// ---------------------------------------------------------------------------
// Accessors for WorldGraph panel (called from Win32Dialogs.cpp)
// ---------------------------------------------------------------------------
int WorldGraph_GetModelCount() { return CachedModelCounter; }
void WorldGraph_GetModelData(int index, float& outX, float& outY, float& outZ, float& outR, float& outS) {
    if (index >= 0 && index < CachedModelCounter) {
        outX = CachedModels[index].X; outY = CachedModels[index].Y; outZ = CachedModels[index].Z;
        outR = CachedModels[index].R; outS = CachedModels[index].S;
    }
}
const char* WorldGraph_GetModelName(int index) {
    if (index < 0 || index >= CachedModelCounter) return nullptr;
    int mid = CachedModels[index].ModelId;
    LoadedModel* lm = WDLModels.GetModelByWDLId(mid);
    return lm ? lm->name.c_str() : TextFormat("Model%d", mid);
}

// Selection accessors for Win32 dialogs
int Editor_GetSelectedType() { return (int)g_sel.type; }
int Editor_GetSelectedIndex() { return g_sel.index; }
std::string Editor_GetCurrentWorldName() {
    if (g_documentPath.empty()) return "";
    fs::path parent = g_documentPath.parent_path();
    return parent.filename().string();
}
std::string Editor_GetCurrentWorldDir() {
    if (g_documentPath.empty()) return "";
    return g_documentPath.parent_path().string();
}
int Editor_GetCsgOperation() { return AngelTechEditor.CSGOperation; }
void Editor_SetCsgOperation(int op) { AngelTechEditor.CSGOperation = op; }
int Editor_GetPlaceMode() { return (int)g_placeMode; }
void Editor_SetPlaceMode(int mode) { g_placeMode = (PlaceMode)mode; }

// ---------------------------------------------------------------------------
// Native Win32 Menu Bar â€” window subclass intercepts WM_COMMAND from menus
// ---------------------------------------------------------------------------
static WNDPROC g_originalWndProc = nullptr;

// Keep the Settings-menu checks in step with the shared state. Called after the
// window is created (the menu is built BEFORE the INI options are loaded, so its
// initial checks are just the defaults), and from the toolbar toggle. One
// definition means the button and the menu cannot disagree.
static void Editor_SyncUnrealMenuChecks() {
#ifdef _WIN32
    HWND w = (HWND)GetWindowHandle();
    if (!w) return;
    HMENU menu = GetMenu(w);
    if (!menu) return;
    const bool on = g_editorPanels.unrealBrushMove;
    CheckMenuItem(menu, IDM_UNREAL_MOVE,
                  MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
    const int sp = g_editorPanels.unrealMoveSpeed;
    CheckMenuItem(menu, IDM_UNREAL_SPEED_SLOW,
                  MF_BYCOMMAND | (sp == 0 ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, IDM_UNREAL_SPEED_MED,
                  MF_BYCOMMAND | (sp == 1 ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, IDM_UNREAL_SPEED_FAST,
                  MF_BYCOMMAND | (sp == 2 ? MF_CHECKED : MF_UNCHECKED));
#endif
}

// View > Show Ambient / Show Fog. Same one-definition rule as the Settings
// checks: the menu is built before [Views] is loaded, so Main.cpp calls this
// once after CreateAllEditorWindows and again on every toggle.
static void Editor_SyncViewMenuChecks() {
#ifdef _WIN32
    HWND w = (HWND)GetWindowHandle();
    if (!w) return;
    HMENU menu = GetMenu(w);
    if (!menu) return;
    CheckMenuItem(menu, IDM_VIEW_AMBIENT,
                  MF_BYCOMMAND | (g_editorPanels.viewAmbient ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, IDM_VIEW_FOG,
                  MF_BYCOMMAND | (g_editorPanels.viewFog ? MF_CHECKED : MF_UNCHECKED));
#endif
}

static LRESULT CALLBACK EditorWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Unsaved-document guard for the window close button / Alt+F4. Returning 0
    // without calling the original proc cancels the close, so GLFW never sees it.
    if (msg == WM_CLOSE) {
        if (!ConfirmDiscardDocument("exiting")) return 0;
    }
    if (msg == WM_COMMAND) {
        int id = LOWORD(wParam);
        switch (id) {
            case IDM_NEW:           FileNew(); return 0;
            case IDM_OPEN:          { std::string p; if (ChooseOpenWorldFile(p)) g_pendingOpenPath = fs::path(p); } return 0;
            case IDM_SAVE:          if (!g_documentPath.empty()) SaveWorldDocument(g_documentPath); return 0;
            case IDM_SAVE_AS:       FileSaveAs(); return 0;
            case IDM_PLAY_TEST: {
                // One implementation; see PlayTestCurrentWorld(). It saves first so
                // the client never runs a stale document, and reports a missing
                // executable instead of silently launching nothing.
                PlayTestCurrentWorld();
            } return 0;
            // Route exit through WM_CLOSE so the unsaved-document prompt runs.
            // Calling CloseWindow() here bypassed ConfirmDiscardDocument (which
            // is only reachable from WM_CLOSE) and closed the window from inside
            // a menu callback; Ctrl+Q already used the PostMessage path.
            case IDM_EXIT:          PostMessageA(hWnd, WM_CLOSE, 0, 0); return 0;
            case IDM_MODEL_BRW:     ToggleModelBrowser(); return 0;
            case IDM_SOUND_MGR:     ToggleSoundMgr(); return 0;
            case IDM_TEXTURE_MGR:   ToggleTextureMgr(); return 0;
            case IDM_PAWN_MGR:      TogglePawnMgr(); return 0;
            case IDM_SCRIPT_MGR:    ToggleScriptMgr(); return 0;
            case IDM_NODE_PANEL:    ToggleNodePanel(); return 0;
            case IDM_PICKUP_PANEL:  TogglePickupPanel(); return 0;
            case IDM_HEIGHTMAP:     ToggleHeightmapEditor(); return 0;
            case IDM_FULLSCREEN:    ToggleFullscreen(); return 0;
            case IDM_MAXIMIZE: {
                HWND w = (HWND)GetWindowHandle();
                if (w) ShowWindow(w, IsZoomed(w) ? SW_RESTORE : SW_MAXIMIZE);
            } return 0;
            case IDM_RESET_LAYOUT:  ResetPanelLayouts(); return 0;
            case IDM_SAVE_LAYOUT:
                SavePanelLayouts();
                EditorLog("Pane layout saved to %s", kEditorIniPath);
                return 0;
            case IDM_VIEW_AMBIENT:
                g_editorPanels.viewAmbient = !g_editorPanels.viewAmbient;
                Editor_SyncViewMenuChecks();
                EditorLog("Ambient preview %s", g_editorPanels.viewAmbient ? "on" : "off");
                return 0;
            case IDM_VIEW_FOG:
                g_editorPanels.viewFog = !g_editorPanels.viewFog;
                Editor_SyncViewMenuChecks();
                EditorLog("Fog preview %s", g_editorPanels.viewFog ? "on" : "off");
                return 0;
            case IDM_UNREAL_MOVE:
                g_editorPanels.unrealBrushMove = !g_editorPanels.unrealBrushMove;
                Editor_SyncUnrealMenuChecks();
                EditorLog("Unreal-style brush movement %s",
                          g_editorPanels.unrealBrushMove ? "on" : "off");
                return 0;
            case IDM_UNREAL_SPEED_SLOW:
            case IDM_UNREAL_SPEED_MED:
            case IDM_UNREAL_SPEED_FAST:
                g_editorPanels.unrealMoveSpeed =
                    (id == IDM_UNREAL_SPEED_SLOW) ? 0 :
                    (id == IDM_UNREAL_SPEED_MED)  ? 1 : 2;
                Editor_SyncUnrealMenuChecks();
                EditorLog("Unreal brush move speed: %s",
                          g_editorPanels.unrealMoveSpeed == 0 ? "slow" :
                          g_editorPanels.unrealMoveSpeed == 1 ? "medium" : "fast");
                return 0;
            case IDM_RESET_CAM:     SetViewPerspective(); ResetCamera(); return 0;
            case IDM_VIEW_TOP:      ViewTop(); return 0;
            case IDM_VIEW_BOTTOM:   ViewBottom(); return 0;
            case IDM_VIEW_RIGHT:    ViewRight(); return 0;
            case IDM_VIEW_LEFT:     ViewLeft(); return 0;
            case IDM_VIEW_PERSPECTIVE: SetViewPerspective(); return 0;
            case IDM_WORLD_GRAPH:   ToggleWorldGraph(); return 0;
            case IDM_LEVEL_LIST:    ShowLevelList(!g_editorPanels.showLevelList); RefreshLevelList(); return 0;
            case IDM_MATERIAL_GRAPH: ShowMaterialGraph(!g_editorPanels.showMaterialPanel); return 0;
            case IDM_BAKE_MATERIALS: {
                std::string log;
                const int n = BakeAllMaterials(&log);
                ozmat::MaterialRegistry::Instance().Reload();
                EditorLog("Material bake: %d file(s) from %s\n%s", n, MaterialBakeDir(), log.c_str());
                MessageBoxA(NULL, log.empty() ? "No .ozmat files found." : log.c_str(),
                            "Bake All Materials", MB_OK | MB_ICONINFORMATION);
            } return 0;
            case IDM_RELOAD_MATERIALS:
                ozmat::MaterialRegistry::Instance().Reload();
                EditorLog("Materials reloaded (%d)", (int)ozmat::MaterialRegistry::Instance().All().size());
                return 0;
            case IDM_KEYMAP:        ShowKeymapPanel(!g_editorPanels.showKeymap); return 0;
            case IDM_ABOUT:         MessageBoxA(NULL, "AngelEd v1.0\nOzWorld Editor\nBased on OmegaTech\nTribeWarez 2026", "About AngelEd", MB_OK | MB_ICONINFORMATION); return 0;
            case IDM_UNDO:          HistoryUndo(); return 0;
            case IDM_REDO:          HistoryRedo(); return 0;
            // Context menu actions
            case IDM_PROPERTIES:    OpenPropertiesForSelection(); return 0;
            case IDM_DELETE_ENTITY: DeleteSelectedEntity(); return 0;
            case IDM_DUPLICATE_ENTITY: DuplicateSelectedEntity(); return 0;
            case IDM_APPLY_TEXTURE:
                g_editorPanels.actionApplyTextureToSel = true;
                return 0;
            case IDM_CANCEL:        return 0;
        }
    }
    return CallWindowProc(g_originalWndProc, hWnd, msg, wParam, lParam);
}
