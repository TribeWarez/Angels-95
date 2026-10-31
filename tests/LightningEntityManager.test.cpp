// LightningEntityManager unit test — lifecycle and hotbar operations.
// Requires raylib for resource loading. Compile with client compiler.
//
// g++ -O0 -g --std=c++20 -I Source \
//   Source/Script/LightningEntityManager.cpp \
//   Source/Script/LightningEntityRegistry.cpp \
//   Source/Script/LightningScriptContext.cpp \
//   Source/Script/LightningScriptParser.cpp \
//   Source/Log.cpp \
//   tests/LightningEntityManager.test.cpp \
//   -o test_entity_manager -lraylib -lopengl32 -lgdi32 -lwinmm -lws2_32 -lm

#include "../Source/Script/LightningEntityManager.hpp"
#include "../Source/Script/LightningEntityRegistry.hpp"
#include "../Source/Script/LightningScriptParser.hpp"
#include "../Source/Events/EventBus.hpp"
#include <cstdio>
#include <cassert>

static int tests_total = 0, tests_passed = 0;
#define TEST(name) do { tests_total++; fprintf(stdout, "  TEST: %s ... ", name);
#define PASS() do { tests_passed++; fprintf(stdout, "PASS\n"); } while(0)
#define FAIL(msg) do { fprintf(stdout, "FAIL: %s\n", msg); return 1; } while(0)
#define CHECK(cond) do { if (!(cond)) { fprintf(stdout, "FAIL: %s\n", #cond); return 1; } } while(0)
#define CHECK_EQ(a, b) do { if ((a) != (b)) { fprintf(stdout, "FAIL: expected %d, got %d\n", (int)(a), (int)(b)); return 1; } } while(0)
#define CHECK_APROX(a, b, eps) do { float _d = (a) - (b); if (_d < 0) _d = -_d; \
    if (_d > (eps)) { fprintf(stdout, "FAIL: expected %f, got %f\n", (float)(b), (float)(a)); return 1; } } while(0)
#define END_TEST() } while(0)

static int test_init() {
    TEST("Init resets state");
    auto& em = LightningEntityManager::Instance();
    em.Init();
    CHECK_EQ(em.Count(), 0);
    CHECK_EQ(em.SelectedSlot(), 0);
    PASS(); return 0; END_TEST();
}

static int test_hotbar() {
    TEST("Hotbar assign and select");
    auto& em = LightningEntityManager::Instance();
    em.Init();
    em.HotbarAssign(0, 5);
    CHECK_EQ(em.HotbarAt(0), 5);
    em.HotbarSwap(0, 1);
    CHECK_EQ(em.HotbarAt(0), -1);
    CHECK_EQ(em.HotbarAt(1), 5);
    em.SelectSlot(3);
    CHECK_EQ(em.SelectedSlot(), 3);
    PASS(); return 0; END_TEST();
}

static int test_spawn_unknown() {
    TEST("Spawn unknown def returns -1");
    auto& em = LightningEntityManager::Instance();
    em.Init();
    int idx = em.Spawn("NonExistentDef");
    CHECK_EQ(idx, -1);
    CHECK_EQ(em.Count(), 0);
    PASS(); return 0; END_TEST();
}

static int test_hotbar_out_of_range() {
    TEST("HotbarAssign with invalid slot is ignored");
    auto& em = LightningEntityManager::Instance();
    em.Init();
    em.HotbarAssign(-1, 99);
    em.HotbarAssign(8, 99);
    // All hotbar slots should still be -1 since init
    for (int s = 0; s < 8; s++) CHECK_EQ(em.HotbarAt(s), -1);
    PASS(); return 0; END_TEST();
}

static int test_select_slot_out_of_range() {
    TEST("SelectSlot with invalid slot is ignored");
    auto& em = LightningEntityManager::Instance();
    em.Init();
    em.SelectSlot(-1);
    CHECK_EQ(em.SelectedSlot(), 0);
    em.SelectSlot(99);
    CHECK_EQ(em.SelectedSlot(), 0);
    PASS(); return 0; END_TEST();
}

// Mouse-wheel cycling skips empty slots and wraps; with an empty bar it must
// not spin or leave the selection somewhere arbitrary.
static int test_wheel_cycle_skips_empty() {
    TEST("SelectSlotSkippingEmpty skips empties and wraps");
    auto& em = LightningEntityManager::Instance();
    em.Init();
    em.HotbarAssign(1, 11);
    em.HotbarAssign(4, 14);
    em.SelectSlot(1);

    em.SelectSlotSkippingEmpty(1, +1);   // 2,3 empty -> lands on 4
    CHECK_EQ(em.SelectedSlot(), 4);
    em.SelectSlotSkippingEmpty(4, +1);   // 5,6,7,0 empty -> wraps to 1
    CHECK_EQ(em.SelectedSlot(), 1);
    em.SelectSlotSkippingEmpty(1, -1);   // back down to 4
    CHECK_EQ(em.SelectedSlot(), 4);
    PASS(); return 0; END_TEST();
}

static int test_wheel_cycle_all_empty() {
    TEST("SelectSlotSkippingEmpty is a no-op on an empty hotbar");
    auto& em = LightningEntityManager::Instance();
    em.Init();
    em.SelectSlot(3);
    em.SelectSlotSkippingEmpty(3, +1);
    CHECK_EQ(em.SelectedSlot(), 3);
    em.SelectSlotSkippingEmpty(3, -1);
    CHECK_EQ(em.SelectedSlot(), 3);
    // dir 0 must do nothing at all.
    em.SelectSlotSkippingEmpty(3, 0);
    CHECK_EQ(em.SelectedSlot(), 3);
    PASS(); return 0; END_TEST();
}

static int test_selected_entity_empty() {
    TEST("SelectedEntity returns null when hotbar empty");
    auto& em = LightningEntityManager::Instance();
    em.Init();
    CHECK(em.SelectedEntity() == nullptr);
    PASS(); return 0; END_TEST();
}

static int test_equipment_assign() {
    TEST("Equipment assign and query");
    auto& em = LightningEntityManager::Instance();
    em.Init();
    // Assign a dummy index (no real entity, just slot tracking)
    em.EquipmentAssign(0, 42);
    CHECK_EQ(em.EquipmentAt(0), 42);
    PASS(); return 0; END_TEST();
}

static int test_equipment_find_free() {
    TEST("EquipmentFindFreeSlot returns first empty");
    auto& em = LightningEntityManager::Instance();
    em.Init();
    CHECK_EQ(em.EquipmentFindFreeSlot(), 0);
    em.EquipmentAssign(0, 1);
    CHECK_EQ(em.EquipmentFindFreeSlot(), 1);
    for (int s = 0; s < 8; s++) em.EquipmentAssign(s, s);
    CHECK_EQ(em.EquipmentFindFreeSlot(), -1); // all full
    PASS(); return 0; END_TEST();
}

static int test_equipment_clear() {
    TEST("EquipmentClear empties all slots");
    auto& em = LightningEntityManager::Instance();
    em.Init();
    for (int s = 0; s < 8; s++) em.EquipmentAssign(s, s);
    em.EquipmentClear();
    for (int s = 0; s < 8; s++) CHECK_EQ(em.EquipmentAt(s), -1);
    PASS(); return 0; END_TEST();
}

