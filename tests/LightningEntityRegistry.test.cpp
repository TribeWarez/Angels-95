// LightningEntityRegistry unit tests
// Compile: g++ -O0 -g --std=c++20 -I ../Source -DOMEGA_TEST_ENV LightngEntityRegistry.test.cpp ../Source/Script/LightningEntityRegistry.cpp ../Source/Script/LightningScriptParser.cpp ../Source/Script/LightningScriptContext.cpp ../Source/Log.cpp -o test_registry

#include "../Source/Script/LightningEntityRegistry.hpp"
#include "../Source/Script/LightningScriptParser.hpp"
#include <cstdio>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

static int tests_total = 0, tests_passed = 0;
#define TEST(name) do { tests_total++; fprintf(stdout, "  TEST: %s ... ", name);
#define PASS() do { tests_passed++; fprintf(stdout, "PASS\n"); } while(0)
#define FAIL(msg) do { fprintf(stdout, "FAIL: %s\n", msg); return 1; } while(0)
#define CHECK(cond) do { if (!(cond)) { fprintf(stdout, "FAIL: %s\n", #cond); return 1; } } while(0)
#define CHECK_EQ(a, b) do { if ((a) != (b)) { fprintf(stdout, "FAIL: expected %d, got %d\n", (int)(a), (int)(b)); return 1; } } while(0)
#define CHECK_PTR(ptr) do { if (!(ptr)) { fprintf(stdout, "FAIL: expected non-null pointer\n"); return 1; } } while(0)
#define CHECK_NULL(ptr) do { if ((ptr)) { fprintf(stdout, "FAIL: expected null pointer\n"); return 1; } } while(0)
#define END_TEST() } while(0)

static int test_register_and_find() {
    TEST("Register and Find by name");
    auto& reg = LightningEntityRegistry::Instance();
    // Clear by re-initializing (simulate fresh state)
    // We can't easily clear, so use a unique name
    EntityDef def;
    def.name = "test_sword";
    def.type = EntityType::WEAPON;
    def.stats.floats["damage"] = 30.0f;
    CHECK(reg.Register(def));
    const EntityDef* found = reg.Find("test_sword");
    CHECK_PTR(found);
    CHECK(found->name == "test_sword");
    CHECK(found->type == EntityType::WEAPON);
    PASS(); return 0; END_TEST();
}

static int test_find_non_existent() {
    TEST("Find non-existent entity returns nullptr");
    auto& reg = LightningEntityRegistry::Instance();
    CHECK_NULL(reg.Find("NonExistentEntity12345"));
    PASS(); return 0; END_TEST();
}

static int test_find_by_type() {
    TEST("FindByType returns correct subset");
    auto& reg = LightningEntityRegistry::Instance();
    EntityDef def1, def2, def3;
    def1.name = "type_test_weapon";
    def1.type = EntityType::WEAPON;
    def2.name = "type_test_armor";
    def2.type = EntityType::ARMOR;
    def3.name = "type_test_weapon2";
    def3.type = EntityType::WEAPON;
    reg.Register(def1);
    reg.Register(def2);
    reg.Register(def3);
    std::vector<const EntityDef*> weapons;
    reg.FindByType(EntityType::WEAPON, weapons);
    CHECK(weapons.size() >= 2);
    bool found1 = false, found3 = false;
    for (auto* d : weapons) {
        if (d->name == "type_test_weapon") found1 = true;
        if (d->name == "type_test_weapon2") found3 = true;
    }
    CHECK(found1);
    CHECK(found3);
    PASS(); return 0; END_TEST();
}

static int test_register_duplicate() {
    TEST("Register duplicate overwrites existing");
    auto& reg = LightningEntityRegistry::Instance();
    EntityDef def1, def2;
    def1.name = "dup_test";
    def1.type = EntityType::WEAPON;
    def1.stats.floats["damage"] = 10.0f;
    reg.Register(def1);
    const EntityDef* first = reg.Find("dup_test");
    CHECK_PTR(first);
    float firstDamage = first->stats.floats.at("damage");
    // Register updated version
    def2.name = "dup_test";
    def2.type = EntityType::ARMOR;
    def2.stats.floats["defense"] = 20.0f;
    reg.Register(def2);
    const EntityDef* second = reg.Find("dup_test");
    CHECK_PTR(second);
    CHECK(second->type == EntityType::ARMOR);
    PASS(); return 0; END_TEST();
}

