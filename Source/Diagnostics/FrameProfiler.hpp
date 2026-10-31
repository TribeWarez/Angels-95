#pragma once
// ---------------------------------------------------------------------------
// FrameProfiler - one low-overhead CPU profiler shared by the client, AngelEd
// and the headless tests.
//
// WHY THIS IS A HEADER
//   A .cpp would have to be added to the root Makefile, AngelEd/Makefile AND the
//   inline g++ list in ci.yml, and those two editor object lists have drifted
//   before (see AGENTS.md). Everything here is inline/inline-static, so a new
//   scope is one include and one macro at the call site.
//
// CONTRACT
//   - Single threaded. The simulation is, and the server does not include this.
//   - Zero allocations while a frame is running. Histories are fixed arrays; the
//     only allocation is the stutter report string, which is built at most once
//     per second and only when a frame is actually slow.
//   - Disabled costs one bool test per scope/counter. Collection is ON by
//     default (the user asked for an always-on profiler); the overlay is what
//     gets toggled from the toolbar.
//
// USAGE
//   OZ_PERF_FRAME  guard at the top of the frame loop (RAII, survives `continue`)
//   OZ_PROFILE_SCOPE("world.geometry")     // inclusive + self time, call count
//   OZ_PROFILE_COUNT("draw.calls")         // +1 this frame
//   OZ_PROFILE_ADD("draw.faces", n)
//
//   The scope id is interned ONCE per call site (function-local static), so the
//   steady-state cost is a clock read and an array index.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace oz::diag {

using ProfileClock = std::chrono::steady_clock;

constexpr int kMaxScopes      = 64;
constexpr int kMaxCounters    = 40;
constexpr int kHistoryFrames  = 240;   // ~4 s at 60 FPS
constexpr int kMaxScopeDepth  = 32;
constexpr int kMaxTopScopes   = 16;
constexpr int kMaxTopCounters = 16;

struct ScopeStat {
    const char* name = nullptr;
    double frameMs       = 0.0;   // inclusive, current frame
    double frameSelfMs   = 0.0;   // inclusive minus children
    int    frameCalls    = 0;
    double peakMs        = 0.0;
    std::array<float, kHistoryFrames> history{};   // aligned to the frame ring
};

struct CounterStat {
    const char* name = nullptr;
    uint64_t frame = 0;
    uint64_t peak  = 0;
    uint64_t total = 0;
    std::array<uint32_t, kHistoryFrames> history{}; // aligned to the frame ring
};

struct TopScope {
    const char* name = nullptr;
    double ms = 0.0;
    int calls = 0;
};

struct TopCounter {
    const char* name = nullptr;
    uint64_t value = 0;
};

class FrameProfiler {
public:
    static FrameProfiler& Instance() {
        // One definition across TUs: the inline member function is COMDAT-merged
        // and the function-local static is shared with it.
        static FrameProfiler instance;
        return instance;
    }

    // --- enable / overlay ---------------------------------------------------
    void SetEnabled(bool on) { m_enabled = on; }
    bool Enabled() const { return m_enabled; }
    bool InFrame() const { return m_inFrame; }

    // --- frame boundary -----------------------------------------------------
    void BeginFrame() {
        if (!m_enabled) { m_inFrame = false; return; }
        m_inFrame = true;
        m_depth = 0;
        m_frameStart = ProfileClock::now();
        for (int i = 0; i < m_scopeCount; ++i) {
            m_scopes[(size_t)i].frameMs = 0.0;
            m_scopes[(size_t)i].frameSelfMs = 0.0;
            m_scopes[(size_t)i].frameCalls = 0;
        }
        for (int i = 0; i < m_counterCount; ++i)
            m_counters[(size_t)i].frame = 0;
    }

    void EndFrame() {
        if (!m_enabled || !m_inFrame) { m_inFrame = false; return; }

        // Defensive: a scope guard that leaked (early return via longjmp, an
        // exception) must not wedge the stack for the rest of the session.
        while (m_depth > 0) PopScope();

        const auto now = ProfileClock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - m_frameStart).count();
        m_inFrame = false;

