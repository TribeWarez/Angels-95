#pragma once
// ---------------------------------------------------------------------------
// ProfilerOverlay - the on-screen half of FrameProfiler.
//
// Header-only for the same reason CullState.hpp and Skybox.hpp are: it is drawn
// by two targets (the client and AngelEd) plus a future perf panel, and a .cpp
// would mean wiring the root Makefile, AngelEd/Makefile and ci.yml's inline g++
// list. Everything here is 2D raylib drawing, so it must be called inside a
// BeginDrawing/EndDrawing pair (and never into a render texture).
//
// Compact rows answer "how bad is it right now"; expanded adds the top scopes
// and counters that answer "which system".
// ---------------------------------------------------------------------------

#include "FrameProfiler.hpp"
#include "raylib.h"
#include <cstdio>

namespace oz::diag {

inline Color ProfilerBadgeColor(double frameMs) {
    if (frameMs >= 50.0) return (Color){255, 90, 90, 255};    // < 20 FPS
    if (frameMs >= 25.0) return (Color){255, 200, 80, 255};   // < 40 FPS
    return (Color){140, 220, 140, 255};
}

// topInset: pixels below the window top (the editor passes its toolbar height).
// rightInset: pixels kept clear on the right (the editor's stats sidebar).
inline void DrawProfilerOverlay(int topInset = 8, int rightInset = 8, bool expanded = false) {
    FrameProfiler& p = Profiler();
    if (!p.Enabled()) return;

    const int lineH = 15;
    char line[256];
    const int rows = expanded ? 3 + 12 + 8 : 3;
    const int panelW = expanded ? 430 : 330;
    const int panelH = rows * lineH + 18;

    int x = GetScreenWidth() - panelW - rightInset;
    int y = topInset;
    if (x < 8) x = 8;
    if (y < 8) y = 8;
    if (y + panelH > GetScreenHeight() - 8) y = GetScreenHeight() - panelH - 8;
    if (y < 8) y = 8;

    DrawRectangle(x, y, panelW, panelH, (Color){8, 10, 12, 205});
    DrawRectangleLines(x, y, panelW, panelH, (Color){90, 110, 120, 220});

    int ty = y + 7;
    const Color dim = (Color){170, 180, 190, 255};
    const Color accent = ProfilerBadgeColor(p.FrameMs());

    // p95 sorts the 240-frame ring; recompute it a few times a second rather
    // than on every drawn frame. The overlay is on by default, so its own cost
    // must not show up in the number it displays.
    static double s_p95 = 0.0;
    static double s_p95Next = 0.0;
    const double now = GetTime();
    if (now >= s_p95Next) {
        s_p95 = p.FramePercentile(0.95f);
        s_p95Next = now + 0.25;
    }
    std::snprintf(line, sizeof(line), "PERF  %.0f fps   %.2f ms   p95 %.2f",
                  p.SmoothedFps(), p.FrameMs(), s_p95);
    DrawText(line, x + 8, ty, 14, accent);
    ty += lineH;

    if (!expanded) {
        std::snprintf(line, sizeof(line),
                      "draws %llu  faces %llu  perface %llu  stat %llu",
                      (unsigned long long)p.CounterFrame("draw.calls"),
                      (unsigned long long)p.CounterFrame("draw.faces"),
                      (unsigned long long)p.CounterFrame("brush.perface"),
                      (unsigned long long)p.CounterFrame("tex.stat"));
        DrawText(line, x + 8, ty, 12, dim);
        ty += lineH;
        std::snprintf(line, sizeof(line),
                      "lights %llu  particles %llu  sim %llu",
                      (unsigned long long)p.CounterFrame("light.submitted"),
                      (unsigned long long)p.CounterFrame("particle.drawn"),
                      (unsigned long long)p.CounterFrame("sim.steps"));
        DrawText(line, x + 8, ty, 12, dim);
        return;
    }

    std::snprintf(line, sizeof(line),
                  "draws %llu  faces %llu  fast %llu  perface %llu  stat %llu",
                  (unsigned long long)p.CounterFrame("draw.calls"),
                  (unsigned long long)p.CounterFrame("draw.faces"),
                  (unsigned long long)p.CounterFrame("brush.fast"),
                  (unsigned long long)p.CounterFrame("brush.perface"),
                  (unsigned long long)p.CounterFrame("tex.stat"));
    DrawText(line, x + 8, ty, 12, dim);
    ty += lineH;

    std::snprintf(line, sizeof(line),
                  "lookups %llu  uploads %llu  lights %llu  parts %llu  sim %llu",
                  (unsigned long long)p.CounterFrame("uniform.lookup"),
                  (unsigned long long)p.CounterFrame("uniform.upload"),
                  (unsigned long long)p.CounterFrame("light.submitted"),
                  (unsigned long long)p.CounterFrame("particle.drawn"),
                  (unsigned long long)p.CounterFrame("sim.steps"));
    DrawText(line, x + 8, ty, 12, dim);
    ty += lineH + 3;

    DrawText("-- top scopes --", x + 8, ty, 12, (Color){140, 200, 140, 255});
    ty += lineH;

    TopScope top[kMaxTopScopes];
    const int n = p.TopScopes(top, 12);
    for (int i = 0; i < n; ++i) {
        std::snprintf(line, sizeof(line), "%-24s %7.2f ms  x%d",
                      top[i].name, top[i].ms, top[i].calls);
        DrawText(line, x + 8, ty, 12, dim);
        ty += lineH;
    }
    if (n == 0) {
        DrawText("(no scopes recorded this frame)", x + 8, ty, 12, dim);
        ty += lineH;
    }

    ty += 3;
    DrawText("-- counters --", x + 8, ty, 12, (Color){180, 190, 120, 255});
    ty += lineH;
    TopCounter counters[kMaxTopCounters];
    const int cn = p.TopCounters(counters, 8);
    for (int i = 0; i < cn; ++i) {
        std::snprintf(line, sizeof(line), "%-28s %llu",
                      counters[i].name, (unsigned long long)counters[i].value);
        DrawText(line, x + 8, ty, 12, dim);
        ty += lineH;
    }
    if (cn == 0) DrawText("(none)", x + 8, ty, 12, dim);
}

} // namespace oz::diag
