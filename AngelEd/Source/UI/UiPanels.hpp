#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "../../../Source/World/GameType.hpp"
// LevelMetadata, ParticleType and Get/SetLevelMetadata moved to Subsystems in R6.
// UI may call Subsystems, so this is the legal direction of that dependency - see
// Subsystems/LevelState.hpp.
#include "../Subsystems/LevelState.hpp"
#include "../Core/EditorPanelState.hpp"
#include "../../../Source/World/SurfaceFlags.hpp"

// =====================================================================
// Win32Dialogs â€” Real OS-level window panels for AngelEd
// =====================================================================
// On Windows, these are actual native OS windows.
// On other platforms, they are stubs (no-ops).
// =====================================================================

// --- Asset scoping moved to Resources/AssetScope.hpp in R6 -------------------
// AssetScopeItem, AssetScopeNode and BuildAssetScope were declared here and
// implemented in UI/Panels/TexturePanel.cpp. They are now one real translation unit
// with the header to match, and tests/AssetScope.test.cpp covers them headlessly.
#ifdef _WIN32
// Collect the leaf entry indices of the subtree rooted at `node` (walked with
// TVGN_CHILD). Pass TVI_ROOT to collect every root. Drives the Texture Manager's
// thumbnail grid from the scope tree. `node` is an HTREEITEM, typed as void*
// because this header is included before <windows.h>.
void CollectScopeLeavesUnder(HWND tree, void* node, std::vector<int>& out);
#endif


// Open the Entity Properties panel on a DEF by name rather than a world
// instance. Used by the Script Manager's Properties button, which is the only
// route to a def with no instance in the open world (Player.ozls).
void ShowDefPropertiesFor(const std::string& defName);

// --- Level state ---

// --- Portals ---
// Portals are edited in the Entity Properties panel's PORTAL section, which reads
// and writes ZoneManager::GetPortals() directly. RefreshPortalList survives only
// because Main.cpp already calls it as the "portals changed" notification; it is
// intentionally inert (see the .cpp). GetPortalCount/GetPortalTargetWorld had no
// callers at all and were deleted.
void RefreshPortalList();           // intentionally inert now — see the .cpp

// --- Pawn management ---
void PawnManagerAddPawn(const char* name, const char* meshPath);
int GetPawnCount();
const char* GetPawnName(int index);

// Pawn tree node for hierarchical view
struct PawnTreeNode {
    std::string label;
    bool isExpanded = false;
    std::vector<PawnTreeNode> children;
    std::string defName;  // empty for category nodes, valid for leaf nodes
    std::string typeTag;  // "enemy", "weapon", "pickup", "playerstart", "zone", "emitter", ""
};
// Build the full tree from current PawnSystem state
PawnTreeNode BuildPawnTree();

#ifdef _WIN32
// --- Windows-only functions ---
void CreateAllEditorWindows(void* hInst, void* hRaylibWnd);

// Restore every panel to its built-in rect and maximize the main window. Backs
// View -> Reset Layout; panel geometry is persisted in System/AngelEd.ini.
void ResetPanelLayouts();

// Keyboard plumbing for the native panels.
//
// The message pump in Main.cpp calls EditorUi_HandleMessage BEFORE
// TranslateMessage/DispatchMessage. It owns the things a plain modeless window
// does not get for free:
//   - Tab / Shift+Tab focus cycling (WS_TABSTOP controls, including the
//     controls that live on the Surface Properties tab PAGE windows),
//   - Ctrl+A select-all in EDIT controls (single-line EDIT does not support it),
//   - Ctrl+Backspace word delete,
//   - Enter = next field, Escape = leave field / close panel.
// `msg` is an MSG*; void* keeps <windows.h> out of this header.
// Returns true when the message was consumed and must NOT be dispatched.
bool EditorUi_HandleMessage(void* msg);

// True when keyboard focus belongs to the raylib viewport (or nothing is
// focused yet). Used to gate viewport shortcuts so typing in a panel field can
// never switch modes, toggle the heightmap or delete the selection.
bool EditorUi_ViewportHasFocus();