        m_frameMs = ms;
        m_smoothedMs = (m_smoothedMs <= 0.0) ? ms : (m_smoothedMs * 0.9 + ms * 0.1);
        ++m_frameIndex;

        const int slot = m_framePos;
        m_frameMsHistory[(size_t)slot] = (float)ms;

        for (int i = 0; i < m_scopeCount; ++i) {
            ScopeStat& s = m_scopes[(size_t)i];
            s.history[(size_t)slot] = (float)s.frameMs;
            if (s.frameMs > s.peakMs) s.peakMs = s.frameMs;
        }
        for (int i = 0; i < m_counterCount; ++i) {
            CounterStat& c = m_counters[(size_t)i];
            c.history[(size_t)slot] = (uint32_t)std::min<uint64_t>(c.frame, 0xFFFFFFFFu);
            if (c.frame > c.peak) c.peak = c.frame;
            c.total += c.frame;
        }

        // Stutter capture. The median is computed over the frames already in the
        // ring, so the first ~30 frames cannot trigger. A load-time frame (a whole
        // world open) is intentionally still captured: it is exactly the hitch a
        // benchmark author wants to see named.
        m_stutterCooldown -= ms;
        // The median sort is only worth doing for a frame that is already slow.
        // Running it on every 5 ms frame made the profiler itself measurable.
        if (m_frameCount >= 30 && m_stutterCooldown <= 0.0 && ms > 33.0) {
            const double median = FramePercentileLocked(0.5);
            const double threshold = std::max(33.0, median * 2.0);
            if (ms > threshold) {
                BuildStutterReport(ms, median);
                m_stutterCooldown = 1000.0;   // one report per second
            }
        }

