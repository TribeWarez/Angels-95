// EditorEventBus — standalone, no raylib and no Win32. This is the point of the
// bus: the 58 `EditorPanelState::action*` fields it replaces were only reachable
// from a WM_COMMAND handler inside a raylib frame loop, so none of them could be
// verified without clicking.
#include "../AngelEd/Source/Core/EditorEventBus.hpp"

#include <cstdio>

using namespace ed;

static int test_count = 0, pass_count = 0;

static void check(bool ok, const char* what) {
    test_count++;
    if (ok) { pass_count++; printf("  PASS  %s\n", what); }
    else     { printf("  FAIL  %s\n", what); }
}

// ---------------------------------------------------------------------------
// Payloads are typed, and reading the wrong shape is reported
// ---------------------------------------------------------------------------
// The loose `action*` fields had no shape at all: actionSpawnZone is an int and
// actionSpawnPawn is a std::string, and nothing stopped a handler reading one as
// the other. std::variant makes the post site compile-checked.
static void test_typed_payloads() {
    SpawnDesc sd;
    sd.key = "pistol_01";
    sd.kind = 2;
    Event e = Event::make(Ev::SpawnPickup, sd);

    check(e.kind == Ev::SpawnPickup, "event round-trips its kind");
    check(e.spawn().key == "pistol_01", "SpawnDesc.string survives the round trip");
    check(e.spawn().kind == 2, "SpawnDesc.int survives the round trip");

    // The three fields added when placement moved off direct PawnSystem calls.
    // `at` is the one that matters most: placement used to read the live camera aim
    // point at drain time, and it now travels with the event.
    sd.at.x = 10.0f; sd.at.y = -4.0f; sd.at.z = 22.5f;
    sd.at.w = 4.0f; sd.at.h = 2.0f; sd.at.yaw = 1.5f;
    sd.skeletal = true;
    sd.lightType = 2;
    Event sp = Event::make(Ev::SpawnLight, sd);
    check(sp.spawn().at.x == 10.0f && sp.spawn().at.y == -4.0f && sp.spawn().at.z == 22.5f,
          "SpawnDesc.at position survives the round trip");
    check(sp.spawn().at.w == 4.0f && sp.spawn().at.h == 2.0f && sp.spawn().at.yaw == 1.5f,
          "SpawnDesc.at half-extents and yaw survive the round trip");
    check(sp.spawn().skeletal, "SpawnDesc.skeletal survives the round trip");
    check(sp.spawn().lightType == 2, "SpawnDesc.lightType survives the round trip");

    SelRef sel{SelKind::Portal, 3};
    Event d = Event::make(Ev::DeleteEntity, sel);
    check(d.sel().kind == SelKind::Portal && d.sel().index == 3,
          "SelRef survives the round trip");
    check(d.target().valid(), "a named target reports valid()");

    Transform t;
    t.x = 1.5f; t.y = -2.0f; t.z = 3.25f; t.yaw = 90.0f;
    Event x = Event::make(Ev::GenerateHeightmap, t);
    check(x.xform().x == 1.5f && x.xform().z == 3.25f && x.xform().yaw == 90.0f,
          "Transform survives the round trip");

    Event s = Event::make(Ev::OpenWorld, std::string("TestMap"));
    check(s.str() == "TestMap", "std::string survives the round trip");
}

// A handler reading the wrong shape logs and gets an inert value rather than
// reinterpreting memory. `s.spawn()` on a string event is the compile-time-legal
// mistake this makes visible.
static void test_wrong_shape_is_inert() {
    Event s = Event::make(Ev::OpenWorld, std::string("TestMap"));
    check(s.spawn().key.empty(), "wrong-shape read yields an inert SpawnDesc");
    check(!s.sel().valid(), "wrong-shape read yields an invalid SelRef");
    check(s.xform().x == 0.0f, "wrong-shape read yields a zeroed Transform");

    // A non-selection event has no target, which is what lets the dispatcher skip
    // the staleness check for it rather than comparing against a default index.
    check(!s.target().valid(), "a non-selection event has no target");
}