// Drag-and-drop between equipment rows. EquipmentSwap reads both occupants out
// before writing either, because EquipmentAssign despawns whatever it replaces:
// two back-to-back Assign calls would have the second despawn the piece the
// first had just moved in, so dragging a helmet onto a full ring lost one.
static int test_equipment_swap() {
    TEST("EquipmentSwap exchanges occupants and loses neither");
    auto& reg = LightningEntityRegistry::Instance();
    EntityDef helm, ring;
    helm.name = "SwapHelm"; helm.type = EntityType::ARMOR;
    helm.stats.strings["equip_slot"] = "helmet";
    helm.stats.floats["defense"] = 3.0f;
    ring.name = "SwapRing"; ring.type = EntityType::ARMOR;
    ring.stats.strings["equip_slot"] = "jewelry1";
    ring.stats.floats["defense"] = 1.0f;
    reg.Register(helm);
    reg.Register(ring);

    auto& em = LightningEntityManager::Instance();
    em.Init();
    em.EquipmentClear();

    // Real instances, so "neither was despawned" is a real assertion rather than
    // a lookup of an index that never existed. Indices are re-read AFTER each
    // assign: EquipmentAssign despawns the slot's previous occupant and Despawn
    // shifts m_instances swap-with-last, so an index captured before an assign is
    // not guaranteed to name the same instance after it.
    const int idxHelm = em.Spawn("SwapHelm");
    CHECK(idxHelm >= 0);
    em.EquipmentAssign(static_cast<int>(EquipSlotType::HELMET), idxHelm);
    const int helmAt = em.EquipmentAt(static_cast<int>(EquipSlotType::HELMET));

    const int idxRing = em.Spawn("SwapRing");
    CHECK(idxRing >= 0);
    em.EquipmentAssign(static_cast<int>(EquipSlotType::JEWELRY1), idxRing);
    const int ringAt = em.EquipmentAt(static_cast<int>(EquipSlotType::JEWELRY1));
    const int slotHelm = static_cast<int>(EquipSlotType::HELMET);
    const int slotRing = static_cast<int>(EquipSlotType::JEWELRY1);

    em.EquipmentSwap(slotHelm, slotRing);
    CHECK_EQ(em.EquipmentAt(slotHelm), ringAt);
    CHECK_EQ(em.EquipmentAt(slotRing), helmAt);
    // Both occupants still resolvable: neither was despawned by the other half.
    CHECK(em.Get(ringAt) != nullptr);
    CHECK(em.Get(helmAt) != nullptr);

    // Same slot and out-of-range are no-ops (must not clear the slot). Checked
    // HERE, while slotRing still holds ringAt from the swap above.
    em.EquipmentSwap(slotRing, slotRing);
    CHECK_EQ(em.EquipmentAt(slotRing), helmAt);
    em.EquipmentSwap(-1, slotRing);
    CHECK_EQ(em.EquipmentAt(slotRing), helmAt);
    em.EquipmentSwap(slotRing, 999);
    CHECK_EQ(em.EquipmentAt(slotRing), helmAt);

    // Swapping onto an empty slot moves; the vacated slot reads empty.
    em.EquipmentClear();
    const int idxBoots = em.Spawn("SwapRing");
    CHECK(idxBoots >= 0);
    em.EquipmentAssign(5, idxBoots);
    const int captured = em.EquipmentAt(5);
    em.EquipmentSwap(5, 6);
    CHECK_EQ(em.EquipmentAt(5), -1);
    CHECK_EQ(em.EquipmentAt(6), captured);
    em.EquipmentClear();
    PASS(); return 0; END_TEST();
}

static int test_equipment_out_of_range() {
    TEST("Equipment access with invalid slot returns -1");
    auto& em = LightningEntityManager::Instance();
    em.Init();
    CHECK_EQ(em.EquipmentAt(-1), -1);
    CHECK_EQ(em.EquipmentAt(8), -1);
    em.EquipmentAssign(-1, 99); // should be ignored
    em.EquipmentAssign(8, 99); // should be ignored
    PASS(); return 0; END_TEST();
}

static int test_serialize_format() {
    TEST("SerializeState returns valid format");
    auto& em = LightningEntityManager::Instance();
    em.Init();
    std::string s = em.SerializeState();
    // Format: "hotbar:...|equip:...|"
    CHECK(s.find("hotbar:") == 0 || s.find("hotbar:") != std::string::npos);
    CHECK(s.find("|equip:") != std::string::npos);
    PASS(); return 0; END_TEST();
}

static int test_deserialize_empty() {
    TEST("DeserializeState with empty string resets slots");
    auto& em = LightningEntityManager::Instance();
    em.Init();
    em.HotbarAssign(2, 5);
    CHECK(em.DeserializeState(""));
    CHECK_EQ(em.HotbarAt(2), -1); // should be reset
    PASS(); return 0; END_TEST();
}

static int test_deserialize_roundtrip() {
    TEST("Serialize then Deserialize restores state");
    auto& reg = LightningEntityRegistry::Instance();
    EntityDef defA, defB, defC;
    defA.name = "ser_a"; defA.type = EntityType::PICKUP;
    defB.name = "ser_b"; defB.type = EntityType::PICKUP;
    defC.name = "ser_c"; defC.type = EntityType::PICKUP;
    reg.Register(defA);
    reg.Register(defB);
    reg.Register(defC);

    auto& em = LightningEntityManager::Instance();
    em.Init();
    int idxA = em.Spawn("ser_a");
    int idxB = em.Spawn("ser_b");
    int idxC = em.Spawn("ser_c");
    CHECK(idxA >= 0);
    CHECK(idxB >= 0);
    CHECK(idxC >= 0);
    em.HotbarAssign(0, idxA);
    em.HotbarAssign(3, idxB);
    em.SelectSlot(3);
    em.EquipmentAssign(1, idxC);

    std::string saved = em.SerializeState();
    em.Init();
    reg.Register(defA);
    reg.Register(defB);
    reg.Register(defC);
    CHECK(em.DeserializeState(saved));
    // No Player entity in test env, so ser_a→idx0, ser_b→idx1, ser_c→idx2
    CHECK_EQ(em.HotbarAt(0), 0);
    CHECK_EQ(em.HotbarAt(3), 1);
    CHECK_EQ(em.EquipmentAt(1), 2);
    CHECK_EQ(em.SelectedSlot(), 0);
    PASS(); return 0; END_TEST();
}

static int test_run_action_no_crash() {
    TEST("RunAction on null entity does not crash");
    auto& em = LightningEntityManager::Instance();
    em.Init();
    em.RunAction(nullptr, "on_fire");
    em.RunAction(nullptr, "");
    PASS(); return 0; END_TEST();
}

static int test_player_stats_default() {
    TEST("GetPlayerStat returns default when no player");
    auto& em = LightningEntityManager::Instance();
    em.Init();
    CHECK_EQ((int)em.GetPlayerHealth(), 100);
    CHECK_EQ((int)em.GetPlayerMaxHealth(), 100);
    CHECK_EQ((int)em.GetPlayerLevel(), 1);
    PASS(); return 0; END_TEST();
}

// --- Ammo / Reload tests (Phase 2) ---

static int test_fire_no_weapon() {
    TEST("FireSelectedWeapon returns -1 when no weapon selected");
    auto& em = LightningEntityManager::Instance();
    em.Init();
    // No entity in hotbar
    Vector3 origin = {0,0,0}, dir = {0,0,-1};
    int result = em.FireSelectedWeapon(origin, dir);
    CHECK_EQ(result, -1);
    PASS(); return 0; END_TEST();
}

static int test_fire_not_a_weapon() {
    TEST("FireSelectedWeapon returns -1 for non-weapon entity");
    auto& reg = LightningEntityRegistry::Instance();
    EntityDef def;
    def.name = "NotWeapon";
    def.type = EntityType::PICKUP;
    reg.Register(def);
    auto& em = LightningEntityManager::Instance();
    em.Init();
    int idx = em.Spawn("NotWeapon");
    CHECK(idx >= 0);
    em.HotbarAssign(0, idx);
    Vector3 origin = {0,0,0}, dir = {0,0,-1};
    int result = em.FireSelectedWeapon(origin, dir);
    CHECK_EQ(result, -1);
    PASS(); return 0; END_TEST();
}

static int test_fire_cooldown() {
    TEST("FireSelectedWeapon returns -1 on cooldown");
    auto& reg = LightningEntityRegistry::Instance();
    EntityDef def;
    def.name = "CooldownGun";
    def.type = EntityType::WEAPON;
    def.stats.floats["fire_rate"] = 0.5f;
    reg.Register(def);
    auto& em = LightningEntityManager::Instance();
    em.Init();
    int idx = em.Spawn("CooldownGun");
    CHECK(idx >= 0);
    em.HotbarAssign(0, idx);
    em.SelectSlot(0);
    Vector3 origin = {0,0,0}, dir = {0,0,-1};
    // First fire should succeed (ranged)
    int r1 = em.FireSelectedWeapon(origin, dir);
    CHECK(r1 > 0); // projectile count
    // Second fire immediately should fail due to cooldown
    int r2 = em.FireSelectedWeapon(origin, dir);
    CHECK_EQ(r2, -1);
    PASS(); return 0; END_TEST();
}

static int test_fire_ranged_weapon() {
    TEST("FireSelectedWeapon returns projectile count for ranged");
    auto& reg = LightningEntityRegistry::Instance();
    EntityDef def;
    def.name = "RangedGun";
    def.type = EntityType::WEAPON;
    def.stats.floats["projectile_count"] = 3;
    def.stats.floats["fire_rate"] = 0.25f;
    reg.Register(def);
    auto& em = LightningEntityManager::Instance();
    em.Init();
    int idx = em.Spawn("RangedGun");
    CHECK(idx >= 0);
    em.HotbarAssign(0, idx);
    em.SelectSlot(0);
    Vector3 origin = {0,0,0}, dir = {0,0,-1};
    int result = em.FireSelectedWeapon(origin, dir);
    CHECK_EQ(result, 3);
    PASS(); return 0; END_TEST();
}