static int test_count_increases() {
    TEST("Count increases after Register");
    auto& reg = LightningEntityRegistry::Instance();
    int before = reg.Count();
    EntityDef def;
    def.name = "count_test_item";
    def.type = EntityType::PICKUP;
    reg.Register(def);
    CHECK(reg.Count() > before);
    PASS(); return 0; END_TEST();
}

static int test_parse_and_register() {
    TEST("Parse then Register produces findable entity");
    std::string ozls = R"(
        entity "parsed_rifle" : weapon {
            stats { damage = 35 fire_rate = 0.2 }
        }
    )";
    EntityDef parsed = LightningScriptParser::Parse(ozls, "parsed.ozls");
    CHECK(parsed.name == "parsed_rifle");
    auto& reg = LightningEntityRegistry::Instance();
    reg.Register(parsed);
    const EntityDef* found = reg.Find("parsed_rifle");
    CHECK_PTR(found);
    CHECK(found->type == EntityType::WEAPON);
    PASS(); return 0; END_TEST();
}

static int test_get_all_keys() {
    TEST("GetAll contains registered names");
    auto& reg = LightningEntityRegistry::Instance();
    EntityDef def;
    def.name = "getall_test_key";
    def.type = EntityType::CONSUMABLE;
    reg.Register(def);
    const auto& all = reg.GetAll();
    CHECK(all.find("getall_test_key") != all.end());
    PASS(); return 0; END_TEST();
}

// LoadWorldOverrides must classify by TYPE first: game-hierarchy and
// trigger/event defs are global even when their source path sits inside a
// world folder. A world-scoped def of another world is still dropped.
static int test_load_world_overrides_type_classification() {
    TEST("LoadWorldOverrides: hierarchy def survives, world-scoped def dropped");
    auto& reg = LightningEntityRegistry::Instance();

    // A mission def "parsed from" ANOTHER world's folder. Type-based
    // classification makes it global, so it must survive a load of TestWorld.
    EntityDef mission;
    mission.name = "testreg_mission";
    mission.type = EntityType::MISSION;
    mission.sourcePath = "GameData/Worlds/OtherWorld/Missions/testreg_mission.ozls";
    reg.Register(mission);

    // Same source path, but a world-scoped type the current world does not
    // define -> must be dropped.
    EntityDef zone;
    zone.name = "testreg_otherworld_zone";
    zone.type = EntityType::SKYZONE;
    zone.sourcePath = "GameData/Worlds/OtherWorld/zones/testreg_otherworld_zone.ozls";
    reg.Register(zone);

    reg.LoadWorldOverrides("GameData/Worlds/TestWorld");

    CHECK_PTR(reg.Find("testreg_mission"));
    CHECK(reg.Find("testreg_mission")->type == EntityType::MISSION);
    CHECK_NULL(reg.Find("testreg_otherworld_zone"));
    PASS(); return 0; END_TEST();
}

// A trigger def referenced by OZONE `trigger <def> ...` lines from any world
// is global too, even from a world-folder path.
static int test_load_world_overrides_trigger_is_global() {
    TEST("LoadWorldOverrides: trigger def from a world folder survives");
    auto& reg = LightningEntityRegistry::Instance();
    EntityDef trig;
    trig.name = "testreg_trigger";
    trig.type = EntityType::TRIGGER;
    trig.sourcePath = "GameData/Worlds/OtherWorld/testreg_trigger.ozls";
    reg.Register(trig);
    reg.LoadWorldOverrides("GameData/Worlds/TestWorld");
    CHECK_PTR(reg.Find("testreg_trigger"));
    PASS(); return 0; END_TEST();
}

