#pragma once
#include "raylib.h"
#include "LevelSettings.hpp"
#include "ZoneManager.hpp"
#include "ZoneTypes.hpp"
#include "../Pawn/OzPawnSystem.hpp"
#include "../Physics/WorldChunk.hpp"
#include "../Physics/AutoConvex.hpp"   // AutoConvexFail, for AppendAutoConvexCollision
#include "../Physics/DynamicCollision.hpp"   // quantum obstacles (SURF_QUANTUM)
#include "OzoneParser.hpp"
#include "SurfaceFlags.hpp"
#include "BrushMesh.hpp"   // oz::brushmesh builders (raylib-free, unit-tested)
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

// windows.h defines LoadString -> LoadStringA/W; that would rename the method
// below and break linking on Windows builds that include windows.h first.
#ifdef LoadString
#undef LoadString
#endif

// Surface behavior flags for OZONE brush primitives now live in
// World/SurfaceFlags.hpp (included above) as typed constants rather than the
// `#define SURF_FAKEBACKDROP / SURF_COLLISION_PROXY` pair that used to live
// here. The values and the unqualified spellings are unchanged - the 299
// `flags=8` painted backdrops in the shipped worlds still resolve - but a
// macro could not coexist with a scoped type of the same name, which is what
// blocked the per-face surface flag set.

// ---------------------------------------------------------------------------
// OzoneLoader — client-side OZONE format loader + mesh renderer
//
// Parses .ozone files (reusing the server-side OzoneParser) and generates
// raylib Mesh / Model objects for each primitive so they can be drawn
// in the 3D world alongside standard WDL models.
//
// Supported primitives:
//   Box, Cylinder, Sphere, Pyramid, Plane
//
// Usage:
//   OzoneLoader::Instance().LoadFile("GameData/Worlds/World1/World.ozone");
//   OzoneLoader::Instance().Draw(camera);
// ---------------------------------------------------------------------------

// A renderable OZONE primitive with its generated raylib model.
struct OzoneRenderable {
    int typeId = 0;              // cast from OzonePrimitiveType
    Vector3 position{0, 0, 0};
    float scale = 1.0f;
    float rotation = 0.0f;
    Model model;                 // generated raylib Model (meshes + materials)
    Shader defaultShader = {0};  // saved original shader before LitFogShader override
    bool loaded = false;
    int csgOp = 0;               // CSG operation (0=SOLID, 1=ADD, 2=SUB, 3=INTERSECT, 4=DE_RESC)
    int texSlot = 0;             // 0=auto, 1..N = index into tileset textures
    int surfaceFlags = 0;        // SURF_* bitmask for surface behavior
    // Per-face surface properties (flags, texture, UV, U/V pan, alpha, glow).
    // The brush-wide default lives in .def; a face only overrides what the
    // author changed. `faceMeshes` caches the per-face sub-mesh split so the
    // renderer can issue one DrawMesh per face with its own uniforms.
    oz::surface::BrushSurface surface;
    bool faceMeshesBuilt = false;
    // One sub-mesh per dominant-axis face, built lazily by BuildFaceMeshes().
    // vaoId == 0 means "not built" (or "this face has no geometry").
    Mesh faceMesh[oz::surface::FACE_COUNT] = {};
    // Resolved face-texture handles, cached per renderable per face. DrawSurface
    // resolves a texture EVERY frame; without this it rebuilt a std::string and
    // hit the path/texture hash maps even when the face had not changed.
    // Handles are borrowed from m_faceTexCache and invalidated by every surface
    // writer (see InvalidateFaceTexCache call sites).
    Texture2D faceTexHandle[oz::surface::FACE_COUNT] = {};
    bool faceTexCached[oz::surface::FACE_COUNT] = {};
    float texScaleU = 1.0f;      // extra texture tiling ON TOP of the generated
    float texScaleV = 1.0f;      // mesh UVs (multiplier, not an absolute)
    float texOffsetU = 0.0f;     // texture shift U
    float texOffsetV = 0.0f;     // texture shift V
    // Pristine texcoords exactly as BuildFromPrimitive generated them (which for
    // a box already includes its baked tiling). ApplyRenderableUV always
    // recomputes `final = uvBase * scale + offset` from this snapshot.
    //
    // It replaces the previous scheme, which tried to UNDO the last transform by
    // dividing by r->texScaleU. That only worked when texScaleU had been seeded
    // with the baked tiling - and it was seeded for BOX only, so for
    // cyl/sph/pyr/pln the division was by 1 while the UVs were already tiled.
    // The first edit then scaled on top of the tiling and every later edit
    // compounded again.
    std::vector<float> uvBase;
    std::string texPath;         // filesystem/package path to custom texture (empty = use tileset)
    Texture2D customTex = {0};   // loaded custom texture (id=0 if using tileset)
    // SKYBOX only: the authored `topTex=` ceiling texture. Kept here (rather
    // than only on the primitive) so the editor's exporter can round-trip it.
    std::string skyTopTex;
    // PLANE orientation, in ENGINE space (the file is Z-up; the loader converts).
    // A plane's normal and distance cannot be recovered from its mesh bounding
    // box, so they are carried explicitly for the editor to round-trip.
    float planeNX = 0.0f, planeNY = 1.0f, planeNZ = 0.0f;
    float planeDist = 0.0f;
    // CYLINDER taper. The mesh bounding box only knows max(rTop, rBot), so a
    // cone could not survive an editor round-trip without these. They are also
    // what the Properties panel's Size X / Size Y mean for a cylinder.
    float cylRTop = 0.0f, cylRBot = 0.0f;
    int   cylSlices = 0;         // 0 = derive from the loaded mesh
    BoundingBox bounds{{0,0,0},{0,0,0}}; // world-space AABB for frustum culling
    bool hasBounds = false;      // true once bounds is meaningful
    // --- Quantum obstacle state (Phase 7.2) --------------------------------
    // `quantumHidden` is written ONCE per frame by Core.hpp's UpdateQuantumObstacles,
    // from the same AabbInFrustum test the world pass would use, so this pass, the
    // dynamic side-list and the editor cannot disagree about the brush's state
    // within a frame. `isQuantum` is resolved at RebuildCollisionVolumes and marks
    // the brush as side-list owned. Non-quantum brushes stay false/false.
    bool quantumHidden = false;
    bool isQuantum     = false;
};

