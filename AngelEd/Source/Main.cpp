// =============================================================================
// Main.cpp - AngelEd entry point and the single TU for the editor core.
//
// WHY A UNITY TU
//
// AngelEd's core was one 5,000-line file holding the frame loop, the event
// dispatcher, selection, entity ops, world I/O, the OZONE exporter, animation
// editing, history, the menu bar and the UI accessors. Splitting it into real
// translation units is not a mechanical move: almost every one of those helpers is
// static and they share file-scope globals (g_sel, g_placeMode, g_histUndo,
// g_editorPanels, ...). Promoting them to real TUs means externalising ~40 symbols
// and hoisting that shared state into a header, which changes linkage and initialise
// order - the one class of change in this refactor that the test suite cannot see,
// because the editor has no headless harness.
//
// So the core layer is split the same way the UI layer is (see UI/UiShell.cpp): the
// subsystems are real, separately-editable files #included here. One object, one
// Makefile rule, one CI line - and Main.o stays Main.o, so this commit cannot break
// the hand-rolled CI build. Order below is the ORIGINAL declaration order of the old
// single file, so the translation unit sees an identical declaration sequence.
//
// Each fragment starts with a banner saying it is not a standalone TU.
//
// THE LAYER RULE, stated exactly (an earlier draft of this comment claimed UI/ never
// includes anything in here; that was false and the build says so):
//   UI/          -> Core/EditorEventBus.hpp only, for the ed:: type vocabulary
//   Subsystems/  -> Core/, UI/, and down into Source/
//   Core/        -> down into Source/
// UI/ must not reach into a Subsystems/ or Core/ IMPLEMENTATION file, and this
// directory has no way to catch that if EditorEventBus.hpp were the only legal seam -
// so EditorEventBus.hpp earns the name: it is the contract all three layers speak
// (Ev, Selection, SurfaceEdit, CsgIntent, AnimIntent, ...), with its .cpp holding the
// queue. Producers publish, Main.cpp drains, UI dispatches. That back-edge is
// deliberate; every other one is not.
// =============================================================================

#include "Core/EditorShell.hpp"
#include "Core/EditorState.hpp"
#include "Core/Viewport.hpp"
#include "Core/ViewportRay.hpp"
// PaneTarget.hpp defines g_paneCams, which the camera-preset helpers in
// Core/EditorShell.cpp and the dispatcher need. It MUST come before the fragment
// includes below (it used to sit after them, so only code that happened to be
// included later could see the pane cameras).
#include "Core/PaneTarget.hpp"
#include "Subsystems/Placement.hpp"
#include "Subsystems/LevelState.hpp"
#include "Subsystems/History.hpp"
#include "Subsystems/OzoneExport.hpp"
#include "Subsystems/Selection.hpp"
#include "Subsystems/PropsApply.hpp"
#include "Subsystems/SurfaceOps.hpp"
// The UI API surface, needed for the panel Show*/Refresh* entry points the frame loop calls.
// Explicit rather than transitive: Core/EditorShell.hpp is a preamble shared with Subsystems/,
// and the Subsystems do not need (or want) any of this.
#include "UI/UiPanels.hpp"
#include "EditorKeys.hpp"
#include "Diagnostics/FrameProfiler.hpp"
#include "Diagnostics/ProfilerOverlay.hpp"
#include "Diagnostics/PerfToggles.hpp"
#include "Diagnostics/BenchMode.hpp"

// DO NOT REORDER THESE. Fragments carry no forward declarations of their own, so
// several statics and functions are declared in the fragment ABOVE their user.
// Examples that have bitten this file at least once each:
//   g_editorLog            declared at the top of EntityOps.cpp, used in EditorLog.cpp
//   g_originalWndProc,
//   EditorWndProc           declared in WorldGraphBridge.cpp, used in EditorMenus.cpp
//   everything the dispatcher calls
// Grouping these by layer ("core before subsystems") reads better and does not
// compile.
//
// Subsystems/Selection.cpp           picking, gizmo snap, adopting a selection
// Subsystems/EntityOps.cpp           delete, duplicate, AutoConvex, CSG commit
// Subsystems/SurfaceOps.hpp          ApplySurfaceEdits (now a REAL translation unit)
// Core/EditorLog.cpp                 AngelEd.log appender
// Core/EditorShell.cpp               view presets, panel toggles, scene reset
// Subsystems/WorldIO.cpp             LoadWorldDocument + name helpers
// Subsystems/OzoneExport.cpp         ExportToOzone, Save, File menu actions
// Subsystems/WorldGraphBridge.cpp    UI accessor surface + Win32 menu dispatch
// Subsystems/HotReload.cpp           watches GameData for .ozls/.cfg edits and
//                                    reloads defs in place (after
//                                    WorldGraphBridge: it reads the current
//                                    world dir; after EditorLog: it logs)
// Core/EditorMenus.cpp               native menu bar construction
// Subsystems/AnimEditing.cpp         vertex-keyframe editing + anim event handlers
// Subsystems/History.cpp             OZONE-snapshot undo/redo
// Subsystems/LevelState.cpp          LevelMetadata owner + the Map-row apply
// Subsystems/PropsApply.hpp          ApplyProperties (was the actionApplyProperties field)
// Subsystems/Placement.hpp           entity placement (was: direct PawnSystem calls
//                                    from UI/Panels/PawnPanel.cpp). After History.cpp
//                                    because it calls HistoryPush.
// Core/EditorDispatcher.cpp           LAST of all. It is the consumer: it calls
//                                    AdoptSelection, DeleteSelectedEntity,
//                                    DuplicateSelectedEntity, CommitBrushRenderable,
//                                    StopSoundPreview and more, so every subsystem
//                                    must be defined above it. The anim / placement /
//                                    surface passes are NOT cases here - each is
//                                    drained and applied by its own subsystem.
#include "Subsystems/Selection.cpp"
#include "Subsystems/Gizmo.cpp"
#include "Subsystems/EntityOps.cpp"
#include "Core/EditorLog.cpp"
#include "Core/EditorShell.cpp"
#include "Subsystems/WorldIO.cpp"
#include "Subsystems/OzoneExport.cpp"
#include "Subsystems/WorldGraphBridge.cpp"
#include "Subsystems/HotReload.cpp"
#include "Core/EditorMenus.cpp"
#include "Subsystems/AnimEditing.cpp"
#include "Core/EditorDispatcher.cpp"
#include "Core/DetachedPane.cpp"

// -----------------------------------------------------------------------------
// Viewport geometry (Phase 0 of multi-viewport). ONE definition of "inside the 3D
// view", replacing six copies of the sidebar/toolbar comparison. The layout is
// still single-pane here; the split render/input lands in the later phases, but
// every input site already asks this helper instead of re-deriving the bounds.
// -----------------------------------------------------------------------------
static ed::viewport::VpRect EditorViewportAreaNow() {
    return ed::viewport::Area((float)GetScreenWidth(), (float)GetScreenHeight(),
                              (float)GetStatsSidebarWidth(), (float)kEditorToolbarHeight);
}

// Split the area for the current layout. 0=1x1, 1=1x2, 2=2x1, 3=2x2.
static void EditorViewportRectsNow(std::vector<ed::viewport::VpRect>& out) {
    if (g_editorPanels.viewMaximized) {
        out.assign(1, EditorViewportAreaNow());
        return;
    }
    int rows = 1, cols = 1;
    switch (g_editorPanels.viewLayout) {
        case 1: rows = 1; cols = 2; break;
        case 2: rows = 2; cols = 1; break;
        case 3: rows = 2; cols = 2; break;
        default: rows = 1; cols = 1; break;
    }
    ed::viewport::Split(EditorViewportAreaNow(), rows, cols, out);
}

// Input is accepted over ANY pane; the pane under the press becomes the active
// input pane (Phase 2). The primary render pass follows the active pane.
static bool EditorViewportContains(Vector2 p) {
    std::vector<ed::viewport::VpRect> rects;
    EditorViewportRectsNow(rects);
    return ed::viewport::IndexAt(rects, p.x, p.y) >= 0;
}

static ed::viewport::VpRect Editor_ActiveRect(const std::vector<ed::viewport::VpRect>& rects) {
    if (rects.empty()) return EditorViewportAreaNow();
    int a = g_editorPanels.viewActive;
    if (a < 0 || a >= (int)rects.size()) a = 0;
    return rects[a];
}

// Secondary-pane cameras are DERIVED from the primary once, at init, so the
// reference views start centred on what is being edited. Each pane then owns its
// camera (and its own preset state), which is what makes "Top here, Persp there"
// possible; the read-only reference panes of a split layout are simply the ones
// the user has not clicked.
//
// The set is the UnrealEd 4-view kit: pane 0 is the editable perspective (the
// startup camera), 1 Top, 2 Front, 3 Side.
static Camera3D MakeSecondaryCam(int pane, const Camera3D& primary) {
    Camera3D c = primary;
    const Vector3 t = primary.target;
    switch (pane) {
        case 1:   // Top: looking down, up = -Z so the X axis stays horizontal
            c.position = {t.x, t.y + 80.0f, t.z + 0.1f};
            c.target = t; c.up = {0, 0, -1}; c.projection = CAMERA_ORTHOGRAPHIC;
            break;
        case 2:   // Front: looking along +Z
            c.position = {t.x, t.y, t.z - 80.0f};
            c.target = t; c.up = {0, 1, 0}; c.projection = CAMERA_ORTHOGRAPHIC;
            break;
        default:  // Side: looking along -X
            c.position = {t.x + 80.0f, t.y, t.z};
            c.target = t; c.up = {0, 1, 0}; c.projection = CAMERA_ORTHOGRAPHIC;
            break;
    }
    return c;
}

// Per-pane cameras. The active pane's camera mirrors OTEditor.MainCamera (which
// every input/preset path already edits); the others are edited only by becoming
// active. Initialised once: pane 0 = the startup camera, 1 = top ortho, 2 = side
// ortho, 3 = a perspective clone.
static bool g_paneCamsInit = false;
static void Editor_EnsurePaneCams() {
    if (g_paneCamsInit) return;
    g_paneCams[0] = OTEditor.MainCamera;
    g_paneCams[1] = MakeSecondaryCam(1, OTEditor.MainCamera);
    g_paneCams[2] = MakeSecondaryCam(2, OTEditor.MainCamera);
    g_paneCams[3] = MakeSecondaryCam(3, OTEditor.MainCamera);
    g_paneCamsInit = true;
}

// Override the projection BeginMode3D just pushed with one whose aspect matches
// the PANE, not the window. Without this a 1x2 split stretches both views.
static void SetPaneProjection(const Camera3D& cam, const ed::viewport::VpRect& r) {
    const double aspect = (r.h > 0.0f) ? (double)r.w / (double)r.h : 1.0;
    rlMatrixMode(RL_PROJECTION);
    rlLoadIdentity();
    if (cam.projection == CAMERA_PERSPECTIVE) {
        const double top = (double)RL_CULL_DISTANCE_NEAR *
                           tan((double)cam.fovy * 0.5 * DEG2RAD);
        const double right = top * aspect;
        rlFrustum(-right, right, -top, top, RL_CULL_DISTANCE_NEAR, RL_CULL_DISTANCE_FAR);
    } else {
        const double top = (double)cam.fovy * 0.5;
        const double right = top * aspect;
        rlOrtho(-right, right, -top, top, RL_CULL_DISTANCE_NEAR, RL_CULL_DISTANCE_FAR);
    }
    rlMatrixMode(RL_MODELVIEW);
}

// GL viewport uses a bottom-left origin; VpRect is top-left screen space.
static void SetPaneViewport(const ed::viewport::VpRect& r) {
    rlViewport((int)r.x, GetScreenHeight() - (int)(r.y + r.h), (int)r.w, (int)r.h);
}

// -----------------------------------------------------------------------------
// main()
// -----------------------------------------------------------------------------
int main(int argc, char **argv){
    // FIRST, before anything reads a relative path. Almost every asset path in the
    // editor is relative and unanchored, so launching from the wrong directory produces a
    // wave of silent failures instead of one clear error. See EnsureProjectRoot() in
    // Core/EditorShell.cpp.
    EnsureProjectRoot();
    oz::diag::RegisterBuiltinCounters();

    // Shortcut overrides must be loaded before the menu is built, because the menu
    // labels are generated from this table.
    EdKeys::Load();

    // Pre-scan for the benchmark BEFORE InitWindow: the window flags (vsync) and
    // the frame cap are creation-time decisions, and the detailed parse below
    // happens after the window exists. Without this a benchmark ran pinned to
    // 16.67 ms by vsync and reported exactly 60 FPS on every world.
    bool benchRequested = false;
    for (int i = 1; i < argc; ++i)
        if (strncmp(argv[i], "--bench", 7) == 0) { benchRequested = true; break; }

    EditorLog("=== AngelEd starting ===");
    // RESIZABLE: the viewport, toolbar and docked stats sidebar all lay out from
    // GetScreenWidth()/GetScreenHeight() each frame, so the only thing missing was
    // the flag. The size/position is restored and saved in System/AngelEd.ini
    // [Layout] (main_x/y/w/h) by Load/SavePanelLayouts.
    SetConfigFlags(benchRequested ? FLAG_WINDOW_RESIZABLE
                                  : (FLAG_VSYNC_HINT | FLAG_WINDOW_RESIZABLE));
    InitWindow(1280, 720, benchRequested ? "AngelEd [bench]" : "AngelEd");
    // Below this the toolbar and the fixed-width stats sidebar start overlapping
    // the viewport, so refuse to shrink further rather than render a broken layout.
    SetWindowMinSize(1024, 640);
    CreateEditorMenuBar();
    SetTraceLogLevel(LOG_WARNING);
    if (!benchRequested) {
        InitAudioDevice();
        SetTargetFPS(60);   // a benchmark measures the uncapped frame
    }
    GuiLoadStyleDark();

    // Load editor toolbar icons
    EditorIcons::Instance().Load();

    // No argument: open a world, but only if one is actually there. This used to
    // default to "../GameData/World.ozone", which does not exist in ANY layout -
    // worlds live at GameData/Worlds/<Name>/World.ozone, and the path also assumed
    // cwd == System/, contradicting the "System/AngelEd.ini" load eight lines below.
    // Arguments: an optional world path plus the FrameProfiler benchmark flags.
    // Parsed in ONE pass so `--bench ... World.ozone` and `World.ozone --bench`
    // both work, and so a flag can never be mistaken for the document path.
    g_documentPath.clear();
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        auto& bench = oz::diag::Bench();
        if (strcmp(arg, "--bench") == 0 || strcmp(arg, "--bench-frames") == 0) {
            bench.active = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                bench.frames = std::max(1, atoi(argv[++i]));
        } else if (strcmp(arg, "--bench-warmup") == 0 && i + 1 < argc) {
            bench.active = true;
            bench.warmup = std::max(0, atoi(argv[++i]));
        } else if (strcmp(arg, "--bench-csv") == 0 && i + 1 < argc) {
            bench.active = true;
            bench.csvPath = argv[++i];
        } else if (strcmp(arg, "--bench-views") == 0 && i + 1 < argc) {
            bench.active = true;
            bench.views = std::clamp(atoi(argv[++i]), 0, 3);
        } else if (strcmp(arg, "--bench-no-overlays") == 0) {
            bench.active = true;
            g_editorPanels.perfOverlay = false;
        } else if (arg[0] != '-') {
            g_documentPath = fs::path(arg);
        }
    }
    if (g_documentPath.empty()) {
        fs::path first = FirstAvailableWorld();
        if (!first.empty()) g_documentPath = first;
        else EditorLog("No world argument and no world found under GameData/Worlds/ - "
                       "starting with an empty scene.");
    }

    if (oz::diag::Bench().active) {
        EditorLog("Bench: document='%s' frames=%d warmup=%d views=%d csv='%s'",
                  g_documentPath.string().c_str(), oz::diag::Bench().frames,
                  oz::diag::Bench().warmup, oz::diag::Bench().views,
                  oz::diag::Bench().csvPath.c_str());
    }
    SetWorldDirectory(g_documentPath.empty() ? fs::path()
                                             : g_documentPath.parent_path());

    // Load INI config
    g_config.Load("System/AngelEd.ini");

    // Initialize package-based asset loading
    PackageAssetLoader::Instance().Init();
    ozmat::MaterialRegistry::Instance().Init();

    // Initialize LightningScript entity registry (loads .ozls pickup defs)
    LightningEntityRegistry::Instance().Init();

    // Initialize engine/item texture mapper (must be before EngineBillboard::Init)
    AssetMapper::Instance().Init();

    // The editor viewport must always show authoring gizmos (player starts,
    // lights, zones, sound/music emitters). These were gated behind the client's
    // Debug flag, which AngelEd never sets, so those entities had no visual
    // representation at all and could not be picked in the viewport.
    PawnSystem::Instance().SetShowAuthoringGizmos(true);

#ifdef _WIN32
    CreateAllEditorWindows(GetModuleHandle(NULL), GetWindowHandle());
    // The menu is built before the INI loads (InitWindow happens first), so the
    // Settings-menu check marks are only correct after LoadPanelLayouts has read
    // [Options] inside CreateAllEditorWindows.
    Editor_SyncUnrealMenuChecks();
    Editor_SyncViewMenuChecks();
