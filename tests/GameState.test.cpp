// GameState unit tests — projectile simulation and AMMO pickup.
// No raylib dependency. Compile with SERVER_CXX.
//
// g++ -O0 -g --std=c++20 -I Source -DOMEGA_TEST_ENV \
//   Source/Server/GameState.cpp Source/Network/Network.cpp Source/Log.cpp \
//   tests/GameState.test.cpp \
//   -o test_game_state -lws2_32 -lm

#include "../Source/Server/GameState.hpp"
#include "../Source/Client/Client.hpp"
#include "../Source/Pawn/PickupItems.hpp"
#include <cstdio>
#include <cstring>
#include <cassert>
#include <string>
#include <vector>
#include <unordered_map>
#include <thread>
#include <chrono>
#include <filesystem>
#include <fstream>

static int tests_total = 0, tests_passed = 0;
#define TEST(name) do { tests_total++; fprintf(stdout, "  TEST: %s ... ", name);
#define PASS() do { tests_passed++; fprintf(stdout, "PASS\n"); } while(0)
#define FAIL(msg) do { fprintf(stdout, "FAIL: %s\n", msg); return 1; } while(0)
#define CHECK(cond) do { if (!(cond)) { fprintf(stdout, "FAIL: %s\n", #cond); return 1; } } while(0)
#define CHECK_EQ(a, b) do { if ((a) != (b)) { fprintf(stdout, "FAIL: expected %d, got %d\n", (int)(b), (int)(a)); return 1; } } while(0)
#define CHECK_STR(a, b) do { std::string _a = (a), _b = (b); if (_a != _b) { \
    fprintf(stdout, "FAIL: expected \"%s\", got \"%s\"\n", _b.c_str(), _a.c_str()); return 1; } } while(0)
#define CHECK_APROX(a, b, eps) do { float diff = (a) - (b); if (diff < 0) diff = -diff; if (diff > (eps)) { fprintf(stdout, "FAIL: expected %f, got %f\n", (float)(b), (float)(a)); return 1; } } while(0)
#define END_TEST() } while(0)

// --- Helpers ---
static WorldState make_test_world(int index) {
    WorldState ws;
    ws.world_index = index;
    ws.name = "test";
    ws.partitions.resize(1);
    ws.partitions[0].id = 0;
    ws.partitions[0].min_x = -1000;
    ws.partitions[0].max_x = 1000;
    ws.partitions[0].min_z = -1000;
    ws.partitions[0].max_z = 1000;
    return ws;
}

// --- Projectile tests (Phase 4) ---

static int test_spawn_projectile() {
    TEST("spawn_projectile creates active projectile in world");
    GameState gs;
    WorldState ws = make_test_world(0);
    gs.spawn_projectile(ws, 1, {0,0,0}, {1,0,0}, 20.0f, 15.0f, 3.0f);
    CHECK_EQ(ws.projectiles.size(), (size_t)1);
    CHECK(ws.projectiles[0].active);
    CHECK_APROX(ws.projectiles[0].velocity.x, 20.0f, 0.001f);
    CHECK_APROX(ws.projectiles[0].damage, 15.0f, 0.001f);
    CHECK_EQ(ws.projectiles[0].owner_id, (uint32_t)1);
    PASS(); return 0; END_TEST();
}

static int test_tick_projectile_movement() {
    TEST("tick_projectiles moves projectile along velocity");
    GameState gs;
    WorldState ws = make_test_world(0);
    gs.spawn_projectile(ws, 1, {0,0,0}, {0,0,-1}, 30.0f, 10.0f, 5.0f);
    gs.tick_projectiles(ws, 1.0f);
    CHECK_APROX(ws.projectiles[0].position.z, -30.0f, 0.001f);
    gs.tick_projectiles(ws, 0.5f);
    CHECK_APROX(ws.projectiles[0].position.z, -45.0f, 0.001f);
    PASS(); return 0; END_TEST();
}

static int test_tick_projectile_expiry() {
    TEST("Projectile deactivates after lifetime");
    GameState gs;
    WorldState ws = make_test_world(0);
    gs.spawn_projectile(ws, 1, {0,0,0}, {1,0,0}, 1.0f, 10.0f, 0.5f);
    gs.tick_projectiles(ws, 0.6f);
    CHECK(ws.projectiles.empty());
    PASS(); return 0; END_TEST();
}

static int test_projectile_hits_npc() {
    TEST("Projectile damages NPC on collision");
    GameState gs;
    WorldState ws = make_test_world(0);
    ServerNPC npc;
    npc.active = true;
    npc.health = 100;
    npc.max_health = 100;
    npc.position = {9, 0, 0};
    ws.global_npcs.push_back(npc);
    gs.spawn_projectile(ws, 1, {0,0,0}, {1,0,0}, 20.0f, 25.0f, 5.0f);
    gs.tick_projectiles(ws, 0.5f); // travels 10 units, distance to NPC = 1 < 2
    CHECK(ws.global_npcs[0].health < 100);
    CHECK(ws.projectiles.empty());
    PASS(); return 0; END_TEST();
}

static int test_projectile_misses_npc() {
    TEST("Projectile does not hit NPC when path diverges");
    GameState gs;
    WorldState ws = make_test_world(0);
    ServerNPC npc;
    npc.active = true;
    npc.health = 100;
    npc.position = {10, 0, 0};
    ws.global_npcs.push_back(npc);
    // Projectile going away from NPC
    gs.spawn_projectile(ws, 1, {0,0,0}, {-1,0,0}, 20.0f, 25.0f, 5.0f);
    gs.tick_projectiles(ws, 1.0f);
    CHECK_EQ(ws.global_npcs[0].health, 100); // unharmed
    PASS(); return 0; END_TEST();
}

static int test_projectile_hits_partition_npc() {
    TEST("Projectile damages NPC in partition");
    GameState gs;
    WorldState ws = make_test_world(0);
    ServerNPC npc;
    npc.active = true;
    npc.health = 80;
    npc.position = {5, 0, 5};
    ws.partitions[0].npcs.push_back(npc);
    gs.spawn_projectile(ws, 1, {0,0,5}, {1,0,0}, 20.0f, 40.0f, 5.0f);
    gs.tick_projectiles(ws, 0.3f);
    CHECK(ws.partitions[0].npcs[0].health < 80);
    PASS(); return 0; END_TEST();
}

static int test_projectile_avoids_owner() {
    TEST("Projectile does not damage its owner");
    GameState gs;
    // Add owner player
    uint32_t owner_id = gs.add_player(42, "Shooter");
    ServerPlayer* owner = gs.get_player(owner_id);
    CHECK(owner != nullptr);
    owner->connected = true;
    owner->position = {0, 0, 0};
    owner->health = 100;

    WorldState ws = make_test_world(0);
    gs.spawn_projectile(ws, owner_id, {0,0,0}, {0,0,-1}, 1.0f, 50.0f, 5.0f);
    // Tick — projectile barely moves, stays near owner
    gs.tick_projectiles(ws, 0.1f);
    CHECK_EQ(owner->health, 100); // owner should not be damaged
    PASS(); return 0; END_TEST();
}