        m_framePos = (m_framePos + 1) % kHistoryFrames;
        if (m_frameCount < kHistoryFrames) ++m_frameCount;
    }

    // --- registration -------------------------------------------------------
    int InternScope(const char* name) {
        if (!name || !*name) return -1;
        for (int i = 0; i < m_scopeCount; ++i)
            if (std::strcmp(m_scopes[(size_t)i].name, name) == 0) return i;
        if (m_scopeCount >= kMaxScopes) return -1;   // silently drop; overlay lists what fit
        const int id = m_scopeCount++;
        m_scopes[(size_t)id].name = name;
        return id;
    }

    int InternCounter(const char* name) {
        if (!name || !*name) return -1;
        for (int i = 0; i < m_counterCount; ++i)
            if (std::strcmp(m_counters[(size_t)i].name, name) == 0) return i;
        if (m_counterCount >= kMaxCounters) return -1;
        const int id = m_counterCount++;
        m_counters[(size_t)id].name = name;
        return id;
    }

    // --- scopes -------------------------------------------------------------
    bool PushScope(int id) {
        if (!m_enabled || !m_inFrame || id < 0 || id >= m_scopeCount) return false;
        if (m_depth >= kMaxScopeDepth) return false;
        m_stack[(size_t)m_depth++] = StackEntry{id, ProfileClock::now(), 0.0};
        return true;
    }

    void PopScope() {
        if (m_depth <= 0) return;
        StackEntry& e = m_stack[(size_t)--m_depth];
        const double ms = std::chrono::duration<double, std::milli>(
                              ProfileClock::now() - e.start).count();
        ScopeStat& s = m_scopes[(size_t)e.id];
        s.frameMs += ms;
        s.frameSelfMs += ms - e.childMs;
        ++s.frameCalls;
        if (m_depth > 0) m_stack[(size_t)(m_depth - 1)].childMs += ms;
    }

    // --- counters -----------------------------------------------------------
    void AddCounter(int id, uint64_t value) {
        if (!m_enabled || !m_inFrame || id < 0 || id >= m_counterCount) return;
        m_counters[(size_t)id].frame += value;
    }

    // --- reads --------------------------------------------------------------
    double FrameMs() const { return m_frameMs; }
    double SmoothedFps() const { return (m_smoothedMs > 0.0) ? 1000.0 / m_smoothedMs : 0.0; }
    uint64_t FrameIndex() const { return m_frameIndex; }
    int FramesRecorded() const { return m_frameCount; }
    int ScopeCount() const { return m_scopeCount; }
    int CounterCount() const { return m_counterCount; }

    const ScopeStat& ScopeAt(int i) const { return m_scopes[(size_t)i]; }
    const CounterStat& CounterAt(int i) const { return m_counters[(size_t)i]; }

    bool FindScope(const char* name, int& outId) const {
        for (int i = 0; i < m_scopeCount; ++i) {
            if (std::strcmp(m_scopes[(size_t)i].name, name) == 0) { outId = i; return true; }
        }
        return false;
    }
    bool FindCounter(const char* name, int& outId) const {
        for (int i = 0; i < m_counterCount; ++i) {
            if (std::strcmp(m_counters[(size_t)i].name, name) == 0) { outId = i; return true; }
        }
        return false;
    }

    uint64_t CounterFrame(const char* name) const {
        int id = -1;
        return FindCounter(name, id) ? m_counters[(size_t)id].frame : 0;
    }

    // Current-frame top list, sorted by inclusive time.
    int TopScopes(TopScope* out, int maxCount) const {
        if (!out || maxCount <= 0) return 0;
        std::vector<TopScope> tmp;
        tmp.reserve((size_t)m_scopeCount);
        for (int i = 0; i < m_scopeCount; ++i) {
            const ScopeStat& s = m_scopes[(size_t)i];
            if (s.frameCalls == 0 && s.frameMs <= 0.0) continue;
            tmp.push_back(TopScope{s.name, s.frameMs, s.frameCalls});
        }
        std::sort(tmp.begin(), tmp.end(),
                  [](const TopScope& a, const TopScope& b) { return a.ms > b.ms; });
        const int n = std::min<int>((int)tmp.size(), maxCount);
        for (int i = 0; i < n; ++i) out[i] = tmp[(size_t)i];
        return n;
    }

    int TopCounters(TopCounter* out, int maxCount) const {
        if (!out || maxCount <= 0) return 0;
        std::vector<TopCounter> tmp;
        tmp.reserve((size_t)m_counterCount);
        for (int i = 0; i < m_counterCount; ++i) {
            const CounterStat& c = m_counters[(size_t)i];
            if (c.frame == 0) continue;
            tmp.push_back(TopCounter{c.name, c.frame});
        }
        std::sort(tmp.begin(), tmp.end(),
                  [](const TopCounter& a, const TopCounter& b) { return a.value > b.value; });
        const int n = std::min<int>((int)tmp.size(), maxCount);
        for (int i = 0; i < n; ++i) out[i] = tmp[(size_t)i];
        return n;
    }

    // Percentile of completed frames (0..1). Locked = only the frames already
    // written, which is what EndFrame needs for its median.
    double FramePercentile(float p) const {
        if (m_frameCount <= 0) return 0.0;
        return PercentileOf(m_frameMsHistory, m_frameCount, m_framePos, p);
    }
    double ScopePercentile(int id, float p) const {
        if (m_frameCount <= 0 || id < 0 || id >= m_scopeCount) return 0.0;
        return PercentileOf(m_scopes[(size_t)id].history, m_frameCount, m_framePos, p);
    }

    // --- reset / dumps ------------------------------------------------------
    void Reset() {
        m_frameMs = 0.0;
        m_smoothedMs = 0.0;
        m_frameIndex = 0;
        m_frameCount = 0;
        m_framePos = 0;
        m_frameMsHistory.fill(0.0f);
        m_depth = 0;
        m_stutterPending = false;
        m_stutterReport.clear();
        for (int i = 0; i < m_scopeCount; ++i) {
            ScopeStat& s = m_scopes[(size_t)i];
            s.frameMs = s.frameSelfMs = s.peakMs = 0.0;
            s.frameCalls = 0;
            s.history.fill(0.0f);
        }
        for (int i = 0; i < m_counterCount; ++i) {
            CounterStat& c = m_counters[(size_t)i];
            c.frame = c.peak = c.total = 0;
            c.history.fill(0);
        }
    }

    // Human-readable summary: frame percentiles + every scope with samples,
    // sorted by average inclusive time.
    std::string Summary() const {
        char line[512];
        std::string out;
        out.reserve(4096);

        const double p50 = FramePercentile(0.50f);
        const double p95 = FramePercentile(0.95f);
        const double p99 = FramePercentile(0.99f);
        std::snprintf(line, sizeof(line),
                      "FrameProfiler: frames=%d last=%.2fms p50=%.2f p95=%.2f p99=%.2f fps=%.1f\n",
                      m_frameCount, m_frameMs, p50, p95, p99, SmoothedFps());
        out += line;

        std::vector<TopScope> scopes;
        scopes.reserve((size_t)m_scopeCount);
        for (int i = 0; i < m_scopeCount; ++i) {
            const ScopeStat& s = m_scopes[(size_t)i];
            if (s.peakMs <= 0.0 && s.frameCalls == 0) continue;
            scopes.push_back(TopScope{s.name, s.peakMs, s.frameCalls});
        }
        std::sort(scopes.begin(), scopes.end(),
                  [](const TopScope& a, const TopScope& b) { return a.ms > b.ms; });
        for (const TopScope& s : scopes) {
            int id = -1;
            if (!FindScope(s.name, id)) continue;
            std::snprintf(line, sizeof(line),
                          "scope %-28s last=%7.3fms p50=%7.3f p95=%7.3f peak=%7.3f calls=%d\n",
                          s.name, m_scopes[(size_t)id].frameMs,
                          ScopePercentile(id, 0.50f), ScopePercentile(id, 0.95f),
                          m_scopes[(size_t)id].peakMs, m_scopes[(size_t)id].frameCalls);
            out += line;
        }
        for (int i = 0; i < m_counterCount; ++i) {
            const CounterStat& c = m_counters[(size_t)i];
            if (c.total == 0) continue;
            const double avg = (m_frameCount > 0) ? (double)c.total / (double)m_frameCount : 0.0;
            std::snprintf(line, sizeof(line),
                          "counter %-26s last=%llu avg=%.1f peak=%llu\n",
                          c.name, (unsigned long long)c.frame, avg, (unsigned long long)c.peak);
            out += line;
        }
        return out;
    }

    bool DumpSummary(const char* path, const char* header = nullptr) const {
        std::FILE* f = std::fopen(path, "a");
        if (!f) return false;
        if (header && *header) std::fprintf(f, "%s\n", header);
        const std::string text = Summary();
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
        return true;
    }

    // Per-frame CSV history, oldest -> newest. `header` is written verbatim with
    // every line prefixed by '#', so metadata survives the CSV round trip.
    bool DumpCSV(const char* path, const char* header = nullptr, int maxFrames = 0) const {
        std::FILE* f = std::fopen(path, "w");
        if (!f) return false;
        if (header && *header) {
            // The caller owns the '#' prefixes (every header we write is a set of
            // "# key=value" lines). Prefixing here produced a literal "##" first
            // line, which a CSV reader then treated as data.
            std::fwrite(header, 1, std::strlen(header), f);
            if (header[std::strlen(header) - 1] != '\n') std::fputc('\n', f);
        }
        std::fputs("frame,ms,fps", f);
        for (int i = 0; i < m_scopeCount; ++i)
            std::fprintf(f, ",scope.%s", m_scopes[(size_t)i].name);
        for (int i = 0; i < m_counterCount; ++i)
            std::fprintf(f, ",counter.%s", m_counters[(size_t)i].name);
        std::fputc('\n', f);

        int count = m_frameCount;
        if (maxFrames > 0 && maxFrames < count) count = maxFrames;
        const int oldest = (m_framePos - count + kHistoryFrames) % kHistoryFrames;
        for (int n = 0; n < count; ++n) {
            const int slot = (oldest + n) % kHistoryFrames;
            const float ms = m_frameMsHistory[(size_t)slot];
            std::fprintf(f, "%llu,%.4f,%.2f",
                         (unsigned long long)(m_frameIndex - (uint64_t)(count - n)),
                         ms, ms > 0.0f ? 1000.0 / ms : 0.0);
            for (int i = 0; i < m_scopeCount; ++i)
                std::fprintf(f, ",%.4f", m_scopes[(size_t)i].history[(size_t)slot]);
            for (int i = 0; i < m_counterCount; ++i)
                std::fprintf(f, ",%u", m_counters[(size_t)i].history[(size_t)slot]);
            std::fputc('\n', f);
        }
        std::fclose(f);
        return true;
    }

    // --- stutter capture ----------------------------------------------------
    bool StutterPending() const { return m_stutterPending; }
    bool TakeStutterReport(std::string& out) {
        if (!m_stutterPending) return false;
        out = m_stutterReport;
        m_stutterPending = false;
        m_stutterReport.clear();
        return true;
    }

