// Headless tests for AngelEd's Core/Viewport.hpp (Phase 0 of multiple viewports).
//
// WHY THIS SUITE EXISTS. The "inside the 3D view" test used to be six copies of
// `mp.x >= GetStatsSidebarWidth() && mp.y >= kEditorToolbarHeight` in Main.cpp.
// A split viewport changes that answer for every input site at once, so the
// geometry now lives in one raylib-free header and this suite pins it:
//   * the window-minus-sidebar/toolbar area,
//   * rows x cols tiling with no gaps and no overlap,
//   * half-open hit-testing (a shared border belongs to exactly one pane).
// It also asserts the duplicated gate is gone from Main.cpp and that every input
// site asks the helper, so the six-copy regression cannot come back silently.
//
// Raylib-free by construction: Viewport.hpp includes only <vector>.

#include "AngelEd/Source/Core/Viewport.hpp"

#include <cstdio>
#include <string>
#include <fstream>
#include <sstream>

static int g_pass = 0, g_fail = 0;

static void check(bool cond, const char* what) {
    if (cond) { g_pass++; return; }
    g_fail++;
    printf("  FAIL  %s\n", what);
}

static bool Near(float a, float b, float eps = 0.001f) {
    float d = a - b; if (d < 0) d = -d; return d <= eps;
}

static std::string ReadFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}

namespace v = ed::viewport;

// The window area must exclude the docked sidebar (left) and toolbar (top) and
// never go negative on a tiny window.
static void test_area_insets() {
    printf("viewport area\n");
    v::VpRect a = v::Area(1280, 720, 200, 56);
    check(Near(a.x, 200) && Near(a.y, 56), "area starts below the sidebar and toolbar");
    check(Near(a.w, 1080) && Near(a.h, 664), "area spans the remaining window");

    v::VpRect tiny = v::Area(100, 20, 200, 56);
    check(tiny.w >= 1.0f && tiny.h >= 1.0f, "a window smaller than the chrome clamps to 1px, not negative");
}

// Every layout tiles the area exactly: no gaps, no overlap, full coverage.
static void test_split_tiles_exactly() {
    printf("split tiling\n");
    const v::VpRect area = v::Area(1280, 720, 200, 56);
    const int cases[][2] = {{1,1},{1,2},{2,1},{2,2},{2,3}};
    for (const auto& lc : cases) {
        std::vector<v::VpRect> rects;
        v::Split(area, lc[0], lc[1], rects);
        char msg[128];
        snprintf(msg, sizeof(msg), "%dx%d yields rows*cols panes", lc[0], lc[1]);
        check((int)rects.size() == lc[0] * lc[1], msg);

        double totalArea = 0.0;
        bool overlap = false;
        for (size_t i = 0; i < rects.size(); i++) {
            totalArea += (double)rects[i].w * (double)rects[i].h;
            for (size_t j = i + 1; j < rects.size(); j++) {
                const v::VpRect& a = rects[i];
                const v::VpRect& b = rects[j];
                const bool ox = a.x < b.x + b.w && a.x + a.w > b.x;
                const bool oy = a.y < b.y + b.h && a.y + a.h > b.y;
                if (ox && oy) overlap = true;
            }
        }
        check(!overlap, "panes do not overlap");
        double want = (double)area.w * (double)area.h;
        check(Near((float)totalArea, (float)want, 0.01f), "panes cover the whole area (no gaps)");
    }

    // The 2x2 case must be exact quarters (with the floating remainder absorbed).
    std::vector<v::VpRect> q;
    v::Split(area, 2, 2, q);
    check(q.size() == 4, "2x2 yields four panes");
    if (q.size() == 4) {
        check(Near(q[0].x, area.x) && Near(q[0].y, area.y), "top-left pane at the origin");
        check(Near(q[1].x, area.x + area.w * 0.5f), "top-right pane starts at mid-width");
        check(Near(q[2].y, area.y + area.h * 0.5f), "bottom-left pane starts at mid-height");
        check(Near(q[3].x + q[3].w, area.x + area.w) &&
              Near(q[3].y + q[3].h, area.y + area.h),
              "bottom-right pane ends exactly at the area corner");
    }
}