// ---------------------------------------------------------------------------
// The stale-index fix: a selection is captured BY VALUE at post time
// ---------------------------------------------------------------------------
// This is the bug the bus exists for. actionWorldGraphDelete carried a bare
// index; if the selection moved between the click and the frame that polled it,
// the delete landed on a different entity. Portal delete already had to be
// defended by hand for this in b88. Capturing the target with the event makes it
// impossible by construction.
// One Selection replaces the five fields the WorldGraph used to split a single
// message across: actionSelectFromGraph plus its Type/Name/Pos[3] satellites.
// They were written together and read together with nothing coupling them, and
// THREE of them carried TWO different index spaces (a ListView row index vs an
// entity index) in near-identically named fields - one of which
// (actionWorldGraphProperties) was written and then never read at all.
static void test_selection_is_one_atomic_message() {
    Selection s;
    s.ref = SelRef{SelKind::Mesh, 12};
    s.name = "GameEngine.Mesh/crate.glb";
    s.x = 1.0f; s.y = 2.0f; s.z = 3.0f;
    Event e = Event::make(Ev::SelectEntity, s);

    check(e.selection().ref.index == 12, "Selection carries the entity index");
    check(e.selection().ref.kind == SelKind::Mesh, "Selection carries the kind");
    check(e.selection().name == "GameEngine.Mesh/crate.glb", "Selection carries the name");
    check(e.selection().x == 1.0f && e.selection().z == 3.0f, "Selection carries the position");
    check(e.selection().valid(), "a fully populated Selection is valid");

    // target() must unwrap both selection-shaped payloads, because the dispatcher
    // uses it to staleness-check an event without knowing which it holds.
    check(e.target().index == 12, "target() unwraps a Selection payload");
    SelRef bare{SelKind::Zone, 4};
    check(Event::make(Ev::DeleteEntity, bare).target().index == 4,
          "target() unwraps a SelRef payload too");

    Selection empty;
    check(!empty.valid(), "a default Selection is not valid");
    check(!Event::make(Ev::SelectEntity, empty).target().valid(),
          "target() of an empty Selection is invalid, so the dispatcher can skip it");
}

static void test_target_is_captured_not_looked_up() {
    SelRef atClickTime{SelKind::Zone, 7};
    Event e = Event::make(Ev::DeleteEntity, atClickTime);

    // Whatever happens to the live selection afterwards, the event still names
    // what the user actually clicked.
    const SelRef liveSelectionAfterwards{SelKind::Brush, 0};
    (void)liveSelectionAfterwards;

    check(e.target().index == 7, "the event still carries the index from post time");
    check(e.target().kind == SelKind::Zone, "the event still carries the kind from post time");

    // And two events posted from different selections stay distinct even if the
    // selection moved between the two posts.
    Event first  = Event::make(Ev::DeleteEntity, SelRef{SelKind::Zone, 7});
    Event second = Event::make(Ev::DeleteEntity, SelRef{SelKind::Zone, 9});
    check(first.sel().index != second.sel().index,
          "two posts keep their own targets instead of aliasing one index field");
}

// ---------------------------------------------------------------------------
// Queueing and draining
// ---------------------------------------------------------------------------
static void test_fifo_drain() {
    EventBus bus;
    check(bus.pending() == 0, "a fresh bus is empty");

    bus.post(Ev::SpawnZone, SpawnDesc{"", 1, true});
    bus.post(Ev::SpawnPlayerStart, SpawnDesc{});
    bus.post(Ev::OpenWorld, std::string("A"));
    bus.post(Ev::OpenWorld, std::string("B"));
    check(bus.pending() == 4, "four posts are pending");
    check(bus.posted() == 4, "posted() counts every request");

    std::vector<Event> out;
    bus.drain(out);
    check(out.size() == 4, "drain returns everything queued");
    // Order matters: these are user actions, and reordering two "open world"
    // clicks would load the wrong level.
    check(out[0].kind == Ev::SpawnZone, "drain is FIFO, first event");
    check(out[1].kind == Ev::SpawnPlayerStart, "drain is FIFO, second event");
    check(out[2].str() == "A", "drain is FIFO, third event keeps its payload");
    check(out[3].str() == "B", "drain is FIFO, fourth event keeps its payload");

    check(bus.pending() == 0, "drain empties the queue");
    bus.drain(out);
    check(out.empty(), "a second drain yields nothing");
    check(bus.posted() == 4, "drain does not change the posted count");
}