private:
    FrameProfiler() { m_frameMsHistory.fill(0.0f); }

    struct StackEntry {
        int id = -1;
        ProfileClock::time_point start{};
        double childMs = 0.0;
    };

    static double PercentileOf(const std::array<float, kHistoryFrames>& hist,
                               int count, int pos, float p) {
        if (count <= 0) return 0.0;
        if (p < 0.0f) p = 0.0f;
        if (p > 1.0f) p = 1.0f;
        std::array<float, kHistoryFrames> tmp{};
        const int oldest = (pos - count + kHistoryFrames) % kHistoryFrames;
        for (int i = 0; i < count; ++i)
            tmp[(size_t)i] = hist[(size_t)((oldest + i) % kHistoryFrames)];
        const int idx = std::min(count - 1, (int)(p * (float)(count - 1) + 0.5f));
        std::nth_element(tmp.begin(), tmp.begin() + idx, tmp.begin() + count);
        return (double)tmp[(size_t)idx];
    }

    double FramePercentileLocked(float p) const {
        return PercentileOf(m_frameMsHistory, m_frameCount, m_framePos, p);
    }

    void BuildStutterReport(double ms, double median) {
        char line[256];
        std::snprintf(line, sizeof(line), "STUTTER %.2fms (median %.2fms, %.1fx)\n",
                      ms, median, median > 0.0 ? ms / median : 0.0);
        m_stutterReport = line;

        std::vector<TopScope> top;
        top.reserve((size_t)m_scopeCount);
        for (int i = 0; i < m_scopeCount; ++i) {
            const ScopeStat& s = m_scopes[(size_t)i];
            if (s.frameMs <= 0.0) continue;
            top.push_back(TopScope{s.name, s.frameMs, s.frameCalls});
        }
        std::sort(top.begin(), top.end(),
                  [](const TopScope& a, const TopScope& b) { return a.ms > b.ms; });
        for (size_t i = 0; i < top.size() && i < 8; ++i) {
            std::snprintf(line, sizeof(line), "  %-28s %7.2fms x%d\n",
                          top[i].name, top[i].ms, top[i].calls);
            m_stutterReport += line;
        }

        std::vector<TopCounter> counters;
        counters.reserve((size_t)m_counterCount);
        for (int i = 0; i < m_counterCount; ++i) {
            const CounterStat& c = m_counters[(size_t)i];
            if (c.frame == 0) continue;
            counters.push_back(TopCounter{c.name, c.frame});
        }
        std::sort(counters.begin(), counters.end(),
                  [](const TopCounter& a, const TopCounter& b) { return a.value > b.value; });
        for (size_t i = 0; i < counters.size() && i < 8; ++i) {
            std::snprintf(line, sizeof(line), "  %-28s %llu\n",
                          counters[i].name, (unsigned long long)counters[i].value);
            m_stutterReport += line;
        }
        m_stutterPending = true;
    }

    bool m_enabled  = true;
    bool m_inFrame  = false;
    int  m_depth    = 0;
    ProfileClock::time_point m_frameStart{};

    std::array<ScopeStat, kMaxScopes>     m_scopes{};
    std::array<CounterStat, kMaxCounters> m_counters{};
    int m_scopeCount = 0;
    int m_counterCount = 0;

    std::array<StackEntry, kMaxScopeDepth> m_stack{};
    std::array<float, kHistoryFrames> m_frameMsHistory{};
    int m_framePos = 0;
    int m_frameCount = 0;
    uint64_t m_frameIndex = 0;

    double m_frameMs = 0.0;
    double m_smoothedMs = 0.0;
    double m_stutterCooldown = 0.0;
    bool m_stutterPending = false;
    std::string m_stutterReport;
};