// Rebuild the Pawn Manager "Actor Hierarchy" tree from the current registries.
// Call after PawnDefs/.ozls defs are loaded (windows are created before then).
void RefreshPawnManager();
void DestroyAllEditorWindows();
void ShowSoundManager(bool show);
void ShowTextureManager(bool show);
void ShowPawnManager(bool show);
void ShowScriptManager(bool show);
void ShowModelBrowser(bool show);
void ShowPickupPanel(bool show);
void ShowNodePanel(bool show);
void ShowHeightmapEditor(bool show);
void ShowWorldGraph(bool show);
void ShowKeymapPanel(bool show);

    // Surface Properties (UT99-style, per-face). `renderable` is an
    // OzoneRenderable index and `faceMask` a bitmask of SurfaceFace, so one
    // dialog can edit "(3 Selected)" at once. Rebuilds the controls on open.
    void ShowSurfaceProps(bool show, int renderable, uint32_t faceMask);
// Same, but explicitly choosing the brush-wide scope (write surface.def instead of
// the faces in faceMask). faceMask must still be non-zero - brush-wide is never
// expressed as an empty mask. The two-arg form above always means face scope.
void ShowSurfacePropsScoped(bool show, int renderable, uint32_t faceMask,
                            bool brushWide);
    // Re-read from the renderable and rebuild the controls, so the dialog shows
    // what was actually stored after an Apply.
    void SurfacePropsRefresh(void* hwnd);
void ShowPropertiesPanel(bool show);
void ShowAnimPanel(bool show);
void RefreshAnimPanel();
void ShowLevelList(bool show);
void RefreshLevelList();
void RefreshWorldGraph();
void UpdateModelPreview(void* hBmp, int w, int h);
void ScanModelBrowserFiles();
bool ChooseOpenWorldFile(std::string& outPath);
bool ChooseSaveWorldFile(std::string& outPath);
void UpdateStatsSidebar(float posX, float posY, float posZ,
                        float sizeX, float sizeY, float sizeZ,
                        float rot, float scale,
                        int collisionVols, int chunks,
                        const char* mode,
                        float camX, float camY, float camZ);
void LayoutStatsSidebar(int clientW, int clientH, int topOffset, int width);
int GetStatsSidebarWidth();
#else
// Stub implementations for non-Windows
inline void CreateAllEditorWindows(void*, void*) {}
inline void ResetPanelLayouts() {}
inline bool EditorUi_HandleMessage(void*) { return false; }
inline bool EditorUi_ViewportHasFocus() { return true; }
inline void RefreshPawnManager() {}
inline void DestroyAllEditorWindows() {}
inline void ShowSoundManager(bool) {}
inline void ShowTextureManager(bool) {}
inline void ShowPawnManager(bool) {}
inline void ShowScriptManager(bool) {}
inline void ShowModelBrowser(bool) {}
inline void ShowAnimPanel(bool) {}
inline void RefreshAnimPanel() {}
inline void ShowPickupPanel(bool) {}
inline void ShowNodePanel(bool) {}
inline void ShowHeightmapEditor(bool) {}
inline void ShowWorldGraph(bool) {}
inline void ShowKeymapPanel(bool) {}
inline void ShowPropertiesPanel(bool) {}
inline void ShowLevelList(bool) {}
inline void RefreshLevelList() {}
inline void RefreshWorldGraph() {}
inline void UpdateModelPreview(void*, int, int) {}
inline void ScanModelBrowserFiles() {}
inline bool ChooseOpenWorldFile(std::string&) { return false; }
inline bool ChooseSaveWorldFile(std::string&) { return false; }
inline void UpdateStatsSidebar(float, float, float, float, float, float, float, float, int, int, const char*, float, float, float) {}
inline void LayoutStatsSidebar(int, int, int, int) {}
inline int GetStatsSidebarWidth() { return 200; }
inline void RefreshPortalList() {}
inline void PawnManagerAddPawn(const char*, const char*) {}
inline int GetPawnCount() { return 0; }
inline const char* GetPawnName(int) { return nullptr; }
#endif