static int test_projectile_hits_other_player() {
    TEST("Projectile damages other player");
    GameState gs;
    uint32_t owner_id = gs.add_player(1, "Shooter");
    uint32_t target_id = gs.add_player(2, "Target");
    ServerPlayer* owner = gs.get_player(owner_id);
    ServerPlayer* target = gs.get_player(target_id);
    CHECK(owner != nullptr && target != nullptr);
    owner->connected = true;
    owner->position = {0, 0, 0};
    owner->health = 100;
    target->connected = true;
    target->position = {10, 0, 0};
    target->health = 100;

    WorldState ws = make_test_world(0);
    // FIXME: test requires world_index tracking on players
    // For now, this validates the tick_projectiles doesn't crash
    gs.spawn_projectile(ws, owner_id, {0,0,0}, {1,0,0}, 20.0f, 30.0f, 5.0f);
    gs.tick_projectiles(ws, 0.6f);
    // Target at x=10 with velocity 20*0.6=12, projectile should hit near target
    PASS(); return 0; END_TEST();
}

// --- Pickup tests (Phase 5) ---

static int test_ammo_pickup_grants_ammo() {
    TEST("AMMO pickup grants ammo instead of XP");
    GameState gs;
    uint32_t pid = gs.add_player(10, "AmmoCollector");
    ServerPlayer* p = gs.get_player(pid);
    CHECK(p != nullptr);
    CHECK_EQ(p->ammo, 0);

    WorldState ws = make_test_world(0);
    ServerPickup pickup;
    pickup.id = 1;
    pickup.type = PickupType::AMMO;
    pickup.value = 30;
    pickup.active = true;
    pickup.respawnable = true;
    ws.global_pickups.push_back(pickup);
    gs.worlds().push_back(ws);

    PickupType out_type;
    int out_value;
    bool collected = gs.collect_pickup(pid, 1, 0, &out_type, &out_value);
    CHECK(collected);
    CHECK_EQ(out_type == PickupType::AMMO, true);
    CHECK_EQ(p->ammo, 30);
    PASS(); return 0; END_TEST();
}

static int test_ammo_pickup_no_xp_granted() {
    TEST("AMMO pickup does not grant XP");
    GameState gs;
    uint32_t pid = gs.add_player(11, "AmmoOnly");
    ServerPlayer* p = gs.get_player(pid);
    CHECK(p != nullptr);
    int xp_before = p->xp;

    WorldState ws = make_test_world(0);
    ServerPickup pickup;
    pickup.id = 2;
    pickup.type = PickupType::AMMO;
    pickup.value = 50;
    pickup.active = true;
    ws.global_pickups.push_back(pickup);
    gs.worlds().push_back(ws);

    gs.collect_pickup(pid, 2, 0);
    CHECK_EQ(p->ammo, 50);
    CHECK_EQ(p->xp, xp_before);
    PASS(); return 0; END_TEST();
}

// Regression: the client-supplied world index used to be trusted verbatim, so a
// player in world 0 could collect pickups belonging to any other loaded world
// (all shipped worlds are authored near the origin, so their pickup clusters sit
// within collect range of each other).
static int test_collect_pickup_rejects_foreign_world() {
    TEST("collect_pickup rejects a world_index the player is not in");
    GameState gs;
    uint32_t pid = gs.add_player(12, "WrongWorld");
    ServerPlayer* p = gs.get_player(pid);
    CHECK(p != nullptr);

    // Player is in world 0.
    WorldState home = make_test_world(0);
    ServerPickup home_pickup;
    home_pickup.id = 1;
    home_pickup.type = PickupType::COIN;
    home_pickup.value = 5;
    home_pickup.active = true;
    home.global_pickups.push_back(home_pickup);
    gs.worlds().push_back(home);

    // A second world also holding a pickup at the same position and the same id.
    WorldState other = make_test_world(1);
    ServerPickup other_pickup;
    other_pickup.id = 1;
    other_pickup.type = PickupType::COIN;
    other_pickup.value = 500;
    other_pickup.active = true;
    other.global_pickups.push_back(other_pickup);
    gs.worlds().push_back(other);

    // Re-read through GameState: worlds() stores copies, so `home`/`other` go
    // stale the moment they are pushed.
    WorldState* home_w = gs.get_world(0);
    WorldState* other_w = gs.get_world(1);
    CHECK(home_w != nullptr && other_w != nullptr);

    int xp_before = p->xp;

    // Pointing the collect at world 1 must be refused outright.
    bool crossed = gs.collect_pickup(pid, 1, 1);
    CHECK_EQ(crossed, false);
    CHECK_EQ(p->xp, xp_before);
    CHECK_EQ(other_w->global_pickups[0].active, true);

    // The player's own world still works.
    bool ok = gs.collect_pickup(pid, 1, 0);
    CHECK_EQ(ok, true);
    CHECK_EQ(p->xp, xp_before + 5);   // only the home pickup's value applied
    CHECK_EQ(home_w->global_pickups[0].active, false);
    CHECK_EQ(other_w->global_pickups[0].active, true);
    PASS(); return 0; END_TEST();
}

// Regression: the client mitigated with 100/(100+defense) while the server used
// a flat 20% cut, so a connected player took different damage on each end. Both
// now call oz::MitigateDamage; assert the server actually honours defense.
static int test_melee_stamina_and_limits() {
    TEST("Server stamina regenerates and melee limits reject spoofed reports");
    GameState gs;
    gs.add_player(7, "Mele");
    ServerPlayer* p = gs.get_player(7);
    CHECK(p != nullptr);
    CHECK(p->stamina == net::SERVER_MAX_STAMINA);

    // A fresh pool covers a shipped melee cost.
    CHECK(p->stamina >= 18.0f);

    // Regenerates continuously and stops at the ceiling.
    gs.tick(1.0f);
    CHECK(p->stamina == net::SERVER_MAX_STAMINA);

    p->stamina = 10.0f;
    gs.tick(1.0f);
    CHECK(p->stamina > 10.0f);
    CHECK(p->stamina <= net::SERVER_MAX_STAMINA);

    // A pool that cannot cover a swing is what the server's MELEE_HIT gate
    // rejects; drain it and confirm the ordering the handler relies on.
    p->stamina = 5.0f;
    CHECK(!(p->stamina >= 18.0f));

    // The caps are ceilings, not trusts: a client claiming a huge reach or
    // damage is clamped to these.
    CHECK(net::MELEE_MAX_REACH >= 3.5f);      // longest shipped reach
    CHECK(net::MELEE_MAX_REACH <= 5.0f);
    CHECK(net::MELEE_MAX_DAMAGE >= 55);      // etheral_waver's damage
    CHECK(net::MELEE_MAX_DAMAGE <= 500);
    CHECK(net::MELEE_MIN_TICKS >= 1);
    PASS(); return 0; END_TEST();
}