// A world-scoped name dropped by a previous world's load must come BACK when its
// own world is opened. The promote/drop loop can only re-point names already in
// the by-name map, so before the resurrection pass a name that only WorldB
// defines was unreachable for the rest of the session after one visit to WorldA.
static int test_load_world_overrides_restores_dropped_name() {
    TEST("LoadWorldOverrides: a dropped world's def returns with its world");
    auto& reg = LightningEntityRegistry::Instance();

    EntityDef zone;
    zone.name = "testreg_zone_worldb";
    zone.type = EntityType::SKYZONE;
    zone.sourcePath = "GameData/Worlds/WorldB/testreg_zone_worldb.ozls";
    reg.Register(zone);

    reg.LoadWorldOverrides("GameData/Worlds/WorldA");
    CHECK_NULL(reg.Find("testreg_zone_worldb"));

    reg.LoadWorldOverrides("GameData/Worlds/WorldB");
    CHECK_PTR(reg.Find("testreg_zone_worldb"));
    PASS(); return 0; END_TEST();
}

// Editor hot reload: ReloadFile replaces a file's defs in place. The pointer
// identity assertion is the load-bearing one - LightningEntityManager instances
// and SkyZoneNode::def hold the address Find() returned, so a reload that
// re-inserted the def would leave them observing a different (or freed) entry.
static int test_reload_file_edits_in_place() {
    TEST("ReloadFile: edits a def in place and keeps the Find() pointer");
    auto& reg = LightningEntityRegistry::Instance();

    std::error_code ec;
    const fs::path dir = fs::temp_directory_path() / "angels95_registry_reload";
    fs::create_directories(dir, ec);
    const fs::path file = dir / "reload_probe.ozls";

    auto write = [&](const char* body) {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << body;
    };

    write("entity \"reload_probe\" : weapon {\n"
          "    stats { damage = 5 }\n"
          "}\n");

    auto firstResult = reg.ReloadFile(file.string());
    CHECK(firstResult.ok);
    CHECK_EQ(firstResult.added.size(), 1);
    const EntityDef* first = reg.Find("reload_probe");
    CHECK_PTR(first);
    CHECK(first->stats.floats.at("damage") == 5.0f);

    write("entity \"reload_probe\" : weapon {\n"
          "    stats { damage = 42 }\n"
          "}\n");

    auto secondResult = reg.ReloadFile(file.string());
    CHECK(secondResult.ok);
    CHECK_EQ(secondResult.changed.size(), 1);
    const EntityDef* second = reg.Find("reload_probe");
    CHECK(second == first);                     // same registry node
    CHECK(second->stats.floats.at("damage") == 42.0f);

    // A half-written / broken file must keep the last good definition: an editor
    // saving mid-edit must not make the entity vanish from the panel.
    write("this is not an entity\n");
    auto broken = reg.ReloadFile(file.string());
    CHECK(!broken.ok);
    CHECK_PTR(reg.Find("reload_probe"));
    CHECK(reg.Find("reload_probe")->stats.floats.at("damage") == 42.0f);

    // Deleting the file IS a removal.
    fs::remove(file, ec);
    auto removed = reg.ReloadFile(file.string());
    CHECK(removed.ok);
    CHECK_EQ(removed.removed.size(), 1);
    CHECK_NULL(reg.Find("reload_probe"));

    fs::remove_all(dir, ec);
    PASS(); return 0; END_TEST();
}

int main() {
    fprintf(stdout, "LightningEntityRegistry Tests:\n");
    fprintf(stdout, "==============================\n");
    int failures = 0;
    failures += test_register_and_find();
    failures += test_find_non_existent();
    failures += test_find_by_type();
    failures += test_register_duplicate();
    failures += test_count_increases();
    failures += test_parse_and_register();
    failures += test_get_all_keys();
    failures += test_load_world_overrides_type_classification();
    failures += test_load_world_overrides_trigger_is_global();
    failures += test_load_world_overrides_restores_dropped_name();
    failures += test_reload_file_edits_in_place();
    fprintf(stdout, "==============================\n");
    fprintf(stdout, "%d/%d passed, %d failed\n",
            tests_passed, tests_total, tests_total - tests_passed);
    return failures;
}
