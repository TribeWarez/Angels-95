// ============================================================================
// Core/EditorPanelState.hpp
//
// The single shared UI/selection/undo state object. MOVED HERE in R9 from
// UI/UiPanels.hpp, and that move was forced rather than cosmetic.
//
// EditorPanelState is 374 lines and is read by everything: Core, Subsystems and UI.
// It was DECLARED in a UI header and DEFINED in UI/UiCommon.hpp, so every Subsystems/
// file that touched it (PropsApply alone reads 85 of its fields) had to include a UI
// header - and once Subsystems/ became real translation units that meant linking every
// one of them against UiShell.o just to reach the state.
//
// It is editor state, not a UI API surface. The Design section of
// Wiki/Editor-Architecture-Refactor.md has said "Core/ - frame loop, editor state"
// since R0; this is that line finally being true.
//
// Two things deliberately did NOT move with it: the Win32 window HANDLEs stay behind
// the #ifdef _WIN32 below, and EditorPanelState remains one flat struct rather than
// being split per concern. Splitting it is the obvious next step, and a much larger
// change than relocating it.
// ============================================================================
#ifndef ANGEL_ED_CORE_EDITORPANELSTATE_HPP
#define ANGEL_ED_CORE_EDITORPANELSTATE_HPP

#include "../SelType.hpp"


// A model offered by the Model Browser: a scanned asset plus its lazily-populated
// mesh stats. Moved here from UI/UiPanels.hpp in R9 for the same reason as
// EditorPanelState - EditorPanelState holds a vector of these, so it had to be
// visible wherever the state is.
struct ModelBrowserEntry {
    std::string name;
    std::string path;
    int triangles = 0;
    int vertices = 0;
    bool loaded = false;
    int modelIndex = -1;
};
struct EditorPanelState {
    bool showSoundMgr = false;
    bool showTextureMgr = false;
    bool showPawnMgr = false;
    bool showScriptMgr = false;
    bool showModelBrowser = false;
    bool showPickupPanel = false;
    bool showNodePanel = false;
    bool showSettingsPanel = false;
    bool showHeightmapEditor = false;
    // Model browser state (used cross-platform)
    std::vector<ModelBrowserEntry> modelEntries;
    int selectedModel = -1;

    // Action flags (set by dialog procs, read by main raylib loop)
    // Renamed off the action* prefix: it is a DIRTY FLAG (the preview must
    // re-read the list), not a message. Same distinction as actionAnimRefresh.
    bool refreshModelBrowser = false;
    // The nine `actionSpawn*` fields (pawn / mesh / pickup / emitter / zone /
    // particle emitter / path node / wind zone / player start) are GONE. They had
    // no writer anywhere in the editor: every one of their handlers in Main.cpp
    // was unreachable, because the only assignments to the fields were the
    // handlers' own resets to their default. Placement actually happens
    // synchronously in SpawnSelectedPawnTreeItem (Pawn Manager tree leaf), which
    // covers all nine types using spawnPos below — so the handlers were ~110 lines
    // of vestigial duplicates that would also have placed at MainCamera.target
    // instead of the ghost position.
    //
    // ed::EventBus::Spawn* exists for the day the layering rule in
    // Wiki/Editor-Architecture-Refactor.md is enforced: SpawnSelectedPawnTreeItem
    // reaching into PawnSystem directly is precisely the UI->Subsystems dependency
    // that R3/R4 remove. Do not reintroduce an action field for these.
    // Camera aim point, refreshed every frame by Main.cpp, so Win32 panels can
    // spawn entities synchronously without waiting on the main-loop action flags.
    float spawnPos[3] = {0.0f, 0.0f, 0.0f};
    int previewSoundCategory = 0;   // 0=SFX, 1=Music, 2=Ambience
    int previewSoundLoop = 0;        // 0=no loop, 1=loop
    int previewSoundVolume = 80;     // 0-100
        // Heightmap editor action flags