// Half-open: a point on a shared border belongs to exactly one pane.
static void test_hit_testing_is_half_open() {
    printf("hit testing\n");
    const v::VpRect area = v::Area(1280, 720, 200, 56);
    std::vector<v::VpRect> rects;
    v::Split(area, 1, 2, rects);
    check(rects.size() == 2, "1x2 yields two panes");

    const float midX = area.x + area.w * 0.5f;
    check(v::IndexAt(rects, area.x + 10, area.y + 10) == 0, "a point in the left pane picks 0");
    check(v::IndexAt(rects, midX + 10, area.y + 10) == 1, "a point in the right pane picks 1");
    // Exactly on the border is inside the right pane only (px >= x, px < x+w).
    check(v::IndexAt(rects, midX, area.y + 10) == 1, "a border point belongs to exactly one pane");
    check(v::IndexAt(rects, area.x - 5, area.y + 10) == -1, "outside the area picks -1");
    check(v::IndexAt(rects, area.x + 10, area.y - 5) == -1, "above the area (toolbar) picks -1");
}

// Source parity: the duplicated gate must not come back, and the helper must be
// the thing every site uses.
static void test_main_uses_one_viewport_helper() {
    printf("Main.cpp wiring\n");
    const std::string main = ReadFile("AngelEd/Source/Main.cpp");
    if (main.empty()) {
        printf("  SKIP  Main.cpp not found (run from the repo root)\n");
        return;
    }
    check(main.find("mp.x >= (float)GetStatsSidebarWidth()") == std::string::npos &&
          main.find("_mp.x >= (float)GetStatsSidebarWidth()") == std::string::npos &&
          main.find("_mp_rel.x >= (float)GetStatsSidebarWidth()") == std::string::npos,
          "the six-copy sidebar/toolbar gate is gone from Main.cpp");
    check(main.find("EditorViewportContains") != std::string::npos,
          "input sites ask the one viewport helper");
    // Phase 1 wiring: the render loop splits the area and gives each pane its own
    // projection aspect + GL viewport. Without these the split stretches or draws
    // every pane with the full-window framing.
    check(main.find("EditorViewportRectsNow") != std::string::npos,
          "the render loop splits the viewport area per layout");
    check(main.find("SetPaneProjection") != std::string::npos &&
          main.find("SetPaneViewport") != std::string::npos,
          "each pane gets its own aspect-correct projection and GL viewport");
    // Phase 2: picking and the gizmo must project THROUGH the active pane. A
    // GetMouseRay/GetWorldToScreen call left in either file maps the mouse over the
    // whole window and mis-aims every click in a split.
    const std::string gizmo = ReadFile("AngelEd/Source/Subsystems/Gizmo.cpp");
    const std::string sel   = ReadFile("AngelEd/Source/Subsystems/Selection.cpp");
    check(!gizmo.empty() && gizmo.find("GetWorldToScreen(") == std::string::npos &&
          gizmo.find("WorldToScreenInRect") != std::string::npos,
          "the gizmo projects through the active pane, not the window");
    check(!sel.empty() && sel.find("GetMouseRay(") == std::string::npos &&
          sel.find("RayFromPane") != std::string::npos,
          "picking builds a pane-aware ray");
    // Phase 3: maximize is a layout flag, and the layout persists in [Views].
    check(main.find("viewMaximized") != std::string::npos,
          "maximize is driven by the layout state");
    const std::string uishell = ReadFile("AngelEd/Source/UI/UiShell.cpp");
    check(!uishell.empty() && uishell.find("\"Views\", \"layout\"") != std::string::npos,
          "the viewport layout persists in [Views]");
    // Tier A: a detached pane renders into its own framebuffer and is presented by
    // a GDI blit (no second GL context).
    check(main.find("PaneIsDetached") != std::string::npos &&
          main.find("BeginTextureMode(g_paneTargets") != std::string::npos,
          "a detached pane renders into its own RenderTexture2D");
    const std::string detached = ReadFile("AngelEd/Source/Core/DetachedPane.cpp");
    check(!detached.empty() &&
          detached.find("DetachedPane_Present") != std::string::npos &&
          detached.find("StretchDIBits") != std::string::npos &&
          detached.find("LoadImageFromTexture") != std::string::npos,
          "the detached window is presented by a texture readback + GDI blit");
    // Tier B: a shared WGL context + textured-quad present (no readback), with the
    // GDI path kept as a fallback. Tier C: the pop-out navigates its pane camera.
    check(detached.find("wglShareLists") != std::string::npos &&
          detached.find("SwapBuffers") != std::string::npos &&
          detached.find("glBindTexture") != std::string::npos,
          "Tier B uses a shared GL context and a textured-quad present");
    check(detached.find("WM_MOUSEWHEEL") != std::string::npos &&
          detached.find("g_paneCams") != std::string::npos,
          "the popped-out pane is navigable (Tier C camera)");
    check(detached.find("StretchDIBits") != std::string::npos,
          "the readback path is kept as the fallback");
}