// ---------------------------------------------------------------------------
// IsEditorPickableSurface — may the editor select this brush at all?
//
// ONE definition of "visible", because this predicate now has three call sites
// (the viewport brush raycast, the per-face surface picker, and the WorldGraph
// brush list) and two copies of "what counts as visible" previously drifted:
// the raycast skipped SURF_COLLISION_PROXY|SURF_INVISIBLE while the surface
// picker skipped nothing, so right-clicking an invisible AutoConvex box offered
// "Surface Properties" for geometry the user cannot see or otherwise select.
//
// The rule is "no part of this brush is drawn":
//   * collision proxies are skipped by every drawing path, always;
//   * a brush is unpickable only when ALL SIX resolved faces are SURF_INVISIBLE.
//     A brush-wide invisible flag with a visible face override is still pickable,
//     because that face is on screen and there is otherwise no way to select the
//     brush and put the face back.
//
// Per-FACE, matching DrawSurface's filter, rather than per-brush on the derived
// mirror `surfaceFlags` - the mirror only reflects surface.def, so it cannot see
// a per-face override at all.
// ---------------------------------------------------------------------------
inline bool IsEditorPickableSurface(const OzoneRenderable& r) {
    using namespace oz::surface;
    if (r.surfaceFlags & (int)SURF_COLLISION_PROXY) return false;
    for (int f = 0; f < FACE_COUNT; f++)
        if (!r.surface.Resolve((SurfaceFace)f).Has(SURF_INVISIBLE)) return true;
    return false;
}

// ---------------------------------------------------------------------------
// DeriveLegacyRenderable — write surface.def's derived views onto `r`.
//
// The renderable-shaped wrapper around oz::surface::DeriveLegacySurfaceMirror,
// which is raylib-free and lives in the header the headless tests link. EVERY
// writer of surface.def must call this; see that function for why re-deriving
// surfaceFlags alone is not enough (ExportToOzone emits the brush-wide UV
// fields from the legacy mirrors, so a stale mirror is a lost edit).
// ---------------------------------------------------------------------------
inline void DeriveLegacyRenderable(OzoneRenderable& r) {
    const oz::surface::LegacySurfaceMirror m =
        oz::surface::DeriveLegacySurfaceMirror(r.surface.def);
    r.surfaceFlags = m.surfaceFlags;
    r.texScaleU    = m.texScaleU;
    r.texScaleV    = m.texScaleV;
    r.texOffsetU   = m.texOffsetU;
    r.texOffsetV   = m.texOffsetV;
    r.texPath      = m.texPath;
}

