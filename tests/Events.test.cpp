// Events / objectives / triggers — Phase-1 engine-generic systems.
// Raylib-free, so it links headlessly (see the Makefile rule).

#include "../Source/Events/EventBus.hpp"
#include "../Source/Events/ObjectiveSystem.hpp"
#include "../Source/Events/TriggerSystem.hpp"

#include <cstdio>
#include <string>
#include <vector>

static int tests_total = 0, tests_passed = 0;
#define TEST(name) do { tests_total++; fprintf(stdout, "  TEST: %s ... ", name);
#define PASS() do { tests_passed++; fprintf(stdout, "PASS\n"); } while(0)
#define CHECK(cond) do { if (!(cond)) { fprintf(stdout, "FAIL: %s\n", #cond); return 1; } } while(0)
#define CHECK_EQ(a, b) do { if ((a) != (b)) { fprintf(stdout, "FAIL: expected %d, got %d\n", (int)(b), (int)(a)); return 1; } } while(0)
#define CHECK_STR(a, b) do { if ((a) != (b)) { fprintf(stdout, "FAIL: expected '%s', got '%s'\n", (b).c_str(), (a).c_str()); return 1; } } while(0)
#define END_TEST() } while(0)

using namespace oz::events;

// --- EventBus ---------------------------------------------------------------
static std::vector<std::string> g_seen;
static void RecordHandler(const Event& ev, void* /*user*/) { g_seen.push_back(ev.topic); }

static int test_eventbus_exact_and_wildcard() {
    TEST("EventBus exact + wildcard dispatch, unsubscribe");
    auto& bus = EventBus::Instance();
    bus.Clear();
    g_seen.clear();

    bus.Subscribe("a.b", &RecordHandler, nullptr);
    bus.Subscribe("*", &RecordHandler, (void*)0x1);

    Event ev; ev.topic = "a.b"; bus.Emit(ev);          // both fire
    ev.topic = "c.d"; bus.Emit(ev);                    // only wildcard
    CHECK_EQ(g_seen.size(), 3);

    bus.Unsubscribe((void*)0x1);                       // drop the wildcard
    g_seen.clear();
    ev.topic = "c.d"; bus.Emit(ev);
    CHECK_EQ(g_seen.size(), 0);
    ev.topic = "a.b"; bus.Emit(ev);
    CHECK_EQ(g_seen.size(), 1);
    PASS(); return 0; END_TEST();
}

// --- ObjectiveSystem --------------------------------------------------------
static std::vector<std::string> g_objTopics;
static void ObjHandler(const Event& ev, void* /*user*/) { g_objTopics.push_back(ev.topic); }

static int test_objectives() {
    TEST("ObjectiveSystem progress, completion, flags, serialize");
    auto& objs = ObjectiveSystem::Instance();
    objs.Reset();
    auto& bus = EventBus::Instance();
    bus.Clear();
    bus.Subscribe("*", &ObjHandler, nullptr);
    g_objTopics.clear();

    objs.Define("relic", 3);
    CHECK(!objs.Add("relic", 1));       // 1/3, not complete
    CHECK(!objs.IsComplete("relic"));
    CHECK(!objs.Add("relic", 1));       // 2/3
    CHECK(objs.Add("relic", 1));        // 3/3 -> completes
    CHECK(objs.IsComplete("relic"));
    // Add() emitted progress x3 + complete x1
    int completes = 0, progresses = 0;
    for (auto& t : g_objTopics) {
        if (t == "objective.complete") completes++;
        if (t == "objective.progress") progresses++;
    }
    CHECK_EQ(completes, 1);
    CHECK_EQ(progresses, 3);
    // A completed objective does not advance again.
    CHECK(!objs.Add("relic", 1));

    objs.SetWorldFlag("gate", 1);
    CHECK_EQ(objs.GetWorldFlag("gate"), 1);
    CHECK_EQ(objs.GetWorldFlag("missing", 42), 42);

    // Round-trip.
    std::string blob = objs.Serialize();
    objs.Reset();
    CHECK_EQ(objs.GetWorldFlag("gate"), 0);
    objs.Deserialize(blob);
    CHECK(objs.IsComplete("relic"));
    CHECK_EQ(objs.GetWorldFlag("gate"), 1);
    PASS(); return 0; END_TEST();
}

// --- TriggerSystem ----------------------------------------------------------
static int test_triggers() {
    TEST("TriggerSystem query, enter/exit, once latch");
    auto& trig = TriggerSystem::Instance();
    trig.Clear();
    auto& bus = EventBus::Instance();
    bus.Clear();
    g_seen.clear();
    bus.Subscribe("trigger.enter", &RecordHandler, nullptr);
    bus.Subscribe("trigger.exit", &RecordHandler, nullptr);

    TriggerVolume v;
    v.defName = "Trigger_EnterVolume";
    v.minX = 0; v.minY = 0; v.minZ = 0;
    v.maxX = 10; v.maxY = 10; v.maxZ = 10;
    trig.AddTrigger(v);

    std::vector<int> inside;
    trig.QueryContaining(5, 5, 5, inside);
    CHECK_EQ(inside.size(), 1);
    trig.QueryContaining(50, 50, 50, inside);
    CHECK_EQ(inside.size(), 0);

    // Outside -> inside -> outside
    trig.UpdateForPoint(5, 5, 5);
    CHECK_EQ(g_seen.size(), 1);
    CHECK_STR(g_seen[0], std::string("trigger.enter"));
    trig.UpdateForPoint(5, 5, 5);       // still inside: no new event
    CHECK_EQ(g_seen.size(), 1);
    trig.UpdateForPoint(50, 50, 50);    // exit
    CHECK_EQ(g_seen.size(), 2);
    CHECK_STR(g_seen[1], std::string("trigger.exit"));

    // `once`: a second entry emits nothing.
    TriggerSystem::Instance().Clear();
    bus.Clear();
    g_seen.clear();
    bus.Subscribe("trigger.enter", &RecordHandler, nullptr);
    TriggerVolume once = v;
    once.once = true;
    trig.AddTrigger(once);
    trig.UpdateForPoint(5, 5, 5);       // enter
    trig.UpdateForPoint(50, 50, 50);    // exit
    trig.UpdateForPoint(5, 5, 5);       // enter again -> suppressed by `once`
    CHECK_EQ(g_seen.size(), 1);
    PASS(); return 0; END_TEST();
}

int main() {
    fprintf(stdout, "Events Tests:\n");
    int failures = 0;
    failures += test_eventbus_exact_and_wildcard();
    failures += test_objectives();
    failures += test_triggers();
    fprintf(stdout, "\n%d/%d tests passed.\n", tests_passed, tests_total);
    return failures;
}