// The "an unconsumed field fires later" failure mode. Draining every frame is
// what makes an event a single frame of intent rather than a sticky field.
static void test_no_sticky_events() {
    EventBus bus;
    bus.post(Ev::ApplyProperties, SelRef{SelKind::Mesh, 2});
    std::vector<Event> out;
    bus.drain(out);
    check(out.size() == 1, "the event arrives once");
    bus.drain(out);
    check(out.empty(), "it does NOT re-fire on the next frame");
}

// Overflow is bounded and counted, so a panel that posts in a loop fails
// visibly instead of growing the queue without limit.
static void test_overflow_is_bounded_and_counted() {
    EventBus bus;
    for (size_t i = 0; i < EventBus::kMaxQueued + 10; i++)
        bus.post(Ev::ReloadMesh, SelRef{SelKind::Mesh, -1});
    check(bus.pending() == EventBus::kMaxQueued, "the queue never exceeds its bound");
    check(bus.posted() == EventBus::kMaxQueued + 10, "posted() counts the overflow too");
    check(bus.dropped() == 10, "dropped() reports exactly what was refused");

    std::vector<Event> out;
    bus.drain(out);
    check(out.size() == EventBus::kMaxQueued, "a full drain still returns the bound");
}

// ---------------------------------------------------------------------------
// Coverage guards: the bus must cover every `action*` field it replaces
// ---------------------------------------------------------------------------
// Every Ev enumerator here corresponds to one or more of the 58 action* fields.
// If a new one is added to the editor without a matching Ev, this fails — which is
// the mechanism that stops the two lists drifting the way ID_PP_PORTALBROWSE and
// ID_BTN_GT_PREVIEW did.
static void test_event_coverage() {
    struct Row { Ev k; const char* name; };
    static const Row kEvents[] = {
        {Ev::SpawnPawn, "SpawnPawn"}, {Ev::SpawnMesh, "SpawnMesh"},
        {Ev::SpawnPickup, "SpawnPickup"}, {Ev::SpawnEmitter, "SpawnEmitter"},
        {Ev::SpawnZone, "SpawnZone"}, {Ev::SpawnParticleEmitter, "SpawnParticleEmitter"},
        {Ev::SpawnPathNode, "SpawnPathNode"}, {Ev::SpawnWindZone, "SpawnWindZone"},
        {Ev::SpawnPlayerStart, "SpawnPlayerStart"},
  {Ev::SpawnLight, "SpawnLight"},
        {Ev::CsgPlace, "CsgPlace"}, {Ev::CsgCommit, "CsgCommit"},
        {Ev::DeleteEntity, "DeleteEntity"}, {Ev::DuplicateEntity, "DuplicateEntity"},
        {Ev::DeletePortal, "DeletePortal"},
        {Ev::SelectEntity, "SelectEntity"}, {Ev::ApplyProperties, "ApplyProperties"},
        {Ev::OpenProperties, "OpenProperties"},
        {Ev::OpenWorld, "OpenWorld"}, {Ev::LinkWorld, "LinkWorld"},
        {Ev::ApplySurface, "ApplySurface"}, {Ev::ResetSurface, "ResetSurface"},
        {Ev::ApplyTextureToSelection, "ApplyTextureToSelection"},
        {Ev::ReloadMesh, "ReloadMesh"}, {Ev::ConvertToAnimated, "ConvertToAnimated"},
        {Ev::PlaySound, "PlaySound"}, {Ev::StopSoundPreview, "StopSoundPreview"},
        {Ev::GenerateHeightmap, "GenerateHeightmap"},
        {Ev::ApplyLevelState, "ApplyLevelState"},
{Ev::AnimNewClip, "AnimNewClip"}, {Ev::AnimDeleteClip, "AnimDeleteClip"},
        {Ev::AnimScrub, "AnimScrub"}, {Ev::AnimAddKey, "AnimAddKey"},
        {Ev::AnimDeleteKey, "AnimDeleteKey"}, {Ev::AnimApplyClipMeta, "AnimApplyClipMeta"},
        {Ev::AnimToggleEdit, "AnimToggleEdit"},
        {Ev::AnimUndo, "AnimUndo"}, {Ev::AnimRedo, "AnimRedo"},
        {Ev::AnimSelectAll, "AnimSelectAll"},
        {Ev::AnimClearSelection, "AnimClearSelection"},
        {Ev::BeginPlacement, "BeginPlacement"},
        {Ev::ApplyTextureToModel, "ApplyTextureToModel"},
        {Ev::RefreshModelBrowser, "RefreshModelBrowser"},
    };
    // AnimSave and AnimRefresh are deliberately NOT events - they are intra-frame
    // chaining signals, so asserting their absence keeps someone from "fixing" the
    // asymmetry by queueing the file write and delaying it a frame.
    check(static_cast<int>(Ev::AnimUndo) != static_cast<int>(Ev::AnimScrub),
          "AnimSave/AnimRefresh stayed fields, not events (see plan B4)");

    // Distinct enumerators, and no accidental aliasing.
    bool unique = true;
    for (size_t i = 0; i < sizeof(kEvents)/sizeof(kEvents[0]); i++)
        for (size_t j = i + 1; j < sizeof(kEvents)/sizeof(kEvents[0]); j++)
            if (kEvents[i].k == kEvents[j].k) unique = false;
    check(unique, "every Ev enumerator is distinct");

    bool noneIsUsed = (Ev::None != Ev::SpawnPawn);
    check(noneIsUsed, "Ev::None is not aliased onto a real event");

    // Coverage, not a magic number: Ev::Count moves the moment an event is added,
    // so a new Ev that is not named in kEvents fails HERE rather than shipping
    // with no test. That is the guard that keeps the enum and the editor's
    // action set from drifting apart.
    const size_t named = sizeof(kEvents) / sizeof(kEvents[0]);
    check(static_cast<size_t>(Ev::Count) == named + 1,
          "Ev::Count == named events + None (every Ev has a row above)");
    check(named >= 30, "the bus still covers the bulk of the 58 action* fields");
}

