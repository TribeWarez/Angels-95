#pragma once
// ============================================================================
// Core/Viewport.hpp — editor viewport LAYOUT math (raylib-free)
//
// Phase 0 of the multi-viewport work. The viewport rect used to be an input gate
// copy-pasted six times in Main.cpp:
//
//     mp.x >= (float)GetStatsSidebarWidth() && mp.y >= (float)kEditorToolbarHeight
//
// That is one definition of "inside the 3D view" written six ways, and it is the
// thing a split viewport has to change first. This header owns the geometry
// (area inset, row/col split, hit-test) so the split can be added without
// re-deriving the bounds at each site.
//
// Deliberately raylib-free and Win32-free (<vector> only): tests/Viewport.test.cpp
// links this header directly, the same discipline as GizmoMath.hpp / AssetScope.
// A raylib Rectangle is mapped onto VpRect at the call site.
// ============================================================================

#include <vector>

namespace ed {
namespace viewport {

// A screen-space rectangle. Same field order as raylib's Rectangle so the bridge
// in Main.cpp is a field copy, not a transform.
struct VpRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

// Point-in-rect. Half-open on the right/bottom edge so two adjacent panes never
// both claim a shared border pixel.
inline bool Contains(const VpRect& r, float px, float py) {
    return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
}

// The 3D viewport area: the window minus the docked stats sidebar (left) and the
// toolbar (top). `sidebarW`/`toolbarH` are passed in so the header does not need
// the editor's constants.
inline VpRect Area(float winW, float winH, float sidebarW, float toolbarH) {
    VpRect r{ sidebarW, toolbarH, winW - sidebarW, winH - toolbarH };
    if (r.w < 1.0f) r.w = 1.0f;
    if (r.h < 1.0f) r.h = 1.0f;
    return r;
}

// Split `area` into rows x cols cells. Row-major order, no gaps and no overlap:
// the last column/row absorbs any float remainder so the cells always tile the
// area exactly.
inline void Split(const VpRect& area, int rows, int cols, std::vector<VpRect>& out) {
    out.clear();
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;
    const float cw = area.w / (float)cols;
    const float ch = area.h / (float)rows;
    for (int r = 0; r < rows; r++) {
        const float y0 = area.y + ch * (float)r;
        const float y1 = (r + 1 == rows) ? (area.y + area.h) : (area.y + ch * (float)(r + 1));
        for (int c = 0; c < cols; c++) {
            const float x0 = area.x + cw * (float)c;
            const float x1 = (c + 1 == cols) ? (area.x + area.w) : (area.x + cw * (float)(c + 1));
            out.push_back(VpRect{ x0, y0, x1 - x0, y1 - y0 });
        }
    }
}

// Index of the rect containing the point, or -1. Used to pick the active pane.
inline int IndexAt(const std::vector<VpRect>& rects, float px, float py) {
    for (int i = 0; i < (int)rects.size(); i++)
        if (Contains(rects[i], px, py)) return i;
    return -1;
}

// Layout state. Cameras are added in Phase 1; Phase 0 uses 1x1 and the geometry
// alone, so this stays plain data.
struct Layout {
    int rows = 1;
    int cols = 1;
    int active = 0;
};

} // namespace viewport
} // namespace ed