inline FrameProfiler& Profiler() { return FrameProfiler::Instance(); }

// Register the counters the overlay/CSV readers expect even when a frame did
// not increment one. Without this a counter appears and disappears from the CSV
// depending on whether the last hot path ran (e.g. `tex.stat` vanishes once the
// path cache is warm), which makes two runs harder to diff.
inline void RegisterBuiltinCounters() {
    static const char* const kNames[] = {
        "draw.calls", "draw.faces", "brush.total", "brush.fast", "brush.perface",
        "tex.stat", "uniform.lookup", "uniform.upload", "light.submitted",
        "light.uploads", "mesh.bind", "particle.live", "particle.drawn",
        "props.drawn", "props.culled", "pawns.drawn", "pawns.culled",
        "sim.steps",
    };
    for (const char* name : kNames) Profiler().InternCounter(name);
}

// RAII frame boundary. Survives `continue` in the frame loop (the client has two
// of them on the paused/game-over paths), which a pair of manual calls cannot.
struct FrameGuard {
    FrameGuard()  { Profiler().BeginFrame(); }
    ~FrameGuard() { Profiler().EndFrame(); }
    FrameGuard(const FrameGuard&) = delete;
    FrameGuard& operator=(const FrameGuard&) = delete;
};

struct ScopeGuard {
    explicit ScopeGuard(int scopeId) : m_active(Profiler().PushScope(scopeId)) {}
    ~ScopeGuard() { if (m_active) Profiler().PopScope(); }
    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;
private:
    bool m_active;
};

} // namespace oz::diag