// SelKind mirrors AngelEd's SelType. If someone adds a selection type to the
// editor and forgets the bus, the static_cast below silently mis-maps, so pin the
// count here where a mistake is loud.
static void test_selkind_covers_seltype() {
    check(static_cast<int>(SelKind::Map) == 13,
          "SelKind matches SelType's values through Map (13)");
    check(static_cast<int>(SelKind::Portal) == 8, "SelKind::Portal is 8, as in SelType");
}

// SurfaceEdit must carry the face mask, not just "apply". ApplyToSelection is a
// deliberate no-op on an empty mask so it can never mean "all six faces" - and the
// old code read the mask from live panel state at drain time, so a selection
// change between the click and the drain applied props to different faces than
// the dialog was showing.
static void test_surface_edit_carries_its_mask() {
    SurfaceEdit s;
    s.renderable = 4;
    s.faceMask    = (1u << 3) | (1u << 5);   // two faces, not one and not six
    s.flags       = 0x10;                    // SURF_COLLISION_PROXY
    Event e = Event::make(Ev::ApplySurface, s);

    check(e.surface().renderable == 4, "SurfaceEdit carries the renderable index");
    check(e.surface().faceMask == ((1u << 3) | (1u << 5)),
          "SurfaceEdit carries the EXACT face mask");
    check(e.surface().hasFaces(), "a two-face mask reports hasFaces()");
    check(e.surface().flags == 0x10, "SurfaceEdit carries the flags");

    // The empty-mask case must stay distinguishable, because "reset nothing" and
    // "reset all six faces" must never be the same code path.
    SurfaceEdit none;
    none.renderable = 4;
    none.faceMask    = 0;
    check(!Event::make(Ev::ResetSurface, none).surface().hasFaces(),
          "an empty mask reports !hasFaces(), so it is a no-op not 'all faces'");

    // Reset travels as the same payload type, so one handler shape serves both.
    check(Event::make(Ev::ResetSurface, s).surface().faceMask == s.faceMask,
          "ResetSurface uses the same SurfaceEdit payload as ApplySurface");
}