static int test_fire_melee_weapon() {
    TEST("FireSelectedWeapon returns 0 for melee swing");
    auto& reg = LightningEntityRegistry::Instance();
    EntityDef def;
    def.name = "MeleeBlade";
    def.type = EntityType::WEAPON;
    def.stats.floats["reach"] = 3.0f;
    def.stats.floats["swing_speed"] = 0.8f;
    reg.Register(def);
    auto& em = LightningEntityManager::Instance();
    em.Init();
    int idx = em.Spawn("MeleeBlade");
    CHECK(idx >= 0);
    em.HotbarAssign(0, idx);
    em.SelectSlot(0);
    Vector3 origin = {0,0,0}, dir = {0,0,-1};
    int result = em.FireSelectedWeapon(origin, dir);
    CHECK_EQ(result, 0); // 0 = melee swing
    PASS(); return 0; END_TEST();
}

static int test_ammo_init_and_decrement() {
    TEST("Ammo initializes from magazine and decrements per shot");
    auto& reg = LightningEntityRegistry::Instance();
    EntityDef def;
    def.name = "AmmoGun";
    def.type = EntityType::WEAPON;
    def.stats.floats["magazine"] = 5;
    def.stats.floats["fire_rate"] = 0.1f;
    reg.Register(def);
    auto& em = LightningEntityManager::Instance();
    em.Init();
    int idx = em.Spawn("AmmoGun");
    CHECK(idx >= 0);
    em.HotbarAssign(0, idx);
    em.SelectSlot(0);
    EntityInstance* ent = em.SelectedEntity();
    CHECK(ent != nullptr);
    Vector3 origin = {0,0,0}, dir = {0,0,-1};
    // Fire 3 shots
    for (int i = 0; i < 3; i++) {
        int r = em.FireSelectedWeapon(origin, dir);
        CHECK(r > 0);
        // Wait for cooldown by setting it to 0
        ent->cooldownRemaining = 0.0f;
    }
    auto it = ent->runtimeStats.find("ammo");
    CHECK(it != ent->runtimeStats.end());
    CHECK_EQ((int)it->second, 2); // 5 - 3 = 2
    PASS(); return 0; END_TEST();
}

static int test_auto_reload_on_empty() {
    TEST("Auto-reload triggers when ammo reaches 0");
    auto& reg = LightningEntityRegistry::Instance();
    EntityDef def;
    def.name = "AutoReloadGun";
    def.type = EntityType::WEAPON;
    def.stats.floats["magazine"] = 2;
    def.stats.floats["fire_rate"] = 0.1f;
    def.stats.floats["reload_time"] = 1.5f;
    reg.Register(def);
    auto& em = LightningEntityManager::Instance();
    em.Init();
    int idx = em.Spawn("AutoReloadGun");
    CHECK(idx >= 0);
    em.HotbarAssign(0, idx);
    em.SelectSlot(0);
    EntityInstance* ent = em.SelectedEntity();
    CHECK(ent != nullptr);
    Vector3 origin = {0,0,0}, dir = {0,0,-1};
    // Fire until empty (2 shots)
    for (int i = 0; i < 2; i++) {
        int r = em.FireSelectedWeapon(origin, dir);
        CHECK(r > 0);
        ent->cooldownRemaining = 0.0f;
    }
    // Next fire should auto-reload (return -1) and reset ammo
    int r = em.FireSelectedWeapon(origin, dir);
    CHECK_EQ(r, -1);
    // Ammo should be back to magazine (2)
    auto it = ent->runtimeStats.find("ammo");
    CHECK(it != ent->runtimeStats.end());
    CHECK_EQ((int)it->second, 2);
    PASS(); return 0; END_TEST();
}

static int test_reload_selected() {
    TEST("ReloadSelectedWeapon refills ammo");
    auto& reg = LightningEntityRegistry::Instance();
    EntityDef def;
    def.name = "ReloadGun";
    def.type = EntityType::WEAPON;
    def.stats.floats["magazine"] = 10;
    def.stats.floats["fire_rate"] = 0.1f;
    def.stats.floats["reload_time"] = 2.0f;
    reg.Register(def);
    auto& em = LightningEntityManager::Instance();
    em.Init();
    int idx = em.Spawn("ReloadGun");
    CHECK(idx >= 0);
    em.HotbarAssign(0, idx);
    em.SelectSlot(0);
    EntityInstance* ent = em.SelectedEntity();
    CHECK(ent != nullptr);
    Vector3 origin = {0,0,0}, dir = {0,0,-1};
    // Fire 4 shots to deplete some ammo
    for (int i = 0; i < 4; i++) {
        em.FireSelectedWeapon(origin, dir);
        ent->cooldownRemaining = 0.0f;
    }
    auto it = ent->runtimeStats.find("ammo");
    CHECK(it != ent->runtimeStats.end());
    CHECK_EQ((int)it->second, 6); // 10-4=6
    // Reload
    bool reloaded = em.ReloadSelectedWeapon();
    CHECK(reloaded);
    CHECK_EQ((int)ent->runtimeStats["ammo"], 10); // back to full
    PASS(); return 0; END_TEST();
}

static int test_reload_no_magazine() {
    TEST("ReloadSelectedWeapon returns false when no magazine stat");
    auto& reg = LightningEntityRegistry::Instance();
    EntityDef def;
    def.name = "NoMagWeapon";
    def.type = EntityType::WEAPON;
    reg.Register(def);
    auto& em = LightningEntityManager::Instance();
    em.Init();
    int idx = em.Spawn("NoMagWeapon");
    CHECK(idx >= 0);
    em.HotbarAssign(0, idx);
    em.SelectSlot(0);
    bool reloaded = em.ReloadSelectedWeapon();
    CHECK(!reloaded);
    PASS(); return 0; END_TEST();
}

static int test_reload_full_ammo() {
    TEST("ReloadSelectedWeapon returns false when ammo is full");
    auto& reg = LightningEntityRegistry::Instance();
    EntityDef def;
    def.name = "FullAmmoGun";
    def.type = EntityType::WEAPON;
    def.stats.floats["magazine"] = 8;
    reg.Register(def);
    auto& em = LightningEntityManager::Instance();
    em.Init();
    int idx = em.Spawn("FullAmmoGun");
    CHECK(idx >= 0);
    em.HotbarAssign(0, idx);
    em.SelectSlot(0);
    // Ammo should be full (not initialized until first fire)
    // Fire once to init ammo, then reload
    Vector3 origin = {0,0,0}, dir = {0,0,-1};
    em.FireSelectedWeapon(origin, dir);
    EntityInstance* ent = em.SelectedEntity();
    CHECK(ent != nullptr);
    ent->cooldownRemaining = 0.0f;
    // Now ammo = 7 (one shot fired)
    em.ReloadSelectedWeapon();
    CHECK_EQ((int)ent->runtimeStats["ammo"], 8);
    // Try reloading again when full
    bool reloaded = em.ReloadSelectedWeapon();
    CHECK(!reloaded);
    PASS(); return 0; END_TEST();
}

static int test_reload_started_flag() {
    TEST("Reload-started flag is set only when a reload actually happens");
    auto& reg = LightningEntityRegistry::Instance();
    EntityDef def;
    def.name = "ReloadFlagGun";
    def.type = EntityType::WEAPON;
    def.stats.floats["magazine"] = 3;
    def.stats.floats["fire_rate"] = 0.0f;
    reg.Register(def);
    auto& em = LightningEntityManager::Instance();
    em.Init();
    int idx = em.Spawn("ReloadFlagGun");
    CHECK(idx >= 0);
    em.HotbarAssign(0, idx);
    em.SelectSlot(0);
    Vector3 origin = {0,0,0}, dir = {0,0,-1};

    // Drain the flag left over from earlier tests.
    em.ConsumeReloadStarted();

    // Firing 3 shots empties the magazine.
    for (int i = 0; i < 3; i++) {
        em.FireSelectedWeapon(origin, dir);
        em.SelectedEntity()->cooldownRemaining = 0.0f;
    }
    EntityInstance* ent = em.SelectedEntity();
    CHECK_EQ((int)ent->runtimeStats["ammo"], 0);
    CHECK(!em.ConsumeReloadStarted());  // emptying the mag is not a reload

    // Firing on empty auto-reloads: that must report a reload.
    ent->cooldownRemaining = 0.0f;
    em.FireSelectedWeapon(origin, dir);
    CHECK(em.ConsumeReloadStarted());  // auto-reload plays the clip
    CHECK_EQ((int)ent->runtimeStats["ammo"], 3);
    CHECK(!em.ConsumeReloadStarted());  // flag is consumed, not sticky

    // Firing again spends ammo, leaving the magazine short.
    ent->cooldownRemaining = 0.0f;
    em.FireSelectedWeapon(origin, dir);
    CHECK(!em.ConsumeReloadStarted());
    CHECK_EQ((int)ent->runtimeStats["ammo"], 2);

    // Manual reload reports one, and a rejected reload does not.
    ent->cooldownRemaining = 0.0f;
    CHECK(em.ReloadSelectedWeapon());
    CHECK(em.ConsumeReloadStarted());
    CHECK(!em.ReloadSelectedWeapon());          // already full
    CHECK(!em.ConsumeReloadStarted());
    PASS(); return 0; END_TEST();
}