#define OZ_PERF_CONCAT2(a, b) a##b
#define OZ_PERF_CONCAT(a, b) OZ_PERF_CONCAT2(a, b)

#define OZ_PROFILE_SCOPE(name)                                                     \
    static const int OZ_PERF_CONCAT(oz_perf_scope_id_, __LINE__) =                 \
        ::oz::diag::Profiler().InternScope(name);                                  \
    ::oz::diag::ScopeGuard OZ_PERF_CONCAT(oz_perf_scope_guard_, __LINE__)(         \
        OZ_PERF_CONCAT(oz_perf_scope_id_, __LINE__))

#define OZ_PROFILE_COUNT(name)                                                     \
    do {                                                                           \
        static const int OZ_PERF_CONCAT(oz_perf_counter_id_, __LINE__) =           \
            ::oz::diag::Profiler().InternCounter(name);                            \
        ::oz::diag::Profiler().AddCounter(                                         \
            OZ_PERF_CONCAT(oz_perf_counter_id_, __LINE__), 1);                     \
    } while (0)

#define OZ_PROFILE_ADD(name, value)                                                \
    do {                                                                           \
        static const int OZ_PERF_CONCAT(oz_perf_counter_id_, __LINE__) =           \
            ::oz::diag::Profiler().InternCounter(name);                            \
        ::oz::diag::Profiler().AddCounter(                                         \
            OZ_PERF_CONCAT(oz_perf_counter_id_, __LINE__),                         \
            (uint64_t)(value));                                                    \
    } while (0)