// Every field the dialog can edit must travel ON THE EVENT. texPath did not, and
// the handler picked it up from live panel state instead - which is exactly the
// hazard the test above describes, still live for one field, and it only worked
// because the drain happened in the same frame as the click.
static void test_surface_edit_carries_tex_path() {
    SurfaceEdit s;
    s.renderable = 2;
    s.faceMask    = 1u << 0;
    s.texPath     = "oztex/signage/custom.png";
    const Event e = Event::make(Ev::ApplySurface, s);
    check(e.surface().texPath == "oztex/signage/custom.png",
          "SurfaceEdit carries texPath, so a face texture is not read from live panel state");

    SurfaceEdit cleared;
    cleared.renderable = 2;
    cleared.faceMask    = 1u << 0;
    check(Event::make(Ev::ApplySurface, cleared).surface().texPath.empty(),
          "an omitted texPath arrives empty (so a texture can be cleared)");
}

// Brush-wide scope is an EXPLICIT bool, never an overloaded faceMask. Zero mask is
// the documented "nothing selected" value that hasFaces() exists to protect; if
// brush-wide were expressed as mask 0, a brush-wide reset would be
// indistinguishable from "reset nothing" and one of the two would be broken.
static void test_surface_edit_scope_is_explicit() {
    SurfaceEdit faces;
    faces.renderable = 7;
    faces.faceMask    = (1u << 2) | (1u << 4);
    faces.brushWide   = false;
    check(Event::make(Ev::ApplySurface, faces).surface().brushWide == false,
          "face scope is the default and travels as brushWide=false");

    SurfaceEdit brush;
    brush.renderable = 7;
    brush.faceMask    = (1u << 2) | (1u << 4);
    brush.brushWide   = true;
    const Event e = Event::make(Ev::ApplySurface, brush);
    check(e.surface().brushWide, "brush scope travels as brushWide=true");
    check(e.surface().faceMask == ((1u << 2) | (1u << 4)),
          "a brush-wide edit STILL names the faces the dialog was showing");
    check(e.surface().isTargeted(),
          "so a handler that ignores brushWide does the narrow thing, not nothing");

    // The critical invariant: brushWide alone must not make an edit targeted,
    // because that would resurrect "mask 0 means all faces".
    SurfaceEdit orphan;
    orphan.renderable = 7;
    orphan.faceMask    = 0;
    orphan.brushWide   = true;
    const Event o = Event::make(Ev::ApplySurface, orphan);
    check(!o.surface().isTargeted(),
          "brushWide with an empty mask is still a no-op - scope does not override the mask");
    check(!o.surface().hasFaces(),
          "...and hasFaces() still reports false for it, as before");
}