    // Light properties live in propLight* (the Entity Properties panel's own state).
    // The 12 `light*` fields and actionApplyLight that used to sit here are GONE:
    // their only reader was the legacy light apply handler, which became
    // unreachable in b88 when LightPropsProc - its only writer - was deleted with
    // the window. Nothing writes or reads them now.
    // --- Surface Properties (UT99-style, per-face) -------------------------
    // The dialog edits one BrushSurface at a time. `surfaceFaces` is the set of
    // faces the current selection covers (a bitmask, so "(3 Selected)" is just a
    // popcount); Apply writes to every one of them.
    bool showSurfaceProps = false;
    bool showKeymap = false;   // Help -> Keyboard Shortcuts
    int  surfaceRenderable = -1;      // OzoneRenderable index
    uint32_t surfaceFaceMask = 0;     // bitmask of SurfaceFace
    // Write surface.def (the brush-wide default) instead of the faces in the mask.
    // Lives here as well as on the event so the dialog can render the right scope
    // on reopen; the AUTHORITATIVE copy is ed::SurfaceEdit::brushWide, which is
    // what the handler reads.
    bool surfaceBrushWide = false;
    // actionApplySurface / actionResetSurface are GONE -> ed::Ev::ApplySurface /
    // ed::Ev::ResetSurface, both carrying one ed::SurfaceEdit with the renderable,
    // the face mask AND the working props.
    //
    // The mask travels with the event because ApplyToSelection is a deliberate
    // no-op on an empty mask so it can never mean "all six faces", and the old
    // code read the mask from live panel state at drain time - so a selection
    // change between the click and the drain applied the props to different faces
    // than the dialog was showing.
    //
    // surfaceEdit below is now only the dialog's own working copy; the main loop
    // reads the values from the event, not from here.
    oz::surface::SurfaceProps surfaceEdit;
    // actionCsgPlace is GONE -> ed::Ev::CsgPlace (payload ed::CsgIntent). Two kinds
    // rather than one because "arm a primitive" resets the ghost to a default box
    // at the camera, while "commit the ghost with this op" reads the ghost as it
    // currently is; collapsing them would either lose the reset or commit a brush
    // the user never positioned.
    int currentToolMode = 0;    // persistent tool mode: 0=cam,1=move,2=scale,3=rotate
    // Gizmo snapping. 0 = off. Ctrl forces 1.0, Shift forces 0.1 regardless.
    // The toolbar shows/cycles this so snapping is discoverable, not a hidden key.
    float gizmoSnap = 0.0f;
    float gizmoAngleSnap = 15.0f;   // degrees; Shift = 1 degree

    // Terrain editing
    int terrainBrushMode = 0;       // 0=raise, 1=lower, 2=flatten
    int terrainBrushSize = 4;       // radius in grid cells
    float terrainBrushStrength = 0.05f; // height change per click [0..1]
    // actionCsgCommitNow is GONE -> ed::Ev::CsgCommit (see actionCsgPlace above).
    // actionWorldGraphProperties / actionWorldGraphDelete / actionWorldGraphDup and
    // the five actionSelectFromGraph* fields are GONE — they became ed::Event
    // SelectEntity / ApplyProperties / DeleteEntity / DuplicateEntity (R2 batch B2).
    //
    // That batch also fixed a live bug: Delete and Duplicate stored the clicked
    // entity index and then IGNORED it, calling DeleteSelectedEntity() against
    // whatever g_sel happened to be. That was correct only because the select field
    // was written in the same WM_NOTIFY. The two also used different index spaces
    // (actionWorldGraphProperties held a ListView ROW, the others an ENTITY), and
    // Properties was never read at all — the handler just called
    // OpenPropertiesForSelection(), i.e. acted on g_sel too.

    // Active texture tracking (for context menu apply + auto-apply)
    std::string activeTexturePath;   // currently selected texture in browser
    int activeTextureSlot = 0;       // tileset slot index if applicable
    // actionApplyTextureToSel deliberately REMAINS a field, the fourth and last
    // documented exception to "everything becomes an event".
    //
    // It reads `g_sel` and `activeTexturePath` at drain time, which is normally
    // the staleness bug this refactor exists to kill. It is safe here for a
    // specific reason: both writers are context-menu commands, and
    // TrackPopupMenu is MODAL - the selection cannot move between the click and
    // the frame that consumes it, because nothing else is pumping input. Capturing
    // a Selection would mean duplicating the brush renderable resolution (which
    // depends on g_sel.pos to disambiguate a renderable index from a collision-volume
    // index) for no behavioural gain.
    bool actionApplyTextureToSel = false;  // apply activeTexturePath to selected entity
    // One-shot diagnostic armed by the texture-apply drains: the frame pass logs
    // this renderable's draw-path state once (view mode, per-face meshes, bound
    // texture handles) so an "it applied but nothing shows" report carries the
    // answer in AngelEd.log instead of a guess. -1 = nothing pending.
    int textureProbeRenderable = -1;