static int test_hotbar_drag_reorder() {
    TEST("Hotbar drag reorders; press and release on the same slot is a no-op");
    auto& reg = LightningEntityRegistry::Instance();
    EntityDef def;
    def.name = "DragItem";
    def.type = EntityType::WEAPON;
    reg.Register(def);
    auto& em = LightningEntityManager::Instance();
    em.Init();
    int a = em.Spawn("DragItem");
    int b = em.Spawn("DragItem");
    int c = em.Spawn("DragItem");
    CHECK(a >= 0); CHECK(b >= 0); CHECK(c >= 0);
    em.HotbarAssign(0, a);
    em.HotbarAssign(1, b);
    em.HotbarAssign(2, c);
    CHECK(!em.IsHotbarDragging());

    // An empty slot has nothing to drag.
    em.HotbarBeginDrag(5);
    CHECK(!em.IsHotbarDragging());
    CHECK_EQ(em.HotbarDragFrom(), -1);

    // Out-of-range press is rejected.
    em.HotbarBeginDrag(-1);
    CHECK(!em.IsHotbarDragging());
    em.HotbarBeginDrag(LightningEntityManager::HOTBAR_SIZE);
    CHECK(!em.IsHotbarDragging());

    // Dragging slot 0 onto slot 2 swaps them.
    em.HotbarBeginDrag(0);
    CHECK(em.IsHotbarDragging());
    CHECK_EQ(em.HotbarDragFrom(), 0);
    CHECK_EQ(em.HotbarDragTo(), 0);          // defaults to the source
    em.HotbarUpdateDrag(2);
    CHECK_EQ(em.HotbarDragTo(), 2);
    em.HotbarEndDrag();
    CHECK(!em.IsHotbarDragging());
    CHECK_EQ(em.HotbarAt(0), c);
    CHECK_EQ(em.HotbarAt(2), a);
    CHECK_EQ(em.HotbarAt(1), b);             // untouched

    // Release on the source slot changes nothing.
    em.HotbarBeginDrag(0);
    em.HotbarUpdateDrag(0);
    em.HotbarEndDrag();
    CHECK_EQ(em.HotbarAt(0), c);
    CHECK_EQ(em.HotbarAt(2), a);

    // Dragging off the bar (hover -1) cancels rather than corrupting the order.
    em.HotbarBeginDrag(1);
    em.HotbarUpdateDrag(-1);
    CHECK_EQ(em.HotbarDragTo(), -1);
    em.HotbarEndDrag();
    CHECK_EQ(em.HotbarAt(0), c);
    CHECK_EQ(em.HotbarAt(1), b);

    // Dropping onto an empty slot is a legitimate reorder into the gap.
    em.HotbarBeginDrag(1);
    em.HotbarUpdateDrag(6);
    em.HotbarEndDrag();
    CHECK_EQ(em.HotbarAt(6), b);
    CHECK_EQ(em.HotbarAt(1), -1);

    // Out-of-range drop target is clamped away, not applied.
    em.HotbarBeginDrag(0);
    em.HotbarUpdateDrag(999);
    CHECK_EQ(em.HotbarDragTo(), -1);
    em.HotbarEndDrag();
    CHECK_EQ(em.HotbarAt(0), c);
    PASS(); return 0; END_TEST();
}

static int test_melee_stamina() {
    TEST("Melee swing spends stamina_cost and is refused when empty");
    auto& reg = LightningEntityRegistry::Instance();
    EntityDef def;
    def.name = "StaminaSword";
    def.type = EntityType::WEAPON;
    def.stats.floats["damage"] = 40.0f;
    def.stats.floats["reach"] = 3.0f;
    def.stats.floats["swing_speed"] = 0.1f;
    def.stats.floats["stamina_cost"] = 18.0f;   // etheral_waver's cost
    reg.Register(def);
    auto& em = LightningEntityManager::Instance();
    em.Init();
    int idx = em.Spawn("StaminaSword");
    CHECK(idx >= 0);
    em.HotbarAssign(0, idx);
    em.SelectSlot(0);
    EntityInstance* ent = em.SelectedEntity();
    CHECK(ent != nullptr);

    // Fresh pool: a full swing budget.
    em.SetPlayerStamina(100.0f);
    CHECK_APROX(em.GetPlayerMaxStamina(), 100.0f, 0.001f);
    CHECK(em.HasStamina(18.0f));

    Vector3 origin = {0,0,0}, dir = {0,0,-1};
    em.SelectedEntity()->cooldownRemaining = 0.0f;
    int r = em.FireSelectedWeapon(origin, dir);
    CHECK_EQ(r, 0);                      // melee swing performed
    CHECK_APROX(em.GetPlayerStamina(), 82.0f, 0.001f);

    // A second swing spends again.
    em.SelectedEntity()->cooldownRemaining = 0.0f;
    em.FireSelectedWeapon(origin, dir);
    CHECK_APROX(em.GetPlayerStamina(), 64.0f, 0.001f);

    // Drain the pool below one swing's cost: the swing must be refused
    // (-1) and must not spend a partial amount.
    em.SetPlayerStamina(10.0f);
    em.SelectedEntity()->cooldownRemaining = 0.0f;
    CHECK(!em.HasStamina(18.0f));
    CHECK_EQ(em.FireSelectedWeapon(origin, dir), -1);
    CHECK_APROX(em.GetPlayerStamina(), 10.0f, 0.001f);

    // Regen moves the pool back up and never past max.
    em.UpdateStamina(1.0f);
    CHECK(em.GetPlayerStamina() > 10.0f);
    em.SetPlayerStamina(100.0f);
    em.UpdateStamina(10.0f);
    CHECK_APROX(em.GetPlayerStamina(), 100.0f, 0.001f);

    // A zero-cost weapon is never gated (stamina_cost omitted entirely).
    def.name = "FreeSword";
    def.stats.floats["stamina_cost"] = 0.0f;
    reg.Register(def);
    int freeIdx = em.Spawn("FreeSword");
    CHECK(freeIdx >= 0);
    em.HotbarAssign(1, freeIdx);
    em.SelectSlot(1);
    em.SetPlayerStamina(0.0f);
    em.SelectedEntity()->cooldownRemaining = 0.0f;
    CHECK_EQ(em.FireSelectedWeapon(origin, dir), 0);
    CHECK_APROX(em.GetPlayerStamina(), 0.0f, 0.001f);
    PASS(); return 0; END_TEST();
}

static int test_stashed_weapon_drop() {
    TEST("Weapon pickup with a full hotbar is stashed, not lost");
    // Reproduces the user's bug: Pistol in slot 1, walked over the
    // Etheral_Weaver, etheral_waver vanished (itemId=0, hotbar full, the
    // old path did Despawn with no fallback). The stash queues the def name
    // and FlushNextStashedWeapon places it as soon as a slot opens.
    auto& reg = LightningEntityRegistry::Instance();
    EntityDef pistol;  pistol.name = "stash_pistol"; pistol.type = EntityType::WEAPON;
    EntityDef sword;   sword.name  = "stash_sword";  sword.type  = EntityType::WEAPON;
    reg.Register(pistol); reg.Register(sword);

    auto& em = LightningEntityManager::Instance();
    em.Init();
    for (int i = 0; i < LightningEntityManager::HOTBAR_SIZE; i++) {
        int p = em.Spawn("stash_pistol");
        CHECK(p >= 0);
        em.HotbarAssign(i, p);
    }
    CHECK_EQ(em.HotbarFirstFreeSlot(), -1);
    CHECK_EQ(em.StashedWeaponCount(), 0);

    // Simulate the walk-over pickup's stash call (the pickup code path itself
    // lives in OzPawnSystem and would need a world tick to exercise; the
    // contract here is that StashWeapon queues by name and FlushNextStashedWeapon
    // fills the first free slot).
    em.StashWeapon("stash_sword");
    CHECK_EQ(em.StashedWeaponCount(), 1);

    // Bar is still full: a flush attempt finds no room and KEEPS the entry, so
    // a later pickup can place it. It used to consume it - and the pickup path
    // flushes at the end of the SAME call that stashes, so a full hotbar
    // destroyed the weapon immediately and the HUD counter read 0.
    CHECK(!em.FlushNextStashedWeapon());
    CHECK_EQ(em.StashedWeaponCount(), 1);

    // Free a slot by despawning whatever was in slot 0; the manager already
    // clears the slot entry on Despawn.
    int freeInstance = em.HotbarAt(0);
    em.Despawn(freeInstance);
    CHECK_EQ(em.HotbarFirstFreeSlot(), 0);

    CHECK(em.FlushNextStashedWeapon());
    CHECK_EQ(em.StashedWeaponCount(), 0);
    EntityInstance* placed = em.Get(em.HotbarAt(0));
    CHECK(placed != nullptr);
    CHECK(placed->def != nullptr);
    CHECK(placed->def->name == std::string("stash_sword"));

    // Multiple stashed defs drain in FIFO order.
    em.Despawn(em.HotbarAt(0));
    CHECK_EQ(em.HotbarFirstFreeSlot(), 0);
    em.StashWeapon("stash_sword");
    em.StashWeapon("stash_pistol");
    CHECK(em.FlushNextStashedWeapon());
    CHECK(em.HotbarAt(0) >= 0);
    PASS(); return 0; END_TEST();
}

