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
    // The mission's OWN objective, resolved from its def. Pinned through the
    // mission rather than a hard-coded id: the old test named the scaffold
    // Objective_ClearArea and would pass only while every mission shared it.
    const oz::game::MissionDef* m = gm.FindMission("Sektor_01_SilentPines");
    CHECK(m != nullptr && !m->objectives.empty());
    const std::string objId =
        (m && !m->objectives.empty()) ? m->objectives.front() : std::string();
    const oz::events::ObjectiveState* ob =
        oz::events::ObjectiveSystem::Instance().Get(objId);
    CHECK(ob != nullptr);
    if (ob) CHECK_EQ(ob->target, 4);   // Objective_S01_GorgeWatch count
    // The local store carries the def's type/target so single-player filters
    // kills with the same rule the server applies.
    CHECK(!gm.ActiveObjectives().empty());
    if (!gm.ActiveObjectives().empty()) {
        CHECK(gm.ActiveObjectives()[0].type == "kill");
        CHECK(gm.ActiveObjectives()[0].target == "Walker");
    }
    PASS(); return 0; END_TEST();
}

static int test_campaign_state_roundtrip() {
    TEST("campaign state round-trips");
    auto& gm = oz::game::GameManager::Instance();
    std::string blob = gm.SerializeState();
    // v1 layout: version line first so a future format is detectable.
    CHECK(blob.rfind("campaign=1\nmission=", 0) == 0);
    gm.SetActiveMission("Sektor_02_MegacityGrid");
    CHECK(gm.DeserializeState(blob));
    CHECK(gm.ActiveMission() == "Sektor_01_SilentPines");
    PASS(); return 0; END_TEST();
}

static int test_campaign_save_versioning() {
    TEST("legacy format loads, unknown version rejected, extra keys ignored");
    auto& gm = oz::game::GameManager::Instance();

    // Legacy v0: the whole file is one mission= line (pre-versioning saves).
    CHECK(gm.DeserializeState("mission=Sektor_02_MegacityGrid"));
    CHECK(gm.ActiveMission() == "Sektor_02_MegacityGrid");

    // CRLF tolerance (a save written/read across platforms).
    CHECK(gm.DeserializeState("campaign=1\r\nmission=Sektor_03_AegisBaseAlpha\r\n"));
    CHECK(gm.ActiveMission() == "Sektor_03_AegisBaseAlpha");

    // Unknown version: reject, do not guess, leave state untouched.
    CHECK(!gm.DeserializeState("campaign=99\nmission=Sektor_01_SilentPines"));
    CHECK(gm.ActiveMission() == "Sektor_03_AegisBaseAlpha");

    // Future additive keys on a known version are ignored.
    CHECK(gm.DeserializeState("campaign=1\nfuture_key=42\nmission=Sektor_01_SilentPines"));
    CHECK(gm.ActiveMission() == "Sektor_01_SilentPines");

    // Garbage / empty: rejected, as before.
    CHECK(!gm.DeserializeState(""));
    CHECK(!gm.DeserializeState("not a save file"));
    PASS(); return 0; END_TEST();
}

static int test_local_objective_advance() {
    TEST("local objectives filter by target and by the fact that produced them");
    auto& gm = oz::game::GameManager::Instance();
    gm.SetActiveMission("Sektor_01_SilentPines");
    auto& objs = oz::events::ObjectiveSystem::Instance();
    const std::string id =
        gm.ActiveObjectives().empty() ? std::string() : gm.ActiveObjectives()[0].id;
    CHECK(!id.empty());

    gm.AdvanceLocalObjectives("kill", "Brute");          // wrong NPC def
    CHECK(objs.Get(id) != nullptr && objs.Get(id)->progress == 0);
    gm.AdvanceLocalObjectives("collect", "HealthVial");  // wrong fact type
    CHECK(objs.Get(id) != nullptr && objs.Get(id)->progress == 0);
    gm.AdvanceLocalObjectives("kill", "Walker");
    CHECK(objs.Get(id) != nullptr && objs.Get(id)->progress == 1);
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
    failures += test_local_objective_advance();
    failures += test_campaign_state_roundtrip();
    failures += test_campaign_save_versioning();
    fprintf(stdout, "\n%d/%d tests passed.\n", tests_passed, tests_total);
    return failures;
}