    // WorldGraph Explorer
    bool showWorldGraph = false;
    // Its list is derived from PawnSystem/ZoneManager/OzoneLoader, but it was built
    // exactly once - in WM_CREATE, from CreateAllEditorWindows, which runs BEFORE the
    // world is loaded and before PawnDefs register. Opening the panel on a fresh session
    // therefore showed one "Map (level)" row and stayed wrong until Refresh was pressed
    // by hand. Mutations set this; the frame loop consumes it once per frame.
    bool worldGraphDirty = false;
    // One-shot: centre the camera on the current selection. Set by the World
    // Hierarchy's "Frame Selection" item and consumed by the frame loop. This is
    // live state, not a mutation: the UI never touches the camera directly.
    bool focusSelection = false;
    // (the actionSelectFromGraph* fields moved to ed::Event SelectEntity — see above)

    // LevelList / Campaign panel
    bool showLevelList = false;
    std::string portalTargetWorld;          // default target for newly placed portals

    // Portal deletion action. Portal *editing* is the Entity Properties panel's
    // PORTAL section; only the delete needs an out-of-band channel because it
    // mutates the vector the panel is indexing into.

    // Properties panel (context-sensitive)
    bool showPropsPanel = false;

    // Viewport collision visualisation. Draws the post-CSG collision volumes as
    // wireframes so it is possible to see what the player will actually stand
    // on, and also reveals the generated SURF_COLLISION_PROXY boxes.
    bool showCollisionBounds = false;
    // Set when a Sub/Intersect brush produced no collision volume at all. The
    // sidebar paints the collision count red until the next brush is committed.
    bool collisionOpWarning = false;
    // Pawn Manager: show metadata-only defs (EntityType::GAMETYPE) that are not
    // placeable. Off by default so the tree stays focused on placeable actors.
    bool showHidden = false;

    int propsTargetType = -1;       // SelType encoded
    int propsTargetIndex = -1;
    std::string propsTargetName;
    float propsTargetPos[3] = {0,0,0};
    float propsTargetScale = 1.0f;
    float propsTargetRotation = 0.0f;
    // False when the selection has no yaw concept (zones, pickups, portals...).
    // The apply handler must not write propsTargetRotation in that case: the
    // Rot row is seeded from a 0.0f default and used to overwrite the authored
    // value (notably `playerstart`'s yaw) on every Apply.
    bool propsTargetHasRotation = false;
    // Properties values (set by panel, read by Subsystems/PropsApply.cpp at drain
    // time - see that file for why the values stay live-read while the TARGET travels
    // in the event).
    //
    // actionApplyProperties is GONE -> ed::Ev::ApplyProperties, carrying a SelRef built
    // by PostApplyProperties() in PropsPanel.cpp. The handler used to read
    // propsTargetType/propsTargetIndex from live panel state, so whichever row the
    // panel happened to be showing at drain time decided what got written.
    float propPosX = 0, propPosY = 0, propPosZ = 0;
    float propSizeX = 1, propSizeY = 1, propSizeZ = 1;  // for brush/zone
    float propRotation = 0;
    float propScale = 1;
    float propTexScaleU = 1.0f;
    float propTexScaleV = 1.0f;
    float propTexOffsetU = 0.0f;
    float propTexOffsetV = 0.0f;

    // Def-aligned property sections (filled by ShowPropertiesPanel, read by Main.cpp)
    std::string propDefPath;        // .ozls/.cfg source path ("" = no def / section hidden)
    std::string propDefTitle;       // e.g. "HealthVial  [pickup]"
    std::string propDefSource;      // source file path shown as a row
    // Explains WHY a def has no editable stat rows (e.g. TRIGGER/EVENT: no
    // runtime-consumed stats, dump-only). Rendered as a note line under the
    // Definition header; empty means no note.
    std::string propDefNote;
    struct DefField { std::string key; std::string value; };
    std::vector<DefField> propDefPawnFields; // PawnDefs/*.cfg values (pawns only)
    std::vector<DefField> propDefFields;     // .ozls stats (floats + strings)
    std::vector<DefField> propDefActions;    // action name -> "N lines"

