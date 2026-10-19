// AngelEd editor event bus — the replacement for EditorPanelState's 58 loose
// `action*` fields.
//
// Raylib-free AND Win32-free on purpose. That is what makes the central invariant
// testable from the headless suites: the 58 fields it replaces are only reachable
// from inside a WM_COMMAND handler and a raylib frame loop, so today none of
// them can be verified without clicking. Same discipline as ZoneTypes.hpp,
// GameType.hpp and SurfaceFlags.hpp.
//
// See Wiki/Editor-Architecture-Refactor.md for the design and the phased plan.
#ifndef ANGELS95_EDITOR_EVENT_BUS_HPP
#define ANGELS95_EDITOR_EVENT_BUS_HPP

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace ed {

// Entity kinds the editor can select. Deliberately a *separate* enum from
// AngelEd's SelType: this header must not depend on the editor's headers, and
// duplicating a 14-value enum is far cheaper than a layering violation. The
// conversion is asserted in tests/EditorEventBus.test.cpp by size.
enum class SelKind : uint8_t {
    None = 0,
    Brush, Model, Npc, Pickup, Light, Zone, Spawn, Portal,
    Mesh, Particle, PathNode, WindZone, Map,
};

// Position / size / rotation. Grouped so a resize cannot be posted with a
// rotation for some other entity's transform.
//
// Declared ABOVE SpawnDesc because SpawnDesc nests it. It was introduced for the
// CSG brush ghost and sat below SpawnDesc; nesting needs it first.
struct Transform {
    float x = 0, y = 0, z = 0;
    float w = 8, h = 8, d = 8;
    float yaw = 0;
};

// What to place. One payload for the whole Spawn* family, which is the point:
// ten `actionSpawn*` fields were ten spellings of "place something", and nothing
// stopped the wrong one being read.
//
// Each field below is scoped to specific event kinds and unused by the rest.
// That is a deliberate trade against a struct per kind: the kinds are a closed set
// of ten, and the alternative is ten near-identical structs whose only difference
// is two fields. The per-kind scoping is what keeps it honest, so it is stated
// per field rather than left to be inferred.
struct SpawnDesc {
    std::string key;        // def name, or the model path for SpawnMesh
    int         kind = 0;   // SpawnZone only: ZoneType
    bool        solid = false;   // CSG op: SOLID when placed directly
    Transform   at;         // position for every kind; w/h/d are zone + wind-zone
                             // HALF-extents; yaw is mesh + playerstart
    bool        skeletal = false;   // SpawnMesh only
    int         lightType = 0;     // SpawnLight only: LitLightType
};

// A selection, captured BY VALUE at post time.
//
// This is the whole reason the bus exists. The `action*` fields carried bare
// indices, so if the selection moved between the click and the frame that polled
// it, the operation landed on a different entity. Portal delete already had to be
// defended by hand for exactly this (see the b88 notes). Capturing the target
// with the event makes it impossible by construction.
struct SelRef {
    SelKind kind = SelKind::None;
    int     index = -1;
    bool valid() const { return kind != SelKind::None && index >= 0; }
};

// Level state lives in Subsystems/LevelState and only the Map row edits it, so
// it needs no selection reference.
struct LevelStateEdit {
    std::string worldName;
    int gameType = 0;
};

// Enter a placement mode.
//
// The Pickups panel used to send a LIST INDEX and the main loop resolved it via
// LegacyPickupType(idx) at drain time — so a panel rebuild between the click and
// the frame placed a different pickup. The def NAME is resolved by the poster and
// travels here instead.
struct PlacementRequest {
    enum class Kind : uint8_t { Pickup, Node, Model };
    Kind kind = Kind::Model;
    std::string key;      // pickup def name; node type name for Kind::Node
};

// Apply the browser's selected texture to the viewport selection. The SelRef is
// captured BY VALUE at post time, the same reasoning SelRef itself documents:
// the Texture Manager is a MODELESS window, so the modal-writer argument that
// keeps actionApplyTextureToSel honest does not apply here — a queued viewport
// click between the Apply button and the frame drain must not redirect the
// apply onto a different brush than the one the user had selected.
struct TextureSelectionApply {
    std::string path;   // asset path (loose file or package key)
    SelRef      target; // captured selection; invalid -> handler skips it
};

// A heightmap build request. Six separate action* fields used to carry this, and
// four of them (image, texture, scale, size) were read at drain time — so a panel
// edit between the click and the frame generated the heightmap from values the
// panel had already moved past.
struct HeightmapDesc {
    std::string imagePath;
    std::string texturePath;
    float x = 0, y = 0, z = 0;
    float scale = 1.0f;
    float sizeX = 100, sizeY = 50, sizeZ = 100;
};

// An animation-editing intent.
//
// The clip name and the mesh id travel with the event because the anim panel's
// live fields were previously read at drain time. "Delete Clip" reads
// `animClipName` — so a click whose list selection moves before the frame drains
// deletes a different clip. That is the same staleness class as the WorldGraph
// index bug, in a place where it is far easier to trigger (a list selection moves
// on every click).
struct AnimIntent {
    int  meshId = -1;        // MeshObjectNode id of the anim target at post time
    std::string clipName;    // clip the command applies to, "" when not clip-scoped
    float time = 0.0f;       // playhead position in seconds
    float fps = 30.0f;       // for NewClip / ApplyClipMeta
    bool loop = true;

    bool hasClip() const { return !clipName.empty(); }
};

// CSG sidebar intent.
//
// Split into "start placing this primitive" and "commit the current ghost with
// this operation" because they are genuinely different: CsgPlace resets the ghost
// to a default box at the camera, while CsgCommit reads whatever the ghost
// currently is. Collapsing them into one event would either lose the reset or
// commit a brush the user never positioned.
struct CsgIntent {
    int primitive = -1;   // 0=box 1=cyl 2=sph 3=pyr 4=pln
    int operation = -1;   // CsgOp value; -1 means "use the ghost's current op"
    bool solid = false;   // commit immediately as SOLID (Add/Solid buttons)

    bool isPrimitive() const { return primitive >= 0; }
    bool isCommit() const { return operation >= 0; }
};

// A per-face surface-properties edit.
//
// Deliberately captures the renderable, the face mask AND the working props. The
// mask matters most: `ApplyToSelection` is a documented no-op on an empty mask
// precisely so it can never mean "apply to all six faces", and a payload that
// carried only "apply" would let the mask be read from whatever the panel shows
// *now* instead of what the user was looking at when they clicked.
struct SurfaceEdit {
    int  renderable = -1;
    uint32_t faceMask = 0;
    uint32_t flags = 0;
    float glowR = 0, glowG = 0, glowB = 0, glowScale = 1.0f;
    float alpha = 1.0f, alphaCutoff = 0.0f;
    float uvScaleU = 1.0f, uvScaleV = 1.0f, uvOffsetU = 0.0f, uvOffsetV = 0.0f;
    float panU = 0.0f, panV = 0.0f;
    int  texSlot = 0;
    // Free-placement texture path. This WAS smuggled through live panel state
    // instead of the event, because the payload had no field for it - which is
    // precisely the hazard the comment above describes, and it worked only
    // because the drain happens in the same frame.
    std::string texPath;

    // Write BrushSurface::def (the brush-wide default) instead of the faces in
    // faceMask. Brush-wide is what makes `flags=8` on a whole brush - the form
    // all 299 shipped painted backdrops use - reachable from the editor at all;
    // every Apply before this wrote only per-face overrides.
    //
    // It is an EXPLICIT bool precisely because it must never be expressed as
    // faceMask == 0. Zero is the documented "no face selected, do nothing" value,
    // and overloading it would resurrect exactly the bug hasFaces() exists to
    // prevent. A brush-wide edit carries BOTH: faceMask still names the faces the
    // dialog was showing, so a handler that ignores brushWide still does the
    // narrow thing.
    bool brushWide = false;

    bool hasFaces() const { return faceMask != 0; }
    // True when this edit targets anything at all. Note brushWide alone is NOT
    // enough: faceMask must still be non-zero, so "no face was ever selected"
    // remains a no-op regardless of scope.
    bool isTargeted() const { return faceMask != 0; }
};

// A complete selection, captured by value.
//
// This replaces FIVE separate action fields — actionSelectFromGraph plus its
// Type/Name/Pos[3] satellites — which were written together and read together,
// but were not coupled by anything: a handler could read the new index with the
// previous frame's type and position and never notice.
//
// It also fixes the index-space confusion the WorldGraph had: its Properties
// entry stored a LIST ROW index while Delete/Duplicate stored an ENTITY index, in
// fields with near-identical names. One type, one index space.
struct Selection {
    SelRef    ref;
    std::string name;
    float x = 0, y = 0, z = 0;

    bool valid() const { return ref.valid(); }
};

enum class Ev : uint8_t {
    None = 0,

    // Placement. Payload: SpawnDesc.
    SpawnPawn, SpawnMesh, SpawnPickup, SpawnEmitter, SpawnZone,
    SpawnParticleEmitter, SpawnPathNode, SpawnWindZone, SpawnPlayerStart,
    // Lights were the one placement path with no event: the Actor-Hierarchy tree
    // can emit light_point / light_spot / light_directional, and SpawnLight was
    // never added, so those three stayed as direct PawnSystem calls from the panel.
    // The kind is explicit rather than a `lightType` field on SpawnMesh because the
    // three differ in more than a tag - a directional light is authored by its
    // SOURCE and aimed at the origin, so it seeds 40 units above the aim point.
    SpawnLight,

    // Enter a placement mode (pickup ghost / node ghost / model ghost).
    // Payload: PlacementRequest - carries the resolved def NAME, not a list index.
    BeginPlacement,

    // CSG. Payload: CsgIntent — isPrimitive() for CsgPlace, isCommit() for CsgCommit.
    CsgPlace, CsgCommit,

    // Selection. Payload: Selection (see below) or SelRef.
    DeleteEntity, DuplicateEntity, SelectEntity,
    // ApplyProperties WRITES the Properties panel's edited values (payload: SelRef,
    // consumed by ApplyPanelProperties). OpenProperties only OPENS that panel on a
    // row (payload: Selection, from the WorldGraph's "Properties" menu item,
    // consumed by DispatchFrameEvents). These used to be ONE kind, and because the
    // bus is a single queue whose first drain takes everything, the open path
    // swallowed every apply. Keeping them distinct is what lets each pass own
    // exactly one meaning.
    ApplyProperties, OpenProperties,

    // Portal deletion, deliberately NOT folded into DeleteEntity: it mutates
    // ZoneManager::GetPortals(), a different container from the entity lists
    // DeleteSelectedEntity understands, and its index space is a GetPortals index
    // rather than an entity id. Keeping them apart stops the dispatcher treating
    // one as the other.
    DeletePortal,              // Payload: SelRef

    // Payload: std::string (world folder name).
    OpenWorld, LinkWorld,

    // Surface. Payload: SurfaceEdit — carries the renderable, the face mask AND
    // the working props, so an Apply cannot land on a different face selection than
    // the one the dialog was showing.
    ApplySurface, ResetSurface,

    // Assets.
    ApplyTextureToSelection,   // Payload: TextureSelectionApply (path + captured SelRef)
    ReloadMesh,                // Payload: SelRef
    ConvertToAnimated,         // Payload: SelRef
    PlaySound,                 // Payload: std::string (path)
    StopSoundPreview,          // Payload: none

    // Heightmap.
    GenerateHeightmap,         // Payload: HeightmapDesc

    // Model browser list changed and the preview must re-read it. A DIRTY FLAG, not
    // a user action - see actionAnimRefresh for why that distinction matters.
    RefreshModelBrowser,       // Payload: none

    // Level state. Payload: LevelStateEdit.
    ApplyLevelState,

    // Animation editing. Payload: AnimIntent (mesh id + clip name + time, captured at
    // post time). AnimSave and AnimRefresh are NOT events - see below.
    AnimNewClip, AnimDeleteClip, AnimScrub,
    AnimAddKey, AnimDeleteKey, AnimApplyClipMeta, AnimToggleEdit,
    AnimUndo, AnimRedo, AnimSelectAll, AnimClearSelection,

    // Sentinel, not an event. tests/EditorEventBus.test.cpp asserts its value
    // against the number of events it names, so adding an Ev without adding it to
    // that list fails the suite instead of silently going untested — the same
    // drift that let ID_PP_PORTALBROWSE and ID_BTN_GT_PREVIEW sit in the editor
    // declared, created and never handled.
    Count,
};

// The payload variant. One type per shape of data, so the post site is checked by
// the compiler against the event kind and a SpawnZone cannot be posted carrying a
// filename. A generic string-topic bus would have traded that compile error for a
// runtime typo.
using Payload = std::variant<std::monostate,
                             int,
                             float,
                             bool,
                             std::string,
                             SelRef,
                             Selection,
                             SurfaceEdit,
                             AnimIntent,
                             PlacementRequest,
                             TextureSelectionApply,
                             HeightmapDesc,
                             CsgIntent,
                             SpawnDesc,
                             Transform,
                             LevelStateEdit>;

struct Event {
    Ev      kind = Ev::None;
    Payload data{};

    template <class T>
    static Event make(Ev k, T v) { return Event{k, Payload(std::move(v))}; }

    // Typed accessors. Each asserts the shape it expects, because a mismatch here
    // means a call site and a handler disagree about the event — exactly the
    // class of bug the loose fields could not even represent.
    const SpawnDesc& spawn() const;
    const SelRef&    sel()   const;
    const Selection& selection() const;
    const SurfaceEdit& surface() const;
    const AnimIntent& anim() const;
    const PlacementRequest& placement() const;
    const TextureSelectionApply& textureSelectionApply() const;
    const HeightmapDesc& heightmap() const;
    const CsgIntent&   csg() const;
    const Transform& xform() const;
    const std::string& str() const;
    const LevelStateEdit& level() const;

    // The target this event names, or None when it is not selection-scoped.
    // Used by the dispatcher to reject a stale event against the live selection
    // rather than silently acting on whatever happens to be selected.
    // Handles BOTH SelRef and Selection payloads.
    SelRef target() const;
};

// ---------------------------------------------------------------------------
// Per-pass kind filters
// ---------------------------------------------------------------------------
// main() drains the one bus queue once per subsystem, and each pass must see only
// the kinds its handler understands. These predicates are the single definition of
// "which kinds belong to which pass"; test_editorbus asserts they partition the
// enum so a new Ev cannot be added without being classified.
//
// The frame pass is the catch-all (everything not claimed below), so an event with
// no handler still reaches DispatchFrameEvents and is counted as unhandled rather
// than left in the queue forever.
inline bool IsSurfaceEvent(const Event& e) {
    return e.kind == Ev::ApplySurface || e.kind == Ev::ResetSurface;
}

inline bool IsPlacementEvent(const Event& e) {
    switch (e.kind) {
        case Ev::SpawnPawn: case Ev::SpawnMesh: case Ev::SpawnPickup:
        case Ev::SpawnEmitter: case Ev::SpawnZone: case Ev::SpawnParticleEmitter:
        case Ev::SpawnPathNode: case Ev::SpawnWindZone: case Ev::SpawnPlayerStart:
        case Ev::SpawnLight:
            return true;
        default:
            return false;
    }
}

inline bool IsAnimEvent(const Event& e) {
    switch (e.kind) {
        case Ev::AnimNewClip: case Ev::AnimDeleteClip: case Ev::AnimScrub:
        case Ev::AnimAddKey: case Ev::AnimDeleteKey: case Ev::AnimApplyClipMeta:
        case Ev::AnimToggleEdit: case Ev::AnimUndo: case Ev::AnimRedo:
        case Ev::AnimSelectAll: case Ev::AnimClearSelection:
            return true;
        default:
            return false;
    }
}

// The Properties pass writes edited values; it owns ApplyProperties/ApplyLevelState
// only. OpenProperties stays with the frame pass (it opens the panel).
inline bool IsPropsEvent(const Event& e) {
    return e.kind == Ev::ApplyProperties || e.kind == Ev::ApplyLevelState;
}

// Catch-all: whatever no other pass claims. Deliberately defined in terms of the
// others so adding a new owned kind cannot leave it in two passes at once.
inline bool IsFrameEvent(const Event& e) {
    return !IsSurfaceEvent(e) && !IsPlacementEvent(e) && !IsAnimEvent(e) &&
           !IsPropsEvent(e);
}

class EventBus {
public:
    // Public on purpose. A bus you cannot instantiate is a bus you cannot test,
    // and the whole reason this type is raylib-free is so the queue can be
    // exercised directly instead of through a window procedure. `instance()` is
    // the convenience singleton for production, not the only way in.
    EventBus() = default;

    // Bounded on purpose. A panel that posts in a loop (a drag handler, say)
    // must not be able to grow the queue without limit; overflow is counted and
    // the newest event is dropped, so the failure is visible instead of silent.
    static constexpr size_t kMaxQueued = 256;

    void post(Event e) {
        m_posted++;
        if (m_queue.size() >= kMaxQueued) { m_dropped++; return; }
        m_queue.push_back(std::move(e));
    }

    template <class T>
    void post(Ev k, T v) { post(Event::make(k, std::move(v))); }

    // For the genuinely payload-less events (StopSoundPreview, ClosePanel).
    // Distinct from post<T> with a default T, which would accept a payload the
    // handler then silently ignores.
    void post(Ev k) { post(Event{k, Payload{}}); }

    // Move the queue out and empty it. Called once per frame, at the single point
    // where dispatching is safe — a panel is mid-layout while it is posting.
    //
    // This takes EVERYTHING. The per-subsystem passes in main() must therefore use
    // the filtered overload below: with one shared queue, an unfiltered drain in the
    // frame pass leaves nothing for surface/props/anim/placement, so those passes
    // saw empty vectors and dropped every event (Surface Properties, the Properties
    // panel's Apply, entity placement and animation commands). Kept unfiltered for
    // the tests and for callers that genuinely want the whole batch.
    void drain(std::vector<Event>& out) {
        out.clear();
        out.swap(m_queue);
    }

    // Move only the events matching `keep` out of the queue, preserving FIFO order.
    // Non-matching events stay queued for a later pass, so each pass gets exactly
    // its own kinds regardless of the order the drains are called in.
    template <class Keep>
    void drain(std::vector<Event>& out, Keep keep) {
        out.clear();
        for (auto it = m_queue.begin(); it != m_queue.end();) {
            if (keep(*it)) { out.push_back(std::move(*it)); it = m_queue.erase(it); }
            else ++it;
        }
    }

    size_t pending() const { return m_queue.size(); }
    size_t posted()   const { return m_posted; }
    size_t dropped()  const { return m_dropped; }
    void   clear()    { m_queue.clear(); m_dropped = 0; }

    // The bus is a process-wide singleton by design: the UI posts from a window
    // procedure and the dispatcher drains from the frame loop, with no other way
    // to hand a reference across.
    static EventBus& instance() { static EventBus b; return b; }

private:
    std::vector<Event> m_queue;
    // m_posted counts every request including drops, so `posted() - dropped()` is
    // everything that actually reached a handler and `pending()` is what is
    // waiting. An event that was posted but never drained is therefore a
    // visible, countable condition instead of a mystery.
    size_t m_posted = 0, m_dropped = 0;
};

} // namespace ed

#endif // ANGELS95_EDITOR_EVENT_BUS_HPP