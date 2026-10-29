#ifndef ANGEL_ED_CORE_EDITORSTATE_HPP
#define ANGEL_ED_CORE_EDITORSTATE_HPP
// =============================================================================
// Core/EditorState.hpp
//
// Shared editor state: menu command IDs, the AutoConvex budget, the event-pass
// buffers, and the selection / mouse / gizmo state that Core, Subsystems and UI all
// read.
//
// Split out of Core/EditorShell.hpp in R6. EditorShell.hpp used to be a preamble AND
// a state header AND - the actual reason for the split - the home of thirteen
// RaycastTest* picking functions whose ONLY caller is Subsystems/Selection.cpp.
// Picking logic sitting in a state header is what made that file 420 lines and
// unreadable; those functions moved down to their caller.
//
// Everything here is `static`, i.e. internal to the one translation unit of Main.cpp.
// That is deliberate for now and is what R6 phase F changes: promoting Subsystems/
// to real translation units means the genuinely shared subset moves to `inline
// variables here and the rest stays private to its fragment.
// =============================================================================

#include "../SelType.hpp"
#include "EditorEventBus.hpp"


// Menu command IDs
enum EditorMenuCmd {
    IDM_NEW = 1001,
    IDM_OPEN,
    IDM_SAVE,
    IDM_SAVE_AS,
    IDM_PLAY_TEST,
    IDM_UNDO,
    IDM_REDO,
    IDM_EXIT,
    IDM_MODEL_BRW = 1100,
    IDM_SOUND_MGR,
    IDM_TEXTURE_MGR,
    IDM_PAWN_MGR,
    IDM_SCRIPT_MGR,
    IDM_NODE_PANEL,
    IDM_PICKUP_PANEL,
    IDM_HEIGHTMAP,
    IDM_WORLD_GRAPH,
    IDM_LEVEL_LIST,
    IDM_MATERIAL_GRAPH,
    IDM_BAKE_MATERIALS,
    IDM_RELOAD_MATERIALS,
    IDM_FULLSCREEN,
    IDM_MAXIMIZE,
    IDM_RESET_LAYOUT,
    // Persist the current panel geometry now instead of only on exit. A layout
    // you just arranged is otherwise kept only if you quit cleanly.
    IDM_SAVE_LAYOUT,
    // Editor input options (Settings menu + the toolbar toggle). Same state in
    // both places, so the check mark and the button can never disagree.
    IDM_UNREAL_MOVE,
    IDM_UNREAL_SPEED_SLOW,
    IDM_UNREAL_SPEED_MED,
    IDM_UNREAL_SPEED_FAST,
    IDM_RESET_CAM,
    IDM_VIEW_TOP,
    IDM_VIEW_BOTTOM,
    IDM_VIEW_RIGHT,
    IDM_VIEW_LEFT,
    IDM_VIEW_PERSPECTIVE,
    IDM_ABOUT,
    IDM_KEYMAP,
    // Context menu actions
    IDM_PROPERTIES = 2001,
    IDM_DELETE_ENTITY,
    IDM_DUPLICATE_ENTITY,
    IDM_CANCEL,
    IDM_APPLY_TEXTURE,
    IDM_APPEND_AUTOCONVEX,
    IDM_SURFACE_PROPS,
    // Opens the same dialog in brush-wide scope: Apply writes surface.def
    // (the brush default) rather than the selected faces. Distinct from
    // IDM_SURFACE_PROPS so the two scopes cannot be confused at the call site.
    IDM_SURFACE_PROPS_BRUSH,
    IDM_SURFACE_RESET,
};

// Box budget for one "Append AutoConvex Collision" run. Past this the command
// refuses outright rather than emitting a partial hull - a missing box in a
// collision wall is the exact failure the feature exists to prevent.
inline constexpr int kAutoConvexMaxBoxes = 2048;

// Height of the editor toolbar. It is TWO rows so every control still fits when
// the (now resizable) window is at its minimum width; the viewport, its input
// gates and the docked stats sidebar all start below this.
inline constexpr int kEditorToolbarHeight = 56;

// Forward declarations
void EditorLog(const char* fmt, ...);
void HistoryPush();

// Unsaved-document flag. Lives here (inline, shared by every TU) because
// Subsystems/History.cpp is a REAL translation unit and sets it from
// HistoryPush(), while the title bar and the New/Open/close guards read it from
// the Main TU. A file-static in EditorLog.cpp was invisible to History.o.
inline bool g_documentDirty = false;

void HistoryUndo();
void HistoryRedo();
void HistoryClear();

// Drop everything currently loaded: renderables, pawns, zones, level metadata.
// Defined in Core/EditorShell.cpp and called from WorldIO, History and Main.
void ClearScene();

// WDLModels definition (extern declared in Editor.hpp). inline so that including this
// header from several translation units still yields ONE object - a plain definition
// in a header is a multiple-definition link error the moment Subsystems/ becomes real
// TUs.
inline GameModels WDLModels;