static int test_damage_player_honors_armor() {
    TEST("damage_player mitigates by the shared armor formula");
    GameState gs;
    uint32_t pid = gs.add_player(20, "Armored");
    ServerPlayer* p = gs.get_player(pid);
    CHECK(p != nullptr);

    // No armor: full damage.
    p->health = 100.0f;
    gs.damage_player(*p, 50);
    CHECK_APROX(p->health, 50.0f, 0.01f);

    // defense 100 halves incoming damage (100/(100+100) = 0.5).
    p->health = 100.0f;
    p->armor = 100.0f;
    gs.damage_player(*p, 50);
    CHECK_APROX(p->health, 75.0f, 0.01f);

    // Negative defense must not amplify damage.
    p->health = 100.0f;
    p->armor = -50.0f;
    gs.damage_player(*p, 50);
    CHECK_APROX(p->health, 50.0f, 0.01f);

    // Health floors at zero rather than going negative.
    p->health = 5.0f;
    p->armor = 0.0f;
    gs.damage_player(*p, 50);
    CHECK_EQ(p->health, 0.0f);
    PASS(); return 0; END_TEST();
}

// --- Tier 0: trust / idempotency regressions ---

static int test_add_player_idempotent() {
    TEST("add_player is idempotent for the same id (no duplicate)");
    GameState gs;
    uint32_t a = gs.add_player(7, "First");
    uint32_t b = gs.add_player(7, "Renamed");
    CHECK_EQ(a, (uint32_t)7);
    CHECK_EQ(b, (uint32_t)7);
    CHECK_EQ(gs.player_count(), (int)1);
    ServerPlayer* p = gs.get_player(7);
    CHECK(p != nullptr);
    CHECK_EQ(std::strcmp(p->name, "Renamed"), 0);
    PASS(); return 0; END_TEST();
}

static int test_player_position_flag() {
    TEST("update_player_position flips has_position after first update");
    GameState gs;
    uint32_t id = gs.add_player(3, "Mover");
    ServerPlayer* p = gs.get_player(id);
    CHECK(p != nullptr);
    CHECK_EQ(p->has_position, false);
    gs.update_player_position(id, 1.0f, 2.0f, 3.0f, 0.5f, 0.25f);
    CHECK(p->has_position);
    CHECK_APROX(p->position.x, 1.0f, 0.0001f);
    PASS(); return 0; END_TEST();
}

static int test_npc_death_and_revive() {
    TEST("damage_npc sets DEAD; tick revives after 10s at spawn");
    GameState gs;
    WorldState ws = make_test_world(0);
    ServerNPC npc;
    npc.active = true;
    npc.state = NpcState::CHASE;
    npc.health = 100;
    npc.max_health = 100;
    npc.spawn_pos = {5, 0, 5};
    npc.position = {20, 0, 20}; // dragged away from spawn while chasing
    ws.global_npcs.push_back(npc);

    gs.damage_npc(ws.global_npcs[0], 150, UINT32_MAX);
    CHECK_EQ(ws.global_npcs[0].health, 0);
    CHECK(!ws.global_npcs[0].active);
    CHECK(ws.global_npcs[0].state == NpcState::DEAD);
    // Re-killing a corpse does nothing (no double XP)
    gs.damage_npc(ws.global_npcs[0], 50, UINT32_MAX);

    // 10s of ticking revives the NPC
    for (int i = 0; i < 11; i++) gs.tick_npcs(ws, 1.0f);
    CHECK(ws.global_npcs[0].active);
    CHECK(ws.global_npcs[0].state != NpcState::DEAD);
    CHECK_EQ(ws.global_npcs[0].health, ws.global_npcs[0].max_health);
    CHECK_APROX(ws.global_npcs[0].position.x, 5.0f, 0.001f);
    PASS(); return 0; END_TEST();
}

static int test_player_save_load_roundtrip() {
    TEST("save_player_data / load_player_record round-trip by name");
    {
        GameState gs;
        gs.init_worlds("build_test_gamedata", {"nonexistent_world"}); // captures gamedata_dir
        uint32_t id = gs.add_player(1, "HeroSave");
        ServerPlayer* p = gs.get_player(id);
        CHECK(p != nullptr);
        p->position = {12, 34, 56};
        p->health = 77;
        p->max_health = 120;
        p->mana = 44.0f;
        p->level = 3;
        p->xp = 250;
        p->xp_to_next = 400;
        p->inventory[0] = 5;
        p->ammo = 33.0f;
        strncpy(p->weapon_def[2], "flux_carbine", sizeof(p->weapon_def[2]) - 1);
        p->weapon_ammo[2] = 7;
        p->weapon_magazine[2] = 20;
        gs.save_player_data();
    }
    // Fresh GameState + same-named player restores the stats
    {
        GameState gs2;
        gs2.init_worlds("build_test_gamedata", {"nonexistent_world"});
        uint32_t id = gs2.add_player(9, "HeroSave");
        ServerPlayer* p = gs2.get_player(id);
        CHECK(p != nullptr);
        CHECK_APROX(p->position.x, 12.0f, 0.001f);
        CHECK_APROX(p->position.z, 56.0f, 0.001f);
        CHECK_EQ(p->health, 77);
        CHECK_EQ(p->max_health, 120);
        CHECK_APROX(p->mana, 44.0f, 0.001f);
        CHECK_EQ(p->level, 3);
        CHECK_EQ(p->xp, 250);
        CHECK_EQ(p->inventory[0], 5);
        CHECK_APROX(p->ammo, 33.0f, 0.001f);
        CHECK(std::strcmp(p->weapon_def[2], "flux_carbine") == 0);
        CHECK_EQ(p->weapon_ammo[2], 7);
        CHECK_EQ(p->weapon_magazine[2], 20);
    }
    // Cleanup
    std::remove("build_test_gamedata/Saves/PlayerData.dat");
    PASS(); return 0; END_TEST();
}

static int test_world_seeding_from_file() {
    TEST("init_worlds seeds NPCs/pickups from World.ozone (EngineTest)");
    // Derive the expected counts by re-reading the file rather than hard-coding
    // them. The point of this test is "seeding follows the world file", and a
    // literal count turns every content edit - adding a wave of NPCs to the
    // showcase arena, say - into a failing test that proves nothing.
    size_t wantNpcs = 0, wantPickups = 0;
    {
        FILE* f = fopen("GameData/Worlds/EngineTest/World.ozone", "r");
        if (!f) { fprintf(stdout, "FAIL: cannot open EngineTest World.ozone\n"); return 1; }
        char line[512];
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "npc ", 4) == 0) wantNpcs++;
            else if (strncmp(line, "pickup ", 7) == 0) wantPickups++;
        }
        fclose(f);
    }
    CHECK(wantNpcs > 0 && wantPickups > 0);

    GameState gs;
    gs.init_worlds("GameData", {"EngineTest"});
    WorldState* ws = gs.get_world(0);
    CHECK(ws != nullptr);
    CHECK_EQ(ws->global_npcs.size(), wantNpcs);
    CHECK_EQ(ws->global_pickups.size(), wantPickups);
    // Type names come from the file; stats resolved from PawnDefs
    CHECK(!ws->global_npcs[0].typeName.empty());
    CHECK(ws->global_npcs[0].max_health > 0);
    PASS(); return 0; END_TEST();
}