// Collision AABB for an OZONE brush primitive.
struct OzoneCollisionVolume {
    BoundingBox aabb;
    int typeId = 0;
    int texSlot = 0;       // 0 = auto/default, 1..6 = face texture tileset index
    int texSlots[6] = {0}; // per-face textures: +X,-X,+Y,-Y,+Z,-Z
    std::string texPath;   // filesystem/package path to texture (non-tileset)
    float texScaleU = 1.0f;  // texture tiling/repeat U
    float texScaleV = 1.0f;  // texture tiling/repeat V
    float texOffsetU = 0.0f; // texture shift U
    float texOffsetV = 0.0f; // texture shift V
    bool isHeightmap = false; // true = terrain volume (support via ground clamp, never an obstacle)
};

// ---------------------------------------------------------------------------
// OzoneEntitySet — every non-brush primitive of a world, parsed but NOT yet
// injected into any runtime system.
//
// OzoneLoader used to push entities straight into PawnSystem::Instance() while
// it was still parsing, which made loading a side-effecting operation that could
// not be tested, replayed or rolled back. Parsing now stops here: the loader
// fills an OzoneEntitySet and the world orchestrator (Core.hpp LoadWorld, and
// AngelEd when opening a world) applies it explicitly via
// InjectOzoneEntities().
// ---------------------------------------------------------------------------

// NPC spawn cannot be resolved while parsing (it needs the PawnDef registry),
// so it is recorded as a deferred request.
struct PawnSpawnRequest {
    Vector3 position{0, 0, 0};
    std::string defName;
};

// A gameplay trigger volume (OZONE `trigger <def> minX..maxZ [once]`). Injected
// into oz::events::TriggerSystem, which the client queries per frame and turns
// into EventBus trigger.enter/exit facts dispatched to the `: trigger` def.
struct TriggerNode {
    std::string defName;
    BoundingBox bounds{};
    bool once = false;
    int  team = 0;
};

struct OzoneEntitySet {
    std::vector<PlayerStartNode>     playerStarts;
    std::vector<PickupNode>          pickups;
    std::vector<ZoneVolumeNode>      zones;
    std::vector<SkyZoneNode>         skyZones;
    std::vector<ZonePortal>          portals;
    std::vector<LightNode>           lights;
    std::vector<MeshObjectNode>      meshObjects;
    std::vector<PathNode>            pathNodes;
    std::vector<WindZoneNode>        windZones;
    std::vector<DoorNode>            doors;
    std::vector<ParticleEmitterNode> particleEmitters;
    std::vector<EmitterNode>         emitters;
    std::vector<TriggerNode>         triggers;
    std::vector<PawnSpawnRequest>    pawnSpawns;
    // `levelinfo` and `particles` metadata (game rules, skybox, weather).
    LevelSettings                    settings;

    void Clear() { *this = OzoneEntitySet{}; }
    bool Empty() const {
        return playerStarts.empty() && pickups.empty() && zones.empty() &&
               skyZones.empty() && portals.empty() && lights.empty() &&
               meshObjects.empty() && pathNodes.empty() && windZones.empty() &&
                doors.empty() &&
               particleEmitters.empty() && emitters.empty() && triggers.empty() &&
               pawnSpawns.empty();
    }
    size_t Count() const {
        return playerStarts.size() + pickups.size() + zones.size() +
               skyZones.size() + portals.size() + lights.size() +
               meshObjects.size() + pathNodes.size() + windZones.size() +
                doors.size() +
               particleEmitters.size() + emitters.size() + triggers.size() +
               pawnSpawns.size();
    }
};

// Zone names are generated per world (zone_sky_0, ...) so the counters must be
// reset on every parse.
using OzoneZoneCounters = std::unordered_map<std::string, int>;

class OzoneLoader {
public:
    bool LoadFile(const char* path);
    // Load OZONE text directly. When worldDir is supplied, tileset textures are
    // loaded from <worldDir>/oztex/tileset/ first so texSlot indices resolve
    // (used by the editor's undo/redo snapshot restore).
    bool LoadString(const char* data, const char* worldDir = nullptr);