// ---------------------------------------------------------------------------
// Entity selection system (hover + click + right-click context menu)
// ---------------------------------------------------------------------------
// SelType itself lives in SelType.hpp because it crosses the Main.cpp <->
// Win32Dialogs.cpp boundary as a raw int on both sides.
#include "../SelType.hpp"
#include "EditorEventBus.hpp"

// Events posted by the Win32 panels during the frame's message pump, drained at
// the single dispatch point below. A file-static buffer rather than a local so
// the drain cannot allocate during dispatch.
inline std::vector<ed::Event> g_editorFrameEvents;
// Surface edits are drained in their own pass, at the point the old
// actionApplySurface / actionResetSurface handlers lived. Kept separate from
// g_editorFrameEvents so this batch's handlers can be moved into
// Subsystems/SurfaceOps during R4 without reordering the selection dispatch.
inline std::vector<ed::Event> g_editorSurfaceEvents;
// Animation commands, drained and dispatched by ApplyAnimIntents. Separate pass
// for the same reason as the surface one: that function is moving to
// Subsystems/AnimEditing in R4 and should not have to be interleaved with the
// selection dispatch.
inline std::vector<ed::Event> g_editorAnimEvents;
// Bumped by any event kind the dispatcher does not handle yet. Should be 0 once
// R2 is complete; a non-zero value means a batch posted an event nobody consumes,
// which is a silently dead click rather than a compile error.
inline int g_editorUnhandledEvents = 0;
// Index of a mesh awaiting Ev::ConvertToAnimated. -1 = none. The conversion itself
// runs at its own site in the frame (it needs EnsureMeshNodeLoaded and the Anim
// panel handover), so the dispatcher only records the target.
inline int evConvertMeshIndex = -1;

struct EditorSelection {
    SelType type = SelType::NONE;
    int index = -1;
    std::string name;
    Vector3 pos{0,0,0};
    float scale = 1.0f;
    float rotation = 0.0f;
    // True only when `rotation` above was actually populated by the raycast
    // that produced this selection. Most entity types have no yaw concept at
    // all (zones, pickups, lights, portals), so they leave `rotation` at its
    // 0.0f default. The properties panel still seeds a "Rot" row from that
    // default, and the apply handler used to write it straight back - which
    // silently reset the authored `playerstart` yaw to 0 on any Apply. Same
    // trap as scale. Anything that consumes `rotation` on write must gate on
    // this flag.
    bool hasRotation = false;
};
inline EditorSelection g_sel;       // left-click selected (red)
inline EditorSelection g_hoverSel;  // mouse hover (yellow)
// Multi-selection (Phase: multi-select + copy/paste). `g_sel` stays the PRIMARY
// (the one the gizmo and Properties panel act on); this set is the full selection
// used by delete and copy/paste. Kept as a separate vector so the ~40 existing
// readers of g_sel do not have to change.
inline std::vector<EditorSelection> g_selSet;

// Right-click state: drag vs click detection
inline bool g_rbDown = false;
inline Vector2 g_rbDownPos{0,0};
// Left-click state: pick fires on release so a held drag (e.g. moving a
// placement ghost) is not mistaken for a selection click.
inline bool g_lbDown = false;
inline Vector2 g_lbDownPos{0,0};
// True only while an explicit placement ghost is armed by BeginPlacement (the
// Model Browser's "Place in World", the Pickups/Node panels, the CSG toolbox does
// NOT arm it). It is NOT the same thing as AngelTechEditor.DrawModel, which also
// means "the selection gizmo is drawn": gating a click on DrawModel alone would
// turn every plain click with something selected into a placement.
inline bool g_placementArmed = false;
// Set by the viewport's left-click RELEASE when a placement ghost is armed, and
// consumed once by the frame's commit block. A click places; it used to only
// deselect the ghost, because the commit was Enter-only. It is a request rather
// than a direct call because the commit lives further down the frame, inside the
// `if (AngelTechEditor.DrawModel)` block that owns the ghost.
inline bool g_commitPlacement = false;

// Gizmo drag: moving a SELECTED entity requires an intentional gesture (Move
// tool active, or the press landing on the selection) â€” a plain viewport click
// must never translate it. g_suppressViewportDrag swallows stale mouse input
// for the frame(s) around a native context menu.
inline bool g_gizmoDrag = false;
inline bool g_gizmoHistPushed = false;
inline bool g_suppressViewportDrag = false;
inline bool g_terrainHistPushed = false;
// Placement commands and Properties applies get their own drains for the same reason
// the surface and anim ones do. Moved here from Placement.cpp / PropsApply.cpp in
// Phase F: Main.cpp reads them, and a real translation unit cannot expose a `static`.
inline std::vector<ed::Event> g_editorPlacementEvents;
inline std::vector<ed::Event> g_editorPropsEvents;

// World size counts (Phase 5.3) live in Core/EditorPanelState.hpp next to
// g_editorPanels, because UiPanels.hpp (the Stats sidebar's declaration) already
// includes that header and must know the type.

#endif // ANGEL_ED_CORE_EDITORSTATE_HPP
