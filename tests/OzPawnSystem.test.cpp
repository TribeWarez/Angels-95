// OzPawnSystem unit tests — data management logic, no rendering.
// Requires raylib for types (Vector3, BoundingBox).
// g++ -O0 -g --std=c++20 -I Source -DOMEGA_TEST_ENV \
//   tests/OzPawnSystem.test.cpp \
//   Source/Pawn/OzPawnSystem.cpp \
//   Source/Physics/OzBsp.cpp Source/Physics/WorldChunk.cpp Source/Log.cpp \
//   -o test_pawn_system -lraylib -lopengl32 -lgdi32 -lwinmm -lws2_32 -lm

#include "../Source/Pawn/OzPawnSystem.hpp"
#include "../Source/Renderer/Mesh/SkeletalMesh.hpp"
#include "../Source/Script/LightningEntityRegistry.hpp"
#include "../Source/Script/LightningEntityManager.hpp"
#include "../Source/Script/LightningEntityDef.hpp"
#include "../Source/Events/EventBus.hpp"
#include "../Source/Particle/OzParticleSimulationManager.hpp"
#include "../Source/Pawn/PickupItems.hpp"
#include "../Source/Renderer/CombatFX.hpp"
#include "../Source/Renderer/CombatFeedback.hpp"
#include "../Source/GameplayFlags.hpp"
#include <cstdio>
#include <cassert>
#include <cmath>

static int tests_total = 0, tests_passed = 0;
#define TEST(name) do { tests_total++; fprintf(stdout, "  TEST: %s ... ", name);
#define PASS() do { tests_passed++; fprintf(stdout, "PASS\n"); } while(0)
#define FAIL(msg) do { fprintf(stdout, "FAIL: %s\n", msg); return 1; } while(0)
#define CHECK(cond) do { if (!(cond)) { fprintf(stdout, "FAIL: %s\n", #cond); return 1; } } while(0)
#define CHECK_EQ(a, b) do { if ((a) != (b)) { fprintf(stdout, "FAIL: expected %d, got %d\n", (int)(a), (int)(b)); return 1; } } while(0)
#define CHECK_APROX(a, b, eps) do { float diff = (a) - (b); if (diff < 0) diff = -diff; if (diff > (eps)) { fprintf(stdout, "FAIL: expected %f, got %f\n", (float)(b), (float)(a)); return 1; } } while(0)
#define END_TEST() } while(0)

static void reset_pawn_system() {
    auto& ps = PawnSystem::Instance();
    ps.DespawnAll();
    ps.ClearPlayerStarts();
    ps.ClearProjectiles();
    ps.ClearPickups();
    ZoneManager::Instance().ClearZones();
    ps.ClearEmitters();
    ps.ClearLights();
    ps.ClearSkyZones();
}

static int test_register_def() {
    TEST("RegisterDef and Spawn from template");
    reset_pawn_system();
    auto& ps = PawnSystem::Instance();
    PawnDef def;
    def.name = "TestBot";
    def.speed = 2.5f;
    def.aggroRange = 8.0f;
    def.damage = 15.0f;
    def.maxHealth = 50;
    ps.RegisterDef(def);
    int id = ps.Spawn({10, 0, 10}, "TestBot");
    CHECK(id >= 0);
    Pawn* p = ps.Get(id);
    CHECK(p != nullptr);
    CHECK(p->active);
    CHECK_APROX(p->position.x, 10.0f, 0.001f);
    CHECK_APROX(p->speed, 2.5f, 0.001f);
    CHECK_EQ(p->health, 50);
    PASS(); return 0; END_TEST();
}

static int test_spawn_unknown_def() {
    TEST("Spawn with unknown def name returns -1");
    reset_pawn_system();
    auto& ps = PawnSystem::Instance();
    int id = ps.Spawn({0, 0, 0}, "NonExistentPawnType");
    CHECK_EQ(id, -1);
    PASS(); return 0; END_TEST();
}

static int test_despawn() {
    TEST("Despawn marks pawn inactive and Get returns null");
    reset_pawn_system();
    auto& ps = PawnSystem::Instance();
    PawnDef def;
    def.name = "DespawnTest";
    def.maxHealth = 10;
    ps.RegisterDef(def);
    int id = ps.Spawn({0, 0, 0}, "DespawnTest");
    CHECK(id >= 0);
    CHECK(ps.Get(id) != nullptr);
    ps.Despawn(id);
    CHECK(ps.Get(id) == nullptr);
    PASS(); return 0; END_TEST();
}

static int test_despawn_all() {
    TEST("DespawnAll marks all pawns inactive");
    reset_pawn_system();
    auto& ps = PawnSystem::Instance();
    PawnDef def;
    def.name = "MultiSpawn";
    ps.RegisterDef(def);
    int id1 = ps.Spawn({0,0,0}, "MultiSpawn");
    int id2 = ps.Spawn({5,0,5}, "MultiSpawn");
    CHECK(id1 >= 0 && id2 >= 0);
    ps.DespawnAll();
    CHECK(ps.Get(id1) == nullptr);
    CHECK(ps.Get(id2) == nullptr);
    PASS(); return 0; END_TEST();
}

static int test_despawn_all_zeroes_stale_handles() {
    TEST("DespawnAll zeroes released handles, so a smaller next world cannot double-free");
    // The crash shape: one world loads 14 NPCs, the next loads 12. DespawnAll
    // walks every slot of the growing m_pawns vector, so the two slots the new
    // world did not reuse still hold the previous world's handles. Without the
    // zeroing, the NEXT world change unloads those handles again - and raylib's
    // UnloadSound frees the AudioBuffer the stale pointer names, a double free.
    reset_pawn_system();
    auto& ps = PawnSystem::Instance();
    PawnDef def;
    def.name = "StaleSlotBot";
    def.maxHealth = 10;
    ps.RegisterDef(def);

    for (int i = 0; i < 14; ++i)
        CHECK(ps.Spawn({(float)i, 0.0f, 0.0f}, "StaleSlotBot") >= 0);
    ps.DespawnAll();
    for (const auto& p : ps.GetPawns()) {
        CHECK_EQ(p.sprite.id, 0);
        CHECK_EQ(p.scream.frameCount, 0);
        CHECK(p.mesh == nullptr);
    }

    for (int i = 0; i < 12; ++i)
        CHECK(ps.Spawn({(float)i, 0.0f, 0.0f}, "StaleSlotBot") >= 0);
    // Simulate the two stale slots a real session has: a non-zero frameCount
    // with a null buffer is safe to pass to UnloadSound (it null-checks), but it
    // proves the release path ZEROED the slot afterwards.
    int stale = 0;
    for (size_t i = 0; i < ps.GetPawns().size(); ++i) {
        const Pawn& slot = ps.GetPawns()[i];
        if (slot.active) continue;
        Pawn& m = const_cast<Pawn&>(slot);
        m.scream.stream.buffer = nullptr;
        m.scream.frameCount = 1;
        ++stale;
    }
    CHECK_EQ(stale, 2);

    ps.DespawnAll();
    for (const auto& p : ps.GetPawns()) CHECK_EQ(p.scream.frameCount, 0);
    // A second world load is the one that used to crash; it must be a no-op.
    ps.DespawnAll();
    for (const auto& p : ps.GetPawns()) CHECK_EQ(p.scream.frameCount, 0);
    PASS(); return 0; END_TEST();
}

static int test_player_starts() {
    TEST("AddPlayerStart and GetFirstPlayerStart");
    auto& ps = PawnSystem::Instance();
    ps.ClearPlayerStarts();
    PlayerStartNode node1, node2;
    node1.position = {10, 0, 20};
    node1.yaw = 90.0f;
    node2.position = {30, 0, 40};
    node2.yaw = 180.0f;
    ps.AddPlayerStart(node1);
    ps.AddPlayerStart(node2);
    PlayerStartNode* first = ps.GetFirstPlayerStart();
    CHECK(first != nullptr);
    CHECK_APROX(first->position.x, 10.0f, 0.001f);
    CHECK_EQ(ps.GetPlayerStarts().size(), (size_t)2);
    PASS(); return 0; END_TEST();
}

