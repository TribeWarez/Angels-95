// FrameProfiler tests - standalone, raylib-free.
//   make test_profiler
//   g++ -std=c++20 -ISource tests/Profiler.test.cpp -o test_profiler
//
// The profiler is the first thing every later performance fix depends on, so the
// invariants that make its numbers trustworthy are pinned here:
//   - inclusive time covers children, self time excludes them;
//   - counters are per-frame + cumulative, with a peak;
//   - the ring reports the LAST kHistoryFrames, not the first;
//   - a genuinely slow frame is captured as a stutter report, a normal one is not;
//   - disabled costs nothing and records nothing;
//   - CSV output has a header and one row per recorded frame.
#include "../Source/Diagnostics/FrameProfiler.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { g_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

using namespace oz::diag;

static void ResetAndEnable() {
    Profiler().SetEnabled(true);
    Profiler().Reset();
}

// ---------------------------------------------------------------------------
// Nested scopes: outer >= inner, self excludes the child, calls aggregate.
// ---------------------------------------------------------------------------
static void TestScopeNesting() {
    printf("Profiler: nested scopes\n");
    ResetAndEnable();

    const int outer = Profiler().InternScope("test.outer");
    const int inner = Profiler().InternScope("test.inner");
    CHECK(outer >= 0 && inner >= 0);
    CHECK(Profiler().InternScope("test.outer") == outer);   // interning is stable
    CHECK(Profiler().InternScope("") == -1);

    for (int f = 0; f < 5; ++f) {
        Profiler().BeginFrame();
        {
            ScopeGuard a(outer);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            {
                ScopeGuard b(inner);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        ScopeGuard a(outer);   // second call in the same frame aggregates
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        Profiler().EndFrame();
    }

    const ScopeStat& o = Profiler().ScopeAt(outer);
    const ScopeStat& i = Profiler().ScopeAt(inner);
    CHECK(o.frameCalls == 2);
    CHECK(i.frameCalls == 1);
    CHECK(i.frameMs > 0.5);
    CHECK(o.frameMs >= i.frameMs);
    CHECK(o.frameSelfMs >= 0.0);
    CHECK(o.frameSelfMs <= o.frameMs);
    CHECK(o.peakMs >= i.peakMs);
    CHECK(Profiler().FramesRecorded() == 5);
    CHECK(Profiler().FrameMs() > 0.0);
    CHECK(Profiler().SmoothedFps() > 0.0);
}

// ---------------------------------------------------------------------------
// Counters: per-frame value resets, total accumulates, peak holds the max.
// ---------------------------------------------------------------------------
static void TestCounters() {
    printf("Profiler: counters\n");
    ResetAndEnable();
    const int c = Profiler().InternCounter("test.count");
    CHECK(c >= 0);

    for (int f = 0; f < 3; ++f) {
        Profiler().BeginFrame();
        Profiler().AddCounter(c, 1);
        Profiler().AddCounter(c, 5);
        Profiler().EndFrame();
    }

    const CounterStat& s = Profiler().CounterAt(c);
    CHECK(s.frame == 6);
    CHECK(s.peak == 6);
    CHECK(s.total == 18);
    CHECK(Profiler().CounterFrame("test.count") == 6);
    CHECK(Profiler().CounterFrame("does.not.exist") == 0);
}

// ---------------------------------------------------------------------------
// Ring history: after > kHistoryFrames frames only the latest window survives.
// ---------------------------------------------------------------------------
static void TestHistoryWindow() {
    printf("Profiler: history window\n");
    ResetAndEnable();
    for (int f = 0; f < kHistoryFrames + 60; ++f) {
        Profiler().BeginFrame();
        Profiler().EndFrame();
    }
    CHECK(Profiler().FramesRecorded() == kHistoryFrames);
    CHECK(Profiler().FrameIndex() == (uint64_t)(kHistoryFrames + 60));

    const double p50 = Profiler().FramePercentile(0.50f);
    const double p95 = Profiler().FramePercentile(0.95f);
    CHECK(p50 >= 0.0);
    CHECK(p95 >= p50);
}

// ---------------------------------------------------------------------------
// Stutter capture: 30 quiet frames then one 60 ms frame must be reported once.
// ---------------------------------------------------------------------------
static void TestStutterCapture() {
    printf("Profiler: stutter capture\n");
    ResetAndEnable();
    for (int f = 0; f < 30; ++f) {
        Profiler().BeginFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        Profiler().EndFrame();
    }
    CHECK(!Profiler().StutterPending());

    Profiler().BeginFrame();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    Profiler().EndFrame();

    CHECK(Profiler().StutterPending());
    std::string report;
    CHECK(Profiler().TakeStutterReport(report));
    CHECK(report.find("STUTTER") != std::string::npos);
    CHECK(!Profiler().StutterPending());
    CHECK(!Profiler().TakeStutterReport(report));   // consumed exactly once
}

// ---------------------------------------------------------------------------
// Disabled: no scopes, no counters, no frame advancement.
// ---------------------------------------------------------------------------
static void TestDisabled() {
    printf("Profiler: disabled\n");
    ResetAndEnable();
    const int id = Profiler().InternScope("test.disabled");
    const int c = Profiler().InternCounter("test.disabled.count");
    Profiler().SetEnabled(false);
    for (int f = 0; f < 3; ++f) {
        Profiler().BeginFrame();
        { ScopeGuard g(id); }
        Profiler().AddCounter(c, 7);
        Profiler().EndFrame();
    }
    CHECK(Profiler().FrameIndex() == 0);
    CHECK(Profiler().FramesRecorded() == 0);
    CHECK(Profiler().ScopeAt(id).frameCalls == 0);
    CHECK(Profiler().CounterFrame("test.disabled.count") == 0);
    Profiler().SetEnabled(true);
}

// ---------------------------------------------------------------------------
// CSV: metadata header, one row per recorded frame, scope/counter columns.
// ---------------------------------------------------------------------------
static void TestCsvDump() {
    printf("Profiler: CSV dump\n");
    ResetAndEnable();
    const int id = Profiler().InternScope("test.csv");
    const int c = Profiler().InternCounter("test.csv.count");

    for (int f = 0; f < 4; ++f) {
        Profiler().BeginFrame();
        { ScopeGuard g(id); }
        Profiler().AddCounter(c, 2);
        Profiler().EndFrame();
    }

    const auto path = std::filesystem::temp_directory_path() / "oz_profiler_test.csv";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    CHECK(Profiler().DumpCSV(path.string().c_str(), "# world=TestMap\n# build=test"));

    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    CHECK(f != nullptr);
    if (!f) return;
    std::string text;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), f)) text += buf;
    std::fclose(f);
    std::filesystem::remove(path, ec);

    CHECK(text.find("# world=TestMap") != std::string::npos);
    CHECK(text.find("frame,ms,fps") != std::string::npos);
    CHECK(text.find("scope.test.csv") != std::string::npos);
    CHECK(text.find("counter.test.csv.count") != std::string::npos);

    int rows = 0;
    size_t pos = 0;
    while ((pos = text.find('\n', pos)) != std::string::npos) {
        if (text[pos + 1] != '#' && text[pos + 1] != '\0') ++rows;
        ++pos;
    }
    CHECK(rows >= 4);   // header row + 4 data rows (the header has no '#')
}

int main() {
    printf("=== FrameProfiler ===\n");
    TestScopeNesting();
    TestCounters();
    TestHistoryWindow();
    TestStutterCapture();
    TestDisabled();
    TestCsvDump();

    printf("\n%d checks, %d failure(s)\n", g_checks, g_fail);
    if (g_fail == 0) printf("PASS\n");
    return g_fail ? 1 : 0;
}