    // --- Editable .ozls stat rows -----------------------------------------
    //
    // propDefFields above is the full authored set, rendered read-only.
    // These are the subset the panel knows how to edit for the selected def's
    // entity type, with a per-row control ID so Apply can read the values back.
    //
    // Keys NOT in this list still render as read-only rows, so an authored stat
    // the editor has never heard of is visible rather than silently dropped.
struct DefStatRow {
    std::string key;
    std::string value;
    // Human label from the schema. The panel used to print only `key`, so every
    // authored label was dead data — yet the label is what makes the row
    // readable ("Reload Time" vs "reload_time").
    std::string label;
    // Schema group this row belongs under; empty means "continues the previous
    // group". Drives the sub-headers inside the Edit stats section.
    std::string group;
    int controlId = 0;          // ID_PP_STATS_BASE + index
    bool isFloat = false;       // right-aligned numeric field
    bool isSoundPath = false;   // gets a Browse... and a Preview button
    };
    std::vector<DefStatRow> propDefEditable;
    // Collected on Apply and consumed by Main.cpp, which performs the actual
    // patch via ozls::PatchOzlsStats (Win32Dialogs must not write files itself).
    std::vector<DefField> propDefPendingEdits;
    // True when propDefPath names a real file on disk. False means the def came
    // from a package, which cannot be written back to - the panel shows the
    // rows read-only in that case rather than failing on Apply.
    bool propDefWritable = false;
    // Set when the user presses Preview on a sound row, holding the path.
    std::string propDefPreviewSound;
    float propHealth = 100;         // NPC instance health override
    float propSpeed = 1.5f;         // NPC instance speed override
    float propRespawnTime = 30;     // pickup instance respawn time
    int   propZoneType = 0;         // ZoneType index (0=water 1=ladder 2=sky 3=reverb 4=sound)
    float propZoneIntensity = 1;
    std::string propZoneName;       // zone script-hook name
    // Per-zone physics overrides (mirrors oz::physics::PhysicsInfo)
    float propZoneGravity = 20.0f;
    float propZoneJump = 8.0f;
    float propZoneTerminal = 60.0f;
    float propZoneWaterGravity = 8.0f;
    float propZoneWaterDrag = 0.95f;
    float propZoneSwimUp = 5.0f;
    float propZoneLadderSpeed = 6.0f;
    float propZoneFlyMult = 1.5f;
    // Per-zone environment overrides (mirrors oz::ZoneEnvOverrides).
    //
    // These are the values the GAME reads: ZoneManager merges them per zone into
    // PointRegion::combinedEnv and Core.hpp applies them on zone entry. Until
    // these rows existed they had NO editor surface at all — envOverrides was
    // written nowhere in AngelEd, so per-zone fog in a shipped level could only
    // be produced by hand-editing the .ozone. The Zone Properties dialog's Fog /
    // Ambient tabs looked like they did this but were level-global shader pokes.
    //
    // applyFog / applyAmbient are the flags the exporter gates on (Main.cpp's
    // ExportToOzone), so they are per-zone booleans here rather than inferred.
    bool  propZoneApplyFog = false;
    int   propZoneFogR = 179, propZoneFogG = 179, propZoneFogB = 204;
    float propZoneFogDensity = 1.0f;
    float propZoneFogStart = 10.0f, propZoneFogEnd = 100.0f;
    bool  propZoneApplyAmbient = false;
    int   propZoneAmbR = 180, propZoneAmbG = 180, propZoneAmbB = 200;
    float propZoneAmbIntensity = 0.4f;
    float propZoneReverbMix = 0.0f, propZoneReverbDecay = 0.0f;
    std::string propPortalWorld;    // portal target world
    float propPortalSpawn[3] = {0,0,0};
    bool  propPortalBidir = true;