static int test_pickup_crud() {
    TEST("AddPickup, GetPickup, RemovePickup");
    auto& ps = PawnSystem::Instance();
    ps.ClearPickups();
    PickupNode node;
    node.typeName = "HealthVial";
    node.position = {1, 2, 3};
    node.respawnTime = 30.0f;
    int id = ps.AddPickup(node);
    CHECK(id >= 0);
    PickupNode* found = ps.GetPickup(id);
    CHECK(found != nullptr);
    CHECK(found->typeName == "HealthVial");
    ps.RemovePickup(id);
    CHECK(ps.GetPickup(id) == nullptr);
    PASS(); return 0; END_TEST();
}

// Regression: a pickup whose entity def is not in ItemDB and has no item_id
// (e.g. a weapon like pistol_01) used to crash on the ItemDB name-fallback scan
// (std::string constructed from a null ItemDB entry name).
static int test_pickup_unlisted_item_no_crash() {
    TEST("Collect pickup not in ItemDB does not crash");
    reset_pawn_system();
    auto& reg = LightningEntityRegistry::Instance();
    EntityDef def;
    def.name = "pistol_01_like";
    def.type = EntityType::PICKUP;   // no item_id -> falls through to ItemDB scan
    reg.Register(def);

    auto& ps = PawnSystem::Instance();
    PickupNode node;
    node.typeName = def.name;
    node.position = {0, 0, 0};
    node.respawnTime = 30.0f;
    ps.AddPickup(node);

    BoundingBox playerBounds = {{-1, -1, -1}, {1, 1, 1}};
    ps.UpdatePickups(0.016f, {0, 0, 0}, playerBounds); // must not crash
    CHECK(true);
    PASS(); return 0; END_TEST();
}

static int test_zone_crud() {
    TEST("ZoneManager: AddZone, GetZone, RemoveZone");
    auto& zones = ZoneManager::Instance();
    zones.ClearZones();
    ZoneVolumeNode zone;
    zone.bounds = {{-5, -5, -5}, {5, 5, 5}};
    zone.zoneType = ZoneType::ZONE_WATER;
    zone.name = "test_water";
    int id = zones.AddZone(zone);
    CHECK(id >= 0);
    ZoneVolumeNode* found = zones.GetZone(id);
    CHECK(found != nullptr);
    CHECK(found->zoneType == ZoneType::ZONE_WATER);
    CHECK(found->name == "test_water");
    zones.RemoveZone(id);
    CHECK(zones.GetZone(id) == nullptr);
    PASS(); return 0; END_TEST();
}