// Despawn must invalidate m_playerEntityIndex when it removes the PLAYER.
//
// Despawn maintains every index into m_instances: the hotbar array, the equipment
// array, and — as of this test — m_playerEntityIndex. It handled the first two and
// not the third, even though m_playerEntityIndex is the same kind of index and is
// what GetPlayerHealth / SetPlayerLevel / ResolveScriptStat read through.
//
// Despawning the player left the index pointing at a slot that no longer holds the
// player. The read sites all bounds-check (`m_playerEntityIndex < m_instances.size()`)
// so this did not corrupt memory, but HasPlayerEntity() is just `>= 0`, so it
// reported a live player over an empty instance list — and every $health script
// variable silently fell back to its compiled-in default instead of the player's
// real value. TriggerEntityAction's own comment already claimed the remap happened.
//
// Note on the OTHER half of the fix (remapping the index across a swap-with-last):
// that branch is currently UNREACHABLE, and this test does not pretend otherwise.
// m_playerEntityIndex is assigned in exactly one place — Init(), immediately after
// Spawn("Player") on a just-cleared vector — so it is always 0, and the swap only
// moves index 0 when the player is the LAST element, which needs size == 1, where
// `index < size - 1` cannot hold. The remap is defensive: it keeps the invariant
// true if the spawn order ever changes. A test that "drove the swap with a player at
// the tail" would have passed against the unfixed code too, because no sequence of
// public calls produces that state — which is why this test asserts the reachable
// half instead.
static int test_despawn_clears_player_index() {
    TEST("Despawn of the player invalidates m_playerEntityIndex");
    auto& reg = LightningEntityRegistry::Instance();
    auto& em  = LightningEntityManager::Instance();

    // A minimal def Init() will spawn as "Player". Registered here rather than at
    // file scope because the registry is a process-wide singleton and every test
    // above assumes the Player spawn FAILS (leaving Count() == 0). Hence "run last".
    EntityDef pdef;
    pdef.name = "Player";
    pdef.type = EntityType::UPGRADE;
    reg.Register(pdef);

    em.Init();
    CHECK(em.HasPlayerEntity());
    const int playerIdx = em.PlayerEntityIndex();
    CHECK(playerIdx >= 0 && playerIdx < em.Count());

    // Despawn the player. The index must not keep naming a slot it no longer owns.
    em.Despawn(playerIdx);
    CHECK_EQ(em.Count(), 0);
    CHECK_EQ(em.PlayerEntityIndex(), -1);
    CHECK(!em.HasPlayerEntity());

    // A stale index also has to stay out of range even when other instances remain,
    // so it cannot silently attach the player's stats to one of them.
    EntityDef fdef;
    fdef.name = "leftover_filler";
    fdef.type = EntityType::UPGRADE;
    reg.Register(fdef);
    em.Init();                                  // player back at 0, alone
    CHECK(em.HasPlayerEntity());
    em.Despawn(em.PlayerEntityIndex());         // player gone, none left
    CHECK(!em.HasPlayerEntity());
    const int filler = em.Spawn(fdef.name.c_str());
    CHECK(filler >= 0);
    CHECK(!em.HasPlayerEntity());               // must NOT adopt the new instance
    CHECK_EQ(em.PlayerEntityIndex(), -1);

    // Restore the state the tests above expect: Init() with a registered "Player"
    // def now DOES spawn one, so nothing further may assume Count() == 0.
    em.Init();
    PASS(); return 0; END_TEST();
}

static int test_multi_despawn_cycles() {
    TEST("Multiple init cycles do not crash");
    auto& em = LightningEntityManager::Instance();
    for (int i = 0; i < 5; i++) {
        em.Init();
        CHECK_EQ(em.Count(), 0);
        em.HotbarAssign(0, 1);
        em.EquipmentAssign(0, 2);
        CHECK_EQ(em.HotbarAt(0), 1);
        CHECK_EQ(em.EquipmentAt(0), 2);
    }
    PASS(); return 0; END_TEST();
}

static int test_equipment_slot_name() {
    TEST("EquipmentSlotFromName maps authored names");
    CHECK_EQ(LightningEntityManager::EquipmentSlotFromName("armor"), 0);
    CHECK_EQ(LightningEntityManager::EquipmentSlotFromName("helmet"), 3);
    CHECK_EQ(LightningEntityManager::EquipmentSlotFromName("boots"), 4);
    CHECK_EQ(LightningEntityManager::EquipmentSlotFromName("accessory1"), 6);
    CHECK_EQ(LightningEntityManager::EquipmentSlotFromName("\"jewelry2\""), 2);
    CHECK_EQ(LightningEntityManager::EquipmentSlotFromName("weapon"), -1);
    CHECK_EQ(LightningEntityManager::EquipmentSlotFromName("nonsense"), -1);
    PASS(); return 0; END_TEST();
}

static int test_auto_equip_and_defense() {
    TEST("AutoEquip places armor in its slot and defense sums");
    auto& reg = LightningEntityRegistry::Instance();
    EntityDef helm;
    helm.name = "test_helmet";
    helm.type = EntityType::ARMOR;
    helm.stats.strings["equip_slot"] = "\"helmet\"";
    helm.stats.floats["defense"] = 10.0f;
    reg.Register(helm);

    auto& em = LightningEntityManager::Instance();
    em.Init();
    CHECK(em.AutoEquip(reg.Find("test_helmet")));
    int idx = em.EquipmentAt(3);
    CHECK(idx >= 0);
    CHECK(em.Get(idx) != nullptr);
    CHECK_EQ((int)em.GetPlayerDefense(), 10);

    em.EquipmentUnequip(3);
    CHECK_EQ(em.EquipmentAt(3), -1);
    CHECK_EQ((int)em.GetPlayerDefense(), 0);
    PASS(); return 0; END_TEST();
}

// DeserializeState runs on startup AND on every death respawn (LoadSave). It
// used to reset the hotbar/equipment arrays without despawning the instances
// they named, so each call leaked one instance per slot until MAX_ENTITIES (512)
// ran out and gear stopped restoring.
static int test_deserialize_reuses_instances() {
    TEST("DeserializeState does not leak instances across loads");
    auto& reg = LightningEntityRegistry::Instance();
    EntityDef sword;
    sword.name = "reuse_sword";
    sword.type = EntityType::WEAPON;
    reg.Register(sword);

    auto& em = LightningEntityManager::Instance();
    em.Init();
    em.HotbarAssign(0, em.Spawn("reuse_sword"));
    em.HotbarAssign(1, em.Spawn("reuse_sword"));
    CHECK(em.HotbarAt(0) >= 0);
    CHECK(em.HotbarAt(1) >= 0);
    const int before = em.Count();
    CHECK(before >= 2);

    const std::string state = em.SerializeState();
    for (int i = 0; i < 30; i++) {
        CHECK(em.DeserializeState(state));
        CHECK_EQ(em.Count(), before);
        CHECK(em.HotbarAt(0) >= 0);
        CHECK(em.HotbarAt(1) >= 0);
    }
    PASS(); return 0; END_TEST();
}

// Equipping a SECOND piece into an occupied slot.
//
// AutoEquip spawns the new piece (so it is the LAST instance) and then
// EquipmentAssign despawned the old occupant - a swap-remove that relocates the
// new piece into the old index. Assigning the pre-despawn index left the slot
// pointing one past the end: Get() returned null, defense dropped, and every
// later pickup into the slot was despawned too.
static int test_auto_equip_replaces_same_slot() {
    TEST("AutoEquip twice into one slot keeps the new piece");
    auto& reg = LightningEntityRegistry::Instance();
    EntityDef first;
    first.name = "repl_armor_1";
    first.type = EntityType::ARMOR;
    first.stats.strings["equip_slot"] = "armor";
    first.stats.floats["defense"] = 10.0f;
    EntityDef second;
    second.name = "repl_armor_2";
    second.type = EntityType::ARMOR;
    second.stats.strings["equip_slot"] = "armor";
    second.stats.floats["defense"] = 25.0f;
    reg.Register(first);
    reg.Register(second);

    auto& em = LightningEntityManager::Instance();
    em.Init();
    // Two unrelated instances AFTER the armour, so the new piece is not the
    // last element and the swap path is the one that used to break.
    EntityDef filler;
    filler.name = "repl_filler";
    filler.type = EntityType::UPGRADE;
    reg.Register(filler);
    CHECK(em.Spawn("repl_filler") >= 0);
    CHECK(em.Spawn("repl_filler") >= 0);

    CHECK(em.AutoEquip(reg.Find("repl_armor_1")));
    const int slotA = 0;   // "armor"
    CHECK(em.Get(em.EquipmentAt(slotA)) != nullptr);
    CHECK_EQ((int)em.GetPlayerDefense(), 10);

    CHECK(em.AutoEquip(reg.Find("repl_armor_2")));
    const int idx = em.EquipmentAt(slotA);
    CHECK(idx >= 0);
    CHECK(em.Get(idx) != nullptr);
    CHECK(em.Get(idx)->def != nullptr);
    CHECK(em.Get(idx)->def->name == std::string("repl_armor_2"));
    CHECK_EQ((int)em.GetPlayerDefense(), 25);

    // The replaced piece must be gone, not merely unreferenced.
    for (int i = 0; i < em.Count(); i++) {
        if (em.Get(i) && em.Get(i)->def)
            CHECK(em.Get(i)->def->name != std::string("repl_armor_1"));
    }
    PASS(); return 0; END_TEST();
}

