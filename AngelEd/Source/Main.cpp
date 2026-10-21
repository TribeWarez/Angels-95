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
#include "Core/EditorMenus.cpp"
#include "Subsystems/AnimEditing.cpp"
#include "Core/EditorDispatcher.cpp"

// -----------------------------------------------------------------------------
// main()
// -----------------------------------------------------------------------------
int main(int argc, char **argv){
    // FIRST, before anything reads a relative path. Almost every asset path in the
    // editor is relative and unanchored, so launching from the wrong directory produces a
    // wave of silent failures instead of one clear error. See EnsureProjectRoot() in
    // Core/EditorShell.cpp.
    EnsureProjectRoot();

    // Shortcut overrides must be loaded before the menu is built, because the menu
    // labels are generated from this table.
    EdKeys::Load();

    EditorLog("=== AngelEd starting ===");
    SetConfigFlags(FLAG_VSYNC_HINT);
    InitWindow(1280, 720, "AngelEd");
    CreateEditorMenuBar();
    SetTraceLogLevel(LOG_WARNING);
    InitAudioDevice();
    SetTargetFPS(60);
    GuiLoadStyleDark();

    // Load editor toolbar icons
    EditorIcons::Instance().Load();

    // No argument: open a world, but only if one is actually there. This used to
    // default to "../GameData/World.ozone", which does not exist in ANY layout -
    // worlds live at GameData/Worlds/<Name>/World.ozone, and the path also assumed
    // cwd == System/, contradicting the "System/AngelEd.ini" load eight lines below.
    if (argc > 1 && argv[1]) {
        g_documentPath = fs::path(argv[1]);
    } else {
        g_documentPath.clear();
        fs::path first = FirstAvailableWorld();
        if (!first.empty()) g_documentPath = first;
        else EditorLog("No world argument and no world found under GameData/Worlds/ - "
                       "starting with an empty scene.");
    }
    SetWorldDirectory(g_documentPath.empty() ? fs::path()
                                             : g_documentPath.parent_path());

    // Load INI config
    g_config.Load("System/AngelEd.ini");

    // Initialize package-based asset loading
    PackageAssetLoader::Instance().Init();

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
#endif

    // Load editor world
    Init();

    // Load pawn definitions from config files (data-driven)
    {
        auto& ps = PawnSystem::Instance();
        const char* defsDir = "GameData/Pawn/PawnDefs";
        // Loose defs first, then packaged ones (.cfg ship in pawns_defs.ozpak).
        // Loose wins: the editor runs from a checkout, where the author's file is
        // the authority, and a packaged copy must not shadow it.
        std::vector<std::pair<std::string, std::string>> cfgFiles; // basename, text
        if (fs::exists(defsDir)) {
            for (auto& entry : fs::directory_iterator(defsDir)) {
                if (!entry.is_regular_file()) continue;
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext != ".cfg") continue;
                std::ifstream f(entry.path());
                if (!f.is_open()) continue;
                std::stringstream ss;
                ss << f.rdbuf();
                cfgFiles.emplace_back(entry.path().filename().string(), ss.str());
            }
        }
        for (const std::string& cfg : PackageAssetLoader::Instance().ListAssetBasenames(".cfg")) {
            bool seen = false;
            for (auto& cf : cfgFiles) if (cf.first == cfg) { seen = true; break; }
            if (seen) continue;
            std::string text = LoadFileTextWithFallback(("GameData/Pawn/PawnDefs/" + cfg).c_str());
            if (!text.empty()) cfgFiles.emplace_back(cfg, text);
        }

        if (!cfgFiles.empty()) {
            int loaded = 0;
            for (auto& cf : cfgFiles) {
                std::istringstream f(cf.second);
                std::string name, sp, sc;
                std::string modelPath, modelTex, meshType;
                std::string animIdle, animPatrol, animChase, animReturn, animDeath;
                float speed = 1.5f, aggroRange = 6.0f, attackRange = 1.5f, damage = 10.0f;
                float animSpeed = 1.0f, modelScale = 1.0f;
                int maxHealth = 100;
                std::string line;
                while (std::getline(f, line)) {
                    line.erase(0, line.find_first_not_of(" \t\r\n"));
                    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
                    size_t eq = line.find('=');
                    if (eq == std::string::npos) continue;
                    std::string key = line.substr(0, eq);
                    std::string val = line.substr(eq + 1);
                    key.erase(0, key.find_first_not_of(" \t"));
                    key.erase(key.find_last_not_of(" \t") + 1);
                    val.erase(0, val.find_first_not_of(" \t"));
                    val.erase(val.find_last_not_of(" \t\r") + 1);
                    if (key == "name") name = val;
                    else if (key == "speed") speed = std::stof(val);
                    else if (key == "aggroRange") aggroRange = std::stof(val);
                    else if (key == "attackRange") attackRange = std::stof(val);
                    else if (key == "damage") damage = std::stof(val);
                    else if (key == "maxHealth") maxHealth = std::stoi(val);
                    else if (key == "sprite_path") sp = val;
                    else if (key == "scream_path") sc = val;
                    // Model fields. The client's loader (Core.hpp) reads these, so
                    // without them the editor fell back to the sprite_path - which
                    // for most shipped defs is a 32x32 placeholder - and enemies
                    // rendered as flat coloured squares while the game showed their
                    // real GLBs. Keep this list in step with Core.hpp's parser.
                    else if (key == "model_path") modelPath = val;
                    else if (key == "model_texture") modelTex = val;
                    else if (key == "mesh_type") meshType = val;
                    else if (key == "model_scale") modelScale = std::stof(val);
                    else if (key == "anim_idle") animIdle = val;
                    else if (key == "anim_patrol") animPatrol = val;
                    else if (key == "anim_chase") animChase = val;
                    else if (key == "anim_return") animReturn = val;
                    else if (key == "anim_death") animDeath = val;
                    else if (key == "anim_speed") animSpeed = std::stof(val);
                }
                if (!name.empty()) {
                    PawnDef def;
                    def.name = name;
                    def.speed = speed;
                    def.aggroRange = aggroRange;
                    def.attackRange = attackRange;
                    def.damage = damage;
                    def.maxHealth = maxHealth;
                    def.sprite_path = sp;
                    def.scream_path = sc;
                    def.model_path = modelPath;
                    def.model_texture = modelTex;
                    def.model_scale = modelScale;
                    def.mesh_type = meshType;
                    def.anim_idle = animIdle;
                    def.anim_patrol = animPatrol;
                    def.anim_chase = animChase;
                    def.anim_return = animReturn;
                    def.anim_death = animDeath;
                    def.anim_speed = animSpeed;
                    def.baseDir = std::string(defsDir) + "/";
                    ps.RegisterDef(def);
                    if (!sp.empty())
                        PawnManagerAddPawn(name.c_str(), sp.c_str());
                    else
                        PawnManagerAddPawn(name.c_str(), (std::string("GameData/Pawn/Pawns/") + name + ".png").c_str());
                    loaded++;
                    EditorLog("Loaded pawn def: %s", name.c_str());
                }
            }
            EditorLog("Loaded %d pawn definitions from %s (loose + packaged)", loaded, defsDir);
        } else {
            // Legacy fallback if no config files are available at all
            EditorLog("WARN: no pawn defs loose or packaged (%s), using hardcoded defaults", defsDir);
            {
                PawnDef d; d.name="Walker"; d.speed=1.5f; d.aggroRange=6.0f; d.attackRange=1.5f; d.damage=10.0f; d.maxHealth=100; ps.RegisterDef(d);
                PawnManagerAddPawn("Walker", "GameData/Pawn/Pawns/Walker.png");
            } {
                PawnDef d; d.name="Skaarj"; d.speed=2.5f; d.aggroRange=10.0f; d.attackRange=2.0f; d.damage=20.0f; d.maxHealth=150; ps.RegisterDef(d);
                PawnManagerAddPawn("Skaarj", "GameData/Pawn/Pawns/Skaarj.png");
            } {
                PawnDef d; d.name="Brute"; d.speed=1.0f; d.aggroRange=4.0f; d.attackRange=1.5f; d.damage=30.0f; d.maxHealth=250; ps.RegisterDef(d);
                PawnManagerAddPawn("Brute", "GameData/Pawn/Pawns/Brute.png");
            } {
                PawnDef d; d.name="Floater"; d.speed=1.2f; d.aggroRange=8.0f; d.attackRange=3.0f; d.damage=15.0f; d.maxHealth=80; ps.RegisterDef(d);
                PawnManagerAddPawn("Floater", "GameData/Pawn/Pawns/Floater.png");
            }
        }
    }

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

    // Suppress raylib's texture-not-found warnings from .obj material refs
    SetTraceLogLevel(LOG_ERROR);

    // Create model preview render texture
    g_previewRT = LoadRenderTexture(256, 256);

    EnableCursor();

    int LastClickTime = 0;
    bool DoubleClick = false;

    while (!WindowShouldClose())
    {
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
        bool _inViewport = (_mp.x >= (float)GetStatsSidebarWidth() && _mp.y >= 28.0f);

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

        // Vertex-edit picking (Phase C): click nearest vertex, Shift adds.

        if (g_editorPanels.animEditVerts && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            Vector2 mp = GetMousePosition();
            if (mp.x >= (float)GetStatsSidebarWidth() && mp.y >= 28.0f) {
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
        // Left-click: select entity (red highlight) â€” suppressed while editing verts.
        // The pick fires on RELEASE, not press, so holding LMB to drag a placement
        // ghost still works: a drag cancels the pick via the movement threshold.
        // Picking is deliberately independent of DrawModel â€” DrawModel only means
        // "a gizmo/placement ghost is drawn". Gating picks on it made the viewport
        // unselectable whenever a placement ghost was active with nothing selected
        // (e.g. after using the CSG toolbox), with no way to recover via Escape.
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            g_lbDown = true;
            g_lbDownPos = GetMousePosition();
            g_gizmoHistPushed = false;
            // Begin an intentional move only when the Move tool is active, or the
            // press lands on the current selection. A plain viewport click never
            // translates a selected entity.
            g_gizmoDrag = false;
            if (!g_suppressViewportDrag && g_sel.type != SelType::NONE &&
                !g_editorPanels.animEditVerts) {
                Vector2 mp = GetMousePosition();
                if (mp.x >= (float)GetStatsSidebarWidth() && mp.y >= 28.0f) {
                    // Handles win over the generic "press on the object" drag: if a
                    // handle was hit, the drag is constrained to that axis/plane.
                    if (Gizmo_BeginDrag(mp, OTEditor.MainCamera,
                                        g_editorPanels.currentToolMode)) {
                        g_gizmoDrag = true;
                        if (!g_gizmoHistPushed) {
                            HistoryPush();
                            g_gizmoHistPushed = true;
                        }
                    } else if (g_editorPanels.currentToolMode == 1) {
                        g_gizmoDrag = true;
                    } else if (g_editorPanels.currentToolMode == 0) {
                        // Cam mode keeps the old convenience: a press that lands on
                        // the selected object drags it. Scale/Rotate mode must NOT
                        // fall back to an unconstrained move.
                        EditorSelection probe;
                        if (EditorRaycastAt(mp, probe) &&
                            probe.type == g_sel.type && probe.index == g_sel.index)
                            g_gizmoDrag = true;
                    }
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
                if (!g_editorPanels.animEditVerts && g_placeMode != PlaceMode::TERRAIN &&
                    mp.x >= (float)GetStatsSidebarWidth() && mp.y >= 28.0f) {
                    EditorPickEntity();
                }
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
                if (mp.x >= (float)GetStatsSidebarWidth() && mp.y >= 28.0f) {
                    if (!g_terrainHistPushed) { HistoryPush(); g_terrainHistPushed = true; }
                    Ray ray = GetMouseRay(mp, OTEditor.MainCamera);
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
        if (IsMouseButtonPressed(MOUSE_RIGHT_BUTTON)) {
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
            bool _inVpRel = (_mp_rel.x >= (float)GetStatsSidebarWidth() && _mp_rel.y >= 28.0f);
            // Per-FACE surface pick, done BEFORE the entity pick so the menu can
            // offer "Surface Properties (N Selected)". Triangle-level rather than
            // AABB-level, so clicking the top of a wall selects the +Y face and
            // not the brush as a whole.
            const SurfaceFacePick spfPick = _inVpRel ? PickSurfaceFace(_mp_rel) : SurfaceFacePick{};
            // Pick without the left-click toggle so right-clicking the already
            // selected entity opens its menu instead of deselecting it. Gate on
            // the hit result â€” on a miss g_sel still holds the previous entity,
            // so a stale selection would otherwise pop a Delete menu onto
            // whatever the user right-clicked next (or on empty space).
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
                        }
                        // A Mesh or a CSG brush carries no usable collision of
                        // its own (the world is AABB-only and a placed prop has
                        // none at all), so offer to derive convex proxies for it.
                        if (g_sel.type == SelType::MESH || g_sel.type == SelType::BRUSH) {
                            AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
                            AppendMenuA(hMenu, MF_STRING, IDM_APPEND_AUTOCONVEX,
                                        "Append AutoConvex Collision");
                        }
                        if (!g_editorPanels.activeTexturePath.empty() &&
                            (g_sel.type == SelType::BRUSH || g_sel.type == SelType::MODEL)) {
                            AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
                            AppendMenuA(hMenu, MF_STRING, IDM_APPLY_TEXTURE, "Apply Texture to Surface");
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
                        else if (cmd == IDM_APPLY_TEXTURE) g_editorPanels.actionApplyTextureToSel = true;
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
        if (LastClickTime != 0){
            LastClickTime ++;
            if (LastClickTime == 30){ LastClickTime = 0; DoubleClick = false; }
        }

        // Mouse-controlled camera: only move when middle-mouse-button is held.
        // Leaves the cursor free for clicking Win32 panels.
        // MMB          = pan horizontal (X/Z)
        // Shift + MMB  = pan vertical (Y)
        // Alt + MMB    = orbit around target
        // Scroll wheel = dolly forward/backward
        {
            if (IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) {
                Vector2 delta = GetMouseDelta();
                float sensitivity = 0.1f;
                bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
                bool alt   = IsKeyDown(KEY_LEFT_ALT)   || IsKeyDown(KEY_RIGHT_ALT);

                if (shift) {
                    // Shift + MMB = pan up/down
                    OTEditor.MainCamera.position.y -= delta.y * sensitivity;
                    OTEditor.MainCamera.target.y -= delta.y * sensitivity;
                } else if (alt) {
                    // Alt + MMB = orbit around target
                    Vector3 v = Vector3Subtract(OTEditor.MainCamera.position, OTEditor.MainCamera.target);
                    float radius = Vector3Length(v);
                    if (radius > 0.01f) {
                        Vector2 ang;
                        ang.x = atan2f(v.x, v.z); // yaw
                        ang.y = asinf(v.y / radius); // pitch
                        ang.x -= delta.x * 0.01f;
                        ang.y += delta.y * 0.01f;
                        if (ang.y > PI/2 - 0.01f) ang.y = PI/2 - 0.01f;
                        if (ang.y < -PI/2 + 0.01f) ang.y = -PI/2 + 0.01f;
                        v.x = radius * cosf(ang.y) * sinf(ang.x);
                        v.y = radius * sinf(ang.y);
                        v.z = radius * cosf(ang.y) * cosf(ang.x);
                        OTEditor.MainCamera.position = Vector3Add(OTEditor.MainCamera.target, v);
                    }
                } else {
                    // Plain MMB = pan X/Z
                    OTEditor.MainCamera.position.x -= delta.x * sensitivity;
                    OTEditor.MainCamera.position.z -= delta.y * sensitivity;
                    OTEditor.MainCamera.target.x -= delta.x * sensitivity;
                    OTEditor.MainCamera.target.z -= delta.y * sensitivity;
                }
            }

            // Scroll wheel = dolly forward/backward.
            // In MODEL placement with Scale/Rotate tool modes the wheel is
            // reserved for scaling/rotating the ghost instead.
            bool wheelReservedForTool =
                (g_placeMode == PlaceMode::MODEL && OmegaTechEditor.DrawModel &&
                 g_editorPanels.currentToolMode != 0);
            if (!wheelReservedForTool) {
                float wheel = GetMouseWheelMove();
                if (wheel != 0) {
                    Vector3 dir = Vector3Normalize(Vector3Subtract(OTEditor.MainCamera.target, OTEditor.MainCamera.position));
                    OTEditor.MainCamera.position.x += dir.x * wheel * 2.0f;
                    OTEditor.MainCamera.position.y += dir.y * wheel * 2.0f;
                    OTEditor.MainCamera.position.z += dir.z * wheel * 2.0f;
                    OTEditor.MainCamera.target.x += dir.x * wheel * 2.0f;
                    OTEditor.MainCamera.target.y += dir.y * wheel * 2.0f;
                    OTEditor.MainCamera.target.z += dir.z * wheel * 2.0f;
                }
            }
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
// Level state (GameType / skybox / particles) no longer has a per-frame apply path.
        // It used to: the removed Zone Properties window set one-shot apply flags on a
        // global, and the next frame this block noticed them and wrote LevelMetadata.
        // The Map row applies level state directly in ApplyMapProperties(), so the
        // flags, the global and this block all went together.

        // Submit lights and viewPos for Lit mode
        if (OTEditor.ViewMode == LightingMode::LIT && OTEditor.LitFogShader.id > 0) {
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
                oz::SurfaceMaterial::Instance().SetFog(fogCol, fogStart, fogEnd,
                                                       fogDensity, fogIntensity);
                oz::SurfaceMaterial::Instance().UpdateFrame(pawnLights, OTEditor.MainCamera, dt, amb);
            }
            if (OTEditor.ViewPosLoc >= 0) {
                Vector3 camPos = OTEditor.MainCamera.position;
                SetShaderValue(OTEditor.LitFogShader, OTEditor.ViewPosLoc, &camPos, SHADER_UNIFORM_VEC3);
            }
        }

        ClearBackground(BLACK);

        // Apply lighting mode before 3D rendering
        rlDrawRenderBatchActive();
        if (OTEditor.ViewMode == LightingMode::WIREFRAME) {
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            glDisable(GL_CULL_FACE);
        } else {
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            glEnable(GL_CULL_FACE);
        }

        BeginMode3D(OTEditor.MainCamera);

        // -------------------------------------------------------------------
        // Skybox cube (viewport backdrop) â€” toggled by the toolbar "Sky" button.
        // Mirrors the client: top/bottom use the cap texture, sides reuse it
        // unless a dedicated side skybox is authored.
        // -------------------------------------------------------------------
        if (OTEditor.ShowSkybox) {
            static Texture2D s_skyTex = {0};
            static std::string s_skyPath;

            // Resolve the skybox texture: an explicitly-applied levelinfo skybox
            // path takes priority; otherwise fall back to the world's
            // Models/Skybox.png default.
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

            if (s_skyTex.id > 0) {
                // The cube itself - face geometry, per-face rotations, distance,
                // fog and the draw loop - is oz::Skybox's, shared with the client.
                // This block used to carry its own copy of all of it at radius
                // 1000 with no fog, so the editor viewport showed a sky at a
                // different angular size than the game and could not show the
                // horizon fog the player actually gets. Both were the same bug in
                // two places.
                //
                // The fog is the world's, read from the same owner the client uses
                // (OzoneLoader, because raylib has no GetShaderValue).
                oz::SkyboxFog fog;
                fog.active = true;
                OzoneLoader::Instance().GetWorldFog(fog.color, fog.start, fog.end,
                                                    fog.density, fog.intensity);

                // No side texture in the editor: passing {0} makes oz::Skybox fall
                // back to the cap, which is what the old drawFace calls did by
                // passing s_skyTex for all six faces.
                oz::Skybox::Instance().Draw(
                    OTEditor.MainCamera.position, s_skyTex, Texture2D{0},
                    /*cullBackfaces=*/OTEditor.ViewMode != LightingMode::WIREFRAME,
                    &fog);
            }
        }

        DrawGrid(1000, 10.0f);

        // OZONE world geometry
        OzoneLoader::Instance().Draw(OTEditor.MainCamera);

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
        OzoneLoader::Instance().DrawGlowGeometry(OTEditor.MainCamera);

        // Pawn system (use lit shader for billboards when in lit/unlit mode)
        {
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
        if (OmegaTechEditor.DrawModel)
        {
            float px = OmegaTechEditor.X, py = OmegaTechEditor.Y, pz = OmegaTechEditor.Z;
            float ps = OmegaTechEditor.S, pr = OmegaTechEditor.R;

            if (g_placeMode == PlaceMode::MODEL) {
                if (EMID >= 200) {
                    // OZONE primitive ghost
                    int primType = EMID - 200;
                    Vector3 size = {OmegaTechEditor.W * ps, OmegaTechEditor.H * ps, OmegaTechEditor.L * ps};
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
                    // User-selected model from the Model Browser â€” ghost preview.
                    int bidx = g_editorPanels.selectedModel;
                    if (bidx >= 0 && bidx < (int)g_editorPanels.modelEntries.size()) {
                        static Model s_browsePreview = {0};
                        static std::string s_browsePreviewPath;
                        const std::string& path = g_editorPanels.modelEntries[bidx].path;
                        if (s_browsePreviewPath != path) {
                            if (s_browsePreview.meshes) UnloadModel(s_browsePreview);
                            s_browsePreview = {0};
                            s_browsePreviewPath = path;
                            s_browsePreview = LoadModelWithFallback(path.c_str());
                        }
                        if (s_browsePreview.meshes)
                            DrawModelEx(s_browsePreview, {px,py,pz},{0,1,0},pr,{ps,ps,ps},WHITE);
                        else
                            DrawCubeWires({px,py,pz}, ps, ps, ps, ORANGE);
                    }
                } else if (EMID == -1) {
                    DrawBoundingBox((BoundingBox){{px,py,pz},{OmegaTechEditor.W,OmegaTechEditor.H,OmegaTechEditor.L}}, ORANGE);
                    DrawCubeWires({OmegaTechEditor.W,OmegaTechEditor.H,OmegaTechEditor.L}, ps, ps, ps, PINK);
                } else if (EMID == -2) {
                    DrawBoundingBox((BoundingBox){{px,py,pz},{OmegaTechEditor.W,OmegaTechEditor.H-5,OmegaTechEditor.L}}, ORANGE);
                    DrawCubeWires({OmegaTechEditor.W,OmegaTechEditor.H-5,OmegaTechEditor.L}, ps, ps, ps, PINK);
                }
            } else if (g_placeMode == PlaceMode::PICKUP) {
                // Draw pickup preview from model (fallback to colored cube)
                static Model g_pickupPreviewModel = {0};
                static std::string g_pickupPreviewName;
                if (g_pickupPreviewName != OmegaTechEditor.ActivePickupName) {
                    if (g_pickupPreviewModel.meshes) UnloadModel(g_pickupPreviewModel);
                    g_pickupPreviewModel = {0};
                    g_pickupPreviewName = OmegaTechEditor.ActivePickupName;
                    const EntityDef* edef = LightningEntityRegistry::Instance().Find(OmegaTechEditor.ActivePickupName);
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
                    const EntityDef* edef = LightningEntityRegistry::Instance().Find(OmegaTechEditor.ActivePickupName);
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
                switch (OmegaTechEditor.ActiveNodeType) {
                    case EditorNodeType::SPAWN:  c = BLUE;    break;
                    case EditorNodeType::NPC:    c = MAGENTA; break;
                    case EditorNodeType::LIGHT:  c = YELLOW;  break;
                    case EditorNodeType::ZONE:   c = SKYBLUE; break;
                    case EditorNodeType::PORTAL: c = PURPLE;  break;
                }
                if (OmegaTechEditor.ActiveNodeType == EditorNodeType::PORTAL) {
                    // Portal preview: full volume wireframe + swirl marker
                    float hw = fmaxf(OmegaTechEditor.W, 1) * 0.5f;
                    float hh = fmaxf(OmegaTechEditor.H, 1) * 0.5f;
                    float hd = fmaxf(OmegaTechEditor.L, 1) * 0.5f;
                    DrawCubeWires({px, py, pz}, hw * 2, hh * 2, hd * 2, c);
                    DrawCube({px, py, pz}, hw * 2, hh * 2, hd * 2, (Color){c.r,c.g,c.b,40});
                    DrawSphere({px, py + hh, pz}, 0.3f, c);
                } else if (OmegaTechEditor.ActiveNodeType == EditorNodeType::ZONE) {
                    float hw = fmaxf(OmegaTechEditor.W, 1) * 0.5f;
                    float hh = fmaxf(OmegaTechEditor.H, 1) * 0.5f;
                    float hd = fmaxf(OmegaTechEditor.L, 1) * 0.5f;
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

            // Movement controls
            // Keyboard nudge/rotate/scale of a selection is an undoable edit.
            if (g_sel.type != SelType::NONE && !g_editorPanels.animEditVerts &&
                EditorUi_ViewportHasFocus() &&
                (IsKeyPressed(KEY_U) || IsKeyPressed(KEY_J) || IsKeyPressed(KEY_H) ||
                 IsKeyPressed(KEY_K) || IsKeyPressed(KEY_Y) || IsKeyPressed(KEY_I) ||
                 IsKeyPressed(KEY_O) || IsKeyPressed(KEY_L) ||
                 IsKeyPressed(KEY_T) || IsKeyPressed(KEY_B))) {
                HistoryPush();
            }
            if (!IsMouseButtonDown(2)) {
                if (IsKeyPressed(KEY_U)) OmegaTechEditor.X -= 2.0f;
                if (IsKeyPressed(KEY_J)) OmegaTechEditor.X += 2.0f;
                if (IsKeyPressed(KEY_H)) OmegaTechEditor.Z += 2.0f;
                if (IsKeyPressed(KEY_K)) OmegaTechEditor.Z -= 2.0f;
                if (IsKeyPressed(KEY_Y)) OmegaTechEditor.Y -= 2.0f;
                if (IsKeyPressed(KEY_I)) OmegaTechEditor.Y += 2.0f;
            } else {
                if (IsKeyPressed(KEY_U)) OmegaTechEditor.W -= 2.0f;
                if (IsKeyPressed(KEY_J)) OmegaTechEditor.W += 2.0f;
                if (IsKeyPressed(KEY_H)) OmegaTechEditor.L += 2.0f;
                if (IsKeyPressed(KEY_K)) OmegaTechEditor.L -= 2.0f;
                if (IsKeyPressed(KEY_Y)) OmegaTechEditor.H -= 2.0f;
                if (IsKeyPressed(KEY_I)) OmegaTechEditor.H += 2.0f;
            }

            if (IsKeyPressed(KEY_O)) OmegaTechEditor.R += 90.0f;
            if (IsKeyPressed(KEY_L)) OmegaTechEditor.R -= 90.0f;
            if (IsKeyDown(KEY_T)) OmegaTechEditor.S += 0.5f;
            // NOTE: scale-down is B, not G â€” G cycles the CSG op in MODEL mode.
            if (IsKeyDown(KEY_B)) OmegaTechEditor.S -= 0.5f;

            // Vertex-edit transform (Phase C): the movement/rotate keys deform
            // the selected vertices instead of moving the mesh node.
            if (g_editorPanels.animEditVerts) {
                MeshObjectNode* vn = PawnSystem::Instance().GetMeshObject(g_editorPanels.animTargetMesh);
                oz::AnimatedMesh* vam = AnimTarget();
                if (!g_editorPanels.animPrevValid) {
                    g_editorPanels.animPrevX = OmegaTechEditor.X;
                    g_editorPanels.animPrevY = OmegaTechEditor.Y;
                    g_editorPanels.animPrevZ = OmegaTechEditor.Z;
                    g_editorPanels.animPrevR = OmegaTechEditor.R;
                    g_editorPanels.animPrevValid = true;
                }
                float vdx = OmegaTechEditor.X - g_editorPanels.animPrevX;
                float vdy = OmegaTechEditor.Y - g_editorPanels.animPrevY;
                float vdz = OmegaTechEditor.Z - g_editorPanels.animPrevZ;
                float vdr = OmegaTechEditor.R - g_editorPanels.animPrevR;
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
                OmegaTechEditor.X = g_editorPanels.animPrevX;
                OmegaTechEditor.Y = g_editorPanels.animPrevY;
                OmegaTechEditor.Z = g_editorPanels.animPrevZ;
                OmegaTechEditor.R = g_editorPanels.animPrevR;
            } else {
                g_editorPanels.animPrevValid = false;
            }

            // Sync gizmo position back to selected entity (for manipulation)
            if (g_sel.type != SelType::NONE && !g_editorPanels.animEditVerts) {
                Vector3 newPos = {OmegaTechEditor.X, OmegaTechEditor.Y, OmegaTechEditor.Z};
                int idx = g_sel.index;
                if (g_sel.type == SelType::NPC) {
                    Pawn* p = PawnSystem::Instance().Get(idx);
                    if (p) p->position = newPos;
                } else if (g_sel.type == SelType::PICKUP) {
                    auto& pickups = PawnSystem::Instance().GetPickups();
                    for (auto& pk : pickups) {
                        if ((int)pk.id == idx) { pk.position = newPos; break; }
                    }
                } else if (g_sel.type == SelType::BRUSH) {
                    // idx is a renderable index (see EditorRaycastAt), so there is no
                    // collision-volume fallback to try. The old code guessed with
                    // `idx < Count()` and otherwise went looking for a matching
                    // collision volume, which is how dragging a brush could move a
                    // different one.
                    const int rIdx = (OzoneLoader::Instance().Get(idx) != nullptr) ? idx : -1;
                    if (rIdx >= 0) {
                        OzoneRenderable* r = OzoneLoader::Instance().Get(rIdx);
                        if (r) {
                            BoundingBox mb = GetMeshBoundingBox(r->model.meshes[0]);
                            Vector3 center = {r->position.x + (mb.min.x + mb.max.x) * 0.5f * r->scale,
                                              r->position.y + (mb.min.y + mb.max.y) * 0.5f * r->scale,
                                              r->position.z + (mb.min.z + mb.max.z) * 0.5f * r->scale};
                            Vector3 delta = {r->position.x - center.x,
                                             r->position.y - center.y,
                                             r->position.z - center.z};
                            r->position = {newPos.x + delta.x, newPos.y + delta.y, newPos.z + delta.z};
                            // Scale/Rotate tool modes write back to the renderable
                            if (g_editorPanels.currentToolMode == 2) r->scale = OmegaTechEditor.S;
                            if (g_editorPanels.currentToolMode == 3) r->rotation = OmegaTechEditor.R * DEG2RAD;
                        }
                    }
                    OzoneLoader::Instance().RebuildCollisionVolumes();
                } else if (g_sel.type == SelType::LIGHT) {
                    LightNode* l = PawnSystem::Instance().GetLight(idx);
                    if (l) l->position = newPos;
                } else if (g_sel.type == SelType::SPAWN) {
                    if (PlayerStartNode* s = FindPlayerStartById(idx))
                        s->position = newPos;
                } else if (g_sel.type == SelType::PORTAL) {
                    auto& portals = ZoneManager::Instance().GetPortals();
                    if (idx >= 0 && idx < (int)portals.size()) {
                        auto& p = portals[idx];
                        Vector3 center = {(p.bounds.min.x + p.bounds.max.x) * 0.5f,
                                          (p.bounds.min.y + p.bounds.max.y) * 0.5f,
                                          (p.bounds.min.z + p.bounds.max.z) * 0.5f};
                        Vector3 delta = {newPos.x - center.x, newPos.y - center.y, newPos.z - center.z};
                        p.bounds.min.x += delta.x; p.bounds.min.y += delta.y; p.bounds.min.z += delta.z;
                        p.bounds.max.x += delta.x; p.bounds.max.y += delta.y; p.bounds.max.z += delta.z;
                    }
                } else if (g_sel.type == SelType::ZONE) {
                    // Bounds-centred, like PORTAL. This branch was missing, so the
                    // Move tool changed the gizmo and then wrote nothing: a zone
                    // could only be moved through the Properties panel.
                    auto& zones = ZoneManager::Instance().GetZones();
                    for (auto& z : zones) {
                        if ((int)z.id != idx) continue;
                        Vector3 center = {(z.bounds.min.x + z.bounds.max.x) * 0.5f,
                                          (z.bounds.min.y + z.bounds.max.y) * 0.5f,
                                          (z.bounds.min.z + z.bounds.max.z) * 0.5f};
                        Vector3 delta = {newPos.x - center.x, newPos.y - center.y, newPos.z - center.z};
                        z.bounds.min.x += delta.x; z.bounds.min.y += delta.y; z.bounds.min.z += delta.z;
                        z.bounds.max.x += delta.x; z.bounds.max.y += delta.y; z.bounds.max.z += delta.z;
                        break;
                    }
                } else if (g_sel.type == SelType::MESH) {
                    if (MeshObjectNode* m = PawnSystem::Instance().GetMeshObject(idx)) {
                        m->yaw = OmegaTechEditor.R;   // UI works in degrees
                        if (g_editorPanels.currentToolMode == 2 && OmegaTechEditor.S > 0.01f)
                            m->scale = OmegaTechEditor.S;
                        // The gizmo / raycast store the model's world CENTRE, but
                        // m->position is the model's ORIGIN (often the feet). Writing
                        // the centre straight into position shifted every model by
                        // its own local offset the moment it was selected.
                        if (m->mesh && m->mesh->Valid()) {
                            const BoundingBox& mb = m->mesh->Bounds();
                            const float lx = (mb.min.x + mb.max.x) * 0.5f * m->scale;
                            const float ly = (mb.min.y + mb.max.y) * 0.5f * m->scale;
                            const float lz = (mb.min.z + mb.max.z) * 0.5f * m->scale;
                            const float c = cosf(m->yaw * DEG2RAD);
                            const float s = sinf(m->yaw * DEG2RAD);
                            const Vector3 offset = { lx * c + lz * s, ly, -lx * s + lz * c };
                            m->position = {newPos.x - offset.x, newPos.y - offset.y,
                                           newPos.z - offset.z};
                        } else if (g_gizmoDrag) {
                            // Unknown local offset (mesh still resolving): only
                            // an explicit drag may write the centre. Doing it on
                            // every frame made a texture reload shift the prop.
                            m->position = newPos;
                        }
                    }
                } else if (g_sel.type == SelType::PARTICLE) {
                    if (ParticleEmitterNode* e = PawnSystem::Instance().GetParticleEmitter(idx)) {
                        e->position = newPos;
                        e->yaw = OmegaTechEditor.R;
                    }
                } else if (g_sel.type == SelType::PATHNODE) {
                    if (PathNode* pn = PawnSystem::Instance().GetPathNode(idx))
                        pn->position = newPos;
                } else if (g_sel.type == SelType::WINDZONE) {
                    if (WindZoneNode* z = PawnSystem::Instance().GetWindZone(idx)) {
                        Vector3 center = {(z->bounds.min.x + z->bounds.max.x) * 0.5f,
                                          (z->bounds.min.y + z->bounds.max.y) * 0.5f,
                                          (z->bounds.min.z + z->bounds.max.z) * 0.5f};
                        Vector3 delta = {newPos.x - center.x, newPos.y - center.y, newPos.z - center.z};
                        z->bounds.min.x += delta.x; z->bounds.min.y += delta.y; z->bounds.min.z += delta.z;
                        z->bounds.max.x += delta.x; z->bounds.max.y += delta.y; z->bounds.max.z += delta.z;
                    }
                } else if (g_sel.type == SelType::MODEL) {
                    if (idx >= 0 && idx < CachedModelCounter) {
                        CachedModels[idx].X = newPos.x;
                        CachedModels[idx].Y = newPos.y;
                        CachedModels[idx].Z = newPos.z;
                        CachedModels[idx].R = OmegaTechEditor.R;
                        if (g_editorPanels.currentToolMode == 2 && OmegaTechEditor.S > 0.01f)
                            CachedModels[idx].S = OmegaTechEditor.S;
                    }
                }
                // Update selection stored position
                g_sel.pos = newPos;
            }

            // Commit placement
            if (IsKeyPressed(KEY_ENTER) || DoubleClick)
            {
                bool willCommit =
                    (g_placeMode == PlaceMode::PICKUP) ||
                    (g_placeMode == PlaceMode::NODE) ||
                    (g_placeMode == PlaceMode::MODEL &&
                     ((EMID == 0 && g_editorPanels.selectedModel >= 0) || EMID >= 200));
                if (willCommit) HistoryPush();
                if (g_placeMode == PlaceMode::MODEL) {
                    if (EMID > 0) {
                        // Legacy WDL Model/Collision placement is removed; OZONE
                        // primitives (EMID >= 200) are committed below.
                    } else {
                        if (EMID == 0) {
                            // Model Browser selection â†’ GameEngine.Mesh.* entity.
                            int bidx = g_editorPanels.selectedModel;
                            if (bidx >= 0 && bidx < (int)g_editorPanels.modelEntries.size()) {
                                const std::string& path = g_editorPanels.modelEntries[bidx].path;
                                std::string ext;
                                size_t dot = path.rfind('.');
                                if (dot != std::string::npos) {
                                    ext = path.substr(dot);
                                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                                }
                                MeshObjectNode node;
                                node.meshPath = path;
                                node.skeletal = (ext == ".glb" || ext == ".gltf" || ext == ".iqm");
                                node.position = {OmegaTechEditor.X, OmegaTechEditor.Y, OmegaTechEditor.Z};
                                node.yaw = OmegaTechEditor.R;
                                node.scale = OmegaTechEditor.S;
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
                    node.position = {OmegaTechEditor.X, OmegaTechEditor.Y, OmegaTechEditor.Z};
                    node.typeName = OmegaTechEditor.ActivePickupName;
                    PawnSystem::Instance().AddPickup(node);
                    EditorLog("Placed pickup '%s'", node.typeName.c_str());
                } else if (g_placeMode == PlaceMode::NODE) {
                    if (OmegaTechEditor.ActiveNodeType == EditorNodeType::ZONE) {
                        float hw = fmaxf(OmegaTechEditor.W, 1) * 0.5f;
                        float hh = fmaxf(OmegaTechEditor.H, 1) * 0.5f;
                        float hd = fmaxf(OmegaTechEditor.L, 1) * 0.5f;
                        float minX = OmegaTechEditor.X - hw, maxX = OmegaTechEditor.X + hw;
                        float minY = OmegaTechEditor.Y - hh, maxY = OmegaTechEditor.Y + hh;
                        float minZ = OmegaTechEditor.Z - hd, maxZ = OmegaTechEditor.Z + hd;
                        ZoneVolumeNode node;
                        node.bounds = {{minX, minY, minZ}, {maxX, maxY, maxZ}};
                        node.zoneType = ZoneType::ZONE_WATER;
                        node.intensity = 1.0f;
                        ZoneManager::Instance().AddZone(node);
                    } else if (OmegaTechEditor.ActiveNodeType == EditorNodeType::PORTAL) {
                        float hw = fmaxf(OmegaTechEditor.W, 1) * 0.5f;
                        float hh = fmaxf(OmegaTechEditor.H, 1) * 0.5f;
                        float hd = fmaxf(OmegaTechEditor.L, 1) * 0.5f;
                        float minX = OmegaTechEditor.X - hw, maxX = OmegaTechEditor.X + hw;
                        float minY = OmegaTechEditor.Y - hh, maxY = OmegaTechEditor.Y + hh;
                        float minZ = OmegaTechEditor.Z - hd, maxZ = OmegaTechEditor.Z + hd;
                        std::string target(g_editorPanels.portalTargetWorld);
                        if (target.empty()) target = "EngineTest";
                        ZonePortal portal;
                        portal.bounds = {{minX, minY, minZ}, {maxX, maxY, maxZ}};
                        portal.targetWorld = target;
                        portal.targetSpawn = {0, 20, 0};
                        portal.bidirectional = true;
                        ZoneManager::Instance().AddPortal(portal);
                        RefreshLevelList();
                    } else if (OmegaTechEditor.ActiveNodeType == EditorNodeType::SPAWN) {
                        PlayerStartNode node;
                        node.position = {OmegaTechEditor.X, OmegaTechEditor.Y, OmegaTechEditor.Z};
                        node.yaw = OmegaTechEditor.R;
                        PawnSystem::Instance().AddPlayerStart(node);
                    } else if (OmegaTechEditor.ActiveNodeType == EditorNodeType::NPC) {
                        PawnSystem::Instance().Spawn(
                            {OmegaTechEditor.X, OmegaTechEditor.Y, OmegaTechEditor.Z}, "Walker");
                    } else if (OmegaTechEditor.ActiveNodeType == EditorNodeType::LIGHT) {
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
                        node.position = {OmegaTechEditor.X, OmegaTechEditor.Y, OmegaTechEditor.Z};
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

OmegaTechEditor.DrawModel = false;
                // This whole block bypasses the event bus and mutates PawnSystem /
                // ZoneManager directly, so it has to flag the WorldGraph itself.
                MarkWorldGraphDirty();

                // CSG: register brush renderable for OZONE primitives (EMID >= 200).
                // Collision comes from OzoneLoader::RebuildCollisionVolumes(), which
                // runs its own CsgProcessor over every renderable's stored csgOp.
                if (EMID >= 200 && g_placeMode == PlaceMode::MODEL) {
                    int primType = EMID - 200;
                    Vector3 center = {OmegaTechEditor.X, OmegaTechEditor.Y, OmegaTechEditor.Z};
                    Vector3 size = {OmegaTechEditor.W, OmegaTechEditor.H, OmegaTechEditor.L};
                    int ridx = CommitBrushRenderable(
                        primType, center, size, OmegaTechEditor.R, OmegaTechEditor.S,
                        (int)OmegaTechEditor.CSGOperation);
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
                if (wheel != 0) OmegaTechEditor.S += wheel * 0.25f;
            } else if (g_editorPanels.currentToolMode == 3) {
                float wheel = GetMouseWheelMove();
                if (wheel != 0) OmegaTechEditor.R += wheel * 15.0f;
            } else if (IsMouseButtonDown(0)) {
                // A selected entity moves only for an intentional gesture; a
                // placement ghost (no selection) still drags freely. Suppressed
                // entirely while the post-context-menu input guard is active.
                bool allowMove = !g_suppressViewportDrag &&
                    (g_sel.type == SelType::NONE || g_gizmoDrag);
                if (allowMove) {
                    if (g_sel.type != SelType::NONE && !g_gizmoHistPushed) {
                        HistoryPush();
                        g_gizmoHistPushed = true;
                    }
                    // Camera-relative fallback for a free drag (ghost or Cam-mode
                    // object drag). The old X += dx/8 was screen-space and did not
                    // follow the cursor; this moves along the camera's right/up
                    // vectors, scaled by distance so it stays 1:1 on screen.
                    if (g_sel.type == SelType::NONE) {
                        OmegaTechEditor.X += GetMouseDelta().x / 8;
                        OmegaTechEditor.Y += GetMouseDelta().y / 8;
                        OmegaTechEditor.Z -= (GetMouseWheelMove() * 2);
                    } else {
                        Vector3 fwd = Vector3Normalize(Vector3Subtract(
                            OTEditor.MainCamera.target, OTEditor.MainCamera.position));
                        Vector3 right = Vector3Normalize(Vector3CrossProduct(fwd, {0, 1, 0}));
                        Vector3 up = Vector3Normalize(Vector3CrossProduct(right, fwd));
                        const float dist = Vector3Distance(OTEditor.MainCamera.position,
                                                           {OmegaTechEditor.X, OmegaTechEditor.Y, OmegaTechEditor.Z});
                        const float k = dist * 0.0016f;
                        const Vector2 d = GetMouseDelta();
                        OmegaTechEditor.X += (right.x * d.x - up.x * d.y) * k;
                        OmegaTechEditor.Y += (right.y * d.x - up.y * d.y) * k;
                        OmegaTechEditor.Z += (right.z * d.x - up.z * d.y) * k;
                    }
                }
            }
            if (IsMouseButtonDown(1) && !g_suppressViewportDrag) {
                OmegaTechEditor.W += GetMouseDelta().x / 8;
                OmegaTechEditor.H += GetMouseDelta().y / 8;
                OmegaTechEditor.L -= (GetMouseWheelMove() * 2);
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
                    BoundingBox mb = GetMeshBoundingBox(r->model.meshes[0]);
                    const AabbBox local{mb.min.x, mb.min.y, mb.min.z,
                                        mb.max.x, mb.max.y, mb.max.z};
                    // Rotation-aware, matching what collision now uses, so the box
                    // always contains the drawn (possibly rotated) brush.
                    const AabbBox w = RotatedAabbY(local, r->rotation,
                                                   r->position.x, r->position.y,
                                                   r->position.z, r->scale);
                    return {{w.minX, w.minY, w.minZ}, {w.maxX, w.maxY, w.maxZ}};
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

        // Transform gizmo for the current selection. Drawn AFTER the highlight so
        // the handles are never hidden by the selection box.
        DrawTransformGizmo(OTEditor.MainCamera, g_editorPanels.currentToolMode);

        // Terrain-following camera
        {
            float gy = SampleHeightmapGroundY(OTEditor.MainCamera.position.x, OTEditor.MainCamera.position.z);
            if (gy > -50000.0f)
                OTEditor.MainCamera.position.y = gy + 2.0f;
        }

        EndMode3D();

        // Reset lighting mode state (always restore defaults)
        rlDrawRenderBatchActive();
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glEnable(GL_CULL_FACE);

        BeginDrawing();
        // Top toolbar with icons
        {
            int tw = GetScreenWidth();
            int tbH = 28;
            DrawRectangle(0, 0, tw, tbH, (Color){35, 35, 40, 220});
            DrawLine(0, tbH, tw, tbH, (Color){60, 60, 70, 255});

            auto& ico = EditorIcons::Instance();
            int bx = 4, iy = 4, is = 20;

            auto tBtn = [&](const char* icon, const char* label, int cmd, bool active = false) {
                int lw = (label ? (int)strlen(label) * 7 + 4 : 0);
                int bw = (icon ? 24 : 0) + lw + 4;
                Rectangle r = {(float)bx, 2, (float)bw, (float)tbH - 4};
                bool hover = CheckCollisionPointRec(GetMousePosition(), r);
                DrawRectangleRec(r, active ? (Color){70,90,120,255}
                                           : hover ? (Color){55,55,65,255}
                                                   : (Color){45,45,50,255});
                if (icon && ico.Has(icon)) ico.Draw(icon, bx+2, iy, is, WHITE);
                if (label) DrawText(label, bx+(icon?26:4), 7, 12, active ? WHITE : LIGHTGRAY);
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
                Rectangle r = {(float)bx, 2, (float)bw, (float)tbH - 4};
                bool hover = CheckCollisionPointRec(GetMousePosition(), r);
                DrawRectangleRec(r, hover ? (Color){50,100,50,255} : (Color){35,80,35,255});
                DrawText(lbl, bx + 4, 7, 12, WHITE);
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
                DrawRectangle(bx,2,lw,tbH-4,lc);
                DrawText(labs[li],bx+4,7,12,(int)OTEditor.ViewMode==li?WHITE:LIGHTGRAY);
                if(CheckCollisionPointRec(GetMousePosition(),{(float)bx,2,(float)lw,(float)tbH-4})&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
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
                DrawRectangle(bx, 2, lw, tbH-4, lc);
                DrawText("Sky", bx+6, 7, 12, on ? WHITE : LIGHTGRAY);
                if (CheckCollisionPointRec(GetMousePosition(), {(float)bx, 2, (float)lw, (float)tbH-4}) &&
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
            bx += 6;
            // Terrain editing buttons
            bool terrainMode = (g_placeMode == PlaceMode::TERRAIN);
            Color terrainColor = terrainMode ? (Color){90,70,50,255} : (Color){45,45,50,255};
            int tbW = 76;
            DrawRectangle(bx, 2, tbW, tbH-4, terrainColor);
            if (ico.Has("BBTerrain")) ico.Draw("BBTerrain", bx+3, 4, 20, WHITE);
            DrawText("Raise", bx+27, 7, 12, terrainMode ? WHITE : LIGHTGRAY);
            if (CheckCollisionPointRec(GetMousePosition(), {(float)bx, 2, (float)tbW, (float)tbH-4}) && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                g_placeMode = PlaceMode::TERRAIN;
                g_editorPanels.terrainBrushMode = 0; // raise
            }
            bx += tbW + 2;
            DrawRectangle(bx, 2, tbW, tbH-4, terrainColor);
            if (ico.Has("BBTerrain")) ico.Draw("BBTerrain", bx+3, 4, 20, WHITE);
            DrawText("Lower", bx+27, 7, 12, terrainMode ? WHITE : LIGHTGRAY);
            if (CheckCollisionPointRec(GetMousePosition(), {(float)bx, 2, (float)tbW, (float)tbH-4}) && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                g_placeMode = PlaceMode::TERRAIN;
                g_editorPanels.terrainBrushMode = 1; // lower
            }
            bx += tbW + 2;
            // Brush size adjustment buttons
            char sizeTxt[32];
            snprintf(sizeTxt, sizeof(sizeTxt), "Sz:%d", g_editorPanels.terrainBrushSize);
            DrawText(sizeTxt, bx+4, 7, 12, LIGHTGRAY);
            int szW = (int)strlen(sizeTxt) * 7 + 10;
            Rectangle szR = {(float)bx, 2, (float)szW, (float)tbH-4};
            if (CheckCollisionPointRec(GetMousePosition(), szR)) {
                if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
                    g_editorPanels.terrainBrushSize = (g_editorPanels.terrainBrushSize % 10) + 1;
            }
            bx += szW + 2;
            // Brush strength
            char strTxt[32];
            snprintf(strTxt, sizeof(strTxt), "Str:%.1f", g_editorPanels.terrainBrushStrength);
            DrawText(strTxt, bx+4, 7, 12, LIGHTGRAY);
            int stW = (int)strlen(strTxt) * 7 + 10;
            Rectangle stR = {(float)bx, 2, (float)stW, (float)tbH-4};
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
            // clicking cycles Off / 0.25 / 0.5 / 1 / 2. Ctrl still forces 1.0 and
            // Shift forces 0.1 while dragging.
            {
                const float steps[] = {0.0f, 0.25f, 0.5f, 1.0f, 2.0f};
                char snapTxt[32];
                if (g_editorPanels.gizmoSnap <= 0.0f)
                    snprintf(snapTxt, sizeof(snapTxt), "Snap:Off");
                else
                    snprintf(snapTxt, sizeof(snapTxt), "Snap:%.2g", g_editorPanels.gizmoSnap);
                int sw = (int)strlen(snapTxt) * 7 + 10;
                bool on = g_editorPanels.gizmoSnap > 0.0f;
                DrawRectangle(bx, 2, sw, tbH - 4, on ? (Color){70,90,120,255} : (Color){45,45,50,255});
                DrawText(snapTxt, bx + 4, 7, 12, on ? WHITE : LIGHTGRAY);
                if (CheckCollisionPointRec(GetMousePosition(),
                                           {(float)bx, 2, (float)sw, (float)tbH - 4}) &&
                    IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                    int cur = 0;
                    for (int i = 0; i < 5; i++)
                        if (fabsf(steps[i] - g_editorPanels.gizmoSnap) < 0.001f) cur = i;
                    g_editorPanels.gizmoSnap = steps[(cur + 1) % 5];
                }
                bx += sw + 2;
            }
        }

        // Draw the 3D viewport render target (offset by native stats sidebar)
        int sbW = GetStatsSidebarWidth();
        const int tbH = 28;
        LayoutStatsSidebar(GetScreenWidth(), GetScreenHeight(), tbH, sbW);
        {
            const char* modeStr =
                g_placeMode == PlaceMode::MODEL ? "MODEL" :
                g_placeMode == PlaceMode::PICKUP ? "PICKUP" :
                g_placeMode == PlaceMode::NODE ? "NODE" : "ENV";
            int volCount = (int)OzoneLoader::Instance().GetCollisionVolumes().size();
            int chunkCount = OzoneLoader::Instance().GetChunkManager().CellCount();
            UpdateStatsSidebar(
                OmegaTechEditor.X, OmegaTechEditor.Y, OmegaTechEditor.Z,
                OmegaTechEditor.W, OmegaTechEditor.H, OmegaTechEditor.L,
                OmegaTechEditor.R, OmegaTechEditor.S,
                volCount, chunkCount, modeStr,
                OTEditor.MainCamera.position.x,
                OTEditor.MainCamera.position.y,
                OTEditor.MainCamera.position.z);
        }
        DrawFPS(GetScreenWidth() - 60, 36);

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
                    // Re-read the defs so the panel's read-only dump and the
                    // registry agree with the file immediately.
                    LightningEntityRegistry::Instance().Init();
                    // The defs just changed on disk, so any icon EngineBillboard
                    // memoised for these names is now stale.
                    EngineBillboard::InvalidateIconCache();
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
            if (IsKeyPressed(KEY_Z)) ed::EventBus::instance().post(ed::Ev::AnimUndo, ai);
            if (IsKeyPressed(KEY_Y)) ed::EventBus::instance().post(ed::Ev::AnimRedo, ai);
        }
        if (g_editorPanels.actionAnimRefresh) {
            g_editorPanels.actionAnimRefresh = false;
            RefreshAnimPanel();
        }

        // Apply active texture to selected entity (from context menu)
        if (g_editorPanels.actionApplyTextureToSel) {
            if (!g_editorPanels.activeTexturePath.empty() && g_sel.type != SelType::NONE) {
                HistoryPush();
                if (g_sel.type == SelType::BRUSH) {
                    // g_sel.index is a renderable index for BRUSH (see
                    // EditorRaycastAt), so the whole "it might be a collision-volume
                    // index, let me hunt for a matching renderable" chain below was
                    // working around an ambiguity that no longer exists. The old chain
                    // could end up applying the texture to a DIFFERENT brush than the
                    // one selected, because a collision index below the renderable
                    // count passed the first branch.
                    //
                    // The context menu is opened from a PER-FACE pick and is called
                    // "Apply Texture to Surface", so apply to the picked faces when
                    // there are any. It used to ignore them and texture the whole
                    // brush, which is not what the menu says and not what the
                    // right-click selected.
                    const int rIdx = g_sel.index;
                    OzoneRenderable* brush = OzoneLoader::Instance().Get(rIdx);
                    uint32_t faceMask = 0;
                    for (const auto& sp : g_selectedSurfaces) {
                        if (sp.renderable != rIdx) continue;
                        if (sp.face < 0 || sp.face >= oz::surface::FACE_COUNT) continue;
                        faceMask |= (1u << (int)sp.face);
                    }

                    if (brush && faceMask != 0) {
                        int appliedFaces = 0;
                        for (int f = 0; f < oz::surface::FACE_COUNT; f++) {
                            if (!(faceMask & (1u << f))) continue;
                            // Keep this face's flags/UV/alpha/glow; substitute only
                            // the texture. Overwriting the whole SurfaceProps here
                            // would silently reset every other property.
                            oz::surface::SurfaceProps p =
                                brush->surface.Resolve((oz::surface::SurfaceFace)f);
                            p.texPath = g_editorPanels.activeTexturePath;
                            OzoneLoader::Instance().SetRenderableFace(
                                rIdx, (oz::surface::SurfaceFace)f, p);
                            appliedFaces++;
                        }
                        EditorLog("Applied texture to %d face(s) of renderable idx=%d: %s",
                                  appliedFaces, rIdx,
                                  g_editorPanels.activeTexturePath.c_str());
                        if (g_editorPanels.hSurfaceProps)
                            SurfacePropsRefresh((HWND)g_editorPanels.hSurfaceProps);
                        g_editorPanels.textureProbeRenderable = rIdx;
                    } else {
                        // No face pick for this brush (e.g. the menu came from a
                        // model row or a stale surface selection): fall back to the
                        // brush-wide default.
                        bool applied = OzoneLoader::Instance().ApplyRenderableTexture(
                            rIdx, g_editorPanels.activeTexturePath.c_str());
                        EditorLog("Applied texture to brush renderable idx=%d (sel=%d): %s",
                                  rIdx, g_sel.index, applied ? "ok" : "FAILED");
                        if (applied) g_editorPanels.textureProbeRenderable = rIdx;
                    }
                } else if (g_sel.type == SelType::MODEL && g_sel.index >= 0 && g_sel.index < CachedModelCounter) {
                    int mid = CachedModels[g_sel.index].ModelId;
                    if (ApplyTextureToModel(mid, g_editorPanels.activeTexturePath.c_str()))
                        EditorLog("Applied texture to model %d: %s", mid,
                                  g_editorPanels.activeTexturePath.c_str());
                }
            } else {
                // A silent no-op is indistinguishable from "the texture did not
                // render" in a user's session log; name the actual reason.
                EditorLog("Apply texture: nothing to apply to (selection=%d, texture='%s')",
                          (int)g_sel.type, g_editorPanels.activeTexturePath.c_str());
            }
            g_editorPanels.actionApplyTextureToSel = false;
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
        EndDrawing();

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

        // Mode switching
        if (EdKeys::Pressed(EdAction::ModeModel))  g_placeMode = PlaceMode::MODEL;
        if (EdKeys::Pressed(EdAction::ModePickup)) g_placeMode = PlaceMode::PICKUP;
        if (EdKeys::Pressed(EdAction::ModeNode))   g_placeMode = PlaceMode::NODE;
        if (EdKeys::Pressed(EdAction::ModeZone))   ToggleZonePlacement();
        if (EdKeys::Pressed(EdAction::ModeTerrain) && g_placeMode != PlaceMode::MODEL)
            g_placeMode = PlaceMode::TERRAIN;

        // Primitive type cycling (1-5 for box/cyl/sph/pyr/pln, only in MODEL mode)
        // Reads real state from EMID (200+prim); never re-fires the actionCsgPlace
        // one-shot (that would reset the ghost's size every frame).
        if (g_placeMode == PlaceMode::MODEL) {
            int prim = (EMID >= 200 && EMID <= 204) ? EMID - 200 : 0;
            // Uses the same remappable key as the terrain mode (5 by default):
            // inside MODEL mode it cycles primitives, otherwise it selects terrain.
            if (IsKeyPressed(EdKeys::Key(EdAction::ModeTerrain))) {
                prim = (prim + 1) % 5;
                EMID = 200 + prim;
                EditorLog("Primitive: %s", prim == 0 ? "box" : prim == 1 ? "cylinder" : prim == 2 ? "sphere" : prim == 3 ? "pyramid" : "plane");
            }
        }

        // CSG operation cycling
        if (EdKeys::Pressed(EdAction::CsgCycle) && g_placeMode == PlaceMode::MODEL) {
            OmegaTechEditor.CSGOperation = (OmegaTechEditor.CSGOperation + 1) % 5;
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
        if (IsKeyPressed(KEY_ESCAPE) && Gizmo_IsDragging()) {
            Gizmo_CancelDrag();
            g_gizmoDrag = false;
            g_gizmoHistPushed = false;
        } else if (IsKeyPressed(KEY_ESCAPE) &&
            (g_sel.type != SelType::NONE || OmegaTechEditor.DrawModel)) {
            g_sel = { SelType::NONE, -1, "", {0,0,0} };
            g_hoverSel = { SelType::NONE, -1, "", {0,0,0} };
            OmegaTechEditor.DrawModel = false;
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
        // Undo / redo (Ctrl+Z, Ctrl+Y, Ctrl+Shift+Z). Deferred to the anim tool
        // while vertex-editing, which owns its own undo stack.
        if (!g_editorPanels.animEditVerts) {
            if (EdKeys::Pressed(EdAction::Undo)) HistoryUndo();
            if (EdKeys::Pressed(EdAction::Redo)) HistoryRedo();
            // Ctrl+Shift+Z is a second, hard-wired redo binding (not in the table:
            // it is the same key as Undo with Shift, and the table has one key per
            // action).
            if (EdKeys::CtrlDown() && EdKeys::ShiftDown() && IsKeyPressed(KEY_Z))
                HistoryRedo();
        }

        // Camera shortcuts
        if (IsKeyPressed(KEY_HOME)) { SetViewPerspective(); ResetCamera(); }
        if (IsKeyPressed(KEY_PAGE_UP)) CamUp();
        if (IsKeyPressed(KEY_PAGE_DOWN)) CamDown();

        // View preset shortcuts (NumPad)
        if (IsKeyPressed(KEY_KP_7)) {
            // Top view
            Vector3 center = OTEditor.MainCamera.target;
            SetViewPreset({center.x, center.y + 80, center.z + 0.1f}, center, {0, 0, -1});
        }
        if (IsKeyPressed(KEY_KP_1)) {
            // Bottom view
            Vector3 center = OTEditor.MainCamera.target;
            SetViewPreset({center.x, center.y - 80, center.z + 0.1f}, center, {0, 0, 1});
        }
        if (IsKeyPressed(KEY_KP_3)) {
            // Right view
            Vector3 center = OTEditor.MainCamera.target;
            SetViewPreset({center.x + 80, center.y, center.z}, center, {0, 1, 0});
        }
        if (IsKeyPressed(KEY_KP_9)) {
            // Left view
            Vector3 center = OTEditor.MainCamera.target;
            SetViewPreset({center.x - 80, center.y, center.z}, center, {0, 1, 0});
        }
        if (IsKeyPressed(KEY_KP_5)) {
            // Perspective / back to default
            SetViewPerspective();
        }

        }  // EditorUi_ViewportHasFocus()

    }

    // Cleanup
    EditorLog("=== AngelEd shutting down ===");
    EditorIcons::Instance().Unload();
    if (g_editorLog) fclose(g_editorLog);
    StopSoundPreview();
    OzoneLoader::Instance().Unload();
    UnloadRenderTexture(g_previewRT);
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