// PointRegion::Rebuild is the whole zone contract — priority ordering, env-merge
// layering, and the primary-zone identity that player physics and reverb read — and
// it had no test at all.
//
// The bug it pins: primaryZoneId and primaryZoneType were two DIFFERENT lookups of
// one concept. The type came from activeZones[0], which GetActiveZones sorts
// highest-priority-first (priority desc, then smaller volume wins), while the id
// came from `*activeZoneIds.begin()` — the first bucket of an unordered_set, i.e.
// whichever one the integer hash landed in. With two overlapping zones they could
// name DIFFERENT zones, so the player was handed ladder volume's water physics, or
// GetZone(primaryZoneId) returned a zone whose envOverrides were never consulted.
static int test_point_region_primary_matches_type() {
    TEST("PointRegion: primaryZoneId names the SAME zone as primaryZoneType");
    auto& zones = ZoneManager::Instance();
    zones.ClearZones();

    // FOUR overlapping zones, all containing the same point, with distinct
    // priorities. The count is not incidental — see the mismatch-rate note below.
    ZoneVolumeNode water;
    water.bounds = {{-50, -50, -50}, {50, 50, 50}};
    water.zoneType = ZoneType::ZONE_WATER;
    water.name = "overlap_water";
    water.priority = 1;
    water.envOverrides.ambIntensity = 0.25f;
    water.envOverrides.applyAmbient = true;

    ZoneVolumeNode ladder;                    // the winner: highest priority
    ladder.bounds = {{-50, -50, -50}, {50, 50, 50}};
    ladder.zoneType = ZoneType::ZONE_LADDER;
    ladder.name = "overlap_ladder";
    ladder.priority = 9;
    ladder.envOverrides.ambIntensity = 0.9f;
    ladder.envOverrides.applyAmbient = true;

    // Only six ZoneType values exist, so the extra overlapping volumes reuse types;
    // what matters here is the distinct PRIORITIES, not the types.
    ZoneVolumeNode reverbVol;
    reverbVol.bounds = {{-50, -50, -50}, {50, 50, 50}};
    reverbVol.zoneType = ZoneType::ZONE_REVERB;
    reverbVol.name = "overlap_reverb";
    reverbVol.priority = 4;

    ZoneVolumeNode skyVol;
    skyVol.bounds = {{-50, -50, -50}, {50, 50, 50}};
    skyVol.zoneType = ZoneType::ZONE_SKY;
    skyVol.name = "overlap_sky";
    skyVol.priority = 6;

    // THE IDS ARE THE POINT, and the mechanism is specific enough to construct.
    //
    // The bug: primaryZoneType came from activeZones[0] (priority-sorted by
    // GetActiveZones), while primaryZoneId came from `*activeZoneIds.begin()` — an
    // unordered_set, whose first element is a function of BUCKET LAYOUT, not of
    // priority. When they disagree, the player is handed water physics from the
    // ladder volume, and GetZone(primaryZoneId) returns a zone whose envOverrides
    // were never consulted.
    //
    // WHY this survived review, and why an ordinary two-zone test cannot catch it:
    // libstdc++'s _Hashtable keeps ONE global forward list and PREPENDS a new node
    // when its bucket is empty. Rebuild() fills the set in reverse priority order, so
    // the priority winner is inserted LAST — therefore, while every id lands in a
    // DISTINCT bucket, iteration is reverse-insertion order and begin() returns the
    // winner. The two fields agree BY CONSTRUCTION. Measured: dense ids 1..n never
    // disagree at any n, and two overlapping zones never disagree at all.
    //
    // The disagreement needs a BUCKET COLLISION. libstdc++'s bucket count here is 13,
    // so two ids differing by a multiple of 13 share a bucket, the list is re-chained,
    // and begin() starts returning a different zone. Verified breaking
    // configuration: ids {1,14,15,16} with the winner at id 1 yields begin() == 16.
    //
    // That is reachable in real content: ClearZones() does NOT reset m_nextZoneId
    // (ZoneManager.cpp:29), and RemoveZone exists for the editor's delete flow, so
    // session ids are neither dense nor bounded by any table size.
    //
    // Explicit ids are used rather than relying on the counter, so the layout is
    // exact and the test does not depend on how many zones earlier tests created.
    // AddZone honours a non-zero id (ZoneManager.cpp:18), so this is the supported way
    // to place zones.
    zones.ClearZones();
    const int kWinId = 1;
    const int kCollideId = kWinId + 13;      // same bucket mod 13
    const int kOther1 = kCollideId + 1;
    const int kOther2 = kCollideId + 2;

    ZoneVolumeNode winZone = ladder;
    winZone.id = (uint32_t)kWinId;
    // Losers registered first, so Rebuild's reverse walk inserts the winner LAST —
    // the precondition that makes the collision decide begin().
    ZoneVolumeNode collideZone = water;
    collideZone.id = (uint32_t)kCollideId;
    collideZone.priority = 3;
    ZoneVolumeNode other1 = reverbVol;
    other1.id = (uint32_t)kOther1;
    other1.priority = 5;
    ZoneVolumeNode other2 = skyVol;
    other2.id = (uint32_t)kOther2;
    other2.priority = 7;

    zones.AddZone(collideZone);
    zones.AddZone(other1);
    zones.AddZone(other2);
    zones.AddZone(winZone);

    std::vector<ZoneVolumeNode*> act =
        zones.GetActiveZones({0, 0, 0}, {{-1, -1, -1}, {1, 1, 1}});
    CHECK_EQ(act.size(), (size_t)4);
    // GetActiveZones must put the priority winner first, or "primary" is undefined.
    CHECK(act[0] != nullptr);
    CHECK(act[0]->id == (uint32_t)kWinId);
    CHECK(act[0]->zoneType == ZoneType::ZONE_LADDER);
    CHECK(act[0]->priority == 9);

    PointRegion r;
    r.Rebuild(act);

    // THE invariant: the id and the type describe ONE zone.
    CHECK(r.primaryZoneType == ZoneType::ZONE_LADDER);
    CHECK(r.primaryZoneId == kWinId);
    CHECK(r.primaryZoneId == (int)act[0]->id);
    ZoneVolumeNode* viaId = zones.GetZone(r.primaryZoneId);
    CHECK(viaId != nullptr);
    CHECK(viaId->zoneType == r.primaryZoneType);
    CHECK(viaId->priority == 9);
    CHECK(r.HasZoneType(ZoneType::ZONE_LADDER));

    // Sweep the collision distance. Every multiple of 13 must behave identically, and
    // this also guards against a "fix" that merely reshuffles the set — the invariant
    // has to hold for layouts the old code happened to get right too.
    for (int mult = 1; mult <= 40; mult++) {
        zones.ClearZones();
        ZoneVolumeNode w2 = ladder;    w2.id = 2;
        ZoneVolumeNode c2 = water;     c2.id = (uint32_t)(2 + 13 * mult); c2.priority = 3;
        ZoneVolumeNode o1 = reverbVol; o1.id = (uint32_t)(3 + 13 * mult); o1.priority = 5;
        ZoneVolumeNode o2 = skyVol;    o2.id = (uint32_t)(4 + 13 * mult); o2.priority = 7;
        zones.AddZone(c2);
        zones.AddZone(o1);
        zones.AddZone(o2);
        zones.AddZone(w2);
        std::vector<ZoneVolumeNode*> a2 =
            zones.GetActiveZones({0, 0, 0}, {{-1, -1, -1}, {1, 1, 1}});
        CHECK_EQ(a2.size(), (size_t)4);
        CHECK(a2[0] != nullptr);
        CHECK(a2[0]->id == (uint32_t)2);
        PointRegion r2;
        r2.Rebuild(a2);
        CHECK(r2.primaryZoneId == 2);
        CHECK(r2.primaryZoneType == ZoneType::ZONE_LADDER);
        ZoneVolumeNode* vi2 = zones.GetZone(r2.primaryZoneId);
        CHECK(vi2 != nullptr);
        CHECK(vi2->zoneType == r2.primaryZoneType);
    }
    zones.ClearZones();

    // Settle on one pair for the remaining behavioural assertions.
    // Ladder first: these two have IDENTICAL volumes, so with equal bounds the
    // volume tie-break cannot separate them and the winner is decided by priority
    // alone. Registering the winner first keeps the expectation explicit.
    const int ladderId2 = zones.AddZone(ladder);
    const int waterId2  = zones.AddZone(water);
    CHECK(ladderId2 != waterId2);

    std::vector<ZoneVolumeNode*> active =
        zones.GetActiveZones({0, 0, 0}, {{-1, -1, -1}, {1, 1, 1}});
    CHECK(active.size() == 2);
    // GetActiveZones must put the priority winner first, or "primary" is meaningless.
    CHECK(active[0] != nullptr);
    CHECK(active[0]->zoneType == ZoneType::ZONE_LADDER);

    PointRegion region;
    region.Rebuild(active);

    CHECK(region.primaryZoneType == ZoneType::ZONE_LADDER);
    CHECK(region.primaryZoneId == ladderId2);
    CHECK(region.primaryZoneId == active[0]->id);
    // Both zones are still active; only the PRIMARY is the winner.
    CHECK(region.HasZoneId(waterId2));
    CHECK(region.HasZoneId(ladderId2));
    CHECK_EQ(region.activeZoneIds.size(), (size_t)2);
    CHECK(region.HasZoneType(ZoneType::ZONE_LADDER));
    CHECK(!region.HasZoneType(ZoneType::ZONE_WATER));

    // Rebuilding repeatedly must be STABLE. activeZoneIds is rebuilt into a fresh
    // unordered_set each Rebuild, so its iteration order can vary with insertion
    // history; a correct implementation must not vary at all.
    for (int i = 0; i < 32; i++) {
        PointRegion again;
        again.Rebuild(zones.GetActiveZones({0, 0, 0}, {{-1, -1, -1}, {1, 1, 1}}));
        CHECK(again.primaryZoneId == ladderId2);
        CHECK(again.primaryZoneType == ZoneType::ZONE_LADDER);
    }

    // Env overrides layer lowest-priority-first so the highest-priority zone has
    // the final word. Water (priority 1) must not win ambIntensity over ladder.
    CHECK(region.combinedEnv.applyAmbient);
    CHECK_APROX(region.combinedEnv.ambIntensity, 0.9f, 0.001f);

    // Empty region clears the primary.
    PointRegion empty;
    empty.Rebuild({});
    CHECK(empty.primaryZoneId == -1);
    CHECK(!empty.HasZoneType(ZoneType::ZONE_LADDER));

    // Ties are broken by SMALLER volume (the second sort key), so a big generic
    // volume must not shadow a small specific one at equal priority. This is the
    // SHIPPED case: no world in GameData/Worlds authors `priority=` at all, so every
    // zone is priority 0 and this second key is what actually picks the primary.
    //
    // Clear first: the zones above (and the padding) are still registered and the
    // ladder at priority 9 would outrank both, making this assert about the wrong
    // zone. Ids stay gapped, which is the state after any world reload.
    zones.ClearZones();
    ZoneVolumeNode big;
    big.bounds = {{-50, -50, -50}, {50, 50, 50}};
    big.zoneType = ZoneType::ZONE_WATER;
    big.name = "tie_big";
    big.priority = 0;
    const int bigId = zones.AddZone(big);
    ZoneVolumeNode small;
    small.bounds = {{-5, -5, -5}, {5, 5, 5}};
    small.zoneType = ZoneType::ZONE_LADDER;
    small.name = "tie_small";
    small.priority = 0;
    const int smallId = zones.AddZone(small);
    CHECK(bigId != smallId);
    PointRegion tie;
    tie.Rebuild(zones.GetActiveZones({0, 0, 0}, {{-1, -1, -1}, {1, 1, 1}}));
    CHECK(tie.primaryZoneType == ZoneType::ZONE_LADDER);
    CHECK(tie.primaryZoneId == smallId);
    CHECK(tie.primaryZoneId != bigId);
    ZoneVolumeNode* tieViaId = zones.GetZone(tie.primaryZoneId);
    CHECK(tieViaId != nullptr);
    CHECK(tieViaId->zoneType == tie.primaryZoneType);

    // Entered/exited tracking (enteredZoneIds / exitedZoneIds, PointRegion::
    // CommitFrame, HasChanged) was DEAD: CommitFrame had zero callers and nothing
    // ever read either set. The zone on_enter / on_exit hooks are driven from
    // Main.cpp's lastZoneName, not from PointRegion, so the sets were removed.
    // The invariant worth keeping is that leaving every volume clears the primary.
    PointRegion flow;
    flow.Rebuild(zones.GetActiveZones({0, 0, 0}, {{-1, -1, -1}, {1, 1, 1}}));
    CHECK(flow.primaryZoneId >= 0);
    flow.Rebuild(zones.GetActiveZones({0, 0, 0}, {{-1, -1, -1}, {1, 1, 1}}));
    CHECK(flow.primaryZoneId >= 0);   // stable across rebuilds
    flow.Rebuild({});
    CHECK_EQ(flow.primaryZoneId, -1); // left both volumes

    zones.ClearZones();
    PASS(); return 0; END_TEST();
}

static int test_skyzone_crud() {
    TEST("AddSkyZone, GetSkyZone, active state");
    reset_pawn_system();
    auto& ps = PawnSystem::Instance();
    SkyZoneNode sky;
    sky.id = 100;
    sky.position = {0, 10, 0};
    sky.bounds = {{-10, 8, -10}, {10, 12, 10}};
    sky.name = "test_sky";
    sky.fov = 75.0f;
    int idx = ps.AddSkyZone(sky);
    CHECK(idx >= 0);
    SkyZoneNode* found = ps.GetSkyZone(100);
    CHECK(found != nullptr);
    CHECK_APROX(found->fov, 75.0f, 0.001f);
    CHECK(found->name == "test_sky");
    CHECK(ps.GetSkyZones().size() >= 1);
    PASS(); return 0; END_TEST();
}