    // Editor "all renderables" pass.
    //
    // `cullBackfaces` is the caller's ViewMode-derived intent, passed in rather
    // than inferred: AngelEd derives it from LightingMode, which this file must
    // not know about (it also builds into worldcheck and the headless tests).
    // The value is APPLIED through the shared tracked setter, not merely
    // inherited, because the editor's culling call used to live inside its
    // skybox block — a world with no skybox texture never set it and silently
    // inherited the previous frame's state. Imported meshes drawn after this
    // pass (PawnSystem::DrawEntities -> oz::Mesh::Draw) opt back out via
    // oz::ScopedCullOff and restore this value; see Renderer/CullState.hpp.
    void Draw(Camera3D& camera, bool cullBackfaces = true);
    void Unload();

    int Count() const { return (int)m_renderables.size(); }
    OzoneRenderable* Get(int index);

    // Collision volumes for OZONE brush primitives
    const std::vector<OzoneCollisionVolume>& GetCollisionVolumes() const { return m_collisionVolumes; }
    void RebuildCollisionVolumes();

    // Dynamic collision side-list (Phase 7.2 quantum obstacles). Rebuilt by
    // RebuildCollisionVolumes(): one blocker per SURF_QUANTUM brush, in
    // m_quantumIndices order, and those brushes are excluded from the static CSG
    // output because a toggled brush cannot live in a CSG result (it carries no
    // brush identity). The runtime consultation lives in Core.hpp's
    // EngineCollisionQuery; the per-frame toggle lives in UpdateQuantumObstacles.
    oz::physics::DynamicCollision& GetDynamicCollision() { return m_dynamicCollision; }
    const oz::physics::DynamicCollision& GetDynamicCollision() const { return m_dynamicCollision; }
    // Renderable indices that carry SURF_QUANTUM, in the same order as the
    // side-list blockers (index i in one names index i in the other).
    const std::vector<int>& GetQuantumIndices() const { return m_quantumIndices; }

    // CSG overflow count from the most recent RebuildCollisionVolumes(). Non-zero
    // means at least one sub/intersect brush hit the per-brush carve budget and
    // stopped early, leaving geometry the author meant to cut still solid.
    // Read by the editor on world open so a pre-existing broken level is loud
    // immediately, not only on the next brush commit.
    int LastCsgOverflowCount() const { return m_lastCsgOverflow; }

    // Editor: add a brush renderable so it becomes visible in the viewport
    // `pos` is the desired WORLD CENTRE of the mesh (what the editor's placement
    // ghost and the selection gizmo use). The stored renderable position is
    // derived from it, so cylinder/pyramid keep their top-based convention.
    // Optional taper values let Duplicate preserve a cone instead of flattening
    // it to a uniform tube.
    int AddBrushRenderable(int primType, const Vector3& pos, const Vector3& size,
                           float rot, float scale, int csgOp,
                           int surfaceFlags = 0,
                           float cylRTop = -1.0f, float cylRBot = -1.0f,
                           int cylSlices = 0);

    // Editor: voxelise a triangle soup into convex collision boxes and append
    // each one as a SURF_COLLISION_PROXY brush, then rebuild the collision
    // world. `verts` is a world-space float array (3 floats per vertex) and
    // `vertFloatCount` counts FLOATS. Returns the number of boxes appended, or
    // 0 when the source is unusable or the box budget was exceeded - it never
    // appends a partial hull, because half a collision wall is worse than none.
    // `failOut`, when given, receives WHY it refused so the editor can say so
    // instead of blaming the budget for every cause.
    int AppendAutoConvexCollision(const float* verts, int vertFloatCount,
                                  float cellSize, int maxBoxes,
                                  AutoConvexFail* failOut = nullptr);

    // Editor: show/hide SURF_COLLISION_PROXY brushes in Draw() (the editor's
    // "all renderables" path). DrawWorldGeometry always hides them, so the
    // client can never see them.
    void SetDrawCollisionProxies(bool on) { m_drawCollisionProxies = on; }

    // Viewport aspect for the frustum cull. 0 = GetScreenWidth/Height (the
    // client). AngelEd sets the ACTIVE PANE's aspect before each Draw: a split
    // pane has its own projection, and using the window aspect built a frustum
    // that was too narrow for the 2x1 layout and culled visible geometry.
    void SetCullAspect(float aspect) { m_cullAspect = (aspect > 0.0f) ? aspect : 0.0f; }
    float GetCullAspect() const { return m_cullAspect; }