static int test_respec_skills() {
    TEST("RespecSkills clears unlocked nodes");
    auto& reg = LightningEntityRegistry::Instance();
    EntityDef sk;
    sk.name = "test_respec_skill";
    sk.type = EntityType::SKILL;
    sk.stats.floats["cost"] = 20.0f;
    sk.stats.strings["cost_type"] = "mana";
    sk.stats.floats["max_health_bonus"] = 10.0f;
    reg.Register(sk);

    auto& em = LightningEntityManager::Instance();
    em.Init();
    em.UnlockSkill("test_respec_skill");
    CHECK(em.IsSkillUnlocked("test_respec_skill"));
    CHECK_EQ((int)em.UnlockedSkills().size(), 1);
    em.RespecSkills();
    CHECK(!em.IsSkillUnlocked("test_respec_skill"));
    CHECK_EQ((int)em.UnlockedSkills().size(), 0);
    PASS(); return 0; END_TEST();
}

// Regression: RunAction used to leave whatever the body queued sitting in the
// context. Only on_tick, the use-item path and TriggerEntityAction drained it,
// so msg / play_sound / heal / damage / playerstat inside on_fire, on_swing,
// on_hit, on_reload, on_equip, on_unequip and the pawn FSM hooks were silently
// discarded. All four shipped pawn death messages never displayed.
static int test_runaction_drains_side_effects() {
    TEST("RunAction drains queued msg from a weapon hook");
    auto& reg = LightningEntityRegistry::Instance();
    EntityDef def;
    def.name = "DrainGun";
    def.type = EntityType::WEAPON;
    def.stats.floats["fire_rate"] = 0.1f;
    EntityAction act;
    act.name = "on_fire";
    act.scriptLines.push_back("msg \"boom\"");
    def.actions.push_back(act);
    reg.Register(def);

    auto& em = LightningEntityManager::Instance();
    em.Init();
    int idx = em.Spawn("DrainGun");
    CHECK(idx >= 0);
    em.HotbarAssign(0, idx);
    em.SelectSlot(0);

    em.ClearPendingMessage();
    Vector3 origin = {0,0,0}, dir = {0,0,-1};
    em.FireSelectedWeapon(origin, dir);      // triggers on_fire internally
    CHECK(em.PendingMessage() == "boom");
    PASS(); return 0; END_TEST();
}

// Regression: set_cooldown wrote __cooldown and nothing ever cleared it, so a
// single `set_cooldown N` permanently replaced the authored fire_rate for the
// instance's lifetime. Reading it now consumes it.
static int test_set_cooldown_is_one_shot() {
    TEST("set_cooldown does not permanently override fire_rate");
    auto& reg = LightningEntityRegistry::Instance();
    EntityDef def;
    def.name = "OneShotCd";
    def.type = EntityType::WEAPON;
    def.stats.floats["fire_rate"] = 0.05f;
    EntityAction act;
    act.name = "on_fire";
    act.scriptLines.push_back("set_cooldown 0.5");
    def.actions.push_back(act);
    reg.Register(def);

    auto& em = LightningEntityManager::Instance();
    em.Init();
    int idx = em.Spawn("OneShotCd");
    CHECK(idx >= 0);
    EntityInstance* inst = em.Get(idx);
    CHECK(inst != nullptr);
    em.HotbarAssign(0, idx);
    em.SelectSlot(0);

    Vector3 origin = {0,0,0}, dir = {0,0,-1};
    em.FireSelectedWeapon(origin, dir);
    // The script value applies once...
    CHECK(inst->cooldownRemaining > 0.49f && inst->cooldownRemaining < 0.51f);

    // ...and is not still sitting there to re-apply on a later trigger.
    CHECK(inst->ctx.TakePendingFloat("__cooldown") == 0.0f);
    PASS(); return 0; END_TEST();
}

// Regression: `variants { "lvlN" { mesh_override = ... } }` parsed into the def
// but nothing read it, so every weapon always rendered its base mesh.
static int test_variant_selected_by_level() {
    TEST("Spawn picks the highest variant tier within the player level");
    auto& reg = LightningEntityRegistry::Instance();

    // Init() spawns a "Player" def to host the player stats; without one the
    // player index stays -1 and SetPlayerLevel silently no-ops, so the tier
    // selection would always see the default level.
    EntityDef pdef;
    pdef.name = "Player";
    pdef.type = EntityType::UPGRADE;
    pdef.stats.floats["level"] = 1.0f;
    reg.Register(pdef);

    EntityDef def;
    def.name = "VariantGun";
    def.type = EntityType::WEAPON;
    def.mesh = "base.obj";
    EntityVariant v1; v1.name = "lvl1"; v1.meshOverride = "t1.obj";
    EntityVariant v2; v2.name = "lvl2"; v2.meshOverride = "t2.obj";
    EntityVariant v3; v3.name = "lvl3"; v3.meshOverride = "t3.obj";
    def.variants.push_back(v1);
    def.variants.push_back(v2);
    def.variants.push_back(v3);
    reg.Register(def);

    auto& em = LightningEntityManager::Instance();
    em.Init();

    // Level 1 -> lvl1.
    em.SetPlayerLevel(1);
    int idx1 = em.Spawn("VariantGun");
    CHECK(idx1 >= 0);
    CHECK_EQ(em.Get(idx1)->variantIndex, 0);

    // Level 2 -> lvl2.
    em.SetPlayerLevel(2);
    int idx2 = em.Spawn("VariantGun");
    CHECK(idx2 >= 0);
    CHECK_EQ(em.Get(idx2)->variantIndex, 1);

    // Level 9 -> clamps to the highest declared tier, lvl3.
    em.SetPlayerLevel(9);
    int idx3 = em.Spawn("VariantGun");
    CHECK(idx3 >= 0);
    CHECK_EQ(em.Get(idx3)->variantIndex, 2);
    PASS(); return 0; END_TEST();
}

// No variants declared must leave variantIndex unset rather than picking one.
static int test_no_variants_leaves_index() {
    TEST("Spawn leaves variantIndex unset when no variants exist");
    auto& reg = LightningEntityRegistry::Instance();
    EntityDef def;
    def.name = "PlainGun";
    def.type = EntityType::WEAPON;
    def.mesh = "base.obj";
    reg.Register(def);

    auto& em = LightningEntityManager::Instance();
    em.Init();
    em.SetPlayerLevel(5);
    int idx = em.Spawn("PlainGun");
    CHECK(idx >= 0);
    CHECK_EQ(em.Get(idx)->variantIndex, -1);
    PASS(); return 0; END_TEST();
}

// ---------------------------------------------------------------------------
// Spells. These run last for the same reason test_despawn_clears_player_index
// does: casting spends a player pool, and GetPlayerStat/SetPlayerStat are no-ops
// until a "Player" entity exists (they read 0 and write nowhere). Every test
// above this point assumes Init() spawns nothing.
// ---------------------------------------------------------------------------

// Registers "Player" (idempotent - Register overwrites by name) and a spell def,
// then re-inits so the player exists.
static void SetupPlayerAndSpell(const char* spellName, EntityType type,
                                float manaCost, float peCost, const char* costType,
                                float cooldown, float castTime = 0.0f) {
    auto& reg = LightningEntityRegistry::Instance();
    EntityDef pdef;
    pdef.name = "Player";
    pdef.type = EntityType::UPGRADE;
    reg.Register(pdef);

    EntityDef sd;
    sd.name = spellName;
    sd.type = type;
    if (manaCost >= 0.0f) sd.stats.floats["mana_cost"] = manaCost;
    if (peCost >= 0.0f)   sd.stats.floats["pe_cost"] = peCost;
    if (costType)         sd.stats.strings["cost_type"] = costType;
    sd.stats.floats["cooldown"] = cooldown;
    sd.stats.floats["damage"] = 20.0f;
    if (castTime > 0.0f) sd.stats.floats["cast_time"] = castTime;
    reg.Register(sd);

    LightningEntityManager::Instance().Init();
}