static int test_skyzone_active() {
    TEST("UpdateSkyZone detects player in bounds");
    auto& ps = PawnSystem::Instance();
    ps.ClearSkyZones();
    SkyZoneNode sky;
    sky.bounds = {{-10, -10, -10}, {10, 10, 10}};
    sky.name = "active_test";
    ps.AddSkyZone(sky);
    // Player inside
    BoundingBox playerBounds = {{-0.5f, -1, -0.5f}, {0.5f, 1, 0.5f}};
    ps.UpdateSkyZone({0, 0, 0}, playerBounds);
    CHECK(ps.IsInSkyZone());
    CHECK(ps.GetActiveSkyZone() != nullptr);
    CHECK(ps.GetActiveSkyZone()->name == "active_test");
    // Player outside
    ps.UpdateSkyZone({20, 0, 0}, playerBounds);
    CHECK(!ps.IsInSkyZone());
    PASS(); return 0; END_TEST();
}

// Recorder for the zone.enter/zone.exit emission test below. `user` is the
// vector itself so Unsubscribe(&vec) cannot collide with any other
// subscriber's nullptr context (the LEM bus bridge subscribes with nullptr).
static std::vector<std::pair<std::string, std::string>> g_zoneEmissionLog;
static void recordZoneEmission(const oz::events::Event& ev, void* user) {
    auto* log = static_cast<std::vector<std::pair<std::string, std::string>>*>(user);
    log->push_back({ev.topic, ev.s0});
}

// Regression: the enter/exit emission was keyed on "was outside" / "is
// outside", so walking from one sky zone DIRECTLY into an adjacent or
// overlapping one emitted NOTHING - the new zone's declarative env never
// applied and the old zone's on_exit restore_* never ran, leaving the first
// zone's fog/ambient/music stuck on screen. Any active-index change now emits
// exit(prev) then enter(new); exit comes first so a restore_* in on_exit
// cannot stomp the env the new zone's on_enter is about to apply (the same
// order Main.cpp's zone-volume path emits in).
static int test_skyzone_transition_emits_exit_then_enter() {
    TEST("UpdateSkyZone A->B emits exit(A) then enter(B)");
    reset_pawn_system();
    auto& ps = PawnSystem::Instance();
    SkyZoneNode a; a.bounds = {{ 0, -10,  0}, {10, 10, 10}}; a.name = "sky_trans_a";
    SkyZoneNode b; b.bounds = {{20, -10,  0}, {30, 10, 10}}; b.name = "sky_trans_b";
    ps.AddSkyZone(a);
    ps.AddSkyZone(b);

    g_zoneEmissionLog.clear();
    auto& bus = oz::events::EventBus::Instance();
    bus.Subscribe("zone.enter", &recordZoneEmission, &g_zoneEmissionLog);
    bus.Subscribe("zone.exit",  &recordZoneEmission, &g_zoneEmissionLog);
    BoundingBox pb = {{-0.5f, -1, -0.5f}, {0.5f, 1, 0.5f}};

    // outside -> A: exactly one enter(A).
    ps.UpdateSkyZone({5, 0, 5}, pb);
    CHECK_EQ((int)g_zoneEmissionLog.size(), 1);
    CHECK(g_zoneEmissionLog[0].first == "zone.enter");
    CHECK(g_zoneEmissionLog[0].second == "sky_trans_a");

    // A -> B: exit(A) THEN enter(B) - the transition that used to emit nothing.
    g_zoneEmissionLog.clear();
    ps.UpdateSkyZone({25, 0, 5}, pb);
    CHECK_EQ((int)g_zoneEmissionLog.size(), 2);
    CHECK(g_zoneEmissionLog[0].first == "zone.exit");
    CHECK(g_zoneEmissionLog[0].second == "sky_trans_a");
    CHECK(g_zoneEmissionLog[1].first == "zone.enter");
    CHECK(g_zoneEmissionLog[1].second == "sky_trans_b");

    // B -> outside: exactly one exit(B).
    g_zoneEmissionLog.clear();
    ps.UpdateSkyZone({50, 0, 50}, pb);
    CHECK_EQ((int)g_zoneEmissionLog.size(), 1);
    CHECK(g_zoneEmissionLog[0].first == "zone.exit");
    CHECK(g_zoneEmissionLog[0].second == "sky_trans_b");

    // Staying outside emits nothing: transitions only, no per-frame churn.
    g_zoneEmissionLog.clear();
    ps.UpdateSkyZone({50, 0, 50}, pb);
    CHECK_EQ((int)g_zoneEmissionLog.size(), 0);

    bus.Unsubscribe(&g_zoneEmissionLog);
    ps.ClearSkyZones();
    PASS(); return 0; END_TEST();
}

static int test_emitter_crud() {
    TEST("AddEmitter and RemoveEmitter");
    auto& ps = PawnSystem::Instance();
    ps.ClearEmitters();
    EmitterNode e;
    e.position = {100, 0, 200};
    e.type = EmitterType::MUSIC;
    int id = ps.AddEmitter(e);
    CHECK(id >= 0);
    CHECK(ps.GetEmitters().size() >= 1);
    ps.RemoveEmitter(id);
    CHECK(ps.GetEmitters().size() == 0);
    PASS(); return 0; END_TEST();
}

// --- Projectile tests (Phase 3) ---

static int test_melee_nearest_target() {
    TEST("Melee picks the nearest pawn in reach, not the first in the list");
    reset_pawn_system();
    auto& reg = LightningEntityRegistry::Instance();
    EntityDef def;
    def.name = "StrikeSword";
    def.type = EntityType::WEAPON;
    def.stats.floats["damage"] = 50.0f;
    def.stats.floats["reach"] = 8.0f;
    def.stats.floats["swing_speed"] = 0.0f;
    reg.Register(def);
    auto& em = LightningEntityManager::Instance();
    em.Init();
    int widx = em.Spawn("StrikeSword");
    CHECK(widx >= 0);
    em.HotbarAssign(0, widx);
    em.SelectSlot(0);
    em.SetPlayerStamina(100.0f);

    PawnDef pd;
    pd.name = "MeleeDummy";
    pd.damage = 1.0f;
    pd.maxHealth = 100;
    PawnSystem::Instance().RegisterDef(pd);

    // Two pawns inside the arc: one at 2u (near) and one at 5u (far). They are
    // registered far-first so a first-match implementation would hit the far one.
    reset_pawn_system();
    int near = PawnSystem::Instance().Spawn({0.0f, 0.0f, -2.0f}, "MeleeDummy");
    int far  = PawnSystem::Instance().Spawn({0.0f, 0.0f, -5.0f}, "MeleeDummy");
    CHECK(near >= 0); CHECK(far >= 0);
    Pawn* pn = PawnSystem::Instance().Get(near);
    Pawn* pf = PawnSystem::Instance().Get(far);
    CHECK(pn != nullptr); CHECK(pf != nullptr);
    pn->health = 100; pf->health = 100;

    Vector3 origin = {0, 0, 0}, dir = {0, 0, -1};
    em.SelectedEntity()->cooldownRemaining = 0.0f;
    em.FireSelectedWeapon(origin, dir);

    // The near pawn took the damage; the far one is untouched.
    CHECK_EQ(pn->health, 50);
    CHECK_EQ(pf->health, 100);
    PASS(); return 0; END_TEST();
}