    // Recompute one renderable's cull AABB after a transform write. AngelEd's
    // gizmo sync writes r->position/scale/rotation directly every frame (the
    // collision rebuild is deliberately deferred to drag end), so without this
    // the selected brush is culled against its OLD box while being moved.
    void RefreshRenderableBounds(int idx);
    bool GetDrawCollisionProxies() const { return m_drawCollisionProxies; }

    // Apply UV transform to a renderable's mesh (updates texcoords on GPU)
    void ApplyRenderableUV(int idx, float su, float sv, float ou, float ov);

    // Editor: remove a brush renderable by index (used for delete)
    void RemoveRenderable(int idx);

    // Editor: regenerate a brush renderable with new position/size/rotation
    void UpdateBrushRenderable(int idx, const Vector3& pos, const Vector3& size, float rot);

    // Editor: apply a custom texture file to a renderable's model material
    bool ApplyRenderableTexture(int idx, const char* path);

    // --- Surface properties (UT99-style per-face) ---------------------------
    // Read/modify a renderable's brush-wide default and per-face overrides.
    // The editor's Surface Properties dialog writes through SetRenderableFace
    // and then calls RebuildSurfaceMeshes so the per-face sub-meshes match.
    oz::surface::BrushSurface* GetSurface(int idx) {
        OzoneRenderable* r = Get(idx);
        return r ? &r->surface : nullptr;
    }
    // Set one face (or the brush default when `face` is FACE_NONE).
    void SetRenderableFace(int idx, oz::surface::SurfaceFace face,
                           const oz::surface::SurfaceProps& p);
    // Drop every per-face override, leaving the brush-wide default.
    void ResetRenderableSurface(int idx);
// Reset the BRUSH-WIDE default only, leaving per-face overrides intact.
// Distinct from ResetRenderableSurface because BrushSurface::ResetToDefault
// deliberately keeps def. This is what the editor's brush-wide scope resets.
void ResetRenderableSurfaceDefault(int idx);
    // Split mesh 0 into per-face sub-meshes and re-upload. No-op until the brush
    // actually needs per-face drawing.
    void RebuildSurfaceMeshes(int idx);

    // Spatial partitioning for efficient collision queries
    const WorldChunkManager& GetChunkManager() const { return m_chunkManager; }

    // Heightmap terrain (loaded from OZONE heightmap primitive)
    bool HasHeightmap() const { return m_hmReady; }
    float SampleHeightmapY(float px, float pz) const;
    const Model& GetHeightmapModel() const { return m_hmModel; }
    Vector3 GetHeightmapPosition() const { return m_hmPosition; }
    Vector3 GetHeightmapSize() const { return m_hmSize; }
    float GetHeightmapScale() const { return m_hmScale; }
    // Original (relative) source paths for round-trip export
    const std::string& GetHeightmapImagePath() const { return m_hmImageRel; }
    const std::string& GetHeightmapTexturePath() const { return m_hmTexRel; }

    // Heightmap grid access for terrain editing
    int GetHeightmapGridW() const { return m_hmGridW; }
    int GetHeightmapGridH() const { return m_hmGridH; }
    float GetHeightmapCellSize() const {
        return (m_hmGridW > 1) ? (m_hmSize.x / (float)(m_hmGridW - 1)) : 0.0f;
    }
    float GetHeightAtGrid(int col, int row) const;
    void SetHeightAtGrid(int col, int row, float height, bool triggerRebuild = true);
    void RebuildHeightmapMesh();

    // Draw only the renderables with SURF_FAKEBACKDROP flag set
    // (used for skybox rendering from SkyZone camera)
    void DrawZoneGeometry(Camera3D& camera, const BoundingBox& zoneBounds);
    void DrawZoneGeometry(Camera3D& camera); // without bounds filter

    // Draw all non-FAKEBACKDROP renderables (main world pass)
    void DrawWorldGeometry(Camera3D& camera);