    // --- SelType::MAP (the level itself) ---
    // These mirror LevelMetadata field for field. They are a flat copy rather
    // than a LevelMetadata member so the panel can show one flat row per field
    // and diff "changed" against the value the panel was opened with, exactly as
    // the .ozls stat rows do.
    int   propMapGameType = 0;            // oz::gametype::GameType index
    int   propMapMaxPlayers = 8;
    float propMapRespawnTime = 5.0f;
    bool  propMapTimeLimitEnabled = false;
    float propMapTimeLimitMinutes = 10.0f;
    int   propMapScoreLimit = 50;
    bool  propMapFriendlyFire = false;
    std::string propMapSkybox;            // level skybox texture path
    std::string propMapSkyboxSide;        // side/cap skybox path
    int   propMapParticleType = 0;        // ParticleType index
    float propMapParticleDensity = 50.0f;
    float propMapParticleSpeed = 1.0f;
    int   propMapParticleR = 200, propMapParticleG = 200, propMapParticleB = 200;
    float propMapParticleWindX = 0.0f, propMapParticleWindZ = 0.0f;

    // GameEngine.Mesh object editing
    std::string propMeshPath;       // model path
    std::string propMeshTex;        // texture path
    std::string propAnimClip;       // skeletal clip name
    std::string propMeshAnimFile;   // external .ozanim clip path
    float propMeshAnimSpeed = 1.0f; // playback speed

    // GameEngine.ParticleEmitter editing
    std::string propEmitterType;    // fire/sparks/smoke/...
    std::string propEmitterTex;     // billboard texture path
    float propEmitterRate = 20.0f;
    float propEmitterLife = 1.0f;
    float propEmitterSpeed = 2.0f;
    float propEmitterSize = 0.4f;
    float propEmitterSpread = 12.0f;   // degrees (cone half-angle)
    int propEmitterR = 255, propEmitterG = 180, propEmitterB = 80;
    // End-of-life size and colour, plus gravity and spawn radius. The panel used
    // to expose only the start values, so an emitter's fade-out was authored by
    // hand in the .ozone and any Apply silently reset it to the defaults - the
    // four fields below were written by the loader and had no editor row.
    float propEmitterSizeEnd = 0.0f;
    int propEmitterREnd = 60, propEmitterGEnd = 20, propEmitterBEnd = 10;
    float propEmitterGravity = 0.0f;
    float propEmitterRadius = 0.0f;
    bool propEmitterBillboard = true;

    // GameEngine.PathNode editing
    std::string propPathName;
    float propPathRadius = 1.0f;
    std::string propPathNext;    // comma-separated successor names
    bool propPathLoop = false;

    // GameEngine.Mesh wind-affection flag
    bool propMeshWind = false;
    // WindZone editing
    float propWindDirX = 1.0f, propWindDirY = 0.0f, propWindDirZ = 0.0f;
    float propWindStrength = 1.0f;
    float propWindFrequency = 1.0f;
    float propWindSizeX = 10.0f, propWindSizeY = 10.0f, propWindSizeZ = 10.0f;

    // GameEngine.Light editing (Properties panel, SelType::LIGHT). This is now the
    // ONLY home for light settings: the standalone Light Properties window and its
    // lightColor*/actionApplyLight backing store are both gone.
    std::string propLightName;
    int   propLightR = 255, propLightG = 255, propLightB = 255;
    float propLightIntensity = 1.0f;
    float propLightRadius = 20.0f;
    int   propLightType = 1;      // LitLightType: 0=directional 1=point 2=spot
    int   propLightEffect = 0;    // LitLightEffect: none/watery/torch/fire/lamp
    float propLightInnerAngle = 18.0f;   // spot inner cone half-angle, degrees
    float propLightOuterAngle = 37.0f;   // spot outer cone half-angle, degrees
    bool  propLightFlare = false, propLightCorona = false;
    float propLightTarget[3] = {0,0,0};