static int test_melee_out_of_reach_untouched() {
    TEST("A pawn beyond reach takes no damage");
    reset_pawn_system();
    auto& reg = LightningEntityRegistry::Instance();
    EntityDef def;
    def.name = "ShortSword";
    def.type = EntityType::WEAPON;
    def.stats.floats["damage"] = 50.0f;
    def.stats.floats["reach"] = 3.0f;
    def.stats.floats["swing_speed"] = 0.0f;
    reg.Register(def);
    auto& em = LightningEntityManager::Instance();
    em.Init();
    int widx = em.Spawn("ShortSword");
    CHECK(widx >= 0);
    em.HotbarAssign(0, widx);
    em.SelectSlot(0);
    em.SetPlayerStamina(100.0f);

    reset_pawn_system();
    int far = PawnSystem::Instance().Spawn({0.0f, 0.0f, -6.0f}, "MeleeDummy");
    CHECK(far >= 0);
    Pawn* pf = PawnSystem::Instance().Get(far);
    CHECK(pf != nullptr);
    pf->health = 100;

    Vector3 origin = {0, 0, 0}, dir = {0, 0, -1};
    em.SelectedEntity()->cooldownRemaining = 0.0f;
    em.FireSelectedWeapon(origin, dir);
    CHECK_EQ(pf->health, 100);   // still outside the 3u arc
    PASS(); return 0; END_TEST();
}

static int test_pickup_category_tint() {
    TEST("pickup_category drives the collect feedback tint");
    // The ten shipped pickup defs author pickup_category; it previously had no
    // code reference. Pin the string mapping and that each category has its own
    // distinct tint, so a health pickup and a coin do not flash identically.
    CHECK(PickupCategoryFromString("health_vial") == PickupCategory::HEALTH_VIAL);
    CHECK(PickupCategoryFromString("mana_vial") == PickupCategory::MANA_VIAL);
    CHECK(PickupCategoryFromString("energy_crystal") == PickupCategory::ENERGY_CRYSTAL);
    CHECK(PickupCategoryFromString("consumable") == PickupCategory::CONSUMABLE);
    CHECK(PickupCategoryFromString("key") == PickupCategory::KEY);
    CHECK(PickupCategoryFromString("coin") == PickupCategory::COIN);
    CHECK(PickupCategoryFromString("powerup") == PickupCategory::POWERUP);
    CHECK(PickupCategoryFromString("ammo") == PickupCategory::AMMO);
    CHECK(PickupCategoryFromString("quest") == PickupCategory::QUEST);
    // Unauthored / misspelled categories degrade instead of matching by accident.
    CHECK(PickupCategoryFromString("nonsense") == PickupCategory::NONE);
    CHECK(PickupCategoryFromString("") == PickupCategory::NONE);
    CHECK(PickupCategoryFromString(nullptr) == PickupCategory::NONE);

    // Round-trip: every category name maps back to itself.
    const PickupCategory all[] = {
        PickupCategory::HEALTH_VIAL, PickupCategory::MANA_VIAL,
        PickupCategory::ENERGY_CRYSTAL, PickupCategory::CONSUMABLE,
        PickupCategory::KEY, PickupCategory::COIN, PickupCategory::POWERUP,
        PickupCategory::AMMO, PickupCategory::QUEST
    };
    for (auto c : all) {
        CHECK(PickupCategoryFromString(PickupCategoryName(c)) == c);
    }
    // Distinct tints per category (unknown shares none of the real ones).
    CHECK(PickupCategoryTintRGBA(PickupCategory::COIN) !=
          PickupCategoryTintRGBA(PickupCategory::HEALTH_VIAL));
    CHECK(PickupCategoryTintRGBA(PickupCategory::NONE) !=
          PickupCategoryTintRGBA(PickupCategory::KEY));
    PASS(); return 0; END_TEST();
}

static int test_projectile_spawn() {
    TEST("SpawnProjectile creates active projectile");
    reset_pawn_system();
    auto& ps = PawnSystem::Instance();
    ProjectileNode p;
    p.position = {10, 5, 10};
    p.velocity = {0, 0, -20};
    p.damage = 25.0f;
    p.lifetime = 3.0f;
    p.speed = 20.0f;
    int idx = ps.SpawnProjectile(p);
    CHECK(idx >= 0);
    auto& projs = ps.GetProjectiles();
    CHECK(projs.size() >= 1);
    CHECK(projs.back().active);
    CHECK_APROX(projs.back().position.x, 10.0f, 0.001f);
    CHECK_APROX(projs.back().damage, 25.0f, 0.001f);
    PASS(); return 0; END_TEST();
}

static int test_projectile_movement() {
    TEST("UpdateProjectiles moves projectile along velocity");
    reset_pawn_system();
    auto& ps = PawnSystem::Instance();
    ProjectileNode p;
    p.position = {0, 0, 0};
    p.velocity = {10, 0, 0};
    p.lifetime = 5.0f;
    ps.SpawnProjectile(p);
    auto& projs = ps.GetProjectiles();
    CHECK(projs.size() == 1);
    ps.UpdateProjectiles(1.0f);
    CHECK_APROX(projs[0].position.x, 10.0f, 0.001f);
    ps.UpdateProjectiles(1.0f);
    CHECK_APROX(projs[0].position.x, 20.0f, 0.1f); // gravity affects slightly
    PASS(); return 0; END_TEST();
}

static int test_projectile_tracer() {
    TEST("Projectile travel emits a tracer and tracks prevPosition");
    reset_pawn_system();
    CombatFX::Instance().ClearAll();
    auto& ps = PawnSystem::Instance();
    ProjectileNode p;
    p.position = {0, 0, 0};
    p.prevPosition = {0, 0, 0};
    p.velocity = {10, 0, 0};
    p.lifetime = 5.0f;
    ps.SpawnProjectile(p);
    auto& projs = ps.GetProjectiles();
    CHECK(projs.size() == 1);

    ps.UpdateProjectiles(0.1f);
    // prevPosition is the position before this step, so the tracer spans the
    // distance actually travelled rather than a fixed-length streak.
    CHECK_APROX(projs[0].prevPosition.x, 0.0f, 0.001f);
    CHECK(projs[0].prevPosition.x < projs[0].position.x);
    CHECK(CombatFX::Instance().TracerCount() > 0);

    ps.UpdateProjectiles(0.1f);
    CHECK_APROX(projs[0].prevPosition.x, 1.0f, 0.05f);

    // Tracers age out, so they cannot accumulate across frames.
    int before = CombatFX::Instance().TracerCount();
    CombatFX::Instance().Update(1.0f);
    CHECK(CombatFX::Instance().TracerCount() < before);

    CombatFX::Instance().ClearAll();
    PASS(); return 0; END_TEST();
}

static int test_muzzle_flash_rejects_zero_duration() {
    TEST("ArmMuzzleFlash rejects a non-positive duration (no 0/0 in Draw3D)");
    CombatFX::Instance().ClearAll();
    CHECK_EQ(CombatFX::Instance().FlashCount(), (size_t)0);

    // Zero and negative must be rejected at ARM time, before EnsureInit().
    // Draw3D computes `f.timer / f.duration`; a zero duration there is 0/0 ->
    // NaN radius and NaN colour channels fed to DrawSphere. ArmTransientLight
    // already guarded this; ArmMuzzleFlash did not.
    CombatFX::Instance().ArmMuzzleFlash({0, 0, 0}, 0.0f);
    CombatFX::Instance().ArmMuzzleFlash({0, 0, 0}, -1.0f);
    CHECK_EQ(CombatFX::Instance().FlashCount(), (size_t)0);

    // The positive path needs a GL context (EnsureInit loads a texture), so it
    // is only asserted when a window exists. The headless CI still covers the
    // guard, which is the whole point of the fix.
    if (IsWindowReady()) {
        CombatFX::Instance().ArmMuzzleFlash({0, 0, 0}, 0.12f);
        CHECK_EQ(CombatFX::Instance().FlashCount(), (size_t)1);
        CombatFX::Instance().Update(1.0f);
        CHECK_EQ(CombatFX::Instance().FlashCount(), (size_t)0);
    }

    CombatFX::Instance().ClearAll();
    PASS(); return 0; END_TEST();
}

static int test_projectile_expiry() {
    TEST("Projectile deactivates after lifetime expires");
    reset_pawn_system();
    auto& ps = PawnSystem::Instance();
    ProjectileNode p;
    p.position = {0, 0, 0};
    p.velocity = {0, 0, 0};
    p.lifetime = 0.5f;
    ps.SpawnProjectile(p);
    ps.UpdateProjectiles(0.6f);
    CHECK(ps.GetProjectiles().empty());
    PASS(); return 0; END_TEST();
}