    // Which faces a given pass is responsible for.
    //
    // A face must be drawn in EXACTLY one of these passes. That used to be a
    // convention spread across three functions - DrawZoneGeometry filtered on the
    // brush-wide SURF_FAKEBACKDROP mirror, DrawWorldGeometry skipped whole
    // brushes, and DrawGlowGeometry re-drew all six faces of any brush with one
    // glowing face - which is why a per-face backdrop could be drawn twice and a
    // wall with a single glowing face picked up an additive ghost of its other
    // five. Making the filter a parameter makes "exactly once" structural.
    enum class SurfacePass {
        Full,          // the OPAQUE world pass: every face except backdrop/portal/blended
        GlowingOnly,   // the glow halo: only SURF_GLOW faces
        BackdropOnly,  // the sky pass: only SURF_FAKEBACKDROP faces
        SkyPortalOnly, // the portal pass: only SURF_PORTAL faces, sampled from the sky view
        // TRANSLUCENT / ALPHABLEND faces. They write no depth (a blended surface
        // must not occlude what is behind it), so drawing them inside the opaque
        // pass made them ORDER-DEPENDENT: an opaque brush at a higher renderable
        // index passed the depth test and erased the glass. They are drawn in a
        // second, back-to-front pass after all opaque geometry instead.
        BlendedOnly,
    };

    // Draw one renderable through the surface-flagged path: up to six per-face
    // DrawMesh calls, each with its own SurfaceProps uniforms. Public because the
    // glow and backdrop passes re-draw subsets of faces outside the world pass.
    //
    // `pass` selects which faces are eligible; see SurfacePass above.
    void DrawSurface(OzoneRenderable& r, SurfacePass pass = SurfacePass::Full);
    // Faces carrying SURF_GLOW, re-drawn additively over the composited frame.
    void DrawGlowGeometry(Camera3D& camera);

    // SURF_PORTAL faces, drawn from the pre-rendered sky view (`skyView`, see
    // Renderer/SkyPortal.hpp). `vpW/vpH` must be the size of the framebuffer
    // being drawn into (the client renders 3D into `Target`, which can be
    // resolution-scaled), because the shader samples by gl_FragCoord.
    //
    // Portal faces are opaque and depth-tested: the face is its own mask, so no
    // stencil buffer or second world render is involved.
    void DrawSkyPortals(Camera3D& camera, Texture2D skyView, int vpW, int vpH);

    // True when any face in the loaded world carries SURF_PORTAL. Callers gate
    // the (cheap but pointless) sky-view render on this.
    bool HasSkyPortalFaces() const;

    // Same question for the other two per-face flags that have a dedicated
    // pass. All three share ONE cached scan (m_faceFlagCache); the passes used
    // to walk every renderable x 6 faces on every frame, twice for portals and
    // twice for glow.
    bool HasGlowFaces() const;
    bool HasBackdropFaces() const;

    // Resolve a FACE handle through the renderable's cache. The props-only
    // overload below stays for the editor's one-shot probe.
    Texture2D ResolveFaceTextureHandle(OzoneRenderable& r, int face);

    // Editor integration — heightmap generation (called from editor main loop)
    Model BuildHeightmap(const std::string& imagePath, const std::string& texPath,
                         const std::vector<float>& args);

    void LoadWorldTextures(const std::string& worldDir);
    void SetLitFogShader(Shader shader);
    void SetLitFogShaderEnabled(bool enabled);
    void ApplyTexSlotToModel(Model& model, int slot);

    // The texture a face should bind, or a zero Texture2D meaning "keep whatever
    // the brush's material already carries". Resolves surface.def's texPath /
    // texSlot into a handle, loading and caching an explicit path on first use.
    // The precedence itself lives in oz::surface::ResolveFaceTexture so the
    // renderer and the tests share one definition.
    Texture2D ResolveFaceTextureHandle(const oz::surface::SurfaceProps& p);

    // Load and cache a world texture by its RESOLVED path. The cache is the sole
    // owner: every caller gets a borrowed handle and must NOT UnloadTexture it.
    // This is what makes a level with 70+ `texPath=` brushes load the shared
    // tileset files a handful of times instead of once per brush (the difference
    // between ~300 ms and ~30 ms on Sektor_01_SilentPines).
    Texture2D GetCachedWorldTexture(const std::string& resolvedPath);

    // Publish the world's current `ambient` uniform so DrawZoneGeometry can put
    // it back. raylib has no GetShaderValue (and adding one would not help: the
    // value lives on the GL program, not in our code), so the owner of the
    // uniform has to declare it. DrawZoneGeometry runs from Core.hpp's sky pass
    // immediately before DrawWorldGeometry in the same frame, so restoring a
    // constant instead of the real value made every world surface in a sky zone
    // render with ambient/10 == 0.1.
    void SetWorldAmbient(float r, float g, float b, float a);