static void test_environment_preview_toggles() {
    const std::string main = ReadFile("AngelEd/Source/Main.cpp");
    const std::string menus = ReadFile("AngelEd/Source/Core/EditorMenus.cpp");
    const std::string bridge = ReadFile("AngelEd/Source/Subsystems/WorldGraphBridge.cpp");
    const std::string uishell = ReadFile("AngelEd/Source/UI/UiShell.cpp");
    const std::string state = ReadFile("AngelEd/Source/Core/EditorPanelState.hpp");
    if (main.empty() || menus.empty() || bridge.empty() || uishell.empty() || state.empty()) {
        printf("  SKIP  editor environment preview toggles (sources not found)\n");
        return;
    }
    check(state.find("bool viewAmbient = true;") != std::string::npos &&
          state.find("bool viewFog = true;") != std::string::npos,
          "the editor state carries independent ambient and fog preview flags");
    check(menus.find("IDM_VIEW_AMBIENT") != std::string::npos &&
          menus.find("IDM_VIEW_FOG") != std::string::npos &&
          menus.find("Show &Ambient") != std::string::npos &&
          menus.find("Show &Fog") != std::string::npos,
          "both toggles have View-menu entries");
    check(bridge.find("case IDM_VIEW_AMBIENT") != std::string::npos &&
          bridge.find("case IDM_VIEW_FOG") != std::string::npos &&
          bridge.find("Editor_SyncViewMenuChecks") != std::string::npos,
          "menu commands toggle the flags and resync their check marks");
    check(main.find("if (!g_editorPanels.viewFog) fogIntensity = 0.0f") != std::string::npos &&
          main.find("if (!g_editorPanels.viewAmbient)") != std::string::npos &&
          main.find("skyFog.intensity = 0.0f") != std::string::npos,
          "preview flags mask LitFog, SurfaceMaterial and sky fog independently");
    check(uishell.find("\"Views\", \"ambient\"") != std::string::npos &&
          uishell.find("\"Views\", \"fog\"") != std::string::npos,
          "the two preview choices persist in [Views]");
}

int main() {
    printf("Viewport tests\n\n");
    test_area_insets();
    test_split_tiles_exactly();
    test_hit_testing_is_half_open();
    test_main_uses_one_viewport_helper();
    test_environment_preview_toggles();

    printf("\nResults: %d/%d passed\n", g_pass, g_pass + g_fail);
    return g_fail == 0 ? 0 : 1;
}