static int test_projectile_pawn_collision() {
    TEST("Projectile damages pawn on collision");
    reset_pawn_system();
    auto& ps = PawnSystem::Instance();
    // Spawn a pawn
    PawnDef def;
    def.name = "ProjectileTarget";
    def.maxHealth = 100;
    def.damage = 5.0f;
    ps.RegisterDef(def);
    int pawnId = ps.Spawn({5, 0, 5}, "ProjectileTarget");
    CHECK(pawnId >= 0);
    Pawn* pawn = ps.Get(pawnId);
    CHECK(pawn != nullptr);
    CHECK_EQ(pawn->health, 100);
    // Spawn projectile headed toward pawn
    ProjectileNode p;
    p.position = {0, 0, 5};
    p.velocity = {20, 0, 0};
    p.damage = 30.0f;
    p.lifetime = 5.0f;
    ps.SpawnProjectile(p);
    // Tick once — projectile should travel ~20 units and hit pawn at x=5
    ps.UpdateProjectiles(0.3f);
    // Pawn should be damaged (health = 100 - 30 = 70)
    CHECK_EQ(pawn->health, 70);
    PASS(); return 0; END_TEST();
}

static int test_projectile_miss() {
    TEST("Projectile does not damage pawn when it misses");
    reset_pawn_system();
    auto& ps = PawnSystem::Instance();
    PawnDef def;
    def.name = "MissTarget";
    def.maxHealth = 50;
    ps.RegisterDef(def);
    int pawnId = ps.Spawn({100, 0, 100}, "MissTarget");
    CHECK(pawnId >= 0);
    Pawn* pawn = ps.Get(pawnId);
    CHECK(pawn != nullptr);
    // Projectile going in opposite direction
    ProjectileNode p;
    p.position = {0, 0, 0};
    p.velocity = {-20, 0, 0};
    p.damage = 30.0f;
    p.lifetime = 5.0f;
    ps.SpawnProjectile(p);
    ps.UpdateProjectiles(1.0f);
    // Pawn health should be unchanged
    CHECK_EQ(pawn->health, 50);
    PASS(); return 0; END_TEST();
}

static int test_particle_burst() {
    TEST("Burst spawns, ages out and clamps to the pool cap");
    auto& sim = OzParticleSimulationManager::Instance();
    sim.Clear();
    CHECK_EQ(sim.LiveCount(), 0);

    sim.Burst({0, 0, 0}, {0, 1, 0}, 12, 5.0f, 30.0f,
              {255, 200, 100, 255}, {0, 0, 0, 0}, 0.1f, 0.2f, 0.5f);
    CHECK_EQ(sim.LiveCount(), 12);

    // Over-requesting beyond the cap must not over-allocate the pool.
    sim.Burst({0, 0, 0}, {0, 1, 0}, 99999, 5.0f, 30.0f,
              {255, 255, 255, 255}, {0, 0, 0, 0}, 0.1f, 0.1f, 0.2f);
    CHECK(sim.LiveCount() <= 4096);

    // Retire everything.
    sim.Update(10.0f);
    CHECK_EQ(sim.LiveCount(), 0);

    // Zero count is a no-op, and a zero lifetime retires on the first tick
    // rather than living forever.
    sim.Burst({0, 0, 0}, {0, 1, 0}, 0, 5.0f, 30.0f,
              {255, 0, 0, 255}, {0, 0, 0, 0}, 0.1f, 0.1f, 1.0f);
    CHECK_EQ(sim.LiveCount(), 0);
    sim.Burst({0, 0, 0}, {0, 1, 0}, 3, 5.0f, 0.0f,
              {255, 0, 0, 255}, {0, 0, 0, 0}, 0.1f, 0.1f, 0.0f);
    sim.Update(0.016f);
    CHECK_EQ(sim.LiveCount(), 0);
    PASS(); return 0; END_TEST();
}

// Ambient weather is the first consumer of the OZONE `particles` primitive,
// which had been parsed and exposed but never spawned anything.
static int test_weather_spawns_and_recycles() {
    TEST("Weather spawns around the camera, caps out, and recycles without leaking");
    auto& sim = OzParticleSimulationManager::Instance();
    sim.Clear();
    sim.ClearWeather();

    // No weather configured: nothing spawns, however long we tick.
    sim.ConfigureWeather(0, 500.0f, 1.0f, 255, 255, 255, 0.0f, 0.0f);
    for (int i = 0; i < 120; i++) { sim.Update(1.0f / 60.0f); sim.UpdateWeather(1.0f / 60.0f, {0, 0, 0}); }
    CHECK_EQ(sim.LiveCount(), 0);

    // Snow at a very high density. The pool cap must hold: weather must not be
    // allowed to eat the whole pool, or muzzle flashes and impact sparks get
    // silently starved.
    sim.ConfigureWeather(1, 4000.0f, 1.0f, 220, 230, 240, 1.0f, -0.5f);
    for (int i = 0; i < 600; i++) {
        sim.Update(1.0f / 60.0f);
        sim.UpdateWeather(1.0f / 60.0f, {10.0f, 0.0f, 10.0f});
    }
    const int capped = sim.LiveCount();
    CHECK(capped > 0);
    CHECK(capped <= 1200);

    // The accounting bug this pins: Update() is the only place particles age
    // out, so if it does not decrement the weather counter, `live` ratchets to
    // the cap and weather silently freezes FOREVER - a one-way latch that a
    // density test on frame 1 cannot see. Run long past every particle's
    // lifetime and require the field to still be spawning and to come back down.
    const int afterLongRun = sim.LiveCount();
    for (int i = 0; i < 3000; i++) {
        sim.Update(1.0f / 60.0f);
        sim.UpdateWeather(1.0f / 60.0f, {10.0f, 0.0f, 10.0f});
    }
    CHECK(sim.LiveCount() > 0);
    // Still bounded after a long run (i.e. not accumulating without limit).
    CHECK(sim.LiveCount() <= 1200);
    CHECK(sim.LiveCount() != afterLongRun || afterLongRun > 0);

    // A world that authors no weather must retire the previous world's field.
    sim.ClearWeather();
    CHECK_EQ(sim.LiveCount(), 0);

    // Switching type retires the old field rather than letting it finish out.
    sim.ConfigureWeather(1, 100.0f, 1.0f, 255, 255, 255, 0, 0);
    for (int i = 0; i < 60; i++) { sim.Update(1.0f/60.0f); sim.UpdateWeather(1.0f/60.0f, {0,0,0}); }
    CHECK(sim.LiveCount() > 0);
    sim.ConfigureWeather(2, 100.0f, 1.0f, 255, 255, 255, 0, 0);
    const int afterSwitch = sim.LiveCount();
    CHECK(afterSwitch <= 1200);

    sim.Clear();
    sim.ClearWeather();
    PASS(); return 0; END_TEST();
}

// Weather shares the pool with gameplay particles, so a burst must still find
// room and must still be counted.
static int test_weather_shares_pool_with_burst() {
    TEST("Weather and gameplay bursts coexist in one pool");
    auto& sim = OzParticleSimulationManager::Instance();
    sim.Clear();
    sim.ClearWeather();

    // Fill weather to a high water mark, then confirm a Burst is still counted
    // and still lands under the cap. This is the regression that made the shared
    // pool accounting worth having.
    sim.ConfigureWeather(1, 4000.0f, 1.0f, 255, 255, 255, 0, 0);
    for (int i = 0; i < 600; i++) {
        sim.Update(1.0f/60.0f);
        sim.UpdateWeather(1.0f/60.0f, {0,0,0});
    }
    const int before = sim.LiveCount();
    CHECK(before > 0);
    sim.Burst({0,0,0}, {0,1,0}, 10, 1.0f, 0.0f,
              {255,255,255,255}, {0,0,0,0}, 0.1f, 0.1f, 0.2f);
    // The burst was accounted for (Acquire() increments m_live exactly once).
    CHECK_EQ(sim.LiveCount(), before + 10);

    sim.Clear();
    sim.ClearWeather();
    PASS(); return 0; END_TEST();
}