    // Read back what SetWorldAmbient published.
    //
    // Exists because oz::SurfaceMaterial carries its OWN copy of the `ambient`
    // uniform and must be fed the same value the world uses — a hardcoded
    // {0.1,0.1,0.1,1} there lit every surface-flagged brush at a flat 0.1 no
    // matter how bright the room was. Same "no GetShaderValue" reasoning as
    // above: the value lives in our code precisely because it also has to be
    // restorable, so reading it back is legitimate rather than a leak of GL state.
    void GetWorldAmbient(float out[4]) const {
        if (!out) return;
        out[0] = m_worldAmbient[0];
        out[1] = m_worldAmbient[1];
        out[2] = m_worldAmbient[2];
        out[3] = m_worldAmbient[3];
    }

    // Same story for the five fog uniforms. Core.hpp sets them on the LitFog
    // program from three separate places (level defaults, zone entry, zone exit),
    // and the surface program needs the same numbers. Owning the values here
    // gives UpdateLightSources ONE mirror point instead of three that can drift.
    void SetWorldFog(const float color[3], float start, float end,
                     float density, float intensity = 1.0f);
    void GetWorldFog(float colorOut[3], float& start, float& end,
                     float& density, float& intensity) const;

    // Access loaded tileset textures by 0-based index (0=auto, 0+ = vector index-1)
    int TilesetCount() const { return (int)m_tilesetTex.size(); }
    Texture2D GetTilesetTex(int idx) const {
        if (idx < 1 || idx > (int)m_tilesetTex.size()) return Texture2D{0};
        return m_tilesetTex[idx - 1];
    }

    // --- Parsed world data (no side effects) ------------------------------
    const OzoneEntitySet& GetEntities() const { return m_entities; }
    const LevelSettings& GetLevelSettings() const { return m_entities.settings; }
    // Directory the world was loaded from ("" when LoadString got no worldDir).
    const std::string& GetWorldDir() const { return m_worldDir; }

    static OzoneLoader& Instance();
    
    static Shader GetLitFogShader() { return s_litFogShader; }

private:
    // ONE primitive -> renderable pass, shared by LoadFile() and LoadString().
    // They used to be a copy each and the copies drifted (see the definition).
    // CSG is rebuilt by the callers, not here.
    bool LoadPrimitives(std::vector<OzonePrimitive>& primitives,
                        const std::string& gameDataWorldDir,
                        const std::string& worldDir);

    // Second pass: every blended (translucent / alpha-blended) face of the
    // renderables named by `visible`, sorted back-to-front. Drawn after the
    // opaque pass by BOTH Draw() (editor) and DrawWorldGeometry() (client), so
    // the two cannot disagree about what a pane of water looks like.
    void DrawBlendedFaces(Camera3D& camera, const std::vector<int>& visible);

    static Shader s_litFogShader;
    static Shader s_backupLitFogShader;
    std::vector<OzoneRenderable> m_renderables;
    std::vector<OzoneCollisionVolume> m_collisionVolumes;
    // Quantum obstacle side-list + the renderable indices it mirrors, rebuilt
    // together by RebuildCollisionVolumes(). Both empty in a world with no
    // SURF_QUANTUM brush, which is what makes the per-frame toggle free there.
    oz::physics::DynamicCollision m_dynamicCollision;
    std::vector<int> m_quantumIndices;
    WorldChunkManager m_chunkManager;
    bool m_drawCollisionProxies = false;  // editor-only debug view of SURF_COLLISION_PROXY
    float m_cullAspect = 0.0f;            // 0 = window aspect; editor sets the pane aspect
    float m_worldAmbient[4] = {0.1f, 0.1f, 0.1f, 1.0f};  // see SetWorldAmbient
    float m_worldFogColor[3] = {0.7f, 0.7f, 0.8f};      // see SetWorldFog
    float m_worldFogStart = 10.0f;
    float m_worldFogEnd = 100.0f;
    float m_worldFogDensity = 1.0f;
    float m_worldFogIntensity = 1.0f;

    // Heightmap state (set from OZONE heightmap primitive)
    bool m_hmReady = false;
    Image m_hmImage{0};
    Texture2D m_hmTexture{0};
    Model m_hmModel;
    Vector3 m_hmPosition{0,0,0};
    Vector3 m_hmSize{100,50,100};
    float m_hmScale = 1.0f;
    int m_hmGridW = 0;           // grid columns (image width)
    int m_hmGridH = 0;           // grid rows (image height)
    std::vector<float> m_hmHeights; // CPU-side height values [row * w + col]
    std::string m_hmImageRel;    // relative image path as authored (for export)
    std::string m_hmTexRel;      // relative texture path as authored (for export)