#endif

    if (oz::diag::Bench().active) {
        // AFTER CreateAllEditorWindows, because LoadPanelLayouts reads [Views]
        // from System/AngelEd.ini and would otherwise overwrite --bench-views.
        // Detached panes are disabled so the benchmark never opens a second
        // window or pays the Tier-A readback.
        auto& b = oz::diag::Bench();
        if (b.views >= 0) g_editorPanels.viewLayout = b.views;
        g_editorPanels.viewMaximized = false;
        g_editorPanels.detachedMask = 0;
        g_editorPanels.perfOverlay = false;
        EditorLog("Bench layout: views=%d", g_editorPanels.viewLayout);
    }

    // Load editor world
    Init();

    // Load pawn definitions from config files (data-driven).
    //
    // The loader lives in Subsystems/HotReload.cpp so the file watcher can run
    // the SAME parser when a .cfg is saved. A second copy here is how the
    // editor's loader and the client's (Core.hpp) drifted before: the editor
    // silently ignored model_path/mesh_type and drew every enemy as its
    // sprite_path placeholder.
    LoadAllPawnDefs();

    // The editor windows (and their Actor Hierarchy tree) were created before
    // the PawnDefs were registered, so rebuild the Pawn Manager tree now that
    // every registry is populated â€” otherwise EnemyPawn is empty and nothing
    // can be spawned from it.
    RefreshPawnManager();

    // Load the document through the ONE open path, whichever way the world was
    // chosen (argv[1] or FirstAvailableWorld()).
    //
    // This used to branch: with argv[1] it called LoadWorldDocument, and without
    // one it loaded the geometry directly - `OzoneLoader::LoadFile` +
    // InjectOzoneEntities - which is a subset that skips the `levelinfo` parse,
    // the .oztex package autoload and MarkWorldGraphDirty. The run scripts pass no
    // world, so a normal launch took the shortcut, left g_levelMeta at its
    // defaults, and the next save wrote a level with NO levelinfo: skybox and game
    // rules silently deleted. Rendering looked right, because the geometry did load.
    if (!g_documentPath.empty()) {
        LoadWorldDocument(g_documentPath);
    } else {
        EditorLog("No world to load - starting with an empty scene.");
    }

    // Watch GameData for .ozls / PawnDefs/*.cfg saves and reload definitions in
    // place. Started AFTER the world is open: the watcher's overflow recovery
    // re-applies the current world's defs, and there is nothing to recover
    // before the first load.
    HotReload_Start();

    // Restore popped-out panes from [Views] (Tier A persistence), then the
    // OZ_ED_DETACH=1 check hook for exercising the FBO + GDI path.
    for (int i = 0; i < 4; i++)
        if (g_editorPanels.detachedMask & (1 << i))
            DetachedPane_Detach(i, g_editorPanels.detachW, g_editorPanels.detachH);
    if (getenv("OZ_ED_DETACH")) DetachedPane_Detach(0, 960, 540);

    // Suppress raylib's texture-not-found warnings from .obj material refs
    SetTraceLogLevel(LOG_ERROR);

    // Create model preview render texture
    g_previewRT = LoadRenderTexture(256, 256);

    EnableCursor();

    auto& benchSession = oz::diag::Bench();
    int benchFrame = 0;

    while (!WindowShouldClose())
    {
        // Profiler frame boundary. Header-only and always-on; the overlay is
        // what the toolbar toggles. RAII so a future early `continue` cannot
        // leave the frame open.
        oz::diag::FrameGuard oz_frame_guard;
        {
            std::string stutter;
            if (oz::diag::Profiler().TakeStutterReport(stutter))
                EditorLog("Perf %s", stutter.c_str());
        }

        // Process Win32 messages for child panels
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            // Panel keyboard plumbing (Tab ring, Ctrl+A, Enter/Escape) runs before
            // dispatch; when it consumes a message, do not also Translate/ Dispatch
            // it into the EDIT control.
            if (EditorUi_HandleMessage(&msg)) continue;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        // --- Multi-viewport input ownership (Phase 2) -------------------------
        // Mirror the active pane's camera into OTEditor.MainCamera (the variable
        // every input/preset/ray path already edits), publish its rect for the
        // pane-aware ray/gizmo helpers, and switch panes on a press.
        Editor_EnsurePaneCams();
        {
            std::vector<ed::viewport::VpRect> prects;
            EditorViewportRectsNow(prects);
            int a = g_editorPanels.viewActive;
            if (a < 0 || a >= (int)prects.size()) { a = 0; g_editorPanels.viewActive = 0; }
            OTEditor.MainCamera = g_paneCams[a];
            g_activeViewportRect = prects.empty() ? EditorViewportAreaNow() : prects[a];

            if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) ||
                IsMouseButtonPressed(MOUSE_RIGHT_BUTTON)) {
                const Vector2 mp = GetMousePosition();
                const int hit = ed::viewport::IndexAt(prects, mp.x, mp.y);
                if (hit >= 0 && hit != a) {
                    g_paneCams[a] = OTEditor.MainCamera;   // keep uncommitted edits
                    g_editorPanels.viewActive = hit;
                    OTEditor.MainCamera = g_paneCams[hit];
                    g_activeViewportRect = prects[hit];
                }
            }
        }
        // Keep the preset bookkeeping in step with whichever pane is active, so a
        // preset chip/menu action applies to THIS pane's projection.
        Editor_SyncViewPresetState();
        // A placement click is a ONE-FRAME request: the release below sets it and
        // the commit block later in THIS frame consumes it. Clearing it here means
        // a click that could not commit (ghost disarmed by the same frame) can
        // never leak into the next armed ghost and place it without a click.
        g_commitPlacement = false;

        if (g_pendingNew) {
            ClearScene();
            HistoryClear();
            g_documentPath.clear();
            g_documentDirty = false;
            g_pendingNew = false;
        }
        if (!g_pendingOpenPath.empty()) {
            LoadWorldDocument(g_pendingOpenPath);
            g_pendingOpenPath.clear();
        }

        // Hot reload: drain def-file changes (debounced in the subsystem) and
        // refresh only the panels that showed the old values. Deliberately
        // BEFORE the event drains and the WorldGraph consume below, so a panel
        // refresh cannot race a pending placement/undo in the same frame. It
        // never marks the world dirty: defs are not world entities.
        {
            OZ_PROFILE_SCOPE("ed.hotreload");
            HotReload_Poll();
        }

        // Window title = document + unsaved marker. Updated only on change, so it
        // does not spam SetWindowText once per frame.
        {
            static std::string lastTitle;
            std::string title = "AngelEd";
            if (!g_documentPath.empty()) {
                title += " - " + g_documentPath.filename().string();
                if (g_documentDirty) title += " *";
            } else if (g_documentDirty) {
                title += " - untitled *";
            }
            if (title != lastTitle) {
                lastTitle = title;
                SetWindowTextA((HWND)GetWindowHandle(), title.c_str());
            }
        }
        // Live volume + optional loop for the sound preview (Sound has no loop
        // flag, so looping is a replay poll). This is LIVE STATE, not a message:
        // the PlaySound/StopSoundPreview EVENTS carry the path, but volume and
        // loop are re-applied every frame so dragging the slider is immediate.
        if (g_previewSoundLoaded) {
            SetSoundVolume(g_previewSound, g_editorPanels.previewSoundVolume / 100.0f);
            if (g_editorPanels.previewSoundLoop && !IsSoundPlaying(g_previewSound))
                PlaySound(g_previewSound);
        }

        // Viewport bounds check â€” all raycasts only fire when mouse is inside 3D viewport
        Vector2 _mp = GetMousePosition();
        bool _inViewport = EditorViewportContains(_mp);

        // Hover raycast (throttled every 4 frames for performance)
        {
            static int g_hoverFrameCounter = 0;
            g_hoverFrameCounter++;
            if (g_hoverFrameCounter >= 4) {
                g_hoverFrameCounter = 0;
                // Hover is a read-only probe: it must not depend on DrawModel
                // (the gizmo/placement-ghost flag).
                if (_inViewport) {
                    g_hoverSel = { SelType::NONE, -1, "", {0,0,0} };
                    OZ_PROFILE_SCOPE("ed.hover");
                    EditorHoverEntity();
                } else {
                    g_hoverSel = { SelType::NONE, -1, "", {0,0,0} };
                }
            }
        }

        // Gizmo hover: recomputed every frame (cheap) so the handle under the
        // cursor highlights before the click. Outside the viewport nothing is hot.
        Gizmo_UpdateHover(_inViewport ? _mp : Vector2{-10000.0f, -10000.0f},
                          OTEditor.MainCamera, g_editorPanels.currentToolMode);

        // --- UnrealEd-style mouse editing (opt-in) ---------------------------
        //
        // Authentic UnrealEd transforms are mouse + modifier, not handle drags:
        //   Ctrl + LMB            translate world X  (horizontal drag)
        //   Ctrl + RMB            translate world Z  (horizontal drag)
        //   Ctrl + LMB + RMB      translate world Y  (vertical drag)
        //   Ctrl + RMB in Rotate  yaw
        //   Shift + LMB           translate the selection AND the camera
        //
        // Translated to this engine's axes (Y is up; Unreal's Z-up becomes Y).
        // Pitch/roll are NOT mapped: a renderable/mesh stores one rotation (yaw),
        // so a control that cannot be written back would lie about what it did.
        //
        // When the option is OFF none of this touches the mouse, so the gizmo,
        // selection and camera gestures behave exactly as before.
        const bool uCtrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
        const bool uShift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        const bool unrealSel = g_editorPanels.unrealBrushMove && _inViewport &&
                               g_sel.type != SelType::NONE && g_sel.type != SelType::MAP &&
                               !g_editorPanels.animEditVerts;
        // SHIFT IS NOT AN UNREAL MODIFIER HERE - it belongs to the viewport PICK.
        // It used to set `unrealMouseEdit`, which (a) skipped the left-click pick
        // block entirely and (b) started a move-the-camera drag, so Shift+click -
        // the multi-select gesture - did nothing at all while Unreal-style editing
        // was on. Ctrl owns the mouse; Ctrl+Shift is the "drag the view with the
        // selection" variant of it (UnrealEd's `Buttons & MOUSE_Shift`).
        // Shift+RMB also still has to reach the per-face surface pick, which is a
        // second reason Shift cannot arm a gesture by itself.
        const bool unrealCtrl = unrealSel && uCtrl;
        const bool unrealMouseEdit = unrealCtrl;

        if (unrealMouseEdit && !Gizmo_IsUnrealDragging() &&
            (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) ||
             IsMouseButtonPressed(MOUSE_RIGHT_BUTTON))) {
            const bool l = IsMouseButtonDown(MOUSE_LEFT_BUTTON);
            const bool r = IsMouseButtonDown(MOUSE_RIGHT_BUTTON);
            UnrealDragMode mode = UnrealDragMode::None;
            bool follow = false;
            if (l || r) {
                // The exact axis is refined by the first Update (adding the second
                // button switches X/Z -> Y without moving the object). Shift also
                // drags the view along, exactly as UnrealEd's
                // `MoveActors(..., (Buttons & MOUSE_Shift) ? Viewport->Actor : NULL)`
                // does - it is NOT a different gesture.
                mode = (l && r) ? UnrealDragMode::TranslateY
                                : (l ? UnrealDragMode::TranslateX
                                     : UnrealDragMode::TranslateZ);
                follow = uShift;
            }
            if (mode != UnrealDragMode::None) {
                // History is pushed on the first actual movement, not on the
                // press: a Ctrl+click that never moves must not leave an undo
                // step behind (the press may be the start of a plain pick).
                Gizmo_BeginUnrealDrag(mode, follow);
                g_gizmoDrag = true;   // lets the "mesh has no bounds yet" branch write
            }
        }
        // Update/end runs even when the modifier is released BEFORE the button:
        // otherwise the drag would stay latched until the next Ctrl press.
        if (Gizmo_IsUnrealDragging()) {
            const bool l = IsMouseButtonDown(MOUSE_LEFT_BUTTON);
            const bool r = IsMouseButtonDown(MOUSE_RIGHT_BUTTON);
            if (g_sel.type == SelType::NONE || g_sel.type == SelType::MAP) {
                Gizmo_CancelUnrealDrag();
            } else {
                const Vector2 md = GetMouseDelta();
                if (!g_gizmoHistPushed && (md.x != 0.0f || md.y != 0.0f)) {
                    HistoryPush();
                    g_gizmoHistPushed = true;
                }
                Gizmo_UpdateUnrealDrag(md, OTEditor.MainCamera,
                                       g_editorPanels.currentToolMode,
                                       uCtrl, uShift, l, r);
            }
            if (!l && !r) {
                Gizmo_EndUnrealDrag();
                g_gizmoDrag = false;
                if (g_gizmoHistPushed) {
                    MarkWorldGraphDirty();
                    g_gizmoHistPushed = false;
                }
                // No "swallow the release" call is needed or possible: the pick
                // handlers above are gated on `!unrealMouseEdit` and on
                // g_lbDown/g_rbDown, neither of which was armed during the drag,
                // so a transform release cannot also pick or open a menu.
            }
        }

        // Unreal-style keyboard nudge: arrows step the selection on the ground
        // plane (X/Z); Ctrl+Up/Down steps vertically (Y). The step is the grid
        // snap when set, else one unit. One History entry per keypress, and it is
        // applied BEFORE the gizmo->entity sync below so it lands this frame.
        if (g_editorPanels.unrealBrushMove && EditorUi_ViewportHasFocus() &&
            g_sel.type != SelType::NONE && g_sel.type != SelType::MAP &&
            !g_editorPanels.animEditVerts &&
            !Gizmo_IsDragging() && !Gizmo_IsUnrealDragging()) {
            const float step = g_editorPanels.gizmoSnap > 0.0f
                                   ? g_editorPanels.gizmoSnap : 1.0f;
            Vector3 nd = {0.0f, 0.0f, 0.0f};
            if (uCtrl) {
                if (IsKeyPressed(KEY_UP))   nd.y += step;
                if (IsKeyPressed(KEY_DOWN)) nd.y -= step;
            } else {
                if (IsKeyPressed(KEY_LEFT))  nd.x -= step;
                if (IsKeyPressed(KEY_RIGHT)) nd.x += step;
                if (IsKeyPressed(KEY_UP))    nd.z -= step;
                if (IsKeyPressed(KEY_DOWN))  nd.z += step;
            }
            if (nd.x != 0.0f || nd.y != 0.0f || nd.z != 0.0f) {
                HistoryPush();
                AngelTechEditor.X += nd.x;
                AngelTechEditor.Y += nd.y;
                AngelTechEditor.Z += nd.z;
                MarkWorldGraphDirty();
            }
        }

        // Vertex-edit picking (Phase C): click nearest vertex, Shift adds.

        if (g_editorPanels.animEditVerts && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            Vector2 mp = GetMousePosition();
            if (EditorViewportContains(mp)) {
                MeshObjectNode* vn = PawnSystem::Instance().GetMeshObject(g_editorPanels.animTargetMesh);
                oz::AnimatedMesh* vam = AnimTarget();
                if (vn && vn->editPose && vam) {
                    int vi = PickVertex(*vn, *vam, *vn->editPose, OTEditor.MainCamera, mp, 12.0f);
                    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
                    if (vi >= 0) {
                        if (shift) {
                            if (std::find(g_editorPanels.animSelVerts.begin(), g_editorPanels.animSelVerts.end(), vi)
                                == g_editorPanels.animSelVerts.end())
                                g_editorPanels.animSelVerts.push_back(vi);
                        } else {
                            g_editorPanels.animSelVerts.clear();
                            g_editorPanels.animSelVerts.push_back(vi);
                        }
                    } else if (!shift) {
                        g_editorPanels.animSelVerts.clear();
                    }
                }
            }
        }
        // Left-click: SELECT ON PRESS, like UnEdClick.cpp. UE1 hit-tests the moment
        // the button goes down (`UEditorEngine::Click` -> FEditorHitObserver) and
        // then lets the drag move whatever ended up selected; picking on RELEASE
        // meant a press could only ever select, never select-and-drag, and the 3px
        // movement threshold existed solely to tell the two apart.
        //
        // The ONE exception is a placement ghost: it is dragged while the button is
        // held and committed only on a release that did not move, so that path keeps
        // the click/drag distinction.
        //
        // Picking is deliberately independent of DrawModel - DrawModel only means
        // "a gizmo/placement ghost is drawn". Gating picks on it made the viewport
        // unselectable whenever a placement ghost was active with nothing selected.
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && !unrealMouseEdit) {
            const Vector2 mp = GetMousePosition();
            g_gizmoHistPushed = false;
            g_gizmoDrag = false;

            if (!g_suppressViewportDrag && !g_editorPanels.animEditVerts &&
                g_placeMode != PlaceMode::TERRAIN && EditorViewportContains(mp)) {
                if (g_placementArmed && g_sel.type == SelType::NONE) {
                    // Ghost: arm the drag; the release below commits if it never moved.
                    g_lbDown = true;
                } else {
                    // ONE raycast resolves all three questions: what is under the
                    // cursor, is this the second click of a double, and was the
                    // entity already selected (Cam mode only drags what you had).
                    EditorSelection probe;
                    const bool hit = EditorRaycastAt(mp, probe);
                    const bool wasSelected = hit && probe.type == g_sel.type &&
                                             probe.index == g_sel.index;

                    static double  s_lastClickTime = -1.0;
                    static Vector2 s_lastClickPos = {-1.0f, -1.0f};
                    static SelType s_lastClickType = SelType::NONE;
                    static int     s_lastClickIndex = -1;
                    const double now = GetTime();
                    const bool isDouble =
                        s_lastClickTime > 0.0 && (now - s_lastClickTime) < 0.35 &&
                        fabsf(mp.x - s_lastClickPos.x) <= 4.0f &&
                        fabsf(mp.y - s_lastClickPos.y) <= 4.0f &&
                        hit && probe.type == s_lastClickType &&
                        probe.index == s_lastClickIndex;

                    // A double-click must not start a transform: opening Properties
                    // and then nudging the entity by a pixel of mouse jitter is the
                    // worst of both.
                    bool openedProperties = false;

                    // A handle under the cursor wins over the object behind it: that
                    // press is a constrained transform, not a selection.
                    if (Gizmo_BeginDrag(mp, OTEditor.MainCamera,
                                        g_editorPanels.currentToolMode)) {
                        g_gizmoDrag = true;
                        HistoryPush();
                        g_gizmoHistPushed = true;
                    } else if (isDouble) {
                        // Click(HActor): MOUSE_LeftDouble -> select + properties.
                        // `false` = no modifier toggle: a double-click REPLACES the
                        // set instead of toggling the actor it just opened.
                        EditorPickEntityAt(probe, hit, false);
                        OpenPropertiesForSelection();
                        openedProperties = true;
                        s_lastClickTime = -1.0;   // a triple click is not two doubles
                    } else {
                        const bool picked = EditorPickEntityAt(probe, hit, true);
                        // HBackdrop::Click: a plain LMB on empty space clears the
                        // selection. A modifier keeps it, so a missed extend-click
                        // cannot wipe the set.
                        if (!picked && !SelectionModifierHeld() &&
                            g_sel.type != SelType::NONE)
                            ClearSelection();
                        s_lastClickTime = now;
                        s_lastClickPos = mp;
                        s_lastClickType = g_sel.type;
                        s_lastClickIndex = g_sel.index;
                    }

                    // Begin an intentional move: Move mode always; Cam mode only
                    // when the press landed on what was ALREADY selected, so a plain
                    // Cam-mode click never drags the scene around. Scale/Rotate must
                    // not fall back to an unconstrained move.
                    if (!g_gizmoDrag && !openedProperties &&
                        (g_editorPanels.currentToolMode == 1 ||
                         (g_editorPanels.currentToolMode == 0 && wasSelected)))
                        g_gizmoDrag = true;
                }
            }
        }
        if (IsMouseButtonDown(MOUSE_LEFT_BUTTON) && g_lbDown) {
            Vector2 delta = GetMouseDelta();
            if (fabsf(delta.x) > 3.0f || fabsf(delta.y) > 3.0f) g_lbDown = false;
        }
        if (IsMouseButtonReleased(MOUSE_LEFT_BUTTON)) {
            if (g_lbDown) {
                g_lbDown = false;
                Vector2 mp = GetMousePosition();
                // Only the placement ghost still resolves on release, so a press-drag
                // can position it and a press-release places it.
                if (!g_editorPanels.animEditVerts && EditorViewportContains(mp) &&
                    g_placementArmed && g_sel.type == SelType::NONE)
                    g_commitPlacement = true;
            }
            // A gizmo drag wrote new positions every frame it was held, so the WorldGraph's
            // position columns are stale. Flagging on release keeps the 60Hz rebuild off
            // the drag itself. Gated on g_gizmoHistPushed because that is set only when
            // a transform was actually pushed to history - without it every plain CLICK
            // refreshed the list, since a pick is also a release.
            const bool didTransform = g_gizmoHistPushed;
            Gizmo_EndDrag();
            g_gizmoDrag = false;
            g_gizmoHistPushed = false;
            g_suppressViewportDrag = false;
            g_terrainHistPushed = false;
            if (didTransform) MarkWorldGraphDirty();
        }
        // Release the post-context-menu input guard once the button is no longer held.
        if (g_suppressViewportDrag && !IsMouseButtonDown(MOUSE_LEFT_BUTTON))
            g_suppressViewportDrag = false;

        // TERRAIN brush: raise/lower heightmap cells (TERRAIN mode, left mouse)
        if (g_placeMode == PlaceMode::TERRAIN && IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
            auto& oz = OzoneLoader::Instance();
            if (oz.HasHeightmap()) {
                Vector2 mp = GetMousePosition();
                if (EditorViewportContains(mp)) {
                    if (!g_terrainHistPushed) { HistoryPush(); g_terrainHistPushed = true; }
                    Ray ray = RayFromPane(mp, OTEditor.MainCamera, g_activeViewportRect);
                    Vector3 hmPos = oz.GetHeightmapPosition();
                    float scale = oz.GetHeightmapScale();
                    float sx = oz.GetHeightmapCellSize() * (oz.GetHeightmapGridW() - 1) * scale;
                    float sz = oz.GetHeightmapCellSize() * (oz.GetHeightmapGridH() - 1) * scale;
                    float halfW = sx * 0.5f;
                    float halfD = sz * 0.5f;

                    // Intersect ray with the heightmap's base plane (Y = hmPos.y)
                    float t = (hmPos.y - ray.position.y) / ray.direction.y;
                    if (t > 0.0f) {
                        float wx = ray.position.x + ray.direction.x * t;
                        float wz = ray.position.z + ray.direction.z * t;
                        float mx = (wx - hmPos.x) / scale;
                        float mz = (wz - hmPos.z) / scale;
                        int col = (int)((mx + halfW) / (sx / (float)(oz.GetHeightmapGridW() - 1)));
                        int row = (int)((mz + halfD) / (sz / (float)(oz.GetHeightmapGridH() - 1)));

                        // Throttle: skip if same cell as last frame
                        static int lastCol = -1, lastRow = -1;
                        if (col == lastCol && row == lastRow) {
                            // Still allow painting if mouse button held (applied once on press)
                        } else {
                            lastCol = col; lastRow = row;
                        }

                        int gw = oz.GetHeightmapGridW();
                        int gh = oz.GetHeightmapGridH();
                        int radius = g_editorPanels.terrainBrushSize;
                        float strength = g_editorPanels.terrainBrushStrength;
                        bool raising = (g_editorPanels.terrainBrushMode == 0);

                        // Batch all cell edits then rebuild once
                        for (int rz = -radius; rz <= radius; rz++) {
                            for (int rx = -radius; rx <= radius; rx++) {
                                int c = col + rx;
                                int r = row + rz;
                                if (c < 0 || c >= gw || r < 0 || r >= gh)
                                    continue;
                                float dist = sqrtf((float)(rx*rx + rz*rz));
                                if (dist > (float)radius) continue;
                                float falloff = 1.0f - dist / (float)(radius + 1);
                                float curH = oz.GetHeightAtGrid(c, r);
                                float delta = strength * falloff;
                                oz.SetHeightAtGrid(c, r,
                                    raising ? curH + delta : curH - delta, false);
                            }
                        }
                        oz.RebuildHeightmapMesh();
                    }
                }
            }
        }

        // Right-click: drag resizes placement ghost; click picks entity + native
        // context menu. It must NEVER commit a placement (that added duplicate
        // entities when right-clicking to delete/duplicate) â€” commit is Enter only.
        if (IsMouseButtonPressed(MOUSE_RIGHT_BUTTON) && !unrealCtrl) {
            g_rbDown = true;
            g_rbDownPos = GetMousePosition();
        }
        // A right-drag cancels the context menu. This must not depend on
        // DrawModel: the menu has to open whenever the user right-clicks an
        // entity in the viewport.
        if (IsMouseButtonDown(MOUSE_RIGHT_BUTTON) && g_rbDown) {
            Vector2 delta = GetMouseDelta();
            if (fabsf(delta.x) > 3.0f || fabsf(delta.y) > 3.0f) g_rbDown = false;
        }
        if (IsMouseButtonReleased(MOUSE_RIGHT_BUTTON) && g_rbDown) {
            g_rbDown = false;
            // Re-check viewport bounds with fresh cursor position
            Vector2 _mp_rel = GetMousePosition();
            bool _inVpRel = EditorViewportContains(_mp_rel);
            // Per-FACE surface pick, done BEFORE the entity pick so the menu can
            // offer "Surface Properties (N Selected)". Triangle-level rather than
            // AABB-level, so clicking the top of a wall selects the +Y face and
            // not the brush as a whole.
            const SurfaceFacePick spfPick = _inVpRel ? PickSurfaceFace(_mp_rel) : SurfaceFacePick{};
            // Right-click never toggles set membership: it opens the menu for
            // whatever was picked, and a toggle here would drop the entity the
            // menu is about to act on. Gate on the hit result - on a miss g_sel
            // still holds the previous entity, so a stale selection would
            // otherwise pop a Delete menu onto whatever the user right-clicked
            // next (or on empty space).
            if (_inVpRel) {
                if (EditorPickEntity(false)) {
                    // Native Win32 context menu with TPM_RETURNCMD (avoids WM_COMMAND routing issues)
                    #ifdef _WIN32
                    HWND hWnd = (HWND)GetWindowHandle();
                    if (hWnd) {
                        HMENU hMenu = CreatePopupMenu();
                        // Surface Properties first, like the UT99 viewport menu:
                        // it is the per-FACE editor and the most-used entry when
                        // texturing a brush.
                        if (spfPick.hit && spfPick.renderable >= 0) {
                            // Hold Shift to keep the previous face selection and
                            // add this face ("(N Selected)"). Plain right-click
                            // starts a fresh single-face selection.
                            if (!(IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)))
                                ClearSurfacePicks();
                            ToggleSurfacePick(spfPick);
                            const int nSurf = (int)g_selectedSurfaces.size();
                            char lbl[96];
                            snprintf(lbl, sizeof(lbl), "Surface Properties (%d Selected)", nSurf);
                            AppendMenuA(hMenu, MF_STRING, IDM_SURFACE_PROPS, lbl);
                            // The brush-wide twin. Apply writes surface.def (the brush
                            // default) instead of the faces in the mask, which is the only
                            // way `flags=8` on a WHOLE brush - the form every shipped
                            // painted backdrop uses - is reachable from the editor at all.
                            // Two entries rather than one toggle, so the scope is stated
                            // at the click and cannot be misread by the handler.
                            AppendMenuA(hMenu, MF_STRING, IDM_SURFACE_PROPS_BRUSH,
                                        "Surface Properties (Brush-Wide)");
                            AppendMenuA(hMenu, MF_STRING, IDM_SURFACE_RESET, "Reset Surface");
                        }
                        AppendMenuA(hMenu, MF_STRING, IDM_PROPERTIES, "Properties");
                        // Delete/Duplicate exist only for placed objects. They are
                        // `else if` chains with no `else`, so offering them on the
                        // Map row would silently deselect the level and push a
                        // no-op undo snapshot instead of failing visibly.
                        if (g_sel.type != SelType::MAP) {
                            AppendMenuA(hMenu, MF_STRING, IDM_DELETE_ENTITY, "Delete");
                            AppendMenuA(hMenu, MF_STRING, IDM_DUPLICATE_ENTITY, "Duplicate");
                            AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
                            AppendMenuA(hMenu, MF_STRING, IDM_SNAP_GROUND, "Snap to Ground");
                            AppendMenuA(hMenu, MF_STRING, IDM_SNAP_SIDE,   "Snap to Side");
                        }
                        // A Mesh or a CSG brush carries no usable collision of
                        // its own (the world is AABB-only and a placed prop has
                        // none at all), so offer to derive convex proxies for it.
                        if (g_sel.type == SelType::MESH || g_sel.type == SelType::BRUSH) {
                            AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
                            AppendMenuA(hMenu, MF_STRING, IDM_APPEND_AUTOCONVEX,
                                        "Append AutoConvex Collision");
                        }
                        // Texture apply works on a brush face, a legacy WDL model,
                        // or a placed Mesh.* prop - and on the WHOLE selection,
                        // which is why the label stops naming "Surface". This used
                        // to be offered only for BRUSH/MODEL and handled by a
                        // second implementation that never learned about Mesh.
                        if (!g_editorPanels.activeTexturePath.empty() &&
                            (g_sel.type == SelType::BRUSH || g_sel.type == SelType::MODEL ||
                             g_sel.type == SelType::MESH)) {
                            AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
                            AppendMenuA(hMenu, MF_STRING, IDM_APPLY_TEXTURE,
                                        g_sel.type == SelType::BRUSH
                                            ? "Apply Texture to Surface"
                                            : "Apply Texture to Selection");
                        }
                        POINT pt;
                        GetCursorPos(&pt);
                        SetForegroundWindow(hWnd);
                        int cmd = TrackPopupMenu(hMenu,
                            TPM_RIGHTBUTTON | TPM_RETURNCMD,
                            pt.x, pt.y, 0, hWnd, NULL);
                        DestroyMenu(hMenu);
                        // Swallow the input that dismissed/used the menu: the modal
                        // loop skips raylib polling, so a stray held button + stale
                        // mouse delta would otherwise drag the selected entity.
                        g_suppressViewportDrag = IsMouseButtonDown(MOUSE_LEFT_BUTTON);
                        g_gizmoDrag = false;
                        g_gizmoHistPushed = false;
                        g_lbDown = false;
                        (void)GetMouseDelta();
                        (void)GetMouseWheelMove();
                        // Keep the gizmo glued to the (possibly re-picked) selection
                        // so no drift can be applied.
                        if (g_sel.type != SelType::NONE) SnapGizmoToSelection(g_sel);
                        // Handle the returned command directly
                        if (cmd == IDM_PROPERTIES) OpenPropertiesForSelection();
                        else if (cmd == IDM_DELETE_ENTITY) DeleteSelectedEntity();
                        else if (cmd == IDM_DUPLICATE_ENTITY) DuplicateSelectedEntity();
                        else if (cmd == IDM_APPLY_TEXTURE) ApplyActiveTextureToSelection();
                        else if (cmd == IDM_SNAP_GROUND) SnapSelectionToGround();
                        else if (cmd == IDM_SNAP_SIDE)   SnapSelectionToSide();
                        else if (cmd == IDM_APPEND_AUTOCONVEX) { AppendAutoConvexForSelection(); MarkWorldGraphDirty(); }
                        else if (cmd == IDM_SURFACE_PROPS || cmd == IDM_SURFACE_PROPS_BRUSH) {
                            // Every picked face must belong to ONE renderable:
                            // the dialog edits a single BrushSurface, and mixing
                            // two brushes' faces into one mask would write face 3
                            // of brush A onto face 3 of brush B.
                            //
                            // Both scopes share this resolution because the mask is
                            // needed either way - it says which faces the dialog
                            // SHOWS - and brush-wide is deliberately NOT expressed
                            // as an empty mask (see ed::SurfaceEdit::brushWide).
                            int rIdx = -1; uint32_t mask = 0; bool mixed = false;
                            for (const auto& sp : g_selectedSurfaces) {
                                if (rIdx < 0) rIdx = sp.renderable;
                                else if (sp.renderable != rIdx) { mixed = true; break; }
                                if (sp.face >= 0) mask |= (1u << (int)sp.face);
                            }
                            if (mixed || rIdx < 0 || mask == 0) {
                                MessageBoxA(hWnd,
                                    "Surfaces from more than one brush were selected.\n\n"
                                    "Right-click without Shift to start a new selection.",
                                    "Surface Properties", MB_OK | MB_ICONWARNING);
                            } else {
                                ShowSurfacePropsScoped(true, rIdx, mask,
                                                        cmd == IDM_SURFACE_PROPS_BRUSH);
                            }
                        }
                        else if (cmd == IDM_SURFACE_RESET) {
                            // Same event the dialog's own Reset button posts, so
                            // both routes share one handler and one payload shape.
                            //
                            // The mask is rebuilt from g_selectedSurfaces - the
                            // accumulated Shift-selection - and NOT from just the
                            // face under the cursor. The old handler set a bare
                            // bool, so Reset ignored the multi-face selection the
                            // title advertised and only ever reset the last picked
                            // face. Rebuilding also keeps an empty mask meaning
                            // "reset nothing", never "reset all six faces".
                            if (spfPick.hit && spfPick.renderable >= 0) {
                                uint32_t mask = 0;
                                for (const auto& s : g_selectedSurfaces)
                                    if (s.renderable == spfPick.renderable)
                                        mask |= (1u << (int)s.face);
                                ed::SurfaceEdit ev;
                                ev.renderable = spfPick.renderable;
                                ev.faceMask    = mask;
                                ed::EventBus::instance().post(ed::Ev::ResetSurface, ev);
                            }
                        }
                    }
                    #endif
                }
            }
        }
        // Mouse-controlled camera: only move when middle-mouse-button is held.
        // Leaves the cursor free for clicking Win32 panels.
        // MMB          = pan in the SCREEN plane (X/Z at the camera's yaw, up/down
        //                at the camera's pitch)
        // Shift + MMB  = pan vertically along world Y
        // Alt + MMB    = orbit around target
        // Scroll wheel = dolly forward/backward
        //
        // All four live in Core/EditorCamera.hpp. They were inline here AND copied
        // verbatim into Core/DetachedPane.cpp, and both copies panned along WORLD
        // X/Z: correct only for the default 3/4 view, sideways-on in every other
        // one. See that header for the UnrealEd (UnEdCam.cpp) reference the scale
        // and the ortho zoom come from.
        {
            const float paneH = g_activeViewportRect.h > 0.0f
                                    ? g_activeViewportRect.h : (float)GetScreenHeight();
            if (IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) {
                Vector2 delta = GetMouseDelta();
                bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
                bool alt   = IsKeyDown(KEY_LEFT_ALT)   || IsKeyDown(KEY_RIGHT_ALT);

                if (shift)     edcam::PanVertical(OTEditor.MainCamera, delta.y, paneH);
                else if (alt)  edcam::Orbit(OTEditor.MainCamera, delta.x, delta.y);
                else           edcam::Pan(OTEditor.MainCamera, delta.x, delta.y, paneH);
            }

            // Scroll wheel = dolly forward/backward.
            // In MODEL placement with Scale/Rotate tool modes the wheel is
            // reserved for scaling/rotating the ghost instead.
            bool wheelReservedForTool =
                (g_placeMode == PlaceMode::MODEL && AngelTechEditor.DrawModel &&
                 g_editorPanels.currentToolMode != 0);
            if (!wheelReservedForTool)
                edcam::Dolly(OTEditor.MainCamera, GetMouseWheelMove());
        }
        
        // Per-zone environment preview.
        //
        // Reads the SELECTED zone's envOverrides, not the legacy global
        // ZoneProperties: envOverrides is what Core.hpp/ZoneManager actually
        // applies at runtime and what ExportToOzone writes, so previewing
        // anything else shows the author a result the game will never produce.
        // It also means this block no longer depends on the legacy Zone
        // Properties window's one-shot apply flags, which is what lets that
        // window be deleted rather than kept in sync.
        if (g_sel.type == SelType::ZONE) {
            const ZoneVolumeNode* selZone = nullptr;
            for (const auto& z : ZoneManager::Instance().GetZones()) {
                if ((int)z.id == g_sel.index) { selZone = &z; break; }
            }
            if (selZone) {
                const auto& eo = selZone->envOverrides;
                if (eo.applyFog) {
                    float fogColor[3] = {(float)eo.fogR / 255.0f,
                                         (float)eo.fogG / 255.0f,
                                         (float)eo.fogB / 255.0f};
                    float fogStart = eo.fogStart, fogEnd = eo.fogEnd;
                    float fogDensity = eo.fogDensity, fogIntensity = 1.0f;
                    // fogStart/fogEnd were cached on OTEditor and set once at
                    // startup but never updated again, so the legacy dialog's
                    // Start/End rows looked live and did nothing — the editor
                    // stayed pinned at 10/100 while colour and density tracked.
                    if (OTEditor.LitFogShader.id > 0) {
                        SetShaderValue(OTEditor.LitFogShader, OTEditor.FogColorLoc,     fogColor,     SHADER_UNIFORM_VEC3);
                        SetShaderValue(OTEditor.LitFogShader, OTEditor.FogStartLoc,     &fogStart,    SHADER_UNIFORM_FLOAT);
                        SetShaderValue(OTEditor.LitFogShader, OTEditor.FogEndLoc,       &fogEnd,      SHADER_UNIFORM_FLOAT);
                        SetShaderValue(OTEditor.LitFogShader, OTEditor.FogDensityLoc,   &fogDensity,  SHADER_UNIFORM_FLOAT);
                        SetShaderValue(OTEditor.LitFogShader, OTEditor.FogIntensityLoc, &fogIntensity,SHADER_UNIFORM_FLOAT);
                    }
                    // Publish, so the SurfaceMaterial program and
                    // DrawZoneGeometry's restore see the same numbers.
                    OzoneLoader::Instance().SetWorldFog(fogColor, fogStart, fogEnd,
                                                        fogDensity, fogIntensity);
                    OTEditor.FogColor = (Color){ (unsigned char)eo.fogR, (unsigned char)eo.fogG, (unsigned char)eo.fogB, 255 };
                    OTEditor.FogDensity = eo.fogDensity;
                }
                if (eo.applyAmbient) {
                    OTEditor.AmbientColor = (Color){ (unsigned char)eo.ambR, (unsigned char)eo.ambG, (unsigned char)eo.ambB, 255 };
                    OTEditor.AmbientIntensity = eo.ambIntensity;
                    float ambient[4] = {(float)eo.ambR / 255.0f * eo.ambIntensity,
                                        (float)eo.ambG / 255.0f * eo.ambIntensity,
                                        (float)eo.ambB / 255.0f * eo.ambIntensity, 1.0f};
                    if (OTEditor.AmbientLoc >= 0 && OTEditor.LitFogShader.id > 0)
                        SetShaderValue(OTEditor.LitFogShader, OTEditor.AmbientLoc, ambient, SHADER_UNIFORM_VEC4);
                    OzoneLoader::Instance().SetWorldAmbient(ambient[0], ambient[1],
                                                            ambient[2], ambient[3]);
                }
            }
        }
        // View > Show Ambient / Show Fog are PREVIEW masks, not document edits.
        // Re-read the store every frame and mask only the uniforms, so toggling
        // back restores exactly what the level (or the selected zone preview)
        // published; OzoneLoader's authored values are never overwritten.
        if (OTEditor.LitFogShader.id > 0) {
            float fogCol[3], fogStart, fogEnd, fogDensity, fogIntensity;
            OzoneLoader::Instance().GetWorldFog(fogCol, fogStart, fogEnd, fogDensity, fogIntensity);
            if (!g_editorPanels.viewFog) fogIntensity = 0.0f;
            if (OTEditor.FogIntensityLoc >= 0)
                SetShaderValue(OTEditor.LitFogShader, OTEditor.FogIntensityLoc,
                               &fogIntensity, SHADER_UNIFORM_FLOAT);

            float amb[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            OzoneLoader::Instance().GetWorldAmbient(amb);
            if (!g_editorPanels.viewAmbient) { amb[0] = amb[1] = amb[2] = 0.0f; }
            if (OTEditor.AmbientLoc >= 0)
                SetShaderValue(OTEditor.LitFogShader, OTEditor.AmbientLoc,
                               amb, SHADER_UNIFORM_VEC4);
        }
// Level state (GameType / skybox / particles) no longer has a per-frame apply path.
        // It used to: the removed Zone Properties window set one-shot apply flags on a
        // global, and the next frame this block noticed them and wrote LevelMetadata.
        // The Map row applies level state directly in ApplyMapProperties(), so the
        // flags, the global and this block all went together.

        // Submit lights and viewPos for Lit mode
        if (OTEditor.ViewMode == LightingMode::LIT && OTEditor.LitFogShader.id > 0 &&
            oz::diag::PerfOn("lights")) {
            OZ_PROFILE_SCOPE("ed.lighting");
            auto& pawnLights = PawnSystem::Instance().GetLights();
            float dt = GetFrameTime();
            LitLightning_Update(pawnLights, OTEditor.LitFogShader, OTEditor.MainCamera, dt);
            // The surface program carries its own light uniforms, so a
            // surface-flagged brush would be lit by a stale light set without
            // this. Same lights, same order - one source of truth.
            //
            // Ambient and fog are read back from OzoneLoader rather than
            // recomputed, because the per-zone preview block above publishes
            // them there. Passing the editor's own copy instead would let the
            // two drift the moment a zone sets only one of the two.
            if (oz::SurfaceMaterial::Instance().Ready()) {
                float amb[4] = {0.1f, 0.1f, 0.1f, 1.0f};
                OzoneLoader::Instance().GetWorldAmbient(amb);
                float fogCol[3] = {0.7f, 0.7f, 0.8f};
                float fogStart = 10.0f, fogEnd = 100.0f, fogDensity = 1.0f, fogIntensity = 1.0f;
                OzoneLoader::Instance().GetWorldFog(fogCol, fogStart, fogEnd,
                                                    fogDensity, fogIntensity);
                if (!g_editorPanels.viewFog) fogIntensity = 0.0f;
                if (!g_editorPanels.viewAmbient) { amb[0] = amb[1] = amb[2] = 0.0f; }
                oz::SurfaceMaterial::Instance().SetFog(fogCol, fogStart, fogEnd,
                                                       fogDensity, fogIntensity);
                oz::SurfaceMaterial::Instance().UpdateFrame(pawnLights, OTEditor.MainCamera, dt, amb);
            }
            if (OTEditor.ViewPosLoc >= 0) {
                Vector3 camPos = OTEditor.MainCamera.position;
                SetShaderValue(OTEditor.LitFogShader, OTEditor.ViewPosLoc, &camPos, SHADER_UNIFORM_VEC3);
            }
        }

        // -------------------------------------------------------------------
        // Sky texture + SURF_PORTAL view.
        //
        // Resolved BEFORE the 3D mode (and before anything seeds the target):
        // raylib cannot nest render textures, so the portal pass must own the
        // render target when it runs. The cube below and the portal view share
        // this one resolution, exactly like the client.
        // -------------------------------------------------------------------
        static Texture2D s_skyTex = {0};
        static std::string s_skyPath;

        oz::SkyboxFog skyFog;
        skyFog.active = true;
        OzoneLoader::Instance().GetWorldFog(skyFog.color, skyFog.start, skyFog.end,
                                            skyFog.density, skyFog.intensity);
        // Fog preview off must also clear the horizon band on the skybox, not
        // just the world shader; Sky.fs reads the same intensity.
        if (!g_editorPanels.viewFog) skyFog.intensity = 0.0f;

        if (OTEditor.ShowSkybox) {
            // An explicitly-applied levelinfo skybox path takes priority;
            // otherwise fall back to the world's Models/Skybox.png default.
            std::string want;
            {
                LevelMetadata meta = GetLevelMetadata();
                if (!meta.skyboxTexturePath.empty())
                    want = meta.skyboxTexturePath;
            }
            if (want.empty() && !g_documentPath.empty()) {
                fs::path worldSky = g_documentPath.parent_path() / "Models" / "Skybox.png";
                if (fs::exists(worldSky)) want = worldSky.string();
            }
            if (want != s_skyPath) {
                if (s_skyTex.id > 0) { UnloadTexture(s_skyTex); s_skyTex = {0}; }
                if (!want.empty()) {
                    s_skyTex = LoadTextureWithFallback(want.c_str());
                    if (s_skyTex.id == 0)
                        EditorLog("Skybox: could not load '%s'", want.c_str());
                }
                s_skyPath = want;
            }
        }
        const bool wantSky = OTEditor.ShowSkybox && s_skyTex.id > 0;
        // Sky portals sample their pre-render by gl_FragCoord / uViewport, i.e. in
        // WINDOW space. A split pane is a sub-rect, so drawing the full-window view
        // into it samples the wrong region. Until per-pane portal targets land
        // (Phase 4), portals are only shown in the single-view layout; a split pane
        // draws the brush's own texture instead of a mis-sampled sky.
        const bool skyPortals = wantSky && g_editorPanels.viewLayout == 0 &&
                                OzoneLoader::Instance().HasSkyPortalFaces();

        if (skyPortals && oz::diag::PerfOn("sky") && oz::diag::PerfOn("portals")) {
            OZ_PROFILE_SCOPE("ed.skyPortalRender");
            SkyZoneNode* sky = PawnSystem::Instance().GetActiveSkyZone();
            const Vector3 anchor = sky ? sky->position : OTEditor.MainCamera.position;
            oz::SkyPortal::Instance().Render(
                OTEditor.MainCamera, anchor, s_skyTex, Texture2D{0}, &skyFog,
                GetScreenWidth(), GetScreenHeight());
        }

        ClearBackground(BLACK);

        // Apply lighting mode before 3D rendering
        rlDrawRenderBatchActive();
        if (OTEditor.ViewMode == LightingMode::WIREFRAME)
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        else
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        // Backface culling is NOT set here. It is applied by
        // OzoneLoader::Draw(camera, cullBackfaces) through the tracked setter in
        // Renderer/CullState.hpp. The raw glEnable/glDisable pair that used to
        // live here left that tracked flag stale, so every later ScopedCullOff
        // restored the wrong value - and Draw() defaulted cullBackfaces=true, so
        // it re-enabled culling immediately afterwards and WIREFRAME drew front
        // faces only (a box lost its far edges).

        // --- Multi-viewport (Phase 1): the primary pane is the full editor pass
        // below; secondary panes are read-only reference views rendered after it.
        // Scissor + viewport + a pane-aspect projection keep each view framed.
        std::vector<ed::viewport::VpRect> vpRects;
        EditorViewportRectsNow(vpRects);
        // A pane renders into the main window (scissor) OR into its own framebuffer
        // when it has been popped out (Tier A). Everything below is identical for
        // both; only the target wrap and the framed rect differ.
        const int vpActive = g_editorPanels.viewActive;
        const bool vpFbo = PaneIsDetached(vpActive);
        if (vpFbo) DetachedPane_EnsureRT(vpActive);
        const ed::viewport::VpRect vpPrimary = vpFbo
            ? PaneRenderRect(vpActive, vpRects)
            : Editor_ActiveRect(vpRects);
        if (vpFbo) {
            BeginTextureMode(g_paneTargets[vpActive].rt);
            ClearBackground(BLACK);
        } else {
            BeginScissorMode((int)vpPrimary.x, (int)vpPrimary.y, (int)vpPrimary.w, (int)vpPrimary.h);
            SetPaneViewport(vpPrimary);
        }

        BeginMode3D(OTEditor.MainCamera);
        SetPaneProjection(OTEditor.MainCamera, vpPrimary);

        // -------------------------------------------------------------------
        // Skybox cube (viewport backdrop) â€” toggled by the toolbar "Sky" button.
        // Mirrors the client: top/bottom use the cap texture, sides reuse it
        // unless a dedicated side skybox is authored. The cube itself - face
        // geometry, per-face rotations, distance, fog and the draw loop - is
        // oz::Skybox's, shared with the client (this block used to carry its own
        // copy at radius 1000 with no fog, so the editor's sky had a different
        // angular size and no horizon fog).
        // -------------------------------------------------------------------
        if (wantSky && oz::diag::PerfOn("sky")) {
            OZ_PROFILE_SCOPE("ed.sky");
            oz::Skybox::Instance().Draw(
                OTEditor.MainCamera.position, s_skyTex, Texture2D{0},
                /*cullBackfaces=*/OTEditor.ViewMode != LightingMode::WIREFRAME,
                &skyFog);
        }

        if (oz::diag::PerfOn("grid")) {
            OZ_PROFILE_SCOPE("ed.grid");
            DrawGrid(1000, 10.0f);
        }

        // OZONE world geometry. The caller's view mode IS the culling intent.
        // The cull aspect must be THIS pane's aspect: BuildFrustum otherwise
        // uses the whole-window aspect, which is too narrow for a 2x1 pane and
        // culls geometry the pane can see.
        {
            OZ_PROFILE_SCOPE("ed.world");
            OzoneLoader::Instance().SetCullAspect(vpPrimary.h > 0.0f
                                                      ? vpPrimary.w / vpPrimary.h
                                                      : 0.0f);
            OzoneLoader::Instance().Draw(OTEditor.MainCamera,
                                         OTEditor.ViewMode != LightingMode::WIREFRAME);
        }

        // SURF_PORTAL faces sample the sky view rendered before BeginMode3D.
        // Drawn after the world so geometry in front of a portal occludes it -
        // the same ordering the client uses. Split panes skip this (see skyPortals).
        if (skyPortals && oz::diag::PerfOn("portals")) {
            OZ_PROFILE_SCOPE("ed.portalPass");
            OzoneLoader::Instance().DrawSkyPortals(OTEditor.MainCamera,
                                                   oz::SkyPortal::Instance().View(),
                                                   GetScreenWidth(), GetScreenHeight());
        }

        // One-shot texture-apply probe (armed by the apply drains), logged after
        // the world pass so the lazy face-mesh build has run for this frame.
        // Distinguishes "state not written" from "state written but not rendered"
        // from "view mode Wireframe" in a user's session log.
        if (g_editorPanels.textureProbeRenderable >= 0) {
            const int probeIndex = g_editorPanels.textureProbeRenderable;
            g_editorPanels.textureProbeRenderable = -1;
            OzoneRenderable* probeRenderable = OzoneLoader::Instance().Get(probeIndex);
            if (probeRenderable) {
                const char* modeName = "LIT";
                switch (OTEditor.ViewMode) {
                    case LightingMode::UNLIT:     modeName = "UNLIT"; break;
                    case LightingMode::WIREFRAME: modeName = "WIREFRAME"; break;
                    case LightingMode::DYNAMIC:   modeName = "DYNAMIC"; break;
                    default: break;
                }
                int builtFaceCount = 0;
                char faceTexIds[96] = {0};
                int textWritten = 0;
                for (int faceIndex = 0; faceIndex < oz::surface::FACE_COUNT; faceIndex++) {
                    if (probeRenderable->faceMesh[faceIndex].vaoId > 0) builtFaceCount++;
                    const Texture2D faceTexture = OzoneLoader::Instance().ResolveFaceTextureHandle(
                        probeRenderable->surface.Resolve((oz::surface::SurfaceFace)faceIndex));
                    const int written = snprintf(faceTexIds + textWritten,
                                                 sizeof(faceTexIds) - textWritten,
                                                 "%s%u", faceIndex ? "," : "", faceTexture.id);
                    if (written < 0) break;
                    textWritten += written;
                    if (textWritten >= (int)sizeof(faceTexIds) - 1) break;
                }
                EditorLog("Texture probe idx=%d mode=%s surfaceShader=%d perFace=%d built=%d "
                          "faces=%d/%d faceTex=[%s] diffuse=%u custom=%u path='%s' mirror='%s'",
                          probeIndex, modeName,
                          oz::SurfaceMaterial::Instance().Ready() ? 1 : 0,
                          probeRenderable->surface.NeedsPerFaceDraw() ? 1 : 0,
                          probeRenderable->faceMeshesBuilt ? 1 : 0,
                          builtFaceCount, oz::surface::FACE_COUNT, faceTexIds,
                          probeRenderable->model.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture.id,
                          probeRenderable->customTex.id,
                          probeRenderable->surface.def.texPath.c_str(),
                          probeRenderable->texPath.c_str());
            }
        }

        // Sky zone rendering in editor
        {
            PawnSystem::Instance().UpdateSkyZone(
                OTEditor.MainCamera.position,
                (BoundingBox){{OTEditor.MainCamera.position.x-1,OTEditor.MainCamera.position.y-10,OTEditor.MainCamera.position.z-1},
                              {OTEditor.MainCamera.position.x+1,OTEditor.MainCamera.position.y,OTEditor.MainCamera.position.z+1}});
            if (PawnSystem::Instance().IsInSkyZone()) {
                rlDisableDepthMask();
                OzoneLoader::Instance().DrawZoneGeometry(
                    OTEditor.MainCamera,
                    PawnSystem::Instance().GetSkyZoneBounds());
                rlEnableDepthMask();
            }
        }

        // Additive glow halo, after the opaque world so it lands on top of the
        // composited frame - the same place the client draws it (Core.hpp, right
        // after DrawWorldGeometry). Without this call the editor showed the
        // emissive term from Surface.fs but never the halo, and since the halo is
        // the ONLY thing the glow COLOUR affects, surfGlow was not authorable:
        // you could set the colour and see no difference at all.
        // No-op unless a surface actually carries SURF_GLOW.
        if (oz::diag::PerfOn("glow")) {
            OZ_PROFILE_SCOPE("ed.glowPass");
            OzoneLoader::Instance().DrawGlowGeometry(OTEditor.MainCamera);
        }

        // Pawn system (use lit shader for billboards when in lit/unlit mode)
        {
            OZ_PROFILE_SCOPE("ed.entities");
            Shader bbShader = {0};
            if (OTEditor.ViewMode == LightingMode::LIT) bbShader = OTEditor.LitFogShader;
            else if (OTEditor.ViewMode == LightingMode::UNLIT) bbShader = OTEditor.UnlitShader;
            PawnSystem::Instance().DrawAll(OTEditor.MainCamera, bbShader);
            PawnSystem::Instance().DrawEntities(OTEditor.MainCamera, bbShader);
        }

        // Vertex-edit overlay (Phase C): highlight selected vertices.
        if (g_editorPanels.animEditVerts) {
            MeshObjectNode* vn = PawnSystem::Instance().GetMeshObject(g_editorPanels.animTargetMesh);
            oz::AnimatedMesh* vam = AnimTarget();
            if (vn && vn->editPose && vam) {
                for (int vi : g_editorPanels.animSelVerts)
                    DrawSphere(MeshVertexWorld(*vn, *vam, *vn->editPose, vi), 0.06f, RED);
            }
        }

        // Collision volume wireframes (editor only, toggled by the sidebar
// "Collision" button). This is the post-CSG result - what the player actually
// stands on - which is the only way to spot a brush whose render mesh is solid
// but whose collision volume does not exist, and to see generated
// SURF_COLLISION_PROXY boxes in place.
if (g_editorPanels.showCollisionBounds) {
    Color warnCol = g_editorPanels.collisionOpWarning
        ? (Color){255, 90, 90, 220} : (Color){120, 220, 160, 160};
    for (const auto& v : OzoneLoader::Instance().GetCollisionVolumes())
        DrawBoundingBox(v.aabb, warnCol);
}

// Zone volume wireframes
        {
            auto& zones = ZoneManager::Instance().GetZones();
            for (auto& z : zones) {
                Color wireColor;
                switch (z.zoneType) {
                    case ZoneType::ZONE_LADDER: wireColor = (Color){180, 120, 0, 80};   break;
                    case ZoneType::ZONE_SKY:    wireColor = (Color){100, 150, 255, 80};  break;
                    case ZoneType::ZONE_REVERB: wireColor = (Color){150, 50, 200, 80};   break;
                    case ZoneType::ZONE_GAMEPLAY_SOUND: wireColor = (Color){50, 200, 50, 80}; break;
                    // Gravity volumes get their own tint so a tilted-gravity
                    // pocket is distinguishable from a water volume at a glance.
                    case ZoneType::ZONE_GRAVITY: wireColor = (Color){230, 90, 220, 80}; break;
                    case ZoneType::ZONE_CRT:     wireColor = (Color){255, 240, 120, 80}; break;
                    default:                    wireColor = (Color){50, 120, 200, 80};   break;
                }
                DrawBoundingBox(z.bounds, wireColor);
            }
        }

        // GameEngine.PathNode markers + link lines (editor visualization)
        {
            auto& paths = PawnSystem::Instance().GetPathNodes();
            Color pathCol = (Color){120, 220, 255, 255};
            for (auto& pn : paths) {
                DrawCubeWires(pn.position, 0.6f, 0.6f, 0.6f, pathCol);
                DrawSphere(pn.position, 0.15f, pathCol);
                if (g_sel.type == SelType::PATHNODE && (int)pn.id == g_sel.index)
                    DrawSphereWires(pn.position, pn.radius, 8, 8, (Color){255, 180, 60, 120});
                for (auto& linkName : pn.next) {
                    PathNode* dst = PawnSystem::Instance().FindPathNodeByName(linkName);
                    if (dst) DrawLine3D(pn.position, dst->position, pathCol);
                }
                if (pn.loop)
                    DrawLine3D(pn.position,
                               {pn.position.x, pn.position.y + 1.5f, pn.position.z},
                               (Color){255, 220, 80, 255});
            }
        }

        // WindZone boxes + direction arrows (editor visualization)
        {
            auto& winds = PawnSystem::Instance().GetWindZones();
            Color windCol = (Color){120, 255, 180, 100};
            for (auto& wz : winds) {
                DrawBoundingBox(wz.bounds, windCol);
                Vector3 c = {(wz.bounds.min.x + wz.bounds.max.x) * 0.5f,
                             (wz.bounds.min.y + wz.bounds.max.y) * 0.5f,
                             (wz.bounds.min.z + wz.bounds.max.z) * 0.5f};
                Vector3 tip = {c.x + wz.direction.x * 3.0f,
                               c.y + wz.direction.y * 3.0f,
                               c.z + wz.direction.z * 3.0f};
                DrawLine3D(c, tip, (Color){120, 255, 180, 255});
                DrawSphere(tip, 0.15f, (Color){120, 255, 180, 255});
            }
        }

        // --- Placement visuals ---
        //
        // `|| Gizmo_IsDragging() || Gizmo_IsUnrealDragging()` is load-bearing: the
        // gizmo drag update, the wheel scale/rotate shortcuts and the per-frame
        // gizmo-to-entity write-back all live in this block. If DrawModel ever
        // dropped mid-gesture (Escape, a pick miss, a panel that recomputed the
        // selection) the drag had started but could never apply, which reads as
        // "Move/Rotate do nothing".
        if (AngelTechEditor.DrawModel || Gizmo_IsDragging() || Gizmo_IsUnrealDragging())
        {
            float px = AngelTechEditor.X, py = AngelTechEditor.Y, pz = AngelTechEditor.Z;
            float ps = AngelTechEditor.S, pr = AngelTechEditor.R;

            if (g_placeMode == PlaceMode::MODEL) {
                if (EMID >= 200) {
                    // OZONE primitive ghost
                    int primType = EMID - 200;
                    Vector3 size = {AngelTechEditor.W * ps, AngelTechEditor.H * ps, AngelTechEditor.L * ps};
                    Vector3 center = {px, py, pz};
                    DrawBoundingBox((BoundingBox){{px - size.x*0.5f, py - size.y*0.5f, pz - size.z*0.5f},
                                                  {px + size.x*0.5f, py + size.y*0.5f, pz + size.z*0.5f}}, ORANGE);
                    // Apply S to the OZONE ghost too. AddBrushRenderable bakes
                    // the size and stores S as r->scale, so drawing the unscaled
                    // ghost showed a brush smaller than the one that got placed.
                    if (primType == 0) { // Box
                        DrawCubeWires(center, size.x, size.y, size.z, (Color){255,165,0,180});
                    } else if (primType == 1) { // Cylinder
                        DrawCylinderWires(center, size.x*0.5f, size.x*0.5f, size.y, 16, ORANGE);
                    } else if (primType == 2) { // Sphere
                        DrawSphereWires(center, size.x*0.5f, 12, 12, ORANGE);
                    } else if (primType == 3) { // Pyramid
                        DrawCubeWires(center, size.x, size.y, size.z, ORANGE);
                    } else if (primType == 4) { // Plane
                        DrawCubeWires(center, size.x, 0.1f, size.z, ORANGE);
                    }
                } else if (EMID > 0) {
                    LoadedModel* lm = WDLModels.GetModelByWDLId(EMID);
                    if (lm) DrawModelEx(lm->model, {px,py,pz},{0,pr,0},pr,{ps,ps,ps},WHITE);
                } else if (EMID == 0) {
                    // Ghost preview of the model captured at "Place in World"
                    // time. Reading g_editorPanels.selectedModel here meant a
                    // rescan (import / Refresh) left the ghost undrawn entirely,
                    // because that index is reset to -1 - and an unplaced ghost
                    // that vanishes on the next click reads as a deleted mesh.
                    //
                    // Drawn ONLY while a placement is armed. DrawModel doubles as
                    // "a selection gizmo is drawn", so without this gate the last
                    // placed model kept hovering at the gizmo of whatever was
                    // selected next - which reads as a stray duplicate.
                    if (g_placementArmed) {
                        static Model s_browsePreview = {0};
                        static std::string s_browsePreviewPath;
                        const std::string& path = AngelTechEditor.ActiveModelPath;
                        if (s_browsePreviewPath != path) {
                            if (s_browsePreview.meshes) UnloadModel(s_browsePreview);
                            s_browsePreview = {0};
                            s_browsePreviewPath = path;
                            if (!path.empty())
                                s_browsePreview = LoadModelWithFallback(path.c_str());
                        }
                        if (s_browsePreview.meshes)
                            DrawModelEx(s_browsePreview, {px,py,pz},{0,1,0},pr,{ps,ps,ps},WHITE);
                        else
                            DrawCubeWires({px,py,pz}, ps, ps, ps, ORANGE);
                    }
                } else if (EMID == -1) {
                    DrawBoundingBox((BoundingBox){{px,py,pz},{AngelTechEditor.W,AngelTechEditor.H,AngelTechEditor.L}}, ORANGE);
                    DrawCubeWires({AngelTechEditor.W,AngelTechEditor.H,AngelTechEditor.L}, ps, ps, ps, PINK);
                } else if (EMID == -2) {
                    DrawBoundingBox((BoundingBox){{px,py,pz},{AngelTechEditor.W,AngelTechEditor.H-5,AngelTechEditor.L}}, ORANGE);
                    DrawCubeWires({AngelTechEditor.W,AngelTechEditor.H-5,AngelTechEditor.L}, ps, ps, ps, PINK);
                }
            } else if (g_placeMode == PlaceMode::PICKUP) {
                // Draw pickup preview from model (fallback to colored cube)
                static Model g_pickupPreviewModel = {0};
                static std::string g_pickupPreviewName;
                if (g_pickupPreviewName != AngelTechEditor.ActivePickupName) {
                    if (g_pickupPreviewModel.meshes) UnloadModel(g_pickupPreviewModel);
                    g_pickupPreviewModel = {0};
                    g_pickupPreviewName = AngelTechEditor.ActivePickupName;
                    const EntityDef* edef = LightningEntityRegistry::Instance().Find(AngelTechEditor.ActivePickupName);
                    if (edef && !edef->mesh.empty()) {
                        // Resolve relative mesh/texture paths beside the .ozls.
                        std::string baseDir;
                        size_t slash = edef->sourcePath.find_last_of("/\\");
                        if (slash != std::string::npos) baseDir = edef->sourcePath.substr(0, slash + 1);
                        std::string meshRes = oz::ResolveMeshAsset(baseDir, edef->mesh);
                        std::string texRes = oz::ResolveMeshAsset(baseDir, edef->texture);
                        g_pickupPreviewModel = LoadModelWithFallback(meshRes.c_str());
                        if (g_pickupPreviewModel.meshes && !texRes.empty()) {
                            Texture2D tex = LoadTextureWithFallback(texRes.c_str());
                            if (tex.id > 0)
                                g_pickupPreviewModel.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = tex;
                        }
                    }
                }
                if (g_pickupPreviewModel.meshes) {
                    DrawModelEx(g_pickupPreviewModel, {px, py, pz}, {0, pr, 0}, pr, {ps, ps, ps}, WHITE);
                } else {
                    Color c = GREEN;
                    const EntityDef* edef = LightningEntityRegistry::Instance().Find(AngelTechEditor.ActivePickupName);
                    if (edef) {
                        auto it = edef->stats.vec3s.find("preview_color");
                        if (it != edef->stats.vec3s.end()) {
                            c.r = (unsigned char)(it->second[0] * 255.0f);
                            c.g = (unsigned char)(it->second[1] * 255.0f);
                            c.b = (unsigned char)(it->second[2] * 255.0f);
                        }
                    }
                    DrawCube({px, py + 0.5f, pz}, 0.6f, 0.8f, 0.6f, c);
                    DrawCubeWires({px, py + 0.5f, pz}, 0.6f, 0.8f, 0.6f, (Color){c.r,c.g,c.b,80});
                }
            } else if (g_placeMode == PlaceMode::NODE) {
                Color c = BLUE;
                switch (AngelTechEditor.ActiveNodeType) {
                    case EditorNodeType::SPAWN:  c = BLUE;    break;
                    case EditorNodeType::NPC:    c = MAGENTA; break;
                    case EditorNodeType::LIGHT:  c = YELLOW;  break;
                    case EditorNodeType::ZONE:   c = SKYBLUE; break;
                    case EditorNodeType::PORTAL: c = PURPLE;  break;
                }
                if (AngelTechEditor.ActiveNodeType == EditorNodeType::PORTAL) {
                    // Portal preview: full volume wireframe + swirl marker
                    float hw = fmaxf(AngelTechEditor.W, 1) * 0.5f;
                    float hh = fmaxf(AngelTechEditor.H, 1) * 0.5f;
                    float hd = fmaxf(AngelTechEditor.L, 1) * 0.5f;
                    DrawCubeWires({px, py, pz}, hw * 2, hh * 2, hd * 2, c);
                    DrawCube({px, py, pz}, hw * 2, hh * 2, hd * 2, (Color){c.r,c.g,c.b,40});
                    DrawSphere({px, py + hh, pz}, 0.3f, c);
                } else if (AngelTechEditor.ActiveNodeType == EditorNodeType::ZONE) {
                    float hw = fmaxf(AngelTechEditor.W, 1) * 0.5f;
                    float hh = fmaxf(AngelTechEditor.H, 1) * 0.5f;
                    float hd = fmaxf(AngelTechEditor.L, 1) * 0.5f;
                    DrawCubeWires({px, py, pz}, hw * 2, hh * 2, hd * 2, c);
                    DrawCube({px, py, pz}, hw * 2, hh * 2, hd * 2, (Color){c.r,c.g,c.b,30});
                } else {
                    DrawCube({px, py, pz}, 0.5f, 0.2f, 0.5f, c);
                    DrawCubeWires({px, py, pz}, 0.5f, 0.2f, 0.5f, (Color){c.r,c.g,c.b,80});
                }
            }

            // Axis gizmo
            float gizmoLen = fmaxf(ps * 3, 2.0f);
            float gizmoTip = gizmoLen * 0.1f;
            // X axis (Red)
            DrawLine3D({px, py, pz}, {px + gizmoLen, py, pz}, RED);
            DrawSphere({px + gizmoLen, py, pz}, gizmoTip, RED);
            // Y axis (Blue)
            DrawLine3D({px, py, pz}, {px, py + gizmoLen, pz}, BLUE);
            DrawSphere({px, py + gizmoLen, pz}, gizmoTip, BLUE);
            // Z axis (Green)
            DrawLine3D({px, py, pz}, {px, py, pz + gizmoLen}, GREEN);
            DrawSphere({px, py, pz + gizmoLen}, gizmoTip, GREEN);
            // Origin sphere
            DrawSphere({px, py, pz}, gizmoTip * 0.5f, (Color){180, 180, 180, 200});

            // Movement controls - LETTER NUDGES.
            //
            // These are a convenience nudge, not a shortcut, so they must never run
            // while Ctrl or Alt is held. That is not tidiness: Windows VK codes and
            // raylib key codes are POSITIONAL, so on German QWERTZ the key LABELLED
            // Z is the physical Y key. Ctrl+Z therefore arrived as KEY_Y, matched
            // nothing in the shortcut table, and fell into this block as
            // `Y -= 2.0f` - the selection stepped down the Y axis instead of
            // undoing. Edit: Undo was unreachable on every non-US layout.
            const bool nudgeMods = EdKeys::CtrlDown() || EdKeys::AltDown();
            if (!nudgeMods &&
                g_sel.type != SelType::NONE && !g_editorPanels.animEditVerts &&
                EditorUi_ViewportHasFocus() &&
                (IsKeyPressed(KEY_U) || IsKeyPressed(KEY_J) || IsKeyPressed(KEY_H) ||
                 IsKeyPressed(KEY_K) || IsKeyPressed(KEY_Y) || IsKeyPressed(KEY_I) ||
                 IsKeyPressed(KEY_O) || IsKeyPressed(KEY_L) ||
                 IsKeyPressed(KEY_T) || IsKeyPressed(KEY_B))) {
                HistoryPush();
            }
            if (!nudgeMods) {
                if (!IsMouseButtonDown(2)) {
                    if (IsKeyPressed(KEY_U)) AngelTechEditor.X -= 2.0f;
                    if (IsKeyPressed(KEY_J)) AngelTechEditor.X += 2.0f;
                    if (IsKeyPressed(KEY_H)) AngelTechEditor.Z += 2.0f;
                    if (IsKeyPressed(KEY_K)) AngelTechEditor.Z -= 2.0f;
                    if (IsKeyPressed(KEY_Y)) AngelTechEditor.Y -= 2.0f;
                    if (IsKeyPressed(KEY_I)) AngelTechEditor.Y += 2.0f;
                } else {
                    if (IsKeyPressed(KEY_U)) AngelTechEditor.W -= 2.0f;
                    if (IsKeyPressed(KEY_J)) AngelTechEditor.W += 2.0f;
                    if (IsKeyPressed(KEY_H)) AngelTechEditor.L += 2.0f;
                    if (IsKeyPressed(KEY_K)) AngelTechEditor.L -= 2.0f;
                    if (IsKeyPressed(KEY_Y)) AngelTechEditor.H -= 2.0f;
                    if (IsKeyPressed(KEY_I)) AngelTechEditor.H += 2.0f;
                }

                if (IsKeyPressed(KEY_O)) AngelTechEditor.R += 90.0f;
                if (IsKeyPressed(KEY_L)) AngelTechEditor.R -= 90.0f;
                if (IsKeyDown(KEY_T)) AngelTechEditor.S += 0.5f;
                // NOTE: scale-down is B, not G â€” G cycles the CSG op in MODEL mode.
                if (IsKeyDown(KEY_B)) AngelTechEditor.S -= 0.5f;
            }

            // Vertex-edit transform (Phase C): the movement/rotate keys deform
            // the selected vertices instead of moving the mesh node.
            if (g_editorPanels.animEditVerts) {
                MeshObjectNode* vn = PawnSystem::Instance().GetMeshObject(g_editorPanels.animTargetMesh);
                oz::AnimatedMesh* vam = AnimTarget();
                if (!g_editorPanels.animPrevValid) {
                    g_editorPanels.animPrevX = AngelTechEditor.X;
                    g_editorPanels.animPrevY = AngelTechEditor.Y;
                    g_editorPanels.animPrevZ = AngelTechEditor.Z;
                    g_editorPanels.animPrevR = AngelTechEditor.R;
                    g_editorPanels.animPrevValid = true;
                }
                float vdx = AngelTechEditor.X - g_editorPanels.animPrevX;
                float vdy = AngelTechEditor.Y - g_editorPanels.animPrevY;
                float vdz = AngelTechEditor.Z - g_editorPanels.animPrevZ;
                float vdr = AngelTechEditor.R - g_editorPanels.animPrevR;
                if (vn && vn->editPose && vam && !g_editorPanels.animSelVerts.empty() &&
                    (vdx != 0.0f || vdy != 0.0f || vdz != 0.0f || vdr != 0.0f)) {
                    AnimSnapshotPush();
                    std::vector<float>& offs = *vn->editPose;
                    const std::vector<float>& base = vam->BasePositions();
                    Vector3 cen = {0, 0, 0};
                    for (int vi : g_editorPanels.animSelVerts) {
                        cen.x += base[vi * 3 + 0] + offs[vi * 3 + 0];
                        cen.y += base[vi * 3 + 1] + offs[vi * 3 + 1];
                        cen.z += base[vi * 3 + 2] + offs[vi * 3 + 2];
                    }
                    float inv = 1.0f / (float)g_editorPanels.animSelVerts.size();
                    cen.x *= inv; cen.y *= inv; cen.z *= inv;
                    float yawRad = vdr * DEG2RAD;
                    for (int vi : g_editorPanels.animSelVerts) {
                        Vector3 local = {base[vi * 3 + 0] + offs[vi * 3 + 0],
                                         base[vi * 3 + 1] + offs[vi * 3 + 1],
                                         base[vi * 3 + 2] + offs[vi * 3 + 2]};
                        local.x += vdx; local.y += vdy; local.z += vdz;
                        if (yawRad != 0.0f) {
                            Vector3 rel = {local.x - cen.x, local.y - cen.y, local.z - cen.z};
                            rel = Vector3RotateByAxisAngle(rel, {0, 1, 0}, yawRad);
                            local = {cen.x + rel.x, cen.y + rel.y, cen.z + rel.z};
                        }
                        offs[vi * 3 + 0] = local.x - base[vi * 3 + 0];
                        offs[vi * 3 + 1] = local.y - base[vi * 3 + 1];
                        offs[vi * 3 + 2] = local.z - base[vi * 3 + 2];
                    }
                }
                // Park the gizmo so the node itself doesn't move while editing.
                AngelTechEditor.X = g_editorPanels.animPrevX;
                AngelTechEditor.Y = g_editorPanels.animPrevY;
                AngelTechEditor.Z = g_editorPanels.animPrevZ;
                AngelTechEditor.R = g_editorPanels.animPrevR;
            } else {
                g_editorPanels.animPrevValid = false;
            }

            // Sync gizmo position back into the selection (for manipulation).
            //
            // The per-type write itself lives in MoveSelectionCentre() so the
            // ADDITIONAL members of a multi-selection can use the same code.
            // UnrealEd's MoveActors applies one Delta to every bSelected actor;
            // here only the primary used to move, so a Shift-selected set came
            // apart the moment you dragged it.
            if (g_sel.type != SelType::NONE && !g_editorPanels.animEditVerts) {
                const Vector3 newPos = {AngelTechEditor.X, AngelTechEditor.Y, AngelTechEditor.Z};

                // Multi-selection: the gizmo tracks the PRIMARY, so the rigid
                // delta is (newPos - primary's last centre). Each other member is
                // offset from its OWN centre by that delta - not assigned the
                // primary's position, which would stack the whole set on one spot.
                if (g_selSet.size() > 1) {
                    const Vector3 d = {newPos.x - g_sel.pos.x,
                                       newPos.y - g_sel.pos.y,
                                       newPos.z - g_sel.pos.z};
                    if (d.x != 0.0f || d.y != 0.0f || d.z != 0.0f) {
                        for (auto& s : g_selSet) {
                            if (s.type == g_sel.type && s.index == g_sel.index) continue;
                            const Vector3 p = {s.pos.x + d.x, s.pos.y + d.y, s.pos.z + d.z};
                            MoveSelectionCentre(s, p, false);
                            s.pos = p;
                        }
                        // Keep the primary's own entry in the set in step, so the
                        // highlight box and the next frame's delta agree.
                        for (auto& s : g_selSet)
                            if (s.type == g_sel.type && s.index == g_sel.index) s.pos = newPos;
                    }
                }

                MoveSelectionCentre(g_sel, newPos, true);

                // Rebuild the CSG world once, AFTER the drag, not every frame.
                // Mid-drag the renderables move continuously, so a per-frame
                // rebuild is a full O(brush) CSG pass for no benefit; nothing
                // reads collision while the gizmo is being dragged.
                if (g_editorPanels.collisionDirty && !g_gizmoDrag) {
                    OzoneLoader::Instance().RebuildCollisionVolumes();
                    g_editorPanels.collisionDirty = false;
                }

                // Update selection stored position
                g_sel.pos = newPos;
            }

            // Commit placement. Enter and a viewport CLICK both place; the click
            // arrives as g_commitPlacement, set on left-click release above.
            const bool commitRequested = IsKeyPressed(KEY_ENTER) || g_commitPlacement;
            g_commitPlacement = false;
            if (commitRequested)
            {
                bool willCommit =
                    (g_placeMode == PlaceMode::PICKUP) ||
                    (g_placeMode == PlaceMode::NODE) ||
                    (g_placeMode == PlaceMode::MODEL &&
                     ((EMID == 0 && !AngelTechEditor.ActiveModelPath.empty()) || EMID >= 200));
                if (willCommit) HistoryPush();
                if (g_placeMode == PlaceMode::MODEL) {
                    if (EMID > 0) {
                        // Legacy WDL Model/Collision placement is removed; OZONE
                        // primitives (EMID >= 200) are committed below.
                    } else {
                        if (EMID == 0) {
                            // Model Browser selection â†’ GameEngine.Mesh.* entity.
                            // The path was captured when "Place in World" was
                            // clicked (ed::PlacementRequest::key), not read back
                            // from the browser here: a rescan between the button
                            // and the click resets selectedModel to -1, and the
                            // old lookup then refused the placement with no other
                            // sign than one log line.
                            const std::string& path = AngelTechEditor.ActiveModelPath;
                            if (!path.empty()) {
                                std::string ext;
                                size_t dot = path.rfind('.');
                                if (dot != std::string::npos) {
                                    ext = path.substr(dot);
                                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                                }
                                MeshObjectNode node;
                                node.meshPath = path;
                                node.skeletal = (ext == ".glb" || ext == ".gltf" || ext == ".iqm");
                                node.position = {AngelTechEditor.X, AngelTechEditor.Y, AngelTechEditor.Z};
                                node.yaw = AngelTechEditor.R;
                                node.scale = AngelTechEditor.S;
                                PawnSystem::Instance().AddMeshObject(node);
                                EditorLog("Placed Mesh.%s '%s'",
                                          node.skeletal ? "Skeletal" : "Static", path.c_str());
                            } else {
                                EditorLog("Place model: select a model in the Model Browser first");
                            }
                        }
                    }
                } else if (g_placeMode == PlaceMode::PICKUP) {
                    PickupNode node;
                    node.position = {AngelTechEditor.X, AngelTechEditor.Y, AngelTechEditor.Z};
                    node.typeName = AngelTechEditor.ActivePickupName;
                    PawnSystem::Instance().AddPickup(node);
                    EditorLog("Placed pickup '%s'", node.typeName.c_str());
                } else if (g_placeMode == PlaceMode::NODE) {
                    if (AngelTechEditor.ActiveNodeType == EditorNodeType::ZONE) {
                        float hw = fmaxf(AngelTechEditor.W, 1) * 0.5f;
                        float hh = fmaxf(AngelTechEditor.H, 1) * 0.5f;
                        float hd = fmaxf(AngelTechEditor.L, 1) * 0.5f;
                        float minX = AngelTechEditor.X - hw, maxX = AngelTechEditor.X + hw;
                        float minY = AngelTechEditor.Y - hh, maxY = AngelTechEditor.Y + hh;
                        float minZ = AngelTechEditor.Z - hd, maxZ = AngelTechEditor.Z + hd;
                        ZoneVolumeNode node;
                        node.bounds = {{minX, minY, minZ}, {maxX, maxY, maxZ}};
                        node.zoneType = ZoneType::ZONE_WATER;
                        node.intensity = 1.0f;
                        ZoneManager::Instance().AddZone(node);
                    } else if (AngelTechEditor.ActiveNodeType == EditorNodeType::PORTAL) {
                        float hw = fmaxf(AngelTechEditor.W, 1) * 0.5f;
                        float hh = fmaxf(AngelTechEditor.H, 1) * 0.5f;
                        float hd = fmaxf(AngelTechEditor.L, 1) * 0.5f;
                        float minX = AngelTechEditor.X - hw, maxX = AngelTechEditor.X + hw;
                        float minY = AngelTechEditor.Y - hh, maxY = AngelTechEditor.Y + hh;
                        float minZ = AngelTechEditor.Z - hd, maxZ = AngelTechEditor.Z + hd;
                        std::string target(g_editorPanels.portalTargetWorld);
                        if (target.empty()) target = "EngineTest";
                        ZonePortal portal;
                        portal.bounds = {{minX, minY, minZ}, {maxX, maxY, maxZ}};
                        portal.targetWorld = target;
                        portal.targetSpawn = {0, 20, 0};
                        portal.bidirectional = true;
                        ZoneManager::Instance().AddPortal(portal);
                        RefreshLevelList();
                    } else if (AngelTechEditor.ActiveNodeType == EditorNodeType::SPAWN) {
                        PlayerStartNode node;
                        node.position = {AngelTechEditor.X, AngelTechEditor.Y, AngelTechEditor.Z};
                        node.yaw = AngelTechEditor.R;
                        PawnSystem::Instance().AddPlayerStart(node);
                    } else if (AngelTechEditor.ActiveNodeType == EditorNodeType::NPC) {
                        PawnSystem::Instance().Spawn(
                            {AngelTechEditor.X, AngelTechEditor.Y, AngelTechEditor.Z}, "Walker");
                    } else if (AngelTechEditor.ActiveNodeType == EditorNodeType::LIGHT) {
                        // The Enter-key / double-click placement path. It used to
                        // AddLight a bare default-constructed LightNode, which has
                        // FOUR problems:
                        //   * active defaults to FALSE, so the light was stored,
                        //     listed in the WorldGraph, exported to World.ozone, and
                        //     lit NOTHING until the level was reloaded from disk
                        //     (the loader sets active = true).
                        //   * no name, so the .ozls defaults layer never ran and the
                        //     Entity Properties panel showed no def section.
                        //   * a directional light placed at the camera sat AT the
                        //     aim point, so its direction normalized to zero.
                        //   * no zone rebind, so AssignLightZones could not classify
                        //     it.
                        // The event-bus path (Subsystems/Placement.cpp) already did
                        // all four; this now matches it.
                        LightNode node;
                        node.active = true;
                        node.position = {AngelTechEditor.X, AngelTechEditor.Y, AngelTechEditor.Z};
                        node.target = {0.0f, 0.0f, 0.0f};
                        node.color = WHITE;
                        node.intensity = 1.0f;
                        node.radius = 20.0f;
                        node.type = LitLightType::POINT;
                        ApplyLightDefDefaultsToNode(node);
                        PawnSystem::Instance().AddLight(node);
                        PawnSystem::Instance().AssignLightZones();
                    }
                }

AngelTechEditor.DrawModel = false;
                // Placement is one-shot: the ghost is spent, so a later click must
                // go back to selecting instead of placing another copy. The path is
                // dropped too, so nothing can redraw the ghost from a stale arm.
                g_placementArmed = false;
                AngelTechEditor.ActiveModelPath.clear();
                // This whole block bypasses the event bus and mutates PawnSystem /
                // ZoneManager directly, so it has to flag the WorldGraph itself.
                MarkWorldGraphDirty();

                // CSG: register brush renderable for OZONE primitives (EMID >= 200).
                // Collision comes from OzoneLoader::RebuildCollisionVolumes(), which
                // runs its own CsgProcessor over every renderable's stored csgOp.
                if (EMID >= 200 && g_placeMode == PlaceMode::MODEL) {
                    int primType = EMID - 200;
                    Vector3 center = {AngelTechEditor.X, AngelTechEditor.Y, AngelTechEditor.Z};
                    Vector3 size = {AngelTechEditor.W, AngelTechEditor.H, AngelTechEditor.L};
                    int ridx = CommitBrushRenderable(
                        primType, center, size, AngelTechEditor.R, AngelTechEditor.S,
                        (int)AngelTechEditor.CSGOperation);
                    if (ridx >= 0) {
                        EditorLog("Brush renderable added idx=%d prim=%d", ridx, primType);
                        // Auto-apply preselected texture to new brush
                        if (!g_editorPanels.activeTexturePath.empty()) {
                            OzoneLoader::Instance().ApplyRenderableTexture(
                                ridx, g_editorPanels.activeTexturePath.c_str());
                            EditorLog("Auto-applied texture to new brush %d: %s", ridx,
                                      g_editorPanels.activeTexturePath.c_str());
                        }
                    }
                }

            }

            // Gizmo drag has priority over every wheel/ghost shortcut.
            if (Gizmo_IsDragging()) {
                Gizmo_UpdateDrag(GetMousePosition(), OTEditor.MainCamera,
                                 g_editorPanels.currentToolMode);
            } else if (g_editorPanels.currentToolMode == 2) {
                float wheel = GetMouseWheelMove();
                if (wheel != 0) AngelTechEditor.S += wheel * 0.25f;
            } else if (g_editorPanels.currentToolMode == 3) {
                float wheel = GetMouseWheelMove();
                if (wheel != 0) AngelTechEditor.R += wheel * 15.0f;
            } else if (IsMouseButtonDown(0) && !Gizmo_IsUnrealDragging()) {
                // A selected entity moves only for an intentional gesture; a
                // placement ghost (no selection) still drags freely. Suppressed
                // entirely while the post-context-menu input guard is active.
                //
                // `!Gizmo_IsUnrealDragging()` is load-bearing: an UnrealEd Ctrl/
                // Shift gesture sets g_gizmoDrag so that the "mesh has no bounds"
                // branch may write, and this fallback then ALSO ran - moving the
                // selection along the camera plane on top of its constrained axis.
                // The result drifted diagonally and never stayed on the axis the
                // modifier named, which is exactly the "axis movement is broken"
                // report. The gesture owns the drag; this must not join in.
                bool allowMove = !g_suppressViewportDrag &&
                    (g_sel.type == SelType::NONE || g_gizmoDrag);
                if (allowMove) {
                    if (g_sel.type != SelType::NONE && !g_gizmoHistPushed) {
                        HistoryPush();
                        g_gizmoHistPushed = true;
                    }
                    // Grid snap applies to the free drag too. The handle drag has
                    // always snapped; this fallback silently did not, which is the
                    // other half of "snap only works occasionally".
                    const float snapStep = Gizmo_SnapStep();
                    // Camera-relative free drag: screen-right/screen-up at the
                    // pivot's depth, so the object tracks the cursor and stays 1:1
                    // on screen at any zoom. ONE path for the ghost and for the
                    // selected entity - the old ghost branch was `X += dx/8;
                    // Y += dy/8`, which ignored the camera entirely and moved the
                    // ghost UP when the mouse went down.
                    const Vector3 fwd = Vector3Normalize(Vector3Subtract(
                        OTEditor.MainCamera.target, OTEditor.MainCamera.position));
                    ozg::Vec3 right, up;
                    if (ozg::ViewBasis({fwd.x, fwd.y, fwd.z}, {0.0f, 1.0f, 0.0f}, right, up)) {
                        const float paneH = g_activeViewportRect.h > 0.0f
                                                ? g_activeViewportRect.h : (float)GetScreenHeight();
                        const float dist = Vector3Distance(
                            OTEditor.MainCamera.position,
                            {AngelTechEditor.X, AngelTechEditor.Y, AngelTechEditor.Z});
                        const float k = edcam::UnitsPerPixel(OTEditor.MainCamera, dist, paneH);
                        const Vector2 d = GetMouseDelta();
                        AngelTechEditor.X += (right.x * d.x - up.x * d.y) * k;
                        AngelTechEditor.Y += (right.y * d.x - up.y * d.y) * k;
                        AngelTechEditor.Z += (right.z * d.x - up.z * d.y) * k;
                    }
                    // An armed ghost used to be moved along world Z by the wheel
                    // (`Z -= wheel*2`), which is a horizontal axis unrelated to the
                    // view - so the wheel did TWO things in that mode and neither
                    // predictably. The wheel now only dollies; drag moves the ghost.
                    if (snapStep > 0.0f) {
                        AngelTechEditor.X = ozg::SnapValue(AngelTechEditor.X, snapStep);
                        AngelTechEditor.Y = ozg::SnapValue(AngelTechEditor.Y, snapStep);
                        AngelTechEditor.Z = ozg::SnapValue(AngelTechEditor.Z, snapStep);
                    }
                }
            }
            if (IsMouseButtonDown(1) && !g_suppressViewportDrag && !Gizmo_IsUnrealDragging()) {
                // Right-drag resizes the placement ghost. Gated off the Unreal
                // gesture for the same reason as the move above: Ctrl+RMB is the
                // world-Z axis move, and this used to resize the selection's
                // bounding box on every one of those drags.
                AngelTechEditor.W += GetMouseDelta().x / 8;
                AngelTechEditor.H += GetMouseDelta().y / 8;
                AngelTechEditor.L -= (GetMouseWheelMove() * 2);
                // Snap the size too, so a resized ghost lands on the grid.
                const float ss = Gizmo_SnapStep();
                if (ss > 0.0f) {
                    AngelTechEditor.W = ozg::SnapValue(AngelTechEditor.W, ss);
                    AngelTechEditor.H = ozg::SnapValue(AngelTechEditor.H, ss);
                    AngelTechEditor.L = ozg::SnapValue(AngelTechEditor.L, ss);
                    if (AngelTechEditor.W < 0.1f) AngelTechEditor.W = 0.1f;
                    if (AngelTechEditor.H < 0.1f) AngelTechEditor.H = 0.1f;
                    if (AngelTechEditor.L < 0.1f) AngelTechEditor.L = 0.1f;
                }
            }
        }

        // Helper lambda: get bounding box for a given selection
        auto GetSelBox = [](const EditorSelection& sel) -> BoundingBox {
            BoundingBox box = {{0,0,0},{0,0,0}};
            if (sel.type == SelType::NPC) {
                auto& pawns = PawnSystem::Instance().GetPawns();
                for (auto& p : pawns) {
                    if ((int)p.id == sel.index && p.active)
                        return {{p.position.x-1,p.position.y-1,p.position.z-1},
                                {p.position.x+1,p.position.y+1,p.position.z+1}};
                }
            } else if (sel.type == SelType::PICKUP) {
                auto& pickups = PawnSystem::Instance().GetPickups();
                for (auto& pk : pickups) {
                    if ((int)pk.id == sel.index && pk.active)
                        return {{pk.position.x-0.6f,pk.position.y-0.3f,pk.position.z-0.6f},
                                {pk.position.x+0.6f,pk.position.y+1.2f,pk.position.z+0.6f}};
                }
            } else if (sel.type == SelType::BRUSH) {
                // `sel.index` is a RENDERABLE index, so it must be read against
                // the renderable. This used to index GetCollisionVolumes(), which
                // is a different index space (the CSG OUTPUT, regenerated on every
                // rebuild), so the hover/selection box belonged to an unrelated
                // volume or to nothing. SnapGizmoToSelection documents the same
                // trap and was fixed; this consumer was missed.
                OzoneRenderable* r = OzoneLoader::Instance().Get(sel.index);
                if (r && r->loaded && r->model.meshCount > 0) {
                    // The one rotation-aware brush box (same as the picker/gizmo).
                    return BrushWorldBox(*r);
                }
            } else if (sel.type == SelType::ZONE) {
                auto& zones = ZoneManager::Instance().GetZones();
                for (auto& z : zones) {
                    if ((int)z.id == sel.index)
                        return z.bounds;
                }
            } else if (sel.type == SelType::LIGHT) {
                LightNode* l = PawnSystem::Instance().GetLight(sel.index);
                if (l)
                    return {{l->position.x-0.5f,l->position.y-0.5f,l->position.z-0.5f},
                            {l->position.x+0.5f,l->position.y+1.0f,l->position.z+0.5f}};
            } else if (sel.type == SelType::SPAWN) {
                if (PlayerStartNode* s = FindPlayerStartById(sel.index))
                    return {{s->position.x-0.6f,s->position.y-0.5f,s->position.z-0.6f},
                            {s->position.x+0.6f,s->position.y+1.2f,s->position.z+0.6f}};
            } else if (sel.type == SelType::PORTAL) {
                auto& portals = ZoneManager::Instance().GetPortals();
                if (sel.index >= 0 && sel.index < (int)portals.size())
                    return portals[sel.index].bounds;
            } else if (sel.type == SelType::MESH) {
                MeshObjectNode* n = PawnSystem::Instance().GetMeshObject(sel.index);
                if (n) {
                    if (n->mesh && n->mesh->Valid()) {
                        const BoundingBox& mb = n->mesh->Bounds();
                        const AabbBox local{mb.min.x, mb.min.y, mb.min.z,
                                            mb.max.x, mb.max.y, mb.max.z};
                        const AabbBox w = RotatedAabbY(local, n->yaw * DEG2RAD,
                                                       n->position.x, n->position.y,
                                                       n->position.z, n->scale);
                        return {{w.minX, w.minY, w.minZ}, {w.maxX, w.maxY, w.maxZ}};
                    }
                    return {{n->position.x - 1, n->position.y - 1, n->position.z - 1},
                            {n->position.x + 1, n->position.y + 1, n->position.z + 1}};
                }
            } else if (sel.type == SelType::MODEL && sel.index >= 0 && sel.index < CachedModelCounter) {
                int mid = CachedModels[sel.index].ModelId;
                LoadedModel* lm = WDLModels.GetModelByWDLId(mid);
                if (lm && lm->loaded) {
                    BoundingBox mb = GetMeshBoundingBox(lm->model.meshes[0]);
                    float sx = CachedModels[sel.index].S;
                    Vector3 pos = {CachedModels[sel.index].X, CachedModels[sel.index].Y, CachedModels[sel.index].Z};
                    mb.min = Vector3Add(Vector3Scale(mb.min, sx), pos);
                    mb.max = Vector3Add(Vector3Scale(mb.max, sx), pos);
                    return mb;
                }
            }
            return box;
        };

        // Hover highlight (yellow)
        if (g_hoverSel.type != SelType::NONE &&
            !(g_hoverSel.type == g_sel.type && g_hoverSel.index == g_sel.index)) {
            BoundingBox hb = GetSelBox(g_hoverSel);
            if (hb.min.x != hb.max.x || hb.min.y != hb.max.y) {
                float hp = 0.5f + 0.5f * sinf(GetTime() * 4);
                DrawBoundingBox(hb, (Color){255,255,(unsigned char)(100*hp),255});
            }
        }

        // Selection highlight (red)
        if (g_sel.type != SelType::NONE) {
            BoundingBox sb = GetSelBox(g_sel);
            if (sb.min.x != sb.max.x || sb.min.y != sb.max.y) {
                float sp = 0.5f + 0.5f * sinf(GetTime() * 4);
                DrawBoundingBox(sb, (Color){255,(unsigned char)(80*sp),(unsigned char)(80*sp),255});
            }
        }
        // Multi-selection: outline every ADDITIONAL member (orange) so the set
        // that Delete / Ctrl+C act on is visible, not just the primary.
        if (g_selSet.size() > 1) {
            for (const auto& s : g_selSet) {
                if (s.type == g_sel.type && s.index == g_sel.index) continue;
                BoundingBox mb = GetSelBox(s);
                if (mb.min.x != mb.max.x || mb.min.y != mb.max.y)
                    DrawBoundingBox(mb, (Color){255, 150, 80, 255});
            }
        }

        // Transform gizmo for the current selection. Drawn AFTER the highlight so
        // the handles are never hidden by the selection box.
        DrawTransformGizmo(OTEditor.MainCamera, g_editorPanels.currentToolMode);

        // Terrain-following camera
        {
            float gy = SampleHeightmapGroundY(OTEditor.MainCamera.position.x, OTEditor.MainCamera.position.z);
            // Don't fight a camera-follow drag (Shift+LMB): the clamp would undo
            // the vertical part of the gesture every frame.
            if (gy > -50000.0f && !Gizmo_IsUnrealDragging())
                OTEditor.MainCamera.position.y = gy + 2.0f;
        }

        EndMode3D();
        if (vpFbo) EndTextureMode(); else EndScissorMode();

        // Commit this frame's camera edits (navigation, terrain-follow) back to the
        // active pane so they persist when another pane becomes active.
        if (g_editorPanels.viewActive >= 0 && g_editorPanels.viewActive < 4)
            g_paneCams[g_editorPanels.viewActive] = OTEditor.MainCamera;

        // --- Secondary panes (Phase 2): every pane EXCEPT the active one is drawn
        // read-only from its own stored camera. The active pane above carries the
        // gizmo/overlays and owns input, so the editing surface is whichever pane
        // the user last clicked.
        for (size_t pi = 0; pi < vpRects.size(); pi++) {
            if ((int)pi == g_editorPanels.viewActive) continue;
            OZ_PROFILE_SCOPE("ed.scene.pane");
            const bool fb = PaneIsDetached((int)pi);
            if (fb) DetachedPane_EnsureRT((int)pi);
            const ed::viewport::VpRect r = fb ? PaneRenderRect((int)pi, vpRects) : vpRects[pi];
            Camera3D cam = g_paneCams[pi];
            if (fb) {
                BeginTextureMode(g_paneTargets[pi].rt);
                ClearBackground(BLACK);
            } else {
                BeginScissorMode((int)r.x, (int)r.y, (int)r.w, (int)r.h);
                SetPaneViewport(r);
            }
            BeginMode3D(cam);
            SetPaneProjection(cam, r);
            if (wantSky && oz::diag::PerfOn("sky")) {
                oz::Skybox::Instance().Draw(
                    cam.position, s_skyTex, Texture2D{0},
                    /*cullBackfaces=*/OTEditor.ViewMode != LightingMode::WIREFRAME,
                    &skyFog);
            }
            if (oz::diag::PerfOn("grid")) DrawGrid(1000, 10.0f);
            OzoneLoader::Instance().SetCullAspect(r.h > 0.0f ? r.w / r.h : 0.0f);
            OzoneLoader::Instance().Draw(cam,
                                         OTEditor.ViewMode != LightingMode::WIREFRAME);
            if (!g_editorPanels.perfPaneFast) {
                Shader bbShader = {0};
                if (OTEditor.ViewMode == LightingMode::LIT) bbShader = OTEditor.LitFogShader;
                else if (OTEditor.ViewMode == LightingMode::UNLIT) bbShader = OTEditor.UnlitShader;
                PawnSystem::Instance().DrawAll(cam, bbShader);
                PawnSystem::Instance().DrawEntities(cam, bbShader);
            }
            EndMode3D();
            if (fb) EndTextureMode(); else EndScissorMode();
        }

        // Present every popped-out pane into its own window (Tier A: GPU readback +
        // GDI blit, no second GL context).
        {
            OZ_PROFILE_SCOPE("ed.present");
            for (int i = 0; i < 4; i++)
                if (g_paneTargets[i].detached) DetachedPane_Present(i);
        }

        // Restore the full-window viewport for the 2D pass. A pane viewport left
        // active would draw the toolbar and every panel into that sub-rect.
        rlViewport(0, 0, GetScreenWidth(), GetScreenHeight());

        // Reset lighting mode state (always restore defaults)
        rlDrawRenderBatchActive();
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glEnable(GL_CULL_FACE);

        BeginDrawing();
        // Multi-viewport pane chrome: a divider per pane and a brighter edge on
        // the active (input) pane, so the read-only reference panes read as such.
        // A popped-out pane's slot is a dark "Detached" card instead of a divider.
        if ((g_editorPanels.viewLayout != 0 || PaneIsDetached(g_editorPanels.viewActive)) &&
            !g_editorPanels.viewMaximized) {
            std::vector<ed::viewport::VpRect> pr;
            EditorViewportRectsNow(pr);
            for (size_t i = 0; i < pr.size(); i++) {
                const bool detached = PaneIsDetached((int)i);
                const bool active = ((int)i == g_editorPanels.viewActive);
                if (detached) {
                    DrawRectangle((int)pr[i].x, (int)pr[i].y, (int)pr[i].w, (int)pr[i].h,
                                  (Color){24, 24, 28, 255});
                    const char* txt = "Detached viewport";
                    int tw2 = MeasureText(txt, 14);
                    DrawText(txt, (int)(pr[i].x + (pr[i].w - tw2) * 0.5f),
                             (int)(pr[i].y + pr[i].h * 0.5f - 7), 14, LIGHTGRAY);
                }
                const Color edge = active ? (Color){90, 120, 170, 255}
                                          : (Color){60, 60, 70, 255};
                DrawRectangleLinesEx({pr[i].x, pr[i].y, pr[i].w, pr[i].h}, 1.0f, edge);
            }
        }
        // Top toolbar with icons
        {
            int tw = GetScreenWidth();
            // TWO rows: the controls no longer fit on one line at the window's
            // minimum width, and a toolbar that silently clips "Snap"/"Unreal"
            // when the user shrinks the window is worse than two rows.
            const int tbH = kEditorToolbarHeight;
            const int rowH = 24;
            int by = 2;                       // current row's y
            DrawRectangle(0, 0, tw, tbH, (Color){35, 35, 40, 220});
            DrawLine(0, tbH, tw, tbH, (Color){60, 60, 70, 255});

            auto& ico = EditorIcons::Instance();
            int bx = 4, is = 20;

            auto tBtn = [&](const char* icon, const char* label, int cmd, bool active = false) {
                int lw = (label ? (int)strlen(label) * 7 + 4 : 0);
                int bw = (icon ? 24 : 0) + lw + 4;
                Rectangle r = {(float)bx, (float)by, (float)bw, (float)rowH};
                bool hover = CheckCollisionPointRec(GetMousePosition(), r);
                DrawRectangleRec(r, active ? (Color){70,90,120,255}
                                           : hover ? (Color){55,55,65,255}
                                                   : (Color){45,45,50,255});
                if (icon && ico.Has(icon)) ico.Draw(icon, bx+2, by+2, is, WHITE);
                if (label) DrawText(label, bx+(icon?26:4), by+5, 12, active ? WHITE : LIGHTGRAY);
                if (hover && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                    if (cmd==0) ToggleModelBrowser(); else if(cmd==1) ToggleSoundMgr();
                    else if(cmd==2) ToggleTextureMgr(); else if(cmd==3) TogglePawnMgr();
                    else if(cmd==4) ToggleScriptMgr();
                    // cmd 5 is the toolbar "Zone" button: placement only. It used
                    // to also toggle the Zone Properties window.
                    else if(cmd==5) ToggleZonePlacement();
                    else if(cmd==6) TogglePickupPanel(); else if(cmd==7) ToggleNodePanel();
                    else if(cmd==8) ToggleHeightmapEditor(); else if(cmd==9) ToggleAnimPanel();
                    else if(cmd==10) { FileNew(); } else if(cmd==11) { FileOpen(); }
                    else if(cmd==12) { FileSaveAs(); }
                    else if(cmd==13) { ToggleWorldGraph(); }
                }
                bx += bw + 2;
            };

            tBtn(nullptr,"New",10); tBtn(nullptr,"Open",11); tBtn(nullptr,"Save",12);
            tBtn(nullptr,"Anim",9);
            // Play button
            {
                const char* lbl = "Play";
                int bw = (int)strlen(lbl) * 7 + 10;
                Rectangle r = {(float)bx, (float)by, (float)bw, (float)rowH};
                bool hover = CheckCollisionPointRec(GetMousePosition(), r);
                DrawRectangleRec(r, hover ? (Color){50,100,50,255} : (Color){35,80,35,255});
                DrawText(lbl, bx + 4, by+5, 12, WHITE);
                if (hover && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                    PlayTestCurrentWorld();
                }
                bx += bw + 2;
            }
            bx += 6;
            tBtn("BBGeneric","Mod",0); tBtn(nullptr,"Snd",1); tBtn(nullptr,"Tex",2);
            tBtn(nullptr,"Pawn",3); tBtn(nullptr,"Scr",4); bx += 6;

            for (int li=0;li<3;li++) {
                const char* labs[]={"Lit","Unlit","Wire"};
                int lw=38; Color lc=(int)OTEditor.ViewMode==li?(Color){70,90,120,255}:(Color){45,45,50,255};
                DrawRectangle(bx,by,lw,rowH,lc);
                DrawText(labs[li],bx+4,by+5,12,(int)OTEditor.ViewMode==li?WHITE:LIGHTGRAY);
                if(CheckCollisionPointRec(GetMousePosition(),{(float)bx,(float)by,(float)lw,(float)rowH})&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                    LightingMode prev = OTEditor.ViewMode;
                    OTEditor.ViewMode = (LightingMode)li;
                    if (prev != OTEditor.ViewMode) {
                        Shader targetShader;
                        if (OTEditor.ViewMode == LightingMode::LIT || OTEditor.ViewMode == LightingMode::WIREFRAME)
                            targetShader = OTEditor.LitFogShader;
                        else
                            targetShader = OTEditor.UnlitShader;
                        if (targetShader.id > 0) {
                            for (auto& m : WDLModels.models)
                                if (m.loaded && m.model.meshCount > 0)
                                    m.model.materials[0].shader = targetShader;
                            if (WDLModels.HeightMapReady)
                                WDLModels.HeightMap.materials[0].shader = targetShader;
                            OzoneLoader::Instance().SetLitFogShader(targetShader);
                        }
                    }
                }
                bx+=lw+2;
            }
            // Skybox visibility toggle
            {
                int lw = 42;
                bool on = OTEditor.ShowSkybox;
                Color lc = on ? (Color){70,90,120,255} : (Color){45,45,50,255};
                DrawRectangle(bx, by, lw, rowH, lc);
                DrawText("Sky", bx+6, by+5, 12, on ? WHITE : LIGHTGRAY);
                if (CheckCollisionPointRec(GetMousePosition(), {(float)bx, (float)by, (float)lw, (float)rowH}) &&
                    IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                    OTEditor.ShowSkybox = !OTEditor.ShowSkybox;
                    EditorLog("Viewport skybox %s", OTEditor.ShowSkybox ? "shown" : "hidden");
                }
                bx += lw + 2;
            }
            bx+=6;
            tBtn("AddVolume","Zone",5); tBtn("ModeCamera","Node",7);
            tBtn("PolyTexInfo","Pickup",6);
            // Scene graph / World Hierarchy. It used to be reachable only from the
            // View menu, which is why it read as a hidden window rather than a tool.
            tBtn("AddMover","Hier",13, g_editorPanels.showWorldGraph);

            // ---- row 2: terrain / snap / Unreal-style input --------------------
            bx = 4;
            by = 2 + rowH + 4;
            // Terrain editing buttons
            bool terrainMode = (g_placeMode == PlaceMode::TERRAIN);
            Color terrainColor = terrainMode ? (Color){90,70,50,255} : (Color){45,45,50,255};
            int tbW = 76;
            DrawRectangle(bx, by, tbW, rowH, terrainColor);
            if (ico.Has("BBTerrain")) ico.Draw("BBTerrain", bx+3, by+2, 20, WHITE);
            DrawText("Raise", bx+27, by+5, 12, terrainMode ? WHITE : LIGHTGRAY);
            if (CheckCollisionPointRec(GetMousePosition(), {(float)bx, (float)by, (float)tbW, (float)rowH}) && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                g_placeMode = PlaceMode::TERRAIN;
                g_editorPanels.terrainBrushMode = 0; // raise
            }
            bx += tbW + 2;
            DrawRectangle(bx, by, tbW, rowH, terrainColor);
            if (ico.Has("BBTerrain")) ico.Draw("BBTerrain", bx+3, by+2, 20, WHITE);
            DrawText("Lower", bx+27, by+5, 12, terrainMode ? WHITE : LIGHTGRAY);
            if (CheckCollisionPointRec(GetMousePosition(), {(float)bx, (float)by, (float)tbW, (float)rowH}) && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                g_placeMode = PlaceMode::TERRAIN;
                g_editorPanels.terrainBrushMode = 1; // lower
            }
            bx += tbW + 2;
            // Brush size adjustment buttons
            char sizeTxt[32];
            snprintf(sizeTxt, sizeof(sizeTxt), "Sz:%d", g_editorPanels.terrainBrushSize);
            DrawText(sizeTxt, bx+4, by+5, 12, LIGHTGRAY);
            int szW = (int)strlen(sizeTxt) * 7 + 10;
            Rectangle szR = {(float)bx, (float)by, (float)szW, (float)rowH};
            if (CheckCollisionPointRec(GetMousePosition(), szR)) {
                if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
                    g_editorPanels.terrainBrushSize = (g_editorPanels.terrainBrushSize % 10) + 1;
            }
            bx += szW + 2;
            // Brush strength
            char strTxt[32];
            snprintf(strTxt, sizeof(strTxt), "Str:%.1f", g_editorPanels.terrainBrushStrength);
            DrawText(strTxt, bx+4, by+5, 12, LIGHTGRAY);
            int stW = (int)strlen(strTxt) * 7 + 10;
            Rectangle stR = {(float)bx, (float)by, (float)stW, (float)rowH};
            if (CheckCollisionPointRec(GetMousePosition(), stR)) {
                if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                    static float strengths[] = {0.01f, 0.03f, 0.05f, 0.1f, 0.2f};
                    static int si = 2;
                    si = (si + 1) % 5;
                    g_editorPanels.terrainBrushStrength = strengths[si];
                }
            }
            bx += stW + 2;

            // Gizmo snap. Discoverable here rather than hidden behind a modifier:
            // clicking cycles Off / 0.25 / 0.5 / 1 / 2 and the value persists in
            // [Options] gizmo_snap. Held Ctrl = snap at this step (0.5 when Off);
            // held Shift = fine (a tenth of it). Ctrl/Shift MODIFY the step now
            // instead of replacing it, so a configured 0.25 stays 0.25.
            {
                const float steps[] = {0.0f, 0.25f, 0.5f, 1.0f, 2.0f};
                char snapTxt[32];
                if (g_editorPanels.gizmoSnap <= 0.0f)
                    snprintf(snapTxt, sizeof(snapTxt), "Snap:Off");
                else
                    snprintf(snapTxt, sizeof(snapTxt), "Snap:%.2g", g_editorPanels.gizmoSnap);
                int sw = (int)strlen(snapTxt) * 7 + 10;
                bool on = g_editorPanels.gizmoSnap > 0.0f;
                DrawRectangle(bx, by, sw, rowH, on ? (Color){70,90,120,255} : (Color){45,45,50,255});
                DrawText(snapTxt, bx + 4, by+5, 12, on ? WHITE : LIGHTGRAY);
                if (CheckCollisionPointRec(GetMousePosition(),
                                           {(float)bx, (float)by, (float)sw, (float)rowH}) &&
                    IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                    int cur = 0;
                    for (int i = 0; i < 5; i++)
                        if (fabsf(steps[i] - g_editorPanels.gizmoSnap) < 0.001f) cur = i;
                    g_editorPanels.gizmoSnap = steps[(cur + 1) % 5];
                }
                bx += sw + 2;
            }

            // UnrealEd-style brush editing. Same state the Settings menu edits,
            // so the button and the check mark can never disagree. The speed chip
            // appears only while it is on; 1/2/3 also set it (UnrealEd convention).
            {
                const bool on = g_editorPanels.unrealBrushMove;
                const char* lbl = on ? "Unreal:On" : "Unreal:Off";
                const int uw = (int)strlen(lbl) * 7 + 10;
                DrawRectangle(bx, by, uw, rowH,
                              on ? (Color){70, 90, 120, 255} : (Color){45, 45, 50, 255});
                DrawText(lbl, bx + 4, by+5, 12, on ? WHITE : LIGHTGRAY);
                if (CheckCollisionPointRec(GetMousePosition(),
                                           {(float)bx, (float)by, (float)uw, (float)rowH}) &&
                    IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                    g_editorPanels.unrealBrushMove = !g_editorPanels.unrealBrushMove;
                    Editor_SyncUnrealMenuChecks();
                    EditorLog("Unreal-style brush movement %s (Ctrl+LMB=X, Ctrl+RMB=Z, "
                              "Ctrl+both=Y, Shift+LMB=move on view plane)",
                              g_editorPanels.unrealBrushMove ? "on" : "off");
                }
                bx += uw + 2;
                if (on) {
                    char spTxt[32];
                    static const char* kSpd[] = {"Slow", "Med", "Fast"};
                    int spd = g_editorPanels.unrealMoveSpeed;
                    if (spd < 0 || spd > 2) spd = 1;
                    snprintf(spTxt, sizeof(spTxt), "%s", kSpd[spd]);
                    const int spW = (int)strlen(spTxt) * 7 + 10;
                    DrawRectangle(bx, by, spW, rowH, (Color){60, 70, 90, 255});
                    DrawText(spTxt, bx + 4, by+5, 12, WHITE);
                    if (CheckCollisionPointRec(GetMousePosition(),
                                               {(float)bx, (float)by, (float)spW, (float)rowH}) &&
                        IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                        g_editorPanels.unrealMoveSpeed =
                            (g_editorPanels.unrealMoveSpeed + 1) % 3;
                        Editor_SyncUnrealMenuChecks();
                    }
                    bx += spW + 2;
                }
            }
            // View presets. Row 2, not row 1: row 1 is already at its width limit,
            // and these are viewport chrome rather than document tools. The active
            // chip is DERIVED from the camera (CurrentViewPreset), so the Camera
            // menu, the NumPad keys and these buttons cannot disagree about which
            // view is being shown - a stored flag would be a second source of truth.
            bx += 6;
            {
                struct PresetChip { const char* label; ViewPresetKind kind; };
                static const PresetChip kChips[] = {
                    {"Top",   ViewPresetKind::Top},
                    {"Side",  ViewPresetKind::Side},
                    {"Down",  ViewPresetKind::Bottom},
                    {"Persp", ViewPresetKind::Perspective},
                };
                const ViewPresetKind cur = CurrentViewPreset();
                for (const auto& chip : kChips) {
                    const int cw = (int)strlen(chip.label) * 7 + 10;
                    const bool on = (cur == chip.kind);
                    DrawRectangle(bx, by, cw, rowH,
                                  on ? (Color){70, 90, 120, 255} : (Color){45, 45, 50, 255});
                    DrawText(chip.label, bx + 4, by + 5, 12, on ? WHITE : LIGHTGRAY);
                    if (CheckCollisionPointRec(GetMousePosition(),
                                               {(float)bx, (float)by, (float)cw, (float)rowH}) &&
                        IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                        switch (chip.kind) {
                            case ViewPresetKind::Top:    ViewTop(); break;
                            case ViewPresetKind::Bottom: ViewBottom(); break;
                            case ViewPresetKind::Side:   ViewRight(); break;
                            case ViewPresetKind::Perspective: SetViewPerspective(); break;
                        }
                    }
                    bx += cw + 2;
                }

                // Snap tools. "Side" pushes along the VIEW's right axis (snapped to
                // the nearest cardinal), so it is the sibling of whatever you are
                // looking through rather than a fixed world direction.
                bx += 6;
                {
                    const bool canSnap = g_sel.type != SelType::NONE &&
                                         g_sel.type != SelType::MAP;
                    struct SnapChip { const char* label; bool ground; };
                    static const SnapChip kSnapChips[] = {
                        {"Ground", true},
                        {"Side",   false},
                    };
                    for (const auto& sc : kSnapChips) {
                        const int cw = (int)strlen(sc.label) * 7 + 10;
                        const Color bg = !canSnap ? (Color){38, 38, 42, 255}
                                                   : (Color){45, 45, 50, 255};
                        DrawRectangle(bx, by, cw, rowH, bg);
                        DrawText(sc.label, bx + 4, by + 5, 12,
                                 canSnap ? LIGHTGRAY : (Color){90, 90, 95, 255});
                        if (canSnap && CheckCollisionPointRec(
                                GetMousePosition(),
                                {(float)bx, (float)by, (float)cw, (float)rowH}) &&
                            IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                            if (sc.ground) SnapSelectionToGround();
                            else           SnapSelectionToSide();
                        }
                        bx += cw + 2;
                    }
                }
                // Viewport layout: 1x1 -> 1x2 -> 2x1 -> 2x2. Every pane has its own
                // camera and its own Top/Side/Down/Persp preset state now, so a
                // split layout can be Top + Front + Side + Persp like UnrealEd.
                bx += 6;
                static const char* kLayoutLabels[] = {"View:1", "View:2V", "View:2H", "View:4"};
                int lay = g_editorPanels.viewLayout;
                if (lay < 0 || lay > 3) lay = 0;
                {
                    const char* lbl = kLayoutLabels[lay];
                    const int lw = (int)strlen(lbl) * 7 + 10;
                    const bool lon = lay != 0;
                    DrawRectangle(bx, by, lw, rowH,
                                  lon ? (Color){70, 90, 120, 255} : (Color){45, 45, 50, 255});
                    DrawText(lbl, bx + 4, by + 5, 12, lon ? WHITE : LIGHTGRAY);
                    if (CheckCollisionPointRec(GetMousePosition(),
                                               {(float)bx, (float)by, (float)lw, (float)rowH}) &&
                        IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                        g_editorPanels.viewLayout = (lay + 1) % 4;
                    }
                    bx += lw + 2;
                }
                // Maximize the active pane (keeps the layout so it can be restored).
                {
                    const char* lbl = g_editorPanels.viewMaximized ? "Max:On" : "Max:Off";
                    const int mw = (int)strlen(lbl) * 7 + 10;
                    const bool mon = g_editorPanels.viewMaximized;
                    DrawRectangle(bx, by, mw, rowH,
                                  mon ? (Color){70, 90, 120, 255} : (Color){45, 45, 50, 255});
                    DrawText(lbl, bx + 4, by + 5, 12, mon ? WHITE : LIGHTGRAY);
                    if (CheckCollisionPointRec(GetMousePosition(),
                                               {(float)bx, (float)by, (float)mw, (float)rowH}) &&
                        IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                        g_editorPanels.viewMaximized = !g_editorPanels.viewMaximized;
                    }
                    bx += mw + 2;
                }
                // Pop the active pane out into its own window (Tier A). The main
                // window keeps its slot, which draws a "Detached" card.
                {
                    const int a = g_editorPanels.viewActive;
                    const bool don = (a >= 0 && a < 4) && g_paneTargets[a].detached;
                    const char* lbl = don ? "Pop:On" : "Pop:Off";
                    const int pw = (int)strlen(lbl) * 7 + 10;
                    DrawRectangle(bx, by, pw, rowH,
                                  don ? (Color){70, 90, 120, 255} : (Color){45, 45, 50, 255});
                    DrawText(lbl, bx + 4, by + 5, 12, don ? WHITE : LIGHTGRAY);
                    if (CheckCollisionPointRec(GetMousePosition(),
                                               {(float)bx, (float)by, (float)pw, (float)rowH}) &&
                        IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && a >= 0 && a < 4) {
                        if (g_paneTargets[a].detached) {
                            DetachedPane_Attach(a);
                        } else {
                            std::vector<ed::viewport::VpRect> rr;
                            EditorViewportRectsNow(rr);
                            const ed::viewport::VpRect r = Editor_ActiveRect(rr);
                            DetachedPane_Detach(a, (int)r.w, (int)r.h);
                        }
                    }
                    bx += pw + 2;
                }

                // FrameProfiler overlay. The client toggles it from its top menu
                // bar / F9; the editor has no free function key (F8-F10 are panel
                // toggles), so this toolbar chip is the entry point. Collection is
                // always on; this only shows/hides the panel.
                bx += 8;
                {
                    const char* lbl = g_editorPanels.perfOverlay ? "Perf:On" : "Perf:Off";
                    const int pw = (int)strlen(lbl) * 7 + 10;
                    const bool on = g_editorPanels.perfOverlay;
                    DrawRectangle(bx, by, pw, rowH,
                                  on ? (Color){70, 90, 120, 255} : (Color){45, 45, 50, 255});
                    DrawText(lbl, bx + 4, by + 5, 12, on ? WHITE : LIGHTGRAY);
                    if (CheckCollisionPointRec(GetMousePosition(),
                                               {(float)bx, (float)by, (float)pw, (float)rowH}) &&
                        IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                        g_editorPanels.perfOverlay = !g_editorPanels.perfOverlay;
                        EditorLog("Perf overlay %s", g_editorPanels.perfOverlay ? "ON" : "OFF");
                    }
                    bx += pw + 2;

                    if (on) {
                        const char* xl = g_editorPanels.perfOverlayExpanded ? "PerfX:On" : "PerfX:Off";
                        const int xw = (int)strlen(xl) * 7 + 10;
                        const bool xon = g_editorPanels.perfOverlayExpanded;
                        DrawRectangle(bx, by, xw, rowH,
                                      xon ? (Color){70, 90, 120, 255} : (Color){45, 45, 50, 255});
                        DrawText(xl, bx + 4, by + 5, 12, xon ? WHITE : LIGHTGRAY);
                        if (CheckCollisionPointRec(GetMousePosition(),
                                                   {(float)bx, (float)by, (float)xw, (float)rowH}) &&
                            IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                            g_editorPanels.perfOverlayExpanded = !g_editorPanels.perfOverlayExpanded;
                        }
                        bx += xw + 2;
                    }

                    // Secondary panes: level geometry only (Fast) or the full
                    // prop/NPC set (Full). Fast is the default because a 2x2
                    // layout otherwise pays the entity draw cost four times.
                    const char* pl = g_editorPanels.perfPaneFast ? "Panes:Fast" : "Panes:Full";
                    const int pw2 = (int)strlen(pl) * 7 + 10;
                    DrawRectangle(bx, by, pw2, rowH,
                                  g_editorPanels.perfPaneFast ? (Color){70, 90, 120, 255}
                                                              : (Color){45, 45, 50, 255});
                    DrawText(pl, bx + 4, by + 5, 12,
                             g_editorPanels.perfPaneFast ? WHITE : LIGHTGRAY);
                    if (CheckCollisionPointRec(GetMousePosition(),
                                               {(float)bx, (float)by, (float)pw2, (float)rowH}) &&
                        IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                        g_editorPanels.perfPaneFast = !g_editorPanels.perfPaneFast;
                        EditorLog("Secondary panes: %s",
                                  g_editorPanels.perfPaneFast ? "geometry only" : "full");
                    }
                    bx += pw2 + 2;
                }
            }
        }
        int sbW = GetStatsSidebarWidth();
        const int tbH = kEditorToolbarHeight;
        LayoutStatsSidebar(GetScreenWidth(), GetScreenHeight(), tbH, sbW);
        {
            const char* modeStr =
                g_placeMode == PlaceMode::MODEL ? "MODEL" :
                g_placeMode == PlaceMode::PICKUP ? "PICKUP" :
                g_placeMode == PlaceMode::NODE ? "NODE" : "ENV";
            int volCount = (int)OzoneLoader::Instance().GetCollisionVolumes().size();
            int chunkCount = OzoneLoader::Instance().GetChunkManager().CellCount();
            // World size (Phase 5.3): the same live containers the viewport draws,
            // so the number cannot disagree with the scene.
            g_worldStats.brushes = OzoneLoader::Instance().Count();
            g_worldStats.meshes = (int)PawnSystem::Instance().GetMeshObjects().size();
            g_worldStats.npcs = 0;
            for (const auto& p : PawnSystem::Instance().GetPawns()) if (p.active) g_worldStats.npcs++;
            g_worldStats.lights = (int)PawnSystem::Instance().GetLights().size();
            g_worldStats.pickups = (int)PawnSystem::Instance().GetPickups().size();
            g_worldStats.pathnodes = (int)PawnSystem::Instance().GetPathNodes().size();
            g_worldStats.windzones = (int)PawnSystem::Instance().GetWindZones().size();
            g_worldStats.doors = (int)PawnSystem::Instance().GetDoors().size();
            UpdateStatsSidebar(
                AngelTechEditor.X, AngelTechEditor.Y, AngelTechEditor.Z,
                AngelTechEditor.W, AngelTechEditor.H, AngelTechEditor.L,
                AngelTechEditor.R, AngelTechEditor.S,
                volCount, chunkCount, modeStr,
                OTEditor.MainCamera.position.x,
                OTEditor.MainCamera.position.y,
                OTEditor.MainCamera.position.z,
                g_worldStats);
        }
        DrawFPS(GetScreenWidth() - 60, kEditorToolbarHeight + 8);
        if (g_editorPanels.perfOverlay) {
            OZ_PROFILE_SCOPE("ed.perfOverlay");
            oz::diag::DrawProfilerOverlay(kEditorToolbarHeight + 8,
                                          GetStatsSidebarWidth() + 8,
                                          g_editorPanels.perfOverlayExpanded);
        }

        // --- Event bus drain + dispatch ---------------------------------------
        //
        // Move the queue out here, BEFORE dispatching: a handler that opens a panel
        // can post further events, and draining while iterating the live queue would
        // let those be dispatched in the same frame.
        //
        // Dispatch is deferred to one point per frame, after the Win32 message pump,
        // because a panel is mid-layout while it posts. What makes that safe is every
        // event carrying its own target captured at post time - not the timing.
        ed::EventBus::instance().drain(g_editorFrameEvents, ed::IsFrameEvent);
        if (!g_editorFrameEvents.empty()) {
            DispatchFrameEvents(g_editorFrameEvents);
            // Placement, delete, duplicate, CSG commit and portal link/delete all
            // arrive here, and every one of them changes what the WorldGraph lists.
            // Gated on non-empty: marking unconditionally rebuilt the list every frame.
            MarkWorldGraphDirty();
        }

        // --- Surface Properties apply / reset ------------------------------
        //
        // Drained separately from the frame pass because these handlers always were
        // their own group, and that separation is what let Subsystems/SurfaceOps.cpp
        // happen without reordering the selection dispatch.
        ed::EventBus::instance().drain(g_editorSurfaceEvents, ed::IsSurfaceEvent);
        if (!g_editorSurfaceEvents.empty()) {
            ApplySurfaceEdits(g_editorSurfaceEvents);
            // Core calling into the UI layer is the legal direction. It used to be
            // SurfaceOps calling SurfacePropsRefresh itself, twice.
            SurfacePropsRefresh((HWND)g_editorPanels.hSurfaceProps);
            // A surface apply can commit a brush, which rebuilds the collision volumes
            // the WorldGraph lists as Brush rows.
            MarkWorldGraphDirty();
        }

        // --- Properties apply -------------------------------------------------
        //
        // ed::Ev::ApplyProperties, carrying the SelRef the panel captured when Apply
        // was pressed. The values are still read from the panel at drain time; see
        // Subsystems/PropsApply.cpp for why that is deliberate and not a leftover.
        //
        // SelType::MAP is dispatched inside PropsApply rather than here, because it
        // writes level metadata and has its own undo policy.
        ed::EventBus::instance().drain(g_editorPropsEvents, ed::IsPropsEvent);
        if (!g_editorPropsEvents.empty()) {
            ApplyPanelProperties(g_editorPropsEvents);
            MarkWorldGraphDirty();
        }

        // --- Editable .ozls stat rows ---------------------------------------
        //
        // Separate from the SelType dispatch above on purpose: the stat rows
        // belong to the DEF, not to the world instance being selected, and the
        // Script Manager can reach them too. The patcher only touches the
        // `stats { }` block, so this is independent of every in-memory edit
        // above and cannot disturb them.
        if (!g_editorPanels.propDefPendingEdits.empty()) {
            std::vector<ozls::StatEdit> edits;
            edits.reserve(g_editorPanels.propDefPendingEdits.size());
            for (const auto& e : g_editorPanels.propDefPendingEdits) {
                if (e.key.empty()) continue;
                ozls::StatKind kind = ozls::StatKind::String;
                // Infer the kind from the authored value: a parenthesised
                // triple is a vec3, a bare number is a float, anything else a
                // string. The writer files it back under the matching map, so
                // Parse reads it the same way next time.
                if (e.value.size() > 2 && e.value.front() == '(' && e.value.back() == ')') {
                    kind = ozls::StatKind::Vec3;
                } else {
                    char* end = nullptr;
                    const float v = std::strtof(e.value.c_str(), &end);
                    if (end && *end == '\0' && !e.value.empty()) {
                        (void)v;
                        kind = ozls::StatKind::Float;
                    }
                }
                edits.push_back({e.key, e.value, kind});
            }

            if (edits.empty()) {
                g_editorPanels.propDefPendingEdits.clear();
            } else if (!g_editorPanels.propDefWritable || g_editorPanels.propDefPath.empty()) {
                // Packaged def: there is no source file to patch. The panel
                // already shows these rows read-only, so reaching here means the
                // state changed underneath us; say so rather than pretend.
                EditorLog("Stat edits skipped: '%s' has no editable source file",
                          g_editorPanels.propDefPath.c_str());
                g_editorPanels.propDefPendingEdits.clear();
            } else {
                const ozls::PatchResult res =
                    ozls::PatchOzlsStats(g_editorPanels.propDefPath, edits);
                if (res.ok) {
                    EditorLog("Patched %s: %d updated, %d inserted, %d erased",
                              g_editorPanels.propDefPath.c_str(),
                              res.updated, res.inserted, res.erased);
                    // Re-read ONLY this file, in place. Init() would reparse the
                    // whole tree and can relocate defs, which leaves the
                    // EntityDef pointers held by LightningEntityManager
                    // instances and SkyZoneNode::def pointing at the wrong def.
                    // ReloadFile updates the by-name entry through its existing
                    // node, so every holder observes the new values.
                    LightningEntityRegistry::Instance().ReloadFile(g_editorPanels.propDefPath);
                    // The watcher will also see this write. Record the stamp so
                    // the pending event is swallowed instead of rebuilding the
                    // panel a second time ~200 ms after Apply.
                    HotReload_NoteApplied(g_editorPanels.propDefPath);
                    // The defs just changed on disk, so any icon EngineBillboard
                    // memoised for these names is now stale.
                    EngineBillboard::InvalidateIconCache();
                    // Keep the Script Manager's action counts / parse-error rows
                    // in step with the file the panel just wrote.
                    RefreshScriptManager();
                    SendMessage((HWND)g_editorPanels.hPropsPanel, WM_USER + 50, 0, 0);
                } else {
                    EditorLog("Stat patch FAILED for %s: %s",
                              g_editorPanels.propDefPath.c_str(), res.error.c_str());
                    MessageBoxA(nullptr, res.error.c_str(),
                                "Could not write .ozls", MB_OK | MB_ICONERROR);
                }
                g_editorPanels.propDefPendingEdits.clear();
            }
        }

        // Preview a sound stat without saving.
        if (!g_editorPanels.propDefPreviewSound.empty()) {
            SoundManager::Instance().PlayStatSound(g_editorPanels.propDefPreviewSound);
            g_editorPanels.propDefPreviewSound.clear();
        }
        // "Reload Mesh" â€” drop the cached asset so it re-resolves next draw
        // ---- Animation tool (Phase B) --------------------------------------
        // Track the selected mesh as the animation target.
        {
            int selMesh = (g_sel.type == SelType::MESH) ? g_sel.index : -1;
            if (selMesh != g_editorPanels.animTargetMesh) {
                // Release the previous target so it resumes normal playback.
                if (MeshObjectNode* old = PawnSystem::Instance().GetMeshObject(g_editorPanels.animTargetMesh)) {
                    old->animPaused = false;
                    old->animPreview = false;   // stop previewing a GLB clip
                    old->editPose.reset();
                }
                g_editorPanels.animTargetMesh = selMesh;
                g_editorPanels.animPlaying = false;
                g_editorPanels.animEditVerts = false;
                g_editorPanels.animSelVerts.clear();
                g_editorPanels.animTime = 0.0f;
                EnsureMeshNodeLoaded(PawnSystem::Instance().GetMeshObject(selMesh));
                RefreshAnimPanel();
            }
        }
        // Convert a static mesh into a vertex-keyframe animated one. The event carries
        // the target, so converting what the user clicked is no longer at the
        // mercy of whatever is selected when the frame drains.
        if (evConvertMeshIndex >= 0) {
            int mi = evConvertMeshIndex;
            evConvertMeshIndex = -1;
            MeshObjectNode* n = PawnSystem::Instance().GetMeshObject(mi);
            if (n && n->animFile.empty()) {
                std::string file = n->meshPath;
                size_t slash = file.find_last_of("/\\");
                if (slash != std::string::npos) file = file.substr(slash + 1);
                size_t dot = file.rfind('.');
                if (dot != std::string::npos) file = file.substr(0, dot);
                if (file.empty()) file = "anim";
                std::string path = "GameData/Environment/Anims/" + file + ".ozanim";
                std::error_code ec;
                fs::create_directories("GameData/Environment/Anims", ec);
                if (!fs::exists(path)) {
                    ozanim::Animation a;
                    ozanim::Clip c; c.name = "Idle"; c.fps = 30.0f; c.loop = true;
                    a.clips.push_back(c);
                    std::ofstream out(path);
                    if (out.is_open()) out << ozanim::Serialize(a);
                }
                n->animFile = path;
                n->skeletal = true;
                n->animTime = 0.0f;
                n->animPaused = true;
                n->mesh.reset();
                EnsureMeshNodeLoaded(n);
                g_editorPanels.animTargetMesh = (int)n->id;
                g_editorPanels.animClipName = "Idle";
                ShowAnimPanel(true);
                RefreshAnimPanel();
                EditorLog("Converted '%s' to animated -> %s", n->meshPath.c_str(), path.c_str());
            } else {
                EditorLog("Convert: select a GameEngine.Mesh with no anim file");
            }
        }
        // Animation commands arrive as ed::Ev::Anim* with the clip name, playhead
        // and fps/loop captured at post time; ApplyAnimIntent owns them. The two
        // remaining fields, actionAnimSave and actionAnimRefresh, are
        // INTRA-FRAME chaining signals rather than user intents: the handlers
        // themselves set Save, and Refresh is a dirty flag. Queueing those would
        // push the file write a frame later for no benefit.
        ed::EventBus::instance().drain(g_editorAnimEvents, ed::IsAnimEvent);
        ApplyAnimIntents(g_editorAnimEvents);

        // Placement arrives as ed::Ev::Spawn* carrying its own position, resolved
        // type and (for meshes) the model path - all captured at post time. The Pawn
        // panel used to call PawnSystem::Add* directly from its WM_COMMAND handler,
        // which broke the layer rule; see Subsystems/Placement.cpp.
        //
        // Drained in its own pass rather than the shared g_editorFrameEvents for the
        // same reason the surface and anim passes have theirs: so Placement.cpp can
        // become a real translation unit without that move reordering the selection
        // dispatch above.
        //
        // Deferral is one frame. It is safe because nothing here reads live panel or
        // selection state, and it is what makes the "the handler did not run"
        // condition observable at all - g_editorUnplacedEvents counts a Spawn* that
        // arrived without a usable payload, which is how the nine dead actionSpawn*
        // fields stayed invisible for so long.
        ed::EventBus::instance().drain(g_editorPlacementEvents, ed::IsPlacementEvent);
        if (!g_editorPlacementEvents.empty()) {
            ApplyPlacementSpawns(g_editorPlacementEvents);
            MarkWorldGraphDirty();
        }

        // --- WorldGraph Explorer ---------------------------------------------
        //
        // Its rows carry a selIndex into the very vectors BuildWorldGraphEntries
        // iterates, so deleting or duplicating a row shifts every later row and a
        // second right-click acts on the wrong entity until Refresh is pressed by
        // hand. It was also built exactly once, in WM_CREATE from
        // CreateAllEditorWindows - which runs BEFORE the world is loaded and before
        // PawnDefs register - so opening the panel on a fresh session showed one
        // "Map (level)" row and stayed that way.
        //
        // One refresh at the end of the frame closes both holes. The flag is set at
        // the choke points - each event drain, the direct placement paths, AutoConvex,
        // undo/redo, world load - and consumed here, after the LAST drain, so a
        // mutation is never reflected a frame late. Subsystems and Core set the flag;
        // this single call is Core reaching down into UI, which is the legal direction.
        // Only rebuild while the panel is actually on screen. Opening it refreshes anyway
        // (ShowWorldGraph), so a session that never opens it pays nothing.
        if (g_editorPanels.worldGraphDirty && g_editorPanels.showWorldGraph) {
            g_editorPanels.worldGraphDirty = false;
            RefreshWorldGraph();
            // Logged from Core, not from the panel: the rebuild is silent, and a list
            // that quietly stays stale is exactly what this replaces. UI/ may not
            // reach up into Core/ for a logging call - Core reaching down into UI here
            // is the legal direction.
            EditorLog("WorldGraph refreshed");
        }

        // "Frame Selection" from the World Hierarchy. Consumed once, after the
        // selection drains, so it centres on the entity that was just selected
        // rather than on whatever was selected when the menu item was clicked.
        if (g_editorPanels.focusSelection) {
            g_editorPanels.focusSelection = false;
            FocusSelectionInViewport();
        }

        // Save the clip file back to disk. Deliberately AFTER ApplyAnimIntents:
        // a NewClip/DeleteKey posted this frame sets actionAnimSave from inside the
        // handler, and the write must still happen in the same frame.
        if (g_editorPanels.actionAnimSave) {
            g_editorPanels.actionAnimSave = false;
            MeshObjectNode* n = PawnSystem::Instance().GetMeshObject(g_editorPanels.animTargetMesh);
            if (oz::AnimatedMesh* am = AnimTarget(); am && n && !n->animFile.empty()) {
                std::error_code ec;
                fs::path p(n->animFile);
                if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
                std::ofstream out(n->animFile);
                if (out.is_open()) out << ozanim::Serialize(am->GetAnimation());
                EditorLog("Saved anim '%s'", n->animFile.c_str());
            }
            g_editorPanels.actionAnimRefresh = true;
        }
        // Scrub / playback: drive the target node's animTime.
        {
            MeshObjectNode* tn = PawnSystem::Instance().GetMeshObject(g_editorPanels.animTargetMesh);
            if (tn && !tn->animFile.empty()) {
                tn->animPaused = !g_editorPanels.animPlaying;
                if (g_editorPanels.animPlaying) g_editorPanels.animTime = tn->animTime;
            } else if (tn && AnimTargetSkeletal()) {
                // Read-only preview of an embedded GLB clip. The clip NAME goes
                // to previewClip, never animClip (which the world exports), and
                // the node owns the playback clock - DrawEntities advances it
                // while unpaused. Scrubbing pushes the panel time the other way.
                tn->animPreview = true;
                tn->previewClip = g_editorPanels.animClipName;
                tn->animPaused = !g_editorPanels.animPlaying;
                if (g_editorPanels.animPlaying) g_editorPanels.animTime = tn->animTime;
                else tn->animTime = g_editorPanels.animTime;
            }
        }
        // Ctrl+Z / Ctrl+Y while the anim tool is active. Posts the SAME events the
        // toolbar posts, so a keyboard undo is not a second code path with its own
        // staleness behaviour.
        if (g_editorPanels.animEditVerts && EditorUi_ViewportHasFocus() &&
            (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL))) {
            ed::AnimIntent ai;
            ai.meshId   = g_editorPanels.animTargetMesh;
            ai.clipName = g_editorPanels.animClipName;
            ai.time     = g_editorPanels.animTime;
            // Layout-aware: these are the keys LABELLED Z / Y, not the US
            // positions. On QWERTZ/AZERTY the two are swapped, so a positional
            // check made Ctrl+Z do nothing and Ctrl+Y hit the wrong stack.
            if (IsKeyPressed(EdKeys::PhysicalKey(KEY_Z))) ed::EventBus::instance().post(ed::Ev::AnimUndo, ai);
            if (IsKeyPressed(EdKeys::PhysicalKey(KEY_Y))) ed::EventBus::instance().post(ed::Ev::AnimRedo, ai);
        }
        if (g_editorPanels.actionAnimRefresh) {
            g_editorPanels.actionAnimRefresh = false;
            RefreshAnimPanel();
        }
        // Live playback readout (~10 Hz). The panel's tick message only rewrites
        // the time/frame line + scrubber; RefreshAnimPanel would rebuild the clip
        // list and lose the selection. Throttled so the Win32 queue is not fed a
        // message every frame.
        {
            static int s_animTickFrame = 0;
            if (g_editorPanels.animPlaying && g_editorPanels.hAnimPanel &&
                (++s_animTickFrame % 6) == 0) {
                PostMessageA((HWND)g_editorPanels.hAnimPanel,
                             EditorPanelState::kAnimPanelTick, 0, 0);
            }
        }

    #ifdef _WIN32
    // --- Model preview render-to-texture (for Win32 Model Browser dialog) ---
    // Rendered inside the current frame (after the main viewport pass) so the
    // offscreen pass is flushed by EndDrawing on every raylib version.
        if (g_editorPanels.selectedModel >= 0 &&
            g_editorPanels.selectedModel < (int)g_editorPanels.modelEntries.size() &&
            g_editorPanels.selectedModel != g_lastPreviewSel) {
            g_lastPreviewSel = g_editorPanels.selectedModel;
            g_previewNeedsUpdate = true;
        }
        if (g_previewNeedsUpdate && g_lastPreviewSel >= 0 &&
            g_lastPreviewSel < (int)g_editorPanels.modelEntries.size()) {
            auto& entry = g_editorPanels.modelEntries[g_lastPreviewSel];
            if (!entry.loaded) {
                // Only need triangle/vertex counts for the info line; guard the
                // "replace last 4 chars" so short/extensionless paths can't UB.
                Model mdl = LoadModelWithFallback(entry.path.c_str());
                if (mdl.meshes != nullptr) {
                    entry.triangles = mdl.meshes[0].triangleCount;
                    entry.vertices = mdl.meshes[0].vertexCount;
                }
                UnloadModel(mdl);
                entry.loaded = true;
            }
            Model mdl = LoadModelWithFallback(entry.path.c_str());
            // Companion texture: "<stem>_texture.png" / "<stem>.png" beside the model.
            std::string texPath, texPath2;
            if (entry.path.size() > 4) {
                texPath = entry.path;  texPath.replace(texPath.end() - 4, texPath.end(), "_texture.png");
                texPath2 = entry.path; texPath2.replace(texPath2.end() - 4, texPath2.end(), ".png");
            }
            Texture2D tex = {0};
            if (!texPath.empty() && fs::exists(texPath)) tex = LoadTextureWithFallback(texPath.c_str());
            else if (!texPath2.empty() && fs::exists(texPath2)) tex = LoadTextureWithFallback(texPath2.c_str());
            else {
                size_t dot = entry.path.rfind('.');
                if (dot != std::string::npos) {
                    std::string fext = entry.path.substr(dot);
                    std::transform(fext.begin(), fext.end(), fext.begin(), ::tolower);
                    if (fext == ".png" || fext == ".jpg" || fext == ".jpeg" || fext == ".bmp" || fext == ".tga")
                        tex = LoadTextureWithFallback(entry.path.c_str());
                }
            }
            if (tex.id > 0) mdl.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = tex;

            // Auto-frame the camera on the model bounds so any size is visible.
            Camera3D prevCam = {0};
            prevCam.up = {0, 1, 0};
            prevCam.fovy = 45.0f;
            prevCam.projection = CAMERA_PERSPECTIVE;
            Vector3 center = {0, 0, 0};
            float radius = 3.0f;
            if (mdl.meshCount > 0 && mdl.meshes != nullptr) {
                BoundingBox bb = GetModelBoundingBox(mdl);
                center = {(bb.min.x + bb.max.x) * 0.5f,
                          (bb.min.y + bb.max.y) * 0.5f,
                          (bb.min.z + bb.max.z) * 0.5f};
                Vector3 ext = {bb.max.x - bb.min.x, bb.max.y - bb.min.y, bb.max.z - bb.min.z};
                radius = fmaxf(ext.x, fmaxf(ext.y, ext.z)) * 0.5f;
                if (radius < 0.05f) radius = 0.05f;
            }
            float dist = radius * 3.0f;
            Vector3 dir = Vector3Normalize({1.0f, 0.7f, 1.0f});
            prevCam.target = center;
            prevCam.position = Vector3Add(center, Vector3Scale(dir, dist));

            BeginTextureMode(g_previewRT);
            ClearBackground((Color){40, 40, 50, 255});
            BeginMode3D(prevCam);
            DrawGrid(20, radius / 2.0f); // scale the grid to the model
            if (mdl.meshes != nullptr) {
                // Imported models opt out of culling via the RAII guard rather
                // than a paired raw rl* call - see Renderer/CullState.hpp.
                oz::ScopedCullOff cullOff;
                DrawModel(mdl, {0, 0, 0}, 1.0f, WHITE);
            }
            EndMode3D();
            EndTextureMode();
            UnloadModel(mdl);
            if (tex.id > 0) UnloadTexture(tex);

            Image img = LoadImageFromTexture(g_previewRT.texture);
            ImageFlipVertical(&img);
            unsigned char* px = (unsigned char*)img.data;
            for (int i = 0; i < img.width * img.height; i++) {
                unsigned char tmp = px[i*4];
                px[i*4] = px[i*4+2];
                px[i*4+2] = tmp;
            }
            BITMAPINFO bmi = {};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = img.width;
            bmi.bmiHeader.biHeight = -img.height;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            HDC hdc = GetDC((HWND)GetWindowHandle());
            void* bits = nullptr;
            HBITMAP hBmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
            if (bits) memcpy(bits, img.data, img.width * img.height * 4);
            ReleaseDC((HWND)GetWindowHandle(), hdc);
            UpdateModelPreview(hBmp, img.width, img.height);
            UnloadImage(img);
            g_previewNeedsUpdate = false;
        }