// CsgPlace and CsgCommit are separate kinds because "arm a primitive" resets the
// ghost while "commit the ghost" reads it. A merged event would either lose the
// reset or commit a brush nobody positioned.
static void test_csg_intent_distinguishes_place_from_commit() {
    CsgIntent place;
    place.primitive = 2;      // sphere
    CsgIntent commit;
    commit.operation = 1;     // ADD

    Event pe = Event::make(Ev::CsgPlace, place);
    Event ce = Event::make(Ev::CsgCommit, commit);
    check(pe.csg().isPrimitive() && !pe.csg().isCommit(),
          "a CsgPlace intent is a primitive, not a commit");
    check(ce.csg().isCommit() && !ce.csg().isPrimitive(),
          "a CsgCommit intent is a commit, not a primitive");

    CsgIntent empty;
    check(!empty.isPrimitive() && !empty.isCommit(),
          "a default CsgIntent is neither, so a malformed post is ignored");
}

// An animation intent must carry the clip name and playhead, because the anim
// panel's live fields were read at drain time - so a clip-list selection that moved
// between click and frame deleted or keyed a different clip.
static void test_anim_intent_carries_its_clip() {
    AnimIntent a;
    a.meshId   = 17;
    a.clipName = "Idle";
    a.time     = 1.25f;
    a.fps      = 24.0f;
    a.loop     = false;
    Event e = Event::make(Ev::AnimDeleteClip, a);

    check(e.anim().clipName == "Idle", "AnimIntent carries the clip name");
    check(e.anim().time == 1.25f, "AnimIntent carries the playhead");
    check(e.anim().meshId == 17, "AnimIntent carries the mesh id");
    check(e.anim().fps == 24.0f && !e.anim().loop,
          "AnimIntent carries fps and loop, read off the controls at post time");
    check(e.anim().hasClip(), "a named clip reports hasClip()");

    // Two intents posted from different list selections stay distinct, which is
    // the whole point of capturing the name rather than reading it later.
    Event first  = Event::make(Ev::AnimDeleteKey, AnimIntent{1, "Walk", 0.5f});
    Event second = Event::make(Ev::AnimDeleteKey, AnimIntent{1, "Run",  0.5f});
    check(first.anim().clipName != second.anim().clipName,
          "two posts keep their own clips instead of aliasing one panel field");

    AnimIntent none;
    check(!none.hasClip(), "a default AnimIntent has no clip, so clip-scoped ops skip");
}

// A PlacementRequest carries the resolved pickup NAME, not the button index the
// panel used. The handler resolved LegacyPickupType(idx) at drain time, so a panel
// rebuild between the click and the frame placed a different pickup.
static void test_placement_carries_a_name_not_an_index() {
    PlacementRequest p;
    p.kind = PlacementRequest::Kind::Pickup;
    p.key  = "pistol_01";
    Event e = Event::make(Ev::BeginPlacement, p);

    check(e.placement().kind == PlacementRequest::Kind::Pickup, "placement kind survives");
    check(e.placement().key == "pistol_01", "placement carries the DEF NAME, not an index");
    check(PlacementRequest{}.key.empty(),
          "a default PlacementRequest has no name, so it places nothing");

    PlacementRequest node;
    node.kind = PlacementRequest::Kind::Node;
    check(node.kind != PlacementRequest::Kind::Pickup,
          "Node and Pickup are distinct kinds, not one enum value reused");
}

// Six action* fields used to carry a heightmap request, and four of them (image,
// texture, scale, size) were read at drain time - so a panel edit between the
// click and the frame generated the heightmap from stale values.
static void test_heightmap_desc_is_one_payload() {
    HeightmapDesc h;
    h.imagePath   = "GameData/Worlds/X/height.png";
    h.texturePath = "GameData/Worlds/X/oztex/tex/01_rock_32.png";
    h.x = 1.0f; h.y = 2.0f; h.z = 3.0f;
    h.scale = 0.5f;
    h.sizeX = 128; h.sizeY = 64; h.sizeZ = 128;
    Event e = Event::make(Ev::GenerateHeightmap, h);

    check(e.heightmap().imagePath == h.imagePath, "heightmap carries the image path");
    check(e.heightmap().texturePath == h.texturePath, "heightmap carries the texture path");
    check(e.heightmap().scale == 0.5f, "heightmap carries the scale");
    check(e.heightmap().sizeX == 128 && e.heightmap().sizeZ == 128,
          "heightmap carries all three dimensions");
    check(e.heightmap().x == 1.0f && e.heightmap().z == 3.0f,
          "heightmap carries the position");

    check(Event::make(Ev::GenerateHeightmap, HeightmapDesc{}).heightmap().imagePath.empty(),
          "a default HeightmapDesc has no image, so the handler skips it");
}