    std::vector<Texture2D> m_tilesetTex;
    // World texture assets, keyed by the RESOLVED path. Loaded once per distinct
    // path and freed in Unload(). This is the SOLE OWNER of every explicit-path
    // texture (brush `texPath=`, per-face `tex=`, skybox): renderables' `customTex`
    // is a borrowed handle into this map, never unloaded per renderable. Sharing
    // one owner is what makes a 70-brush `texPath=` level load its shared files a
    // handful of times instead of 70 (and avoids the double-free a second owner
    // would cause).
    std::unordered_map<std::string, Texture2D> m_faceTexCache;

    // Cached "does any face carry X" scan. Bits: 1 portal, 2 glow, 4 backdrop.
    // -1 means "not computed since the last mutation". Every surface writer
    // invalidates it; the three Has*Faces() accessors share one pass.
    mutable int m_faceFlagCache = -1;
    void InvalidateFaceFlagCache() const { m_faceFlagCache = -1; }
    void EnsureFaceFlagCache() const;

    void InvalidateFaceTexCache(OzoneRenderable& r) {
        for (int f = 0; f < oz::surface::FACE_COUNT; ++f) {
            r.faceTexHandle[f] = Texture2D{0};
            r.faceTexCached[f] = false;
        }
    }

    std::string m_worldDir;        // current world directory (for .ozls def matching)
    int m_lastCsgOverflow = 0;     // overflow_count() from the last collision rebuild
    OzoneZoneCounters m_zoneCounters; // per-load zone name counters
    OzoneEntitySet m_entities;     // parsed, not-yet-injected entity nodes

    // Sky-portal pass (Source/Renderer/SkyPortal.hpp owns the offscreen view).
    // The shader is owned here, lazily loaded, and released in Unload() with the
    // face meshes - it is a real GL program, so it must not outlive the context.
    Shader m_skyPortalShader{0};
    int    m_skyPortalViewportLoc = -1;
    bool   m_skyPortalShaderTried = false;
    bool   EnsureSkyPortalShader();

    void UnloadTextures();
    void UnloadHeightmap();
    void ComputeCollisionAABB(int type, const std::vector<float>& args, Vector3 position, BoundingBox& out);

    Model BuildFromPrimitive(int type, const std::vector<float>& args);
    // Six inward-facing quads with projected UVs. Render-only: never contributes
    // to the CSG collision world. The texture is applied by the caller.
    Model BuildSkybox(float cx, float cy, float cz, float size, const char* topPath);

    Model BuildBox(float w, float h, float d);
    Model BuildCylinder(float rTop, float rBot, float h, int slices);
    Model BuildSphere(float r, int segments);
    Model BuildPyramid(float w, float d, float h);
    Model BuildPlane(float nx, float ny, float nz, float dist);
    Model BuildHeightmapMesh(const std::vector<float>& heights, int gw, int gh,
                             float cellX, float cellZ, float heightScale,
                             float uvTileSize = 8.0f);
};

// ---------------------------------------------------------------------------
// Entity ingestion — the explicit seam between world parsing and the engine.
// ---------------------------------------------------------------------------

// Convert every entity primitive into plain node structures. Pure: performs no
// allocation of GPU resources and touches no global system state.
// `worldDir` is used to resolve world-scoped .ozls skyzone defs.
void ParseOzoneEntities(const std::vector<struct OzonePrimitive>& primitives,
                        const std::string& worldDir,
                        OzoneZoneCounters& zoneCounters,
                        OzoneEntitySet& out);

// Apply a parsed entity set to the runtime systems (PawnSystem + ZoneManager)
// and copy the level metadata into the world's WorldInfo. Lights are bound to
// the zone volume that contains them. Safe to call on an empty set.
void InjectOzoneEntities(const OzoneEntitySet& entities, class PawnSystem& pawns);

// Apply the EntityType::LIGHT .ozls defaults layer to a light built in code.
// The OZONE load path uses the file-static ApplyLightDefDefaults, which needs
// the primitive to know which optional fields the line authored; a node built by
// the editor authored none, so every def-owned field applies. Resolved by the
// node's `name`, exactly as the load path does.
void ApplyLightDefDefaultsToNode(class LightNode& node);