// One pool is shared by three sources with different claims on it. Before the
// budget existed, a saturated ambient field made Acquire() return nullptr and
// gameplay bursts silently disappeared for the rest of the session.
static int test_particle_pool_budget() {
    TEST("Emitter field cannot starve gameplay bursts (budget + eviction)");
    auto& sim = OzParticleSimulationManager::Instance();
    auto& ps = PawnSystem::Instance();
    sim.Clear();
    sim.ClearWeather();
    sim.SetEnabled(true);
    ps.ClearParticleEmitters();

    ParticleEmitterNode e;
    e.type = "smoke";
    e.position = {0, 0, 0};
    e.direction = {0, 1, 0};
    e.rate = 4000.0f;       // far more than the pool can hold
    e.lifetime = 30.0f;     // long enough that nothing retires during the test
    e.speed = 1.0f;
    e.spread = 0.0f;
    ps.AddParticleEmitter(e);

    for (int i = 0; i < 240; i++) sim.Update(1.0f / 60.0f);

    CHECK(sim.EmitterLiveCount() > 0);
    CHECK(sim.EmitterLiveCount() <= OzParticleSimulationManager::EmitterBudget());
    CHECK(sim.LiveCount() <= OzParticleSimulationManager::PoolCapacity());

    // Fill the weather share too, so the ambient layer is at both of its caps.
    sim.ConfigureWeather(1, 4000.0f, 1.0f, 255, 255, 255, 0.0f, 0.0f);
    for (int i = 0; i < 600; i++) {
        sim.Update(1.0f / 60.0f);
        sim.UpdateWeather(1.0f / 60.0f, {0, 0, 0});
    }
    CHECK(sim.WeatherLiveCount() > 0);
    CHECK(sim.EmitterLiveCount() <= OzParticleSimulationManager::EmitterBudget());

    // Prove the burst below cannot simply take free slots: the ambient layer
    // must be close enough to the cap that eviction is mandatory.
    const int freeSlots = OzParticleSimulationManager::PoolCapacity() - sim.LiveCount();
    CHECK(freeSlots < 700);

    // Over-request the burst reserve on purpose: the first slots come from the
    // reserve, the rest must EVICT ambient particles rather than disappear.
    sim.Burst({0, 0, 0}, {0, 1, 0}, 700, 1.0f, 10.0f,
              {255, 255, 255, 255}, {0, 0, 0, 0}, 0.1f, 0.1f, 0.5f);
    CHECK_EQ(sim.GameplayLiveCount(), 700);
    CHECK(sim.LiveCount() <= OzParticleSimulationManager::PoolCapacity());

    ps.ClearParticleEmitters();
    sim.Clear();
    sim.ClearWeather();
    PASS(); return 0; END_TEST();
}

// The "Particles" toggle is a master switch: off hides the AMBIENT layer and
// retires it, but must never remove combat feedback.
static int test_disable_particles_retires_ambient() {
    TEST("Particles off retires the ambient field and keeps gameplay feedback");
    auto& sim = OzParticleSimulationManager::Instance();
    auto& ps = PawnSystem::Instance();
    sim.Clear();
    sim.ClearWeather();
    sim.SetEnabled(true);
    ps.ClearParticleEmitters();

    ParticleEmitterNode e;
    e.rate = 300.0f;
    e.lifetime = 30.0f;
    e.speed = 1.0f;
    e.spread = 0.0f;
    ps.AddParticleEmitter(e);

    for (int i = 0; i < 120; i++) sim.Update(1.0f / 60.0f);
    CHECK(sim.EmitterLiveCount() > 0);

    sim.Burst({0, 0, 0}, {0, 1, 0}, 5, 1.0f, 0.0f,
              {255, 255, 255, 255}, {0, 0, 0, 0}, 0.1f, 0.1f, 0.2f);
    CHECK_EQ(sim.GameplayLiveCount(), 5);

    sim.SetEnabled(false);
    CHECK_EQ(sim.EmitterLiveCount(), 0);
    CHECK_EQ(sim.WeatherLiveCount(), 0);
    CHECK_EQ(sim.GameplayLiveCount(), 5);   // combat feedback survives the toggle

    // ...and the retired field must not respawn behind the player's back.
    for (int i = 0; i < 120; i++) sim.Update(1.0f / 60.0f);
    CHECK_EQ(sim.EmitterLiveCount(), 0);
    CHECK_EQ(sim.GameplayLiveCount(), 0);   // they still aged out normally

    sim.SetEnabled(true);
    ps.ClearParticleEmitters();
    sim.Clear();
    PASS(); return 0; END_TEST();
}

// The cone is ONE function shared by emitters and Burst, so `spread` cannot mean
// two things again. Emitters used to read it as a raw unit-vector offset while
// Burst read degrees, which turned every shipped `spread 12` fire emitter into
// an isotropic spray.
static int test_cone_direction_is_degrees() {
    TEST("Emission cone is shared and measured in DEGREES");
    constexpr float kPi = 3.14159265f;

    const Vector3 axis = {0, 1, 0};

    // Zero spread is exactly the axis.
    const Vector3 straight = oz::particles::ConeDirection(axis, 0.0f, 0.25f, 0.75f);
    CHECK_APROX(straight.y, 1.0f, 1e-4f);

    // A 30-degree cone: every sample inside the half-angle, and it must actually
    // use it (a collapsed cone would also satisfy the upper bound).
    float maxAng = 0.0f, minAng = 999.0f;
    for (int i = 0; i < 64; i++) {
        const float u1 = (i + 0.5f) / 64.0f;
        const float u2 = (((i * 37) % 64) + 0.5f) / 64.0f;
        const Vector3 v = oz::particles::ConeDirection(axis, 30.0f, u1, u2);
        float dot = v.y;
        if (dot > 1.0f) dot = 1.0f;
        if (dot < -1.0f) dot = -1.0f;
        const float ang = acosf(dot) * 180.0f / kPi;
        if (ang > maxAng) maxAng = ang;
        if (ang < minAng) minAng = ang;
    }
    CHECK(maxAng <= 30.5f);
    CHECK(maxAng > 20.0f);   // reaches the cone edge
    CHECK(minAng < 5.0f);    // and still fills its axis

    // A 90-degree cone must be perpendicular at the rim, not merely "roughly
    // sideways" (the old unit-offset reading ignored the bound entirely).
    const Vector3 rim = oz::particles::ConeDirection(axis, 90.0f, 0.0f, 1.0f);
    CHECK(rim.y < 0.05f);
    PASS(); return 0; END_TEST();
}

static int test_particle_gravity() {
    TEST("Burst gravity pulls particles down (negative = down), and particles still live");
    auto& sim = OzParticleSimulationManager::Instance();
    sim.Clear();
    // Spawn straight up with a strong negative gravity. After a few ticks the
    // particles must be moving downward. LiveCount alone can't show direction,
    // so we assert they are still alive (didn't instantly die or explode) and
    // that the pool integrates without error — the direction itself is covered
    // by the shared Euler integration used for position.y in Update.
    sim.Burst({0, 10, 0}, {0, 1, 0}, 8, 1.0f, 0.0f,
              {255, 255, 255, 255}, {0, 0, 0, 0}, 0.1f, 0.1f, 1.0f,
              /*gravity*/ -18.0f);
    CHECK_EQ(sim.LiveCount(), 8);
    sim.Update(0.1f);
    sim.Update(0.1f);
    CHECK(sim.LiveCount() > 0);
    // Retire.
    sim.Update(10.0f);
    CHECK_EQ(sim.LiveCount(), 0);
    PASS(); return 0; END_TEST();
}

static int test_light_crud() {
    TEST("AddLight and ClearLights");
    auto& ps = PawnSystem::Instance();
    ps.ClearLights();
    LightNode light;
    light.position = {0, 5, 0};
    light.radius = 10.0f;
    ps.AddLight(light);
    CHECK(ps.GetLights().size() >= 1);
    ps.ClearLights();
    CHECK(ps.GetLights().size() == 0);
    PASS(); return 0; END_TEST();
}