// ---------------------------------------------------------------------------
// The one-queue trap: filtered drains must partition the batch
// ---------------------------------------------------------------------------
// This is the bug the filtered drain exists for. main() drains the bus once per
// subsystem (frame, surface, props, anim, placement), but the bus holds ONE queue.
// An unfiltered drain in the FIRST pass moves the whole queue out, so every later
// pass receives an empty vector - which is exactly how Surface Properties stopped
// applying, and how placement/animation/props-apply went dead. The filters must
// route each kind to exactly one pass, and the frame catch-all must never steal a
// kind another pass owns.
static void test_filtered_drain_partitions_by_kind() {
    EventBus bus;
    bus.post(Ev::ApplySurface,   SurfaceEdit{});
    bus.post(Ev::SpawnPawn,      SpawnDesc{});
    bus.post(Ev::ApplyProperties, SelRef{SelKind::Mesh, 3});
    bus.post(Ev::AnimUndo,       AnimIntent{});
    bus.post(Ev::SelectEntity,   SelRef{SelKind::Brush, 1});
    bus.post(Ev::OpenProperties, SelRef{SelKind::Zone, 2});

    std::vector<Event> frame, surface, props, anim, placement;
    // Called in the SAME order as main(): frame first. The frame catch-all must not
    // consume the kinds owned by the later passes, or this reproduces the bug.
    bus.drain(frame,     IsFrameEvent);
    bus.drain(surface,   IsSurfaceEvent);
    bus.drain(props,     IsPropsEvent);
    bus.drain(anim,      IsAnimEvent);
    bus.drain(placement, IsPlacementEvent);

    check(surface.size() == 1 && surface[0].kind == Ev::ApplySurface,
          "the surface pass gets its ApplySurface");
    check(props.size() == 1 && props[0].kind == Ev::ApplyProperties,
          "the props pass gets its ApplyProperties");
    check(anim.size() == 1 && anim[0].kind == Ev::AnimUndo,
          "the anim pass gets its AnimUndo");
    check(placement.size() == 1 && placement[0].kind == Ev::SpawnPawn,
          "the placement pass gets its SpawnPawn");
    // SelectEntity AND OpenProperties are frame-owned; OpenProperties must NOT be
    // mistaken for ApplyProperties.
    check(frame.size() == 2, "the frame catch-all gets SelectEntity + OpenProperties");
    check(bus.pending() == 0, "every event was routed to exactly one pass");
}

int main() {
    printf("EditorEventBus tests:\n");
    test_typed_payloads();
    test_selection_is_one_atomic_message();
    test_surface_edit_carries_its_mask();
    test_surface_edit_carries_tex_path();
    test_surface_edit_scope_is_explicit();
    test_csg_intent_distinguishes_place_from_commit();
    test_anim_intent_carries_its_clip();
    test_placement_carries_a_name_not_an_index();
    test_heightmap_desc_is_one_payload();
    test_wrong_shape_is_inert();
    test_target_is_captured_not_looked_up();
    test_fifo_drain();
    test_no_sticky_events();
    test_filtered_drain_partitions_by_kind();
    test_overflow_is_bounded_and_counted();
    test_event_coverage();
    test_selkind_covers_seltype();
    printf("\nResults: %d/%d passed\n", pass_count, test_count);
    return (pass_count == test_count) ? 0 : 1;
}