#endif
        // Publish the editing camera LAST. The pane's camera is the authority
        // (Main.cpp restores it at the top of every frame), so anything that
        // changed OTEditor.MainCamera after the mid-frame commit used to be
        // discarded next frame - which is why the Top/Side/Persp chips, the
        // NumPad presets, the Camera menu, Reset Camera and Focus Selection all
        // looked dead. The mid-frame commit above stays for the pane switch, but
        // this is the one that decides what survives the frame.
        Editor_CommitActiveCamera();

        EndDrawing();

        if (benchSession.active) {
            ++benchFrame;
            if (benchFrame == benchSession.warmup) {
                oz::diag::Profiler().Reset();
                EditorLog("Bench: warm-up complete, measuring %d frames",
                          benchSession.frames);
            }
            if (benchFrame >= benchSession.warmup + benchSession.frames) {
                benchSession.done = true;
                break;
            }
        }

        // Keep the camera aim point available to Win32 panels, which spawn
        // entities synchronously (see Win32Dialogs SpawnSelectedPawnTreeItem).
        g_editorPanels.spawnPos[0] = OTEditor.MainCamera.target.x;
        g_editorPanels.spawnPos[1] = OTEditor.MainCamera.target.y;
        g_editorPanels.spawnPos[2] = OTEditor.MainCamera.target.z;

        // Handle action flags from Win32 dialogs
        // Heightmap generate handler
        // The legacy Light Properties apply handler is GONE. It became unreachable
        // in b88 when LightPropsProc - its only writer - was deleted with the window,
        // and it was already a strict subset of the properties panel's
        // tgtType == SelType::LIGHT branch, which additionally handles position, name
        // and target and clamps type/effect defensively. Its 12 light* backing
        // fields were referenced by nothing else and went with it.


        // Portal DELETION is now ed::Ev::DeletePortal, handled in the event dispatch
        // above. It keeps its own event kind rather than riding DeleteEntity because
        // it mutates ZoneManager::GetPortals() - a different container with a
        // different index space than the entity lists DeleteSelectedEntity knows.
        // Portal *editing* remains the panel's PORTAL section (generic apply).

        // LevelList / Campaign actions

        // Every shortcut below belongs to the VIEWPORT. When a native panel field
        // has keyboard focus the pump consumes Tab/Ctrl+A/Enter/Escape, and this
        // gate makes sure stray raylib key state can never also trigger a mode
        // change, a panel toggle or a delete while the user is typing.
        if (EditorUi_ViewportHasFocus()) {

        // File commands, from the remappable EdKeys table. The menu advertises
        // the same table, so the hint and the handler cannot disagree.
        if (EdKeys::Pressed(EdAction::SaveAs)) FileSaveAs();
        else if (EdKeys::Pressed(EdAction::Save)) {
            if (!g_documentPath.empty()) SaveWorldDocument(g_documentPath);
            else FileSaveAs();
        }
        if (EdKeys::Pressed(EdAction::New)) FileNew();
        if (EdKeys::Pressed(EdAction::Open)) FileOpen();
        if (EdKeys::Pressed(EdAction::Play)) PlayTestCurrentWorld();
        if (EdKeys::Pressed(EdAction::Exit))
            PostMessageA((HWND)GetWindowHandle(), WM_CLOSE, 0, 0);

        // Mode switching / Unreal movement speed.
        //
        // With Unreal-style editing ON, bare 1/2/3 are the movement-speed keys
        // (the UnrealEd convention), so the placement modes move to Alt+key. Both
        // routes are in the Keyboard Shortcuts panel and the toolbar; the speed
        // keys are the only hard-wired ones here because they follow that
        // convention rather than the remappable action table.
        const bool uAlt = IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
        if (g_editorPanels.unrealBrushMove) {
            if (uAlt) {
                if (IsKeyPressed(EdKeys::Key(EdAction::ModeModel)))  g_placeMode = PlaceMode::MODEL;
                if (IsKeyPressed(EdKeys::Key(EdAction::ModePickup))) g_placeMode = PlaceMode::PICKUP;
                if (IsKeyPressed(EdKeys::Key(EdAction::ModeNode)))   g_placeMode = PlaceMode::NODE;
                if (IsKeyPressed(EdKeys::Key(EdAction::ModeZone)))   ToggleZonePlacement();
                if (IsKeyPressed(EdKeys::Key(EdAction::ModeTerrain)) && g_placeMode != PlaceMode::MODEL)
                    g_placeMode = PlaceMode::TERRAIN;
            } else {
                bool speedChanged = false;
                if (IsKeyPressed(KEY_ONE))   { g_editorPanels.unrealMoveSpeed = 0; speedChanged = true; }
                if (IsKeyPressed(KEY_TWO))   { g_editorPanels.unrealMoveSpeed = 1; speedChanged = true; }
                if (IsKeyPressed(KEY_THREE)) { g_editorPanels.unrealMoveSpeed = 2; speedChanged = true; }
                if (speedChanged) Editor_SyncUnrealMenuChecks();
            }
        } else {
            if (EdKeys::Pressed(EdAction::ModeModel))  g_placeMode = PlaceMode::MODEL;
            if (EdKeys::Pressed(EdAction::ModePickup)) g_placeMode = PlaceMode::PICKUP;
            if (EdKeys::Pressed(EdAction::ModeNode))   g_placeMode = PlaceMode::NODE;
            if (EdKeys::Pressed(EdAction::ModeZone))   ToggleZonePlacement();
            if (EdKeys::Pressed(EdAction::ModeTerrain) && g_placeMode != PlaceMode::MODEL)
                g_placeMode = PlaceMode::TERRAIN;
        }

        // Primitive type cycling (1-5 for box/cyl/sph/pyr/pln, only in MODEL mode)
        // Reads real state from EMID (200+prim); never re-fires the actionCsgPlace
        // one-shot (that would reset the ghost's size every frame).
        if (g_placeMode == PlaceMode::MODEL) {
            int prim = (EMID >= 200 && EMID <= 204) ? EMID - 200 : 0;
            // Uses the same remappable key as the terrain mode (5 by default):
            // inside MODEL mode it cycles primitives, otherwise it selects terrain.
            // With Unreal-style editing on, Alt+5 is the terrain mode switch, so it
            // must not also cycle here.
            if (IsKeyPressed(EdKeys::Key(EdAction::ModeTerrain)) &&
                !(g_editorPanels.unrealBrushMove && uAlt)) {
                prim = (prim + 1) % 5;
                EMID = 200 + prim;
                EditorLog("Primitive: %s", prim == 0 ? "box" : prim == 1 ? "cylinder" : prim == 2 ? "sphere" : prim == 3 ? "pyramid" : "plane");
            }
        }

        // CSG operation cycling
        if (EdKeys::Pressed(EdAction::CsgCycle) && g_placeMode == PlaceMode::MODEL) {
            AngelTechEditor.CSGOperation = (AngelTechEditor.CSGOperation + 1) % 5;
        }

        // Heightmap editor toggle — suppressed in vertex-edit mode (its key moves verts)
        if (EdKeys::Pressed(EdAction::Heightmap) && !g_editorPanels.animEditVerts)
            ToggleHeightmapEditor();

        // Panel shortcuts (remappable; F3-F10 by default)
        if (EdKeys::Pressed(EdAction::Hierarchy))      ToggleWorldGraph();
        if (EdKeys::Pressed(EdAction::ModelBrowser))   ToggleModelBrowser();
        if (EdKeys::Pressed(EdAction::SoundManager))  ToggleSoundMgr();
        if (EdKeys::Pressed(EdAction::TextureManager)) ToggleTextureMgr();
        if (EdKeys::Pressed(EdAction::PawnManager))    TogglePawnMgr();
        if (EdKeys::Pressed(EdAction::ScriptManager))  ToggleScriptMgr();
        if (EdKeys::Pressed(EdAction::PickupPanel))    TogglePickupPanel();
        if (IsKeyPressed(KEY_F11)) ToggleFullscreen();  // supersedes old F11 handling
        // F12 is unbound. It used to open Zone Properties; zone and light editing
        // is now the Entity Properties panel (right-click the entity).

        // Selection shortcuts. Escape must also cancel an active placement ghost
        // (DrawModel true with nothing selected), otherwise there is no keyboard
        // way out of placement mode.
        // Escape cancels an active gizmo drag first (restoring the pre-drag
        // transform), and only then clears the selection.
        if (IsKeyPressed(KEY_ESCAPE) &&
            (Gizmo_IsDragging() || Gizmo_IsUnrealDragging())) {
            // Both cancel paths restore their own start values; the frame sync then
            // publishes the restored gizmo, so the entity reverts too.
            Gizmo_CancelUnrealDrag();
            Gizmo_CancelDrag();
            g_gizmoDrag = false;
            g_gizmoHistPushed = false;
        } else if (IsKeyPressed(KEY_ESCAPE) &&
            (g_sel.type != SelType::NONE || AngelTechEditor.DrawModel)) {
            g_sel = { SelType::NONE, -1, "", {0,0,0} };
            g_hoverSel = { SelType::NONE, -1, "", {0,0,0} };
            g_selSet.clear();
            AngelTechEditor.DrawModel = false;
            g_placementArmed = false;
            // A cancelled ghost must not leave its model path armed: the ghost
            // preview and the commit both read it, and a stale path would place
            // the previous model on the next "Place in World".
            AngelTechEditor.ActiveModelPath.clear();
        }
        // Delete/Duplicate are for placed objects only. Both targets are `else if`
        // chains with no `else`, so gating on `!= NONE` alone would let the Map
        // row (the level) be "deleted" — silently deselecting it and leaving a
        // stray undo snapshot behind.
        const bool selIsPlaceable = g_sel.type != SelType::NONE &&
                                    g_sel.type != SelType::MAP;
        if (EdKeys::Pressed(EdAction::Delete) && selIsPlaceable) {
            DeleteSelectedEntity();
        }
        if (EdKeys::Pressed(EdAction::Duplicate) && selIsPlaceable) {
            DuplicateSelectedEntity();
        }
        // Ctrl+C / Ctrl+V for the (multi-)selected brushes. Deliberately plain
        // modifiers rather than EdKeys actions: the cut/copy/paste vocabulary is
        // universal and does not need rebinding, and keeping it out of the key
        // table avoids a conflict row for something every user expects fixed.
        if (EditorUi_ViewportHasFocus() &&
            (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL))) {
            if (IsKeyPressed(EdKeys::PhysicalKey(KEY_C))) CopySelectedEntities();
            if (IsKeyPressed(EdKeys::PhysicalKey(KEY_V))) PasteEntities();
        }
        // Undo / redo (Ctrl+Z, Ctrl+Y, Ctrl+Shift+Z). Deferred to the anim tool
        // while vertex-editing, which owns its own undo stack.
        if (!g_editorPanels.animEditVerts) {
            if (EdKeys::Pressed(EdAction::Undo)) HistoryUndo();
            if (EdKeys::Pressed(EdAction::Redo)) HistoryRedo();
            // Ctrl+Shift+Z is a second, hard-wired redo binding (not in the table:
            // it is the same key as Undo with Shift, and the table has one key per
            // action).
            if (EdKeys::CtrlDown() && EdKeys::ShiftDown() &&
                IsKeyPressed(EdKeys::PhysicalKey(KEY_Z)))
                HistoryRedo();
        }

        // Camera shortcuts
        if (IsKeyPressed(KEY_HOME)) { SetViewPerspective(); ResetCamera(); }
        if (IsKeyPressed(KEY_PAGE_UP)) CamUp();
        if (IsKeyPressed(KEY_PAGE_DOWN)) CamDown();

        // View preset shortcuts (NumPad). Same named presets the Camera menu and
        // the toolbar chips use, so all three stay in step.
        if (IsKeyPressed(KEY_KP_7)) ViewTop();
        if (IsKeyPressed(KEY_KP_1)) ViewBottom();
        if (IsKeyPressed(KEY_KP_3)) ViewRight();
        if (IsKeyPressed(KEY_KP_9)) ViewLeft();
        if (IsKeyPressed(KEY_KP_2)) ViewFront();
        if (IsKeyPressed(KEY_KP_5)) SetViewPerspective();

        }  // EditorUi_ViewportHasFocus()

    }

    if (benchSession.active && benchSession.done) {
        char header[768];
        std::snprintf(header, sizeof(header),
                      "# AngelEd\n# document=%s\n# views=%d\n"
                      "# frames=%d warmup=%d\n# resolution=%dx%d\n",
                      g_documentPath.string().c_str(), g_editorPanels.viewLayout,
                      benchSession.frames, benchSession.warmup,
                      GetScreenWidth(), GetScreenHeight());
        if (oz::diag::Profiler().DumpCSV(benchSession.csvPath.c_str(), header,
                                         benchSession.frames))
            EditorLog("Bench CSV -> %s (%d frames)", benchSession.csvPath.c_str(),
                      benchSession.frames);
        else
            EditorLog("Bench CSV: could not write %s", benchSession.csvPath.c_str());
        EditorLog("Bench summary:\n%s", oz::diag::Profiler().Summary().c_str());
    }

    // Cleanup
    EditorLog("=== AngelEd shutting down ===");
    // Stop the file watcher before the windows and the log go away: its thread
    // may still be holding a ReadDirectoryChangesW on GameData.
    HotReload_Stop();
    EditorIcons::Instance().Unload();
    if (g_editorLog) fclose(g_editorLog);
    StopSoundPreview();
    OzoneLoader::Instance().Unload();
    ozmat::MaterialRegistry::Instance().Shutdown();
    // The portal view is a render texture; release it with the other GPU
    // resources, before CloseWindow(), while the GL context is still alive.
    oz::SkyPortal::Instance().Shutdown();
    UnloadRenderTexture(g_previewRT);
    // Popped-out panes own a RenderTexture2D + an HWND; release both while the GL
    // context is still alive.
    DetachedPane_Shutdown();
#ifdef _WIN32
    DestroyAllEditorWindows();
#endif
    CloseAudioDevice();
    CloseWindow();
}

// =====================================================================
// Top menu bar with dropdowns
// =====================================================================
// All panel managers are implemented as Win32 native dialogs
// in Win32Dialogs.cpp. Panels toggled via keyboard shortcuts (F5-F12).