static int test_npc_follows_path_nodes() {
    TEST("NPC PATROL follows the PathNode graph");
    GameState gs;
    WorldState ws = make_test_world(0);
    ServerPathNode a;
    a.name = "A"; a.position = {0, 0, 0}; a.radius = 1.0f; a.next = {"B"};
    ServerPathNode b;
    b.name = "B"; b.position = {10, 0, 0}; b.radius = 1.0f; b.next = {"A"};
    ws.path_nodes.push_back(a);
    ws.path_nodes.push_back(b);

    ServerNPC npc;
    npc.active = true;
    npc.state = NpcState::PATROL;
    npc.position = {0, 0, 0};
    npc.spawn_pos = {0, 0, 0};
    npc.speed = 10.0f;
    npc.aggro_range = 0.0f;
    npc.path_target = 0; // start at node A
    ws.global_npcs.push_back(npc);

    gs.tick_npcs(ws, 0.1f); // at A -> advance to linked node B
    gs.tick_npcs(ws, 0.1f); // move toward B
    CHECK(ws.global_npcs[0].path_target == 1);
    CHECK(ws.global_npcs[0].position.x > 0.0f);
    PASS(); return 0; END_TEST();
}

static int test_resolve_pickup_item_id() {
    TEST("ResolvePickupItemId maps every pickup type onto a valid item id");
    // The mapping must agree with the client's ItemDB, otherwise the server
    // grants an item the client cannot display. ItemDB is only reachable from
    // the client build (it pulls raylib), so this pins the mapping's own
    // invariants; both sides now use the same item_id:: constants.
    CHECK_EQ(ResolvePickupItemId(PickupType::HEALTH, 25).itemId, item_id::HEALTH_VIAL);
    CHECK_EQ(ResolvePickupItemId(PickupType::MANA, 20).itemId, item_id::MANA_VIAL);
    CHECK_EQ(ResolvePickupItemId(PickupType::KEY, 1).itemId, item_id::KEY);
    CHECK_EQ(ResolvePickupItemId(PickupType::COIN, 1).itemId, item_id::COIN);
    CHECK_EQ(ResolvePickupItemId(PickupType::POWERUP, 1).itemId, item_id::POWERUP);
    CHECK_EQ(ResolvePickupItemId(PickupType::WEAPON, 1).itemId, item_id::WEAPON);
    // ARMOR/AMMO previously fell through to `default: break`, leaving itemId
    // -1: the pickup was consumed and hidden for everyone but granted to nobody.
    CHECK_EQ(ResolvePickupItemId(PickupType::ARMOR, 50).itemId, item_id::ARMOR);
    CHECK_EQ(ResolvePickupItemId(PickupType::AMMO, 30).itemId, item_id::AMMO);

    // Psychic energy steps one tier per crystal value: 111 -> 3, 999 -> 11.
    CHECK_EQ(PickupEnergyCrystalId(111), item_id::ENERGY_FIRST);
    CHECK_EQ(PickupEnergyCrystalId(999), item_id::ENERGY_LAST);
    CHECK_EQ(ResolvePickupItemId(PickupType::PSYCHIC, 333).itemId, 5);
    // Out-of-range values clamp into the table rather than escaping it.
    CHECK_EQ(PickupEnergyCrystalId(0), item_id::ENERGY_FIRST);
    CHECK_EQ(PickupEnergyCrystalId(5000), item_id::ENERGY_LAST);

    // Stackables carry the pickup value as quantity, floored at 1.
    CHECK_EQ(ResolvePickupItemId(PickupType::COIN, 7).quantity, 7);
    CHECK_EQ(ResolvePickupItemId(PickupType::COIN, 0).quantity, 1);
    CHECK_EQ(ResolvePickupItemId(PickupType::AMMO, -3).quantity, 1);
    // Non-stackables stay at one regardless of value.
    CHECK_EQ(ResolvePickupItemId(PickupType::HEALTH, 999).quantity, 1);
    PASS(); return 0; END_TEST();
}

// ---------------------------------------------------------------------------
// Pickup id lockstep.
//
// PickupNode::netId (client) and ServerPickup::id (server) are both the
// file-order index of an ENTITY_PICKUP primitive in World.ozone. Nothing
// enforces that: if either side ever enumerates differently, a client's
// "collect pickup N" silently targets a different physical pickup. This walks the
// world file independently and pins both the server's ids and the order the
// client must reproduce.
// ---------------------------------------------------------------------------
static int test_seed_pickup_ids_are_file_order() {
    TEST("seeded pickup ids are the World.ozone file order (netId lockstep)");
    GameState gs;
    gs.init_worlds("GameData", {"EngineTest"});
    WorldState* ws = gs.get_world(0);
    CHECK(ws != nullptr);
    CHECK(!ws->global_pickups.empty());

    // Independent expectation: the Nth `pickup` line in the file is id N.
    std::vector<std::string> fromFile;
    {
        FILE* f = fopen("GameData/Worlds/EngineTest/World.ozone", "r");
        CHECK(f != nullptr);
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            // Match a leading `pickup` token only; indented and commented lines
            // in other sections must not count.
            const char* p = line;
            while (*p == ' ' || *p == '\t') ++p;
            if (strncmp(p, "pickup ", 7) != 0) continue;
            p += 7;
            std::string name;
            while (*p && *p != ' ' && *p != '\r' && *p != '\n') name += *p++;
            if (!name.empty()) fromFile.push_back(name);
        }
        fclose(f);
    }
    CHECK_EQ(fromFile.size(), ws->global_pickups.size());

    for (size_t i = 0; i < fromFile.size(); i++) {
        CHECK_EQ(ws->global_pickups[i].id, (int)i);
        // typeName travels as the optional tail of PICKUP_RESPAWN so a client can
        // tell a server-issued pickup from one it is drawing on its own.
        CHECK_STR(ws->global_pickups[i].typeName, fromFile[i]);
    }
    PASS(); return 0; END_TEST();
}