static int test_spell_bar_bind_and_init_clear() {
    TEST("Spell bar binds by name and Init() clears it");
    SetupPlayerAndSpell("BindBolt", EntityType::SPELL, 10.0f, -1.0f, nullptr, 0.5f);
    auto& em = LightningEntityManager::Instance();

    CHECK(em.GetSpellInSlot(0).empty());
    em.SetSpellInSlot(2, "BindBolt");
    CHECK(em.GetSpellInSlot(2) == "BindBolt");

    // Out-of-range slots are ignored, not clamped onto a real one.
    em.SetSpellInSlot(-1, "BindBolt");
    em.SetSpellInSlot(LightningEntityManager::SPELL_BAR_SIZE + 3, "BindBolt");
    CHECK(em.GetSpellInSlot(0) == "");

    // Init() (New Game) must drop the bindings - the bar is name-backed so it is
    // invisible to the index remapping in Despawn, and would otherwise survive.
    em.Init();
    CHECK(em.GetSpellInSlot(2) == "");
    PASS(); return 0; END_TEST();
}

static int test_cast_spell_spends_mana_and_sets_cooldown() {
    TEST("CastSpell spends mana, sets cooldown, and refuses while empty/on CD");
    SetupPlayerAndSpell("ManaBolt", EntityType::SPELL, 12.0f, -1.0f, nullptr, 0.5f);
    auto& em = LightningEntityManager::Instance();

    CHECK(em.HasPlayerEntity());      // the whole test depends on this
    em.SetPlayerMana(100.0f);
    em.SetSpellInSlot(0, "ManaBolt");

    const Vector3 o = {0, 0, 0}, d = {0, 0, -1};
    const int r = em.CastSpell(0, o, d);
    CHECK(r != -1);                    // accepted (headless: no projectiles)
    CHECK_APROX(em.GetPlayerMana(), 88.0f, 0.001f);
    CHECK(em.GetSpellCooldown(0) > 0.0f);

    // On cooldown: refused, and spending nothing further.
    const float manaAfter = em.GetPlayerMana();
    CHECK_EQ(em.CastSpell(0, o, d), -1);
    CHECK_APROX(em.GetPlayerMana(), manaAfter, 0.001f);

    // Clear the cooldown and drain the pool: an unaffordable cast is refused
    // WITHOUT spending, so there is no partial spend and no banked cast.
    em.SetSpellInSlot(0, "");          // clears + despawns the instance
    em.SetSpellInSlot(0, "ManaBolt");  // fresh instance, cooldown reset
    em.SetPlayerMana(5.0f);
    CHECK_EQ(em.CastSpell(0, o, d), -1);
    CHECK_APROX(em.GetPlayerMana(), 5.0f, 0.001f);
    CHECK(em.GetSpellCooldown(0) <= 0.0f);   // refused cast set NO cooldown

    // Empty slot: refused, silently.
    em.SetSpellInSlot(0, "");
    CHECK_EQ(em.CastSpell(0, o, d), -1);
    PASS(); return 0; END_TEST();
}

static int test_cast_spell_uses_psychic_energy() {
    TEST("A cost_type=psychic_energy spell draws on PE, not mana");
    SetupPlayerAndSpell("PeBolt", EntityType::SPELL, -1.0f, 30.0f,
                        "psychic_energy", 0.5f);
    auto& em = LightningEntityManager::Instance();

    em.SetPlayerMana(100.0f);
    em.SetPlayerPsychicEnergy(100.0f);
    em.SetSpellInSlot(1, "PeBolt");

    const Vector3 o = {0, 0, 0}, d = {0, 0, -1};
    CHECK(em.CastSpell(1, o, d) != -1);
    CHECK_APROX(em.GetPlayerPsychicEnergy(), 70.0f, 0.001f);
    CHECK_APROX(em.GetPlayerMana(), 100.0f, 0.001f);   // mana untouched

    // The affordability gate is on the RIGHT pool: an empty PE pool must refuse
    // even with full mana.
    em.SetSpellInSlot(1, "");
    em.SetSpellInSlot(1, "PeBolt");
    em.SetPlayerMana(100.0f);
    em.SetPlayerPsychicEnergy(0.0f);
    CHECK_EQ(em.CastSpell(1, o, d), -1);
    CHECK_APROX(em.GetPlayerMana(), 100.0f, 0.001f);
    PASS(); return 0; END_TEST();
}

static int test_cast_spell_rejects_non_spell_def() {
    TEST("CastSpell refuses a slot bound to a non-spell def");
    SetupPlayerAndSpell("NotASpellWeapon", EntityType::WEAPON, -1.0f, -1.0f, nullptr, 0.1f);
    auto& em = LightningEntityManager::Instance();
    em.SetPlayerMana(100.0f);

    // Bind a WEAPON def into a spell slot. Casting must refuse: the type gate is
    // what stops a weapon's magazine/stamina semantics leaking into a cast.
    em.SetSpellInSlot(3, "NotASpellWeapon");
    const Vector3 o = {0, 0, 0}, d = {0, 0, -1};
    CHECK_EQ(em.CastSpell(3, o, d), -1);
    CHECK_APROX(em.GetPlayerMana(), 100.0f, 0.001f);
    PASS(); return 0; END_TEST();
}

static int test_cast_time_defers_cost_to_resolve() {
    TEST("cast_time: wind-up spends nothing until it resolves");
    SetupPlayerAndSpell("ChargeBolt", EntityType::SPELL, 12.0f, -1.0f, nullptr, 0.5f, 0.4f);
    auto& em = LightningEntityManager::Instance();
    em.SetPlayerMana(100.0f);
    em.SetSpellInSlot(0, "ChargeBolt");
    const Vector3 o = {0, 0, 0}, d = {0, 0, -1};

    // Accepted, but nothing is spent and no cooldown starts while it charges.
    CHECK(em.CastSpell(0, o, d) != -1);
    CHECK(em.IsCasting(0));
    CHECK_APROX(em.GetPlayerMana(), 100.0f, 0.001f);
    CHECK(em.GetSpellCooldown(0) <= 0.0f);

    // A second press during the wind-up is refused: no queued cast, no double
    // spend.
    CHECK_EQ(em.CastSpell(0, o, d), -1);
    CHECK_APROX(em.GetPlayerMana(), 100.0f, 0.001f);

    // Halfway: still charging, still nothing spent.
    em.Update(0.2f);
    CHECK(em.IsCasting(0));
    CHECK_APROX(em.GetPlayerMana(), 100.0f, 0.001f);

    // Past cast_time: resolved, spent, cooldown started, not casting any more.
    em.Update(0.25f);
    CHECK(!em.IsCasting(0));
    CHECK_APROX(em.GetPlayerMana(), 88.0f, 0.001f);
    CHECK(em.GetSpellCooldown(0) > 0.0f);
    PASS(); return 0; END_TEST();
}

static int test_cast_time_cancels_when_unaffordable_at_resolve() {
    TEST("cast_time: a cast that becomes unaffordable resolves as a cancel");
    SetupPlayerAndSpell("DrainBolt", EntityType::SPELL, 50.0f, -1.0f, nullptr, 0.5f, 0.3f);
    auto& em = LightningEntityManager::Instance();
    em.SetPlayerMana(60.0f);
    em.SetSpellInSlot(0, "DrainBolt");
    const Vector3 o = {0, 0, 0}, d = {0, 0, -1};

    CHECK(em.CastSpell(0, o, d) != -1);
    // Something else eats the pool during the wind-up.
    em.SetPlayerMana(10.0f);
    em.Update(0.35f);
    // Cancelled: nothing spent (it could not be), no cooldown, no charge left.
    CHECK(!em.IsCasting(0));
    CHECK_APROX(em.GetPlayerMana(), 10.0f, 0.001f);
    CHECK(em.GetSpellCooldown(0) <= 0.0f);
    PASS(); return 0; END_TEST();
}

static int test_spell_bar_serialize_roundtrip() {
    TEST("The spell bar round-trips through SerializeState/DeserializeState");
    SetupPlayerAndSpell("RoundBolt", EntityType::SPELL, 10.0f, -1.0f, nullptr, 0.5f);
    auto& em = LightningEntityManager::Instance();

    em.SetSpellInSlot(0, "RoundBolt");
    em.SetSpellInSlot(4, "");
    const std::string blob = em.SerializeState();
    CHECK(blob.find("spells=") != std::string::npos);

    // Init() wipes the bar...
    em.Init();
    CHECK(em.GetSpellInSlot(0) == "");

    // ...and restoring the blob brings it back.
    em.DeserializeState(blob);
    CHECK(em.GetSpellInSlot(0) == "RoundBolt");
    CHECK(em.GetSpellInSlot(1).empty());

    // A blob with no `spells=` section (an old save) must CLEAR the bar rather
    // than leave whatever the process had bound.
    em.SetSpellInSlot(0, "RoundBolt");
    em.DeserializeState("hotbar:|equip:|skills=|");
    CHECK(em.GetSpellInSlot(0) == "");
    PASS(); return 0; END_TEST();
}

