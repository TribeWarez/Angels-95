// GameManager — Phase-2 game-hierarchy loading (registry -> GameManager).
// Integration test: it scans the real GameData hierarchy through the registry.
// Needs RAYLIB_INC to compile (the registry pulls raylib.h) but links no raylib.

#include "../Source/Script/LightningEntityRegistry.hpp"
#include "../Source/Game/GameManager.hpp"
#include "../Source/Events/ObjectiveSystem.hpp"

#include <cstdio>
#include <string>

static int tests_total = 0, tests_passed = 0;
#define TEST(name) do { tests_total++; fprintf(stdout, "  TEST: %s ... ", name);
#define PASS() do { tests_passed++; fprintf(stdout, "PASS\n"); } while(0)
#define CHECK(cond) do { if (!(cond)) { fprintf(stdout, "FAIL: %s\n", #cond); return 1; } } while(0)
#define CHECK_EQ(a, b) do { if ((a) != (b)) { fprintf(stdout, "FAIL: expected %d, got %d\n", (int)(b), (int)(a)); return 1; } } while(0)
#define END_TEST() } while(0)

static int test_hierarchy_loads() {
    TEST("hierarchy loads from GameData/Game");
    auto& gm = oz::game::GameManager::Instance();
    CHECK(gm.Loaded());
    CHECK(gm.Sectors().size() >= 5);
    CHECK(gm.Missions().size() >= 14);
    CHECK_EQ((int)gm.Campaign().sectors.size(), 5);
    PASS(); return 0; END_TEST();
}

static int test_known_sector_mission() {
    TEST("known sector/mission resolve");
    auto& gm = oz::game::GameManager::Instance();
    const oz::game::SectorDef* sec = gm.FindSector("Sefirah_1_TerranPrime");
    CHECK(sec != nullptr);
    CHECK_EQ(sec->numerology, 1);
    const oz::game::MissionDef* m = gm.FindMission("Sektor_01_SilentPines");
    CHECK(m != nullptr);
    CHECK(m->world == "Sektor_01_SilentPines");
    CHECK(m->mode == "campaign");
    PASS(); return 0; END_TEST();
}

static int test_active_mission() {
    TEST("start mission world + SetActiveMission seeds objectives");
    auto& gm = oz::game::GameManager::Instance();
    CHECK(!gm.FirstMissionWorld().empty());
    std::string world = gm.SetActiveMission("Sektor_01_SilentPines");
    CHECK(world == "Sektor_01_SilentPines");
    CHECK(gm.ActiveMission() == "Sektor_01_SilentPines");
    CHECK(gm.ActiveSector() == "Sefirah_1_TerranPrime");
    const oz::events::ObjectiveState* ob =
        oz::events::ObjectiveSystem::Instance().Get("Objective_ClearArea");
    CHECK(ob != nullptr);
    CHECK_EQ(ob->target, 5);
    PASS(); return 0; END_TEST();
}

static int test_campaign_state_roundtrip() {
    TEST("campaign state round-trips");
    auto& gm = oz::game::GameManager::Instance();
    std::string blob = gm.SerializeState();
    gm.SetActiveMission("Sektor_02_MegacityGrid");
    CHECK(gm.DeserializeState(blob));
    CHECK(gm.ActiveMission() == "Sektor_01_SilentPines");
    PASS(); return 0; END_TEST();
}

int main() {
    fprintf(stdout, "GameManager Tests:\n");
    LightningEntityRegistry::Instance().Init();
    oz::game::GameManager::Instance().Init();

    int failures = 0;
    failures += test_hierarchy_loads();
    failures += test_known_sector_mission();
    failures += test_active_mission();
    failures += test_campaign_state_roundtrip();
    fprintf(stdout, "\n%d/%d tests passed.\n", tests_passed, tests_total);
    return failures;
}