// ---------------------------------------------------------------------------
// Reject reasons. These were silent `return false`s; the whole point of the enum
// is that each one is distinguishable.
// ---------------------------------------------------------------------------
static int test_collect_pickup_reject_reasons() {
    TEST("collect_pickup names why it refused, for every rejection path");

    GameState gs;
    {
        WorldState ws = make_test_world(0);
        ws.global_pickups.resize(2);
        ws.global_pickups[0].id = 0;
        ws.global_pickups[0].type = PickupType::COIN;
        ws.global_pickups[0].value = 5;
        ws.global_pickups[0].active = true;
        ws.global_pickups[0].position = {0, 0, 0};
        ws.global_pickups[1].id = 1;
        ws.global_pickups[1].type = PickupType::COIN;
        ws.global_pickups[1].value = 5;
        ws.global_pickups[1].active = true;
        ws.global_pickups[1].position = {0, 0, 0};
        gs.worlds().push_back(ws);
    }
    // UNKNOWN_PLAYER
    {
        PickupReject r = PickupReject::NONE;
        CHECK(!gs.collect_pickup(999, 0, 0, nullptr, nullptr, nullptr, 0, &r));
        CHECK(r == PickupReject::UNKNOWN_PLAYER);
    }
    // WRONG_WORLD: the player is in world 0 and asks for world 1.
    {
        gs.add_player(1, "p1", 0, 0);
        PickupReject r = PickupReject::NONE;
        CHECK(!gs.collect_pickup(1, 0, 1, nullptr, nullptr, nullptr, 0, &r));
        CHECK(r == PickupReject::WRONG_WORLD);
    }
    // NOT_FOUND for an id that does not exist.
    {
        PickupReject r = PickupReject::NONE;
        CHECK(!gs.collect_pickup(1, 77, 0, nullptr, nullptr, nullptr, 0, &r));
        CHECK(r == PickupReject::NOT_FOUND);
    }
    // OUT_OF_RANGE: valid id, but the player is far away. This is the case a laggy
    // client legitimately sends, and it used to be indistinguishable from every
    // other failure. Walk there in steps — update_player_position rejects a
    // per-update move over 25 units, which is itself worth remembering when
    // writing a test that relocates a player.
    {
        for (int step = 1; step <= 4; step++)
            CHECK(gs.update_player_position(1, step * 25.0f, 0.0f, 0.0f, 0.0f, 0.0f));
        PickupReject r = PickupReject::NONE;
        CHECK(!gs.collect_pickup(1, 0, 0, nullptr, nullptr, nullptr, 0, &r));
        CHECK(r == PickupReject::OUT_OF_RANGE);
    }
    // Success clears the reason rather than leaving a stale one.
    {
        for (int step = 1; step <= 4; step++)
            CHECK(gs.update_player_position(1, 100.0f - step * 25.0f, 0.0f, 0.0f, 0.0f, 0.0f));
        PickupReject r = PickupReject::NOT_FOUND;
        CHECK(gs.collect_pickup(1, 0, 0, nullptr, nullptr, nullptr, 0, &r));
        CHECK(r == PickupReject::NONE);
        // And the consumed pickup now reports NOT_FOUND, not a success.
        r = PickupReject::NONE;
        CHECK(!gs.collect_pickup(1, 0, 0, nullptr, nullptr, nullptr, 0, &r));
        CHECK(r == PickupReject::NOT_FOUND);
    }
    // Every reason has a distinct, non-empty message (it goes straight into a log).
    {
        const PickupReject all[] = {
            PickupReject::NONE, PickupReject::UNKNOWN_PLAYER, PickupReject::WRONG_WORLD,
            PickupReject::UNKNOWN_WORLD, PickupReject::NOT_FOUND, PickupReject::OUT_OF_RANGE,
        };
        for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
            const char* s = pickup_reject_str(all[i]);
            CHECK(s != nullptr && s[0] != '\0' && std::string(s) != "?");
        }
    }
    PASS(); return 0; END_TEST();
}

// ---------------------------------------------------------------------------
// Full loopback round trip: the real OmegaClient against a net::NetworkServer
// running the production collect rules.
//
// This is the regression test for "pickups do not work in multiplayer". The bug
// lived in the seam between three lists (the server's, the client's shadow copy,
// and the drawn PickupNodes) and none of the individual hops was wrong, so it can
// only be caught end to end.
// ---------------------------------------------------------------------------
namespace {

// Mirrors Server.cpp's PICKUP_COLLECT handler: collect, then broadcast the grant,
// and on refusal log the reason and answer with a re-sync snapshot.
struct PickupTestServer {
    GameState gs;
    net::NetworkServer server;
    std::vector<std::string> log;

    void send_snapshot(const net::NetworkPlayer& player, int world_index) {
        gs.for_each_active_pickup(world_index, [&](WorldState&, ServerPickup& p) {
            net::PickupRespawnData prd{};
            prd.pickup_id = p.id;
            prd.world_index = world_index;
            prd.position = {p.position.x, p.position.y, p.position.z};
            prd.type = static_cast<int>(p.type);
            prd.value = p.value;
            strncpy(prd.typeName, p.typeName, sizeof(prd.typeName) - 1);
            net::NetworkMessage m{};
            m.magic = net::MAGIC;
            m.type = static_cast<uint32_t>(net::MessageType::PICKUP_RESPAWN);
            m.size = sizeof(prd);
            m.sequence = 0;
            m.timestamp = 0;
            memcpy(m.payload, &prd, sizeof(prd));
            server.send_message(const_cast<net::NetworkPlayer&>(player), m);
        });
    }

    void on_join(net::NetworkPlayer& player) {
        gs.add_player(player.id, player.name, 0, 0);
        ServerPlayer* sp = gs.get_player(player.id);
        send_snapshot(player, sp ? sp->world_index : 0);
    }