// Regression: m_activeEnvZoneDef was written on apply and read by NOTHING, so
// there was no duplicate-enter guard at all - a second zone.enter re-pended
// fog/ambient and restarted the world track - and no exit path cleared it, so
// "re-enter this zone" and "duplicate enter" were indistinguishable. The guard
// is now read on enter, cleared by that def's zone.exit (name-matched) and by
// Init on world load.
static int test_zone_env_guard_single_application() {
    TEST("zone.enter applies once; zone.exit clears the guard; re-enter re-applies");
    auto& reg = LightningEntityRegistry::Instance();
    EntityDef z;
    z.name = "ZoneEnvGuardTest";
    z.type = EntityType::SKYZONE;
    // vec3s holds a raw float[3]; brace-init into it is not a valid assignment.
    z.stats.vec3s["fog_color"][0] = 0.2f;
    z.stats.vec3s["fog_color"][1] = 0.3f;
    z.stats.vec3s["fog_color"][2] = 0.4f;
    z.stats.floats["fog_density"] = 0.05f;
    z.music = "guard_test_track";
    reg.Register(z);

    auto& em = LightningEntityManager::Instance();
    em.Init();  // world-load reset: pending flags + applied-zone guard cleared
    CHECK(em.ActiveEnvZoneDef().empty());

    oz::events::Event ev;
    ev.topic = "zone.enter";
    ev.s0 = "ZoneEnvGuardTest";

    // First enter applies.
    em.DispatchBusEvent(ev);
    CHECK(em.HasPendingFog());
    CHECK(em.HasPendingMusic());
    CHECK_APROX(em.PendingFogDensity(), 0.05f, 0.001f);
    CHECK(em.ActiveEnvZoneDef() == "ZoneEnvGuardTest");

    // A duplicate enter with no exit in between is swallowed by the guard.
    em.ClearPendingFog();
    em.ClearPendingMusic();
    em.DispatchBusEvent(ev);
    CHECK(!em.HasPendingFog());
    CHECK(!em.HasPendingMusic());

    // An exit for a DIFFERENT zone must not clear this zone's guard: whose env
    // is on screen is decided by name match, not by "an exit happened".
    oz::events::Event otherExit;
    otherExit.topic = "zone.exit";
    otherExit.s0 = "SomeOtherZone";
    em.DispatchBusEvent(otherExit);
    CHECK(em.ActiveEnvZoneDef() == "ZoneEnvGuardTest");
    em.DispatchBusEvent(ev);
    CHECK(!em.HasPendingFog());  // still guarded

    // This zone's own exit clears it...
    oz::events::Event ex;
    ex.topic = "zone.exit";
    ex.s0 = "ZoneEnvGuardTest";
    em.DispatchBusEvent(ex);
    CHECK(em.ActiveEnvZoneDef().empty());

    // ...so re-entering applies again (a guard that never lets go would make
    // every revisit of a zone keep the previous zone's fog).
    em.DispatchBusEvent(ev);
    CHECK(em.HasPendingFog());
    CHECK(em.ActiveEnvZoneDef() == "ZoneEnvGuardTest");
    PASS(); return 0; END_TEST();
}

// Phase F: play_clip has two legs. The world-pawn leg (PlayPawnClip) existed;
// the first-person view-model leg did not, so a weapon's script could never
// animate the held copy. When the action runs on the weapon currently held,
// the "clip" command is also queued into the pending world commands for
// Core.hpp to route to ViewModel::PlayClip. The def match is made HERE
// because ScriptCommand carries no def: a "clip" command that reaches Core
// always means "play on the view-model". Without the match, every other
// entity's play_clip would animate the held weapon.
static int test_play_clip_queues_viewmodel_command() {
    TEST("play_clip on the held weapon queues the view-model command");
    auto& reg = LightningEntityRegistry::Instance();
    EntityDef def;
    def.name = "ClipRouteGun";
    def.type = EntityType::WEAPON;
    def.stats.floats["fire_rate"] = 0.05f;
    EntityAction act;
    act.name = "on_fire";
    act.scriptLines.push_back("play_clip \"Reload\"");
    def.actions.push_back(act);
    reg.Register(def);

    auto& em = LightningEntityManager::Instance();
    em.Init();
    int idx = em.Spawn("ClipRouteGun");
    CHECK(idx >= 0);
    em.HotbarAssign(0, idx);
    em.SelectSlot(0);
    em.TakePendingWorldCommands();       // flush anything queued by Init/Spawn

    Vector3 origin = {0,0,0}, dir = {0,0,-1};
    em.FireSelectedWeapon(origin, dir);  // runs on_fire on the held weapon
    auto cmds = em.TakePendingWorldCommands();
    CHECK(cmds.size() == 1);
    CHECK(cmds[0].op == "clip");
    CHECK(cmds[0].s0 == "Reload");

    // Negative: the same action on a NON-held def must not queue — the
    // command carries no def, so an unconditional push would animate the
    // held weapon whenever any other entity played a clip.
    EntityDef other;
    other.name = "ClipRouteOther";
    other.type = EntityType::WEAPON;
    EntityAction act2;
    act2.name = "on_fire";
    act2.scriptLines.push_back("play_clip \"Idle\"");
    other.actions.push_back(act2);
    reg.Register(other);
    em.TriggerEntityAction(&other, "on_fire");   // runs on its own instance
    auto cmds2 = em.TakePendingWorldCommands();
    CHECK(cmds2.empty());
    PASS(); return 0; END_TEST();
}

int main() {
    fprintf(stdout, "LightningEntityManager Tests\n");
    fprintf(stdout, "============================\n");
    int failures = 0;
    failures += test_init();
    failures += test_hotbar();
    failures += test_spawn_unknown();
    failures += test_hotbar_out_of_range();
    failures += test_select_slot_out_of_range();
    failures += test_wheel_cycle_skips_empty();
    failures += test_wheel_cycle_all_empty();
    failures += test_runaction_drains_side_effects();
    failures += test_set_cooldown_is_one_shot();
    failures += test_selected_entity_empty();
    failures += test_equipment_assign();
    failures += test_equipment_find_free();
    failures += test_equipment_clear();
    failures += test_equipment_swap();
    failures += test_equipment_out_of_range();
    failures += test_serialize_format();
    failures += test_deserialize_empty();
    failures += test_deserialize_roundtrip();
    failures += test_run_action_no_crash();
    failures += test_player_stats_default();
    failures += test_reload_started_flag();
    failures += test_melee_stamina();
    failures += test_hotbar_drag_reorder();
    failures += test_stashed_weapon_drop();
    failures += test_multi_despawn_cycles();
    failures += test_deserialize_reuses_instances();
    failures += test_fire_no_weapon();
    failures += test_fire_not_a_weapon();
    failures += test_fire_cooldown();
    failures += test_fire_ranged_weapon();
    failures += test_fire_melee_weapon();
    failures += test_ammo_init_and_decrement();
    failures += test_auto_reload_on_empty();
    failures += test_reload_selected();
    failures += test_reload_no_magazine();
    failures += test_reload_full_ammo();
    failures += test_equipment_slot_name();
    failures += test_auto_equip_and_defense();
    failures += test_auto_equip_replaces_same_slot();
    failures += test_respec_skills();
    // Registers a SKYZONE def (no "Player" involvement) - safe before the
    // ordering-sensitive tail.
    failures += test_zone_env_guard_single_application();
    // Registers WEAPON defs only - also safe before the tail.
    failures += test_play_clip_queues_viewmodel_command();
    // Last: these register a "Player" def that Init() spawns, and the registry
    // is a process-wide singleton, so running them earlier would change the
    // player-instance assumptions of the tests above.
    failures += test_variant_selected_by_level();
    failures += test_no_variants_leaves_index();
    // Last of all: registers a "Player" def permanently (no Unregister exists), so
    // it must follow every other test that assumes Init() spawns nothing.
    failures += test_despawn_clears_player_index();
    // Spell tests: need a live Player entity for pool spend/refusal, so they run
    // after the test above has registered "Player".
    failures += test_spell_bar_bind_and_init_clear();
    failures += test_cast_spell_spends_mana_and_sets_cooldown();
    failures += test_cast_time_defers_cost_to_resolve();
    failures += test_cast_time_cancels_when_unaffordable_at_resolve();
    failures += test_cast_spell_uses_psychic_energy();
    failures += test_cast_spell_rejects_non_spell_def();
    failures += test_spell_bar_serialize_roundtrip();

    fprintf(stdout, "============================\n");
    fprintf(stdout, "%d/%d passed, %d failed\n",
            tests_passed, tests_total, tests_total - tests_passed);
    return failures;
}