    // Animation / vertex-keyframe tool
    bool showAnimPanel = false;
    // Material Graph Explorer
    bool showMaterialPanel = false;
    int animTargetMesh = -1;        // MeshObjectNode id (set by Main.cpp from selection)
    std::string animClipName;       // selected clip name
    float animTime = 0.0f;          // scrub time (seconds)
    float animFps = 30.0f;
    bool animLoop = true;
    bool animPlaying = false;
    int animTimeSlider = 0;         // 0..1000 (drives animTime)
    std::string animStatus;         // read-only status line
    // Eleven animation commands are GONE -> ed::Ev::Anim* carrying an
    // ed::AnimIntent (mesh id + clip name + playhead + fps/loop, captured at post
    // time). "Delete Clip" used to read animClipName from live panel state at
    // drain time, so a clip-list selection that moved between the click and the
    // frame deleted a different clip.
    //
    // actionAnimSave and actionAnimRefresh deliberately REMAIN fields. They are
    // intra-frame chaining signals, not user intents: the anim handlers set
    // actionAnimSave themselves and the file write consumes it later in the SAME
    // frame, while Refresh is a dirty flag consumed by RefreshAnimPanel().
    // Queueing either would delay the write a frame for no benefit.
    bool actionAnimSave = false;
    bool actionAnimRefresh = false;
    // Panel message: refresh ONLY the live time/frame readout and the scrubber.
    // The structural refresh is WM_USER+50 (PopulateAnimPanel), which rebuilds the
    // clip list and therefore re-selects a row; doing that every frame while a
    // clip plays made the list unusable. 0x0400 is WM_USER on Win32; this header
    // deliberately does not include <windows.h>.
    static constexpr unsigned int kAnimPanelTick = 0x0400 + 51;
    // Vertex definition tool (Phase C)
    bool animEditVerts = false;         // vertex-edit mode active
    std::vector<int> animSelVerts;      // selected global vertex indices
    // Undo/redo snapshot stacks live in Main.cpp (they hold ozanim::Animation).
    float animPrevX = 0, animPrevY = 0, animPrevZ = 0, animPrevR = 0;
    bool animPrevValid = false;

#ifdef _WIN32
    // Window handles (Windows only)
    void* hSoundMgr = nullptr;
    void* hTextureMgr = nullptr;
    void* hPawnMgr = nullptr;
    void* hScriptMgr = nullptr;
    void* hModelBrowser = nullptr;
    void* hPickupPanel = nullptr;
    void* hNodePanel = nullptr;
    void* hSettingsPanel = nullptr;
    void* hHeightmapEditor = nullptr;
    void* hWorldGraph = nullptr;
    void* hPropsPanel = nullptr;
    void* hStatsSidebar = nullptr;
    void* hLevelList = nullptr;
    void* hAnimPanel = nullptr;
    void* hMaterialPanel = nullptr;
    void* hSurfaceProps = nullptr;
    void* hKeymap = nullptr;

    // Preview bitmap (Windows only)
    void* hPreviewBitmap = nullptr;
    int previewW = 0, previewH = 0;

    struct WinPos { int x, y, w, h; };
    WinPos soundMgrPos   = {50, 50, 400, 280};
    WinPos textureMgrPos = {480, 50, 520, 480};
    WinPos pawnMgrPos    = {50, 300, 480, 300};
    WinPos scriptMgrPos  = {440, 340, 560, 450};
    WinPos modelBrwPos   = {100, 80, 540, 500};
    WinPos pickPanelPos  = {60, 400, 200, 280};
    WinPos nodePanelPos  = {290, 400, 200, 200};
    WinPos settingsPos        = {50, 50, 400, 600};
    WinPos heightmapEditorPos = {120, 100, 520, 480};
    WinPos worldGraphPos = {540, 100, 600, 400};
    // 540 (not 470): the vertical scrollbar costs ~17px of client width, so this
    // keeps the usable value column as wide as it was before it was added.
WinPos propsPanelPos = {300, 120, 540, 560};
    WinPos levelListPos = {200, 120, 560, 420};
    WinPos animPanelPos  = {430, 180, 380, 390};
    WinPos materialPanelPos = {260, 140, 720, 460};
    WinPos surfacePropsPos = {760, 140, 520, 440};
    WinPos keymapPos = {420, 240, 520, 420};
#endif
};

// ONE definition, shared by every translation unit. inline (C++17) rather than a
// plain definition, which would be a multiple-definition link error the moment a second
// TU included this header.
inline EditorPanelState g_editorPanels;

// Flag the WorldGraph list as needing a rebuild. Cheaper to call from the ~30 mutation
// sites than to call RefreshWorldGraph() from each, and it keeps the layer direction
// right: Subsystems/ and Core/ set a flag, the Core frame loop makes the one call
// downward into UI. Setting a flag is not a mutation.
inline void MarkWorldGraphDirty() { g_editorPanels.worldGraphDirty = true; }

#endif // ANGEL_ED_CORE_EDITORPANELSTATE_HPP