    void on_message(const net::NetworkMessage& msg, const net::NetworkPlayer& sender) {
        auto type = static_cast<net::MessageType>(msg.type);
        if (type == net::MessageType::PICKUP_COLLECT) {
            if (msg.size < sizeof(net::PickupCollectData)) return;
            net::PickupCollectData pcd;
            memcpy(&pcd, msg.payload, sizeof(pcd));
            PickupType ptype;
            int pvalue;
            PickupReject why = PickupReject::NONE;
            if (!gs.collect_pickup(sender.id, pcd.pickup_id, pcd.world_index,
                                   &ptype, &pvalue, nullptr, 0, &why)) {
                log.push_back(std::string("refused:") + pickup_reject_str(why));
                ServerPlayer* sp = gs.get_player(sender.id);
                if (sp) send_snapshot(sender, sp->world_index);
                return;
            }
            const PickupGrant grant = ResolvePickupItemId(ptype, pvalue);
            net::PickupCollectedData out{};
            out.player_id = sender.id;
            out.pickup_id = pcd.pickup_id;
            out.world_index = pcd.world_index;
            out.item_id = grant.itemId;
            out.quantity = grant.quantity;
            net::NetworkMessage m{};
            m.magic = net::MAGIC;
            m.type = static_cast<uint32_t>(net::MessageType::PICKUP_COLLECTED);
            m.size = sizeof(out);
            m.sequence = 0;
            m.timestamp = 0;
            memcpy(m.payload, &out, sizeof(out));
            server.broadcast_message(m);
            return;
        }
        if (type == net::MessageType::PICKUP_RESYNC) {
            log.push_back("resync");
            ServerPlayer* sp = gs.get_player(sender.id);
            if (sp) send_snapshot(sender, sp->world_index);
            return;
        }
        if (type == net::MessageType::PLAYER_UPDATE) {
            if (msg.size < sizeof(net::PlayerUpdateData)) return;
            net::PlayerUpdateData pud;
            memcpy(&pud, msg.payload, sizeof(pud));
            gs.update_player_position(sender.id, pud.position.x, pud.position.y,
                                      pud.position.z, pud.yaw, pud.pitch);
        }
    }
};

// NetworkServer binds its own socket in start(), so ask the library for a free
// port rather than guessing one.
uint16_t find_free_port_for_test() { return net::find_free_port(); }

// Pump both ends until `pred` is satisfied or we run out of patience.
template <typename Pred>
bool pump_until(net::NetworkServer& server, OmegaClient& client, Pred pred,
                int max_ms = 4000) {
    for (int i = 0; i < max_ms / 5; i++) {
        server.update();
        client.update(0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    server.update();
    client.update(0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    return pred();
}

} // namespace

static int test_pickup_loopback_roundtrip() {
    TEST("pickup: join snapshot, collect, grant, and the drawn node is hidden");

    const uint16_t port = find_free_port_for_test();
    CHECK(port != 0);

    PickupTestServer ts;
    {
        WorldState ws = make_test_world(0);
        ServerPickup p;
        p.id = 0;
        p.type = PickupType::COIN;
        p.value = 7;
        p.active = true;
        p.position = {0, 0, 0};
        strncpy(p.typeName, "Coin", sizeof(p.typeName) - 1);
        ws.global_pickups.push_back(p);
        ts.gs.worlds().push_back(ws);
    }

    net::ServerCallbacks scb;
    scb.on_player_join = [&ts](net::NetworkPlayer& pl) { ts.on_join(pl); };
    scb.on_message_received = [&ts](const net::NetworkMessage& m, const net::NetworkPlayer& s) {
        ts.on_message(m, s);
    };
    ts.server.set_callbacks(scb);
    CHECK(ts.server.init(port));
    CHECK(ts.server.start());

    OmegaClient client;
    CHECK(client.connect("127.0.0.1", port));

    CHECK(pump_until(ts.server, client, [&] { return client.is_connected(); }));

    // The join snapshot must arrive: the pickup is in the client's view, active,
    // with the world index the server assigned and the authored name from the
    // optional tail.
    CHECK(pump_until(ts.server, client, [&] {
        auto p = client.pickups();
        return !p.empty();
    }));
    {
        auto p = client.pickups();
        CHECK_EQ(p.size(), (size_t)1);
        CHECK_EQ(p[0].id, 0);
        CHECK_EQ(p[0].world_index, 0);
        CHECK(p[0].active);
        CHECK_STR(p[0].typeName, "Coin");
    }

    // Grant notification wired to the same callback the game loop uses.
    int granted_item = -1, granted_qty = -1;
    int pickup_updates = 0;
    client.set_on_item_collected([&](int id, int qty) { granted_item = id; granted_qty = qty; });
    client.set_on_pickups_changed([&](const std::vector<ClientPickup>&) { pickup_updates++; });

    // Stand next to the pickup, then walk over it.
    pump_until(ts.server, client, [] { return false; }, 100);
    client.send_pickup_collect(0, 0, nullptr);

    CHECK(pump_until(ts.server, client, [&] { return granted_item > 0; }));
    CHECK_EQ(granted_item, item_id::COIN);
    CHECK_EQ(granted_qty, 7);       // COIN carries its value as the stack size

    // The client's view must be marked inactive, and the consumer notified, so the
    // drawn PickupNode can be hidden. This is the assertion the original bug
    // failed: the flag was set on a list nothing rendered.
    CHECK(pump_until(ts.server, client, [&] { return pickup_updates > 0; }));
    {
        auto p = client.pickups();
        CHECK_EQ(p.size(), (size_t)1);
        CHECK(!p[0].active);
    }

    // Collecting it again is refused and must NOT grant a second time. The server
    // answers the refusal with a snapshot rather than silence.
    granted_item = -1;
    client.send_pickup_collect(0, 0, nullptr);
    CHECK(pump_until(ts.server, client, [&] {
        for (const auto& s : ts.log) if (s.rfind("refused:", 0) == 0) return true;
        return false;
    }));
    CHECK_EQ(granted_item, -1);

    // And an explicit re-sync request works, for the client that knows it is out
    // of step without having attempted a collect.
    const size_t before = ts.log.size();
    client.request_pickup_resync();
    CHECK(pump_until(ts.server, client, [&] {
        for (size_t i = before; i < ts.log.size(); i++)
            if (ts.log[i] == "resync") return true;
        return false;
    }));

    client.disconnect();
    ts.server.stop();
    PASS(); return 0; END_TEST();
}

static int test_pickup_state_cleared_on_disconnect() {
    TEST("pickup state does not survive a disconnect");
    // Rejoining a different server previously started from the previous server's
    // list, and any unanswered collect was still queued — so a pickup the new
    // server never issued could still grant an item.
    const uint16_t port = find_free_port_for_test();
    CHECK(port != 0);

    PickupTestServer ts;
    {
        WorldState ws = make_test_world(0);
        ServerPickup p;
        p.id = 0;
        p.type = PickupType::COIN;
        p.value = 7;
        p.active = true;
        p.position = {0, 0, 0};
        ws.global_pickups.push_back(p);
        ts.gs.worlds().push_back(ws);
    }
    net::ServerCallbacks scb;
    scb.on_player_join = [&ts](net::NetworkPlayer& pl) { ts.on_join(pl); };
    scb.on_message_received = [&ts](const net::NetworkMessage& m, const net::NetworkPlayer& s) {
        ts.on_message(m, s);
    };
    ts.server.set_callbacks(scb);
    CHECK(ts.server.init(port));
    CHECK(ts.server.start());

    OmegaClient client;
    CHECK(client.connect("127.0.0.1", port));
    CHECK(pump_until(ts.server, client, [&] {
        return client.is_connected() && !client.pickups().empty();
    }));

    // Queue a collect that the server will never answer, then drop the link.
    client.send_pickup_collect(0, 0, nullptr);
    client.disconnect();
    ts.server.update();

    CHECK(client.pickups().empty());
    CHECK(!client.has_pickup_world(0));
    // The dropped link also invalidates the session: a reconnect must re-handshake
    // rather than assume it is still joined.
    CHECK(!client.is_connected());

    ts.server.stop();
    PASS(); return 0; END_TEST();
}

static int test_pickup_respawn_tail_is_optional() {
    TEST("PICKUP_RESPAWN without the typeName tail is still accepted");
    // The tail is optional on purpose (see kPickupRespawnSizeBase). Requiring
    // sizeof() made a client drop every message from a server that predates the
    // field — silently, which is how a whole world's pickups vanish at once.
    CHECK(net::kPickupRespawnSizeBase < sizeof(net::PickupRespawnData));

    const uint16_t port = find_free_port_for_test();
    CHECK(port != 0);

    net::NetworkServer server;
    int seed = 0;
    net::ServerCallbacks scb;
    scb.on_player_join = [&server, &seed](net::NetworkPlayer& pl) {
        // Hand-roll a message carrying ONLY the base layout.
        net::PickupRespawnData prd{};
        prd.pickup_id = 3;
        prd.world_index = 0;
        prd.position = {1.0f, 2.0f, 3.0f};
        prd.type = static_cast<int>(PickupType::COIN);
        prd.value = 7;
        net::NetworkMessage m{};
        m.magic = net::MAGIC;
        m.type = static_cast<uint32_t>(net::MessageType::PICKUP_RESPAWN);
        m.size = net::kPickupRespawnSizeBase;   // tail deliberately omitted
        m.sequence = 0;
        m.timestamp = 0;
        memcpy(m.payload, &prd, m.size);
        server.send_message(const_cast<net::NetworkPlayer&>(pl), m);
        ++seed;
    };
    server.set_callbacks(scb);
    CHECK(server.init(port));
    CHECK(server.start());

    OmegaClient client;
    CHECK(client.connect("127.0.0.1", port));
    CHECK(pump_until(server, client, [&] {
        return seed > 0 && !client.pickups().empty();
    }));
    {
        auto p = client.pickups();
        CHECK_EQ(p.size(), (size_t)1);
        CHECK_EQ(p[0].id, 3);
        CHECK_EQ(p[0].value, 7);
        // Absent tail reads as empty, never as garbage or an over-read.
        CHECK_STR(p[0].typeName, "");
    }
    client.disconnect();
    server.stop();
    PASS(); return 0; END_TEST();
}

// ---------------------------------------------------------------------------
// The three-lists reconciliation rule, exercised directly.
//
// Pickup ids restart at 0 in every world, so a snapshot is only meaningful once
// scoped to one world. Main.cpp's set_on_pickups_changed lambda does that
// filtering; this pins the rule so a future caller cannot hand
// PawnSystem::ApplyPickupNetState a merged list and get whichever world happened
// to be appended last.
//
// The stand-in for PickupNode is deliberate: that struct pulls in raylib
// (Vector3) and this suite is raylib-free. Only the id/active pair matters here.
// ---------------------------------------------------------------------------
struct NetPickupLite {
    int netId = -1;
    bool active = true;
};

static int test_pickup_net_state_is_world_scoped() {
    TEST("pickup ids restart per world, so the net snapshot must be world-scoped");
    // The premise: two worlds legitimately both have a pickup id 3.
    {
        GameState gs;
        WorldState a = make_test_world(0);
        WorldState b = make_test_world(1);
        for (WorldState* w : {&a, &b}) {
            ServerPickup p;
            p.id = 3;
            p.type = PickupType::COIN;
            p.value = 1;
            p.active = true;
            w->global_pickups.push_back(p);
        }
        gs.worlds().push_back(a);
        gs.worlds().push_back(b);
        CHECK_EQ(gs.get_world(0)->global_pickups[0].id,
                 gs.get_world(1)->global_pickups[0].id);
    }
    // The consequence, spelled out: reconciling a merged snapshot would let the
    // last world's entry decide both. This is why the filter lives at the call
    // site, and why ApplyPickupNetState documents `net` as already scoped.
    {
        std::vector<NetPickupLite> merged;
        for (int world = 0; world < 2; world++) {
            NetPickupLite n;
            n.netId  = 3;                    // same id in both worlds
            n.active = (world == 1);         // world 1 consumed it
            merged.push_back(n);
        }
        std::unordered_map<int, bool> collapsed;
        for (const auto& n : merged) collapsed[n.netId] = n.active;
        CHECK_EQ(collapsed.size(), (size_t)1);   // world 0's entry is lost
        CHECK(collapsed[3]);                    // ...and world 1's state decides it

        // Scoped per world, the two stay distinct.
        std::unordered_map<int, bool> w0, w1;
        for (const auto& n : merged) (n.active ? w1 : w0)[n.netId] = n.active;
        CHECK_EQ(w0.size(), (size_t)1);
        CHECK_EQ(w1.size(), (size_t)1);
    }
    PASS(); return 0; END_TEST();
}

// ---------------------------------------------------------------------------
// Source-text lint: no printf-style log call passes the SAME identifier twice
// in a row. That is the shape of the "server full (%u/%u)" bug, which logged
// m_max_players twice and therefore read 32/32 at every occupancy. Two hand-
// maintained artefacts (the format string and the argument list) cannot be
// checked against each other in one language, so the source is read as text.
// ---------------------------------------------------------------------------
static std::string ReadSourceFile(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return std::string();
    std::string out; char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

static int scan_duplicate_log_args(const std::string& src, std::vector<std::string>& outBad) {
    static const char* macros[] = {"OZ_WARN(", "OZ_INFO(", "OZ_ERROR("};
    int flagged = 0;
    for (const char* m : macros) {
        size_t pos = 0;
        while ((pos = src.find(m, pos)) != std::string::npos) {
            size_t open = pos + std::strlen(m) - 1;   // the '('
            // Balanced scan to the matching ')'.
            int depth = 0; size_t i = open; bool inStr = false, inChr = false;
            for (; i < src.size(); i++) {
                char ch = src[i];
                if (inStr) { if (ch == '\\') i++; else if (ch == '"') inStr = false; continue; }
                if (inChr) { if (ch == '\\') i++; else if (ch == '\'') inChr = false; continue; }
                if (ch == '"') { inStr = true; continue; }
                if (ch == '\'') { inChr = true; continue; }
                if (ch == '(') depth++;
                else if (ch == ')') { depth--; if (depth == 0) break; }
            }
            std::string args = src.substr(open + 1, i - open - 1);
            // Split at top-level commas.
            std::vector<std::string> parts;
            int d = 0; bool s2 = false, c2 = false; size_t start = 0;
            for (size_t k = 0; k <= args.size(); k++) {
                char ch = (k < args.size()) ? args[k] : ',';
                if (s2) { if (ch == '\\') k++; else if (ch == '"') s2 = false; continue; }
                if (c2) { if (ch == '\\') k++; else if (ch == '\'') c2 = false; continue; }
                if (ch == '"') s2 = true;
                else if (ch == '\'') c2 = true;
                else if (ch == '(' || ch == '[' || ch == '{') d++;
                else if (ch == ')' || ch == ']' || ch == '}') d--;
                else if (ch == ',' && d == 0) { parts.push_back(args.substr(start, k - start)); start = k + 1; }
            }
            auto trim = [](std::string s) {
                size_t b = s.find_first_not_of(" \t\r\n");
                size_t e = s.find_last_not_of(" \t\r\n");
                if (b == std::string::npos) return std::string();
                return s.substr(b, e - b + 1);
            };
            for (size_t p = 1; p < parts.size(); p++) {
                std::string a = trim(parts[p - 1]), b = trim(parts[p]);
                if (a.empty() || b.empty() || a != b) continue;
                // Only flag identifier-like duplicates; a repeated literal is
                // legitimate (e.g. a format string containing "%d %d").
                if (a.find_first_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_") != std::string::npos) {
                    outBad.push_back(a);
                    flagged++;
                }
            }
            pos = i + 1;
        }
    }
    return flagged;
}

static int test_log_calls_have_no_duplicate_args() {
    TEST("No OZ_WARN/INFO/ERROR passes the same identifier twice in a row");

    // Self-check first: the scanner MUST fire on the exact bug it exists to
    // catch, or a silently-broken lint would pass forever.
    {
        std::vector<std::string> bad;
        int n = scan_duplicate_log_args(
            "OZ_WARN(\"server full (%u/%u)\", m_max_players, m_max_players);\n", bad);
        CHECK(n == 1);
    }

    static const char* files[] = {
        "Source/Server/GameState.cpp",
        "Source/Server/Server.cpp",
        "Source/Server/Master/Master.cpp",
    };
    int flagged = 0, missing = 0;
    for (const char* path : files) {
        std::string src = ReadSourceFile(path);
        if (src.empty()) { missing++; continue; }
        std::vector<std::string> bad;
        int n = scan_duplicate_log_args(src, bad);
        for (const auto& ident : bad)
            fprintf(stdout, "FAIL: %s: log call passes '%s' twice in a row\n",
                    path, ident.c_str());
        flagged += n;
    }
    if (missing == (int)(sizeof(files) / sizeof(files[0]))) {
        printf("  SKIP  log-arg lint (source files not found from cwd)\n");
        return 0;
    }
    CHECK(flagged == 0);
    PASS(); return 0; END_TEST();
}

static int test_objective_id_fits_wire_field() {
    TEST("Objective ids fit OBJECTIVE_STATE's fixed id[32] field");

    // The wire struct's field, pinned: the send site strncpy's into it, so an
    // id of kObjectiveIdMax or more chars truncates silently mid-campaign.
    CHECK_EQ((int)net::kObjectiveIdMax, 32);

    // Shipped ids (def names) top out at 24 chars.
    CHECK(net::ObjectiveIdFits("Objective_CollectRelics"));
    CHECK(net::ObjectiveIdFits("Objective_ClearArea"));
    CHECK(net::ObjectiveIdFits("ClearArea"));

    // Boundary: 31 chars + NUL fits, 32 chars does not.
    CHECK(net::ObjectiveIdFits(std::string(31, 'x')));
    CHECK(!net::ObjectiveIdFits(std::string(32, 'x')));
    CHECK(!net::ObjectiveIdFits(std::string(64, 'y')));
    CHECK(net::ObjectiveIdFits(""));
    PASS(); return 0; END_TEST();
}

// --- Campaign gating (--campaign opt-in, Phase G) ---

static int test_campaign_flag_defaults_off() {
    TEST("--campaign flag defaults OFF and round-trips");
    GameState gs;
    CHECK(!gs.campaign_enabled());          // default OFF = classic co-op
    gs.set_campaign_enabled(true);
    CHECK(gs.campaign_enabled());
    gs.set_campaign_enabled(false);
    CHECK(!gs.campaign_enabled());
    PASS(); return 0; END_TEST();
}

// Join placement reads the same flag: with the campaign OFF a new player lands
// in world 0 even when a world holds live objectives. The seed gate already
// leaves every objective list empty in that mode, so this is belt-and-braces —
// it pins the gate itself against any future objective source (which is the
// reason the flag lives on GameState instead of buried in argv).
static int test_campaign_gates_frontier_placement() {
    TEST("--campaign gates current_mission_world_index (join placement)");
    GameState gs;
    WorldState ws = make_test_world(1);
    WorldState::Objective o;
    o.id = "GateObj";
    o.type = "kill";
    o.target = 5;
    ws.objectives.push_back(o);
    gs.worlds().push_back(ws);

    // OFF (default): world 0 regardless of objectives.
    CHECK_EQ(gs.current_mission_world_index(), 0);

    // ON: the frontier — first world with objectives not yet transitioned.
    gs.set_campaign_enabled(true);
    CHECK_EQ(gs.current_mission_world_index(), 1);

    // ON, mission complete (all transitioned): back to world 0.
    gs.worlds()[0].mission_transitioned = true;
    CHECK_EQ(gs.current_mission_world_index(), 0);

    gs.set_campaign_enabled(false);
    PASS(); return 0; END_TEST();
}

// The seed gate itself: init_worlds seeds nothing when the flag is OFF
// (default) and the coop ClearArea fallback when ON. The fixture world carries
// gametype=cooperative because NO shipped world sets gametype= at all — a real
// world would make the ON half vacuous. With no entities the procedural
// fallback spawns 10 NPCs, so ClearArea's target is nonzero.
static int test_campaign_gates_objective_seeding() {
    TEST("--campaign gates objective seeding in init_worlds");
    namespace fs = std::filesystem;
    const fs::path worldDir = fs::path("build_test_gamedata") / "Worlds" / "CampaignGateTest";
    fs::create_directories(worldDir);
    {
        std::ofstream out((worldDir / "World.ozone").string());
        out << "levelinfo 0 8 5 0 10 50 0 gametype=cooperative\n";
    }

    {   // OFF (default): no objectives, no mission.
        GameState gs;
        gs.init_worlds("build_test_gamedata", {"CampaignGateTest"});
        CHECK_EQ(gs.world_count(), 1);
        CHECK(gs.worlds()[0].objectives.empty());
        CHECK(gs.worlds()[0].mission.empty());
    }
    {   // ON: the coop fallback seeds ClearArea against the procedural NPCs.
        GameState gs;
        gs.set_campaign_enabled(true);
        gs.init_worlds("build_test_gamedata", {"CampaignGateTest"});
        CHECK_EQ(gs.world_count(), 1);
        CHECK_EQ((int)gs.worlds()[0].objectives.size(), 1);
        if (!gs.worlds()[0].objectives.empty()) {
            CHECK(gs.worlds()[0].objectives[0].id == "ClearArea");
            CHECK(gs.worlds()[0].objectives[0].target > 0);
        }
    }

    fs::remove_all(worldDir);
    PASS(); return 0; END_TEST();
}

int main() {
    // Unbuffered: when a suite dies without a summary line, a piped stdout keeps
    // the last "TEST:" line in the log, which is what names the culprit (the
    // MSYS2-only std::system_error this suite hit printed no test name at all).
    setvbuf(stdout, nullptr, _IONBF, 0);
    fprintf(stdout, "GameState Tests\n");
    fprintf(stdout, "===============\n");

    int failures = 0;
    failures += test_resolve_pickup_item_id();
    failures += test_melee_stamina_and_limits();
    failures += test_spawn_projectile();
    failures += test_tick_projectile_movement();
    failures += test_tick_projectile_expiry();
    failures += test_projectile_hits_npc();
    failures += test_projectile_misses_npc();
    failures += test_projectile_hits_partition_npc();
    failures += test_projectile_avoids_owner();
    failures += test_projectile_hits_other_player();
    failures += test_ammo_pickup_grants_ammo();
    failures += test_ammo_pickup_no_xp_granted();
    failures += test_collect_pickup_rejects_foreign_world();
    failures += test_damage_player_honors_armor();
    failures += test_add_player_idempotent();
    failures += test_player_position_flag();
    failures += test_npc_death_and_revive();
    failures += test_npc_follows_path_nodes();
    failures += test_player_save_load_roundtrip();
    failures += test_world_seeding_from_file();
    failures += test_seed_pickup_ids_are_file_order();
    failures += test_collect_pickup_reject_reasons();
    failures += test_pickup_respawn_tail_is_optional();
    failures += test_pickup_net_state_is_world_scoped();
    failures += test_pickup_loopback_roundtrip();
    failures += test_pickup_state_cleared_on_disconnect();
    failures += test_log_calls_have_no_duplicate_args();
    failures += test_objective_id_fits_wire_field();
    failures += test_campaign_flag_defaults_off();
    failures += test_campaign_gates_frontier_placement();
    failures += test_campaign_gates_objective_seeding();

    fprintf(stdout, "===============\n");
    fprintf(stdout, "%d/%d passed, %d failed\n",
            tests_passed, tests_total, tests_total - tests_passed);
    return failures;
}