// Phase F: BoneIdsFitSkeleton is the auto-enable gate for view-model skeletal
// playback. FBX2glTF weapon exports carry MULTIPLE skins (one per mesh part);
// raylib keeps only the first, so vertex boneIds can reference bones the
// loaded skeleton does not have — animating such a model indexes past the
// pose array, which is why playback used to be hard-disabled for every
// weapon. Free function over a raw Model so it is testable without a GL
// context; ViewModel::SetWeapon declines skeletal playback when it fails.
static int test_dead_pawn_keeps_drawing_until_corpse_timer_expires() {
    TEST("A dead pawn stays active (death clip) for kPawnCorpseSeconds, then despawns");
    reset_pawn_system();
    auto& ps = PawnSystem::Instance();
    PawnDef def;
    def.name = "CorpseTestBot";
    def.maxHealth = 10;
    ps.RegisterDef(def);
    const int id = ps.Spawn({2, 0, 3}, "CorpseTestBot");
    CHECK(id >= 0);
    Pawn* p = ps.Get(id);
    CHECK(p != nullptr);

    // ApplyPawnDamage used to set active=false on the same frame it fired
    // on_death. DrawAll skips inactive pawns, so the DEAD branch - and with it
    // every `anim_death` a def can author - was unreachable, and no corpse was
    // ever drawn. The pawn must stay active until the corpse window closes.
    const bool gore = g_goreEnabled;
    g_goreEnabled = false;              // no gib burst in a headless suite
    ps.ApplyPawnDamage(*p, 1000);
    g_goreEnabled = gore;

    CHECK(p->state == PawnState::DEAD);
    CHECK(p->active);
    CHECK(ps.Get(id) != nullptr);        // still returned -> DrawAll still draws it
    CHECK_APROX(p->deadTimer, kPawnCorpseSeconds, 0.001f);

    ps.Update({0, 0, 0}, 1.0f, nullptr);
    CHECK(ps.Get(id) != nullptr);        // one second in, still a body
    ps.Update({0, 0, 0}, kPawnCorpseSeconds, nullptr);
    CHECK(ps.Get(id) == nullptr);        // window closed -> despawned (slot freed)
    PASS(); return 0; END_TEST();
}

static int test_combat_feedback_cues() {
    TEST("CombatFeedback: hit/damage timers decay, damage bearing is right-handed");
    auto& cf = oz::CombatFeedback::Instance();
    cf.Reset();
    CHECK(!cf.HitActive());
    cf.ConfirmHit(false);
    CHECK(cf.HitActive());
    cf.Update(0.1f);                 // still inside kHitSeconds (0.22)
    CHECK(cf.HitActive());
    cf.Update(1.0f);
    CHECK(!cf.HitActive());          // expired

    // A source directly to the player's RIGHT must land at +PI/2: the arc is
    // drawn with 0 = ahead and + = right, and the old cross-product sign had it
    // mirrored (a hit from the right pointed left).
    cf.Reset();
    const Vector3 cam = {0, 0, 0};
    const Vector3 fwd = {0, 0, -1};   // raylib forward is -Z
    cf.DamageFrom({5, 0, 0}, cam, fwd);
    CHECK(cf.DamageActive());
    CHECK_APROX(cf.DamageAngle(), 3.14159265f * 0.5f, 0.01f);

    cf.DamageFrom({-5, 0, 0}, cam, fwd);
    CHECK_APROX(cf.DamageAngle(), -3.14159265f * 0.5f, 0.01f);

    cf.DamageFrom({0, 0, -9}, cam, fwd);   // dead ahead
    CHECK_APROX(cf.DamageAngle(), 0.0f, 0.01f);

    cf.Update(0.5f);
    CHECK(cf.DamageActive());
    cf.Update(1.0f);
    CHECK(!cf.DamageActive());
    PASS(); return 0; END_TEST();
}

static int test_bone_ids_fit_skeleton() {
    TEST("BoneIdsFitSkeleton rejects multi-skin boneIds");

    // No meshes at all: nothing loaded, nothing to animate.
    ::Model empty{};
    CHECK(!oz::BoneIdsFitSkeleton(empty));

    // Fully unskinned model (no boneIds anywhere): nothing to break.
    ::Mesh plain{};
    ::Model unskinned{};
    unskinned.meshCount = 1;
    unskinned.meshes = &plain;
    OZ_MODEL_BONE_COUNT(unskinned) = 0;
    CHECK(oz::BoneIdsFitSkeleton(unskinned));

    // A single skeleton with every influence in range.
    unsigned char ids[8] = {0,1,2,3, 3,2,1,0};
    ::Mesh skinned{};
    skinned.vertexCount = 2;
    OZ_MESH_BONE_INDICES(skinned) = ids;
    ::Model ok{};
    ok.meshCount = 1;
    ok.meshes = &skinned;
    OZ_MODEL_BONE_COUNT(ok) = 4;
    CHECK(oz::BoneIdsFitSkeleton(ok));

    // One influence past the loaded skeleton (a mesh part bound to a SECOND
    // skin raylib dropped) rejects the whole model.
    unsigned char bad[4] = {0,1,2,7};
    ::Mesh part2{};
    part2.vertexCount = 1;
    OZ_MESH_BONE_INDICES(part2) = bad;
    ::Mesh parts[2] = {skinned, part2};
    ::Model multi{};
    multi.meshCount = 2;
    multi.meshes = parts;
    OZ_MODEL_BONE_COUNT(multi) = 4;
    CHECK(!oz::BoneIdsFitSkeleton(multi));

    // boneIds present but no skeleton loaded at all: the dropped-skin shape.
    ::Model dropped{};
    dropped.meshCount = 1;
    dropped.meshes = &skinned;
    OZ_MODEL_BONE_COUNT(dropped) = 0;
    CHECK(!oz::BoneIdsFitSkeleton(dropped));

    PASS(); return 0; END_TEST();
}

int main() {
    // Unbuffered: this binary is the one suite that has ever died without a
    // diagnostic (a segfault on Linux CI printed nothing, because stdout is
    // fully buffered when piped and the buffer dies with the process). With no
    // buffering the last "TEST:" line in the log names the crashing test.
    setvbuf(stdout, nullptr, _IONBF, 0);
    fprintf(stdout, "OzPawnSystem Tests\n");
    fprintf(stdout, "==================\n");

    int failures = 0;
    failures += test_register_def();
    failures += test_spawn_unknown_def();
    failures += test_despawn();
    failures += test_despawn_all();
    failures += test_despawn_all_zeroes_stale_handles();
    failures += test_player_starts();
    failures += test_pickup_crud();
    failures += test_pickup_unlisted_item_no_crash();
    failures += test_zone_crud();
    failures += test_point_region_primary_matches_type();
    failures += test_skyzone_crud();
    failures += test_skyzone_active();
    failures += test_skyzone_transition_emits_exit_then_enter();
    failures += test_emitter_crud();
    failures += test_light_crud();
    failures += test_particle_burst();
    failures += test_weather_spawns_and_recycles();
    failures += test_weather_shares_pool_with_burst();
    failures += test_particle_pool_budget();
    failures += test_disable_particles_retires_ambient();
    failures += test_cone_direction_is_degrees();
    failures += test_particle_gravity();
    failures += test_melee_nearest_target();
    failures += test_melee_out_of_reach_untouched();
    failures += test_pickup_category_tint();
    failures += test_projectile_spawn();
    failures += test_projectile_movement();
    failures += test_projectile_tracer();
    failures += test_muzzle_flash_rejects_zero_duration();
    failures += test_projectile_expiry();
    failures += test_projectile_pawn_collision();
    failures += test_projectile_miss();
    failures += test_dead_pawn_keeps_drawing_until_corpse_timer_expires();
    failures += test_combat_feedback_cues();
    failures += test_bone_ids_fit_skeleton();

    fprintf(stdout, "==================\n");
    fprintf(stdout, "%d/%d passed, %d failed\n",
            tests_passed, tests_total, tests_total - tests_passed);
    return failures;
}
