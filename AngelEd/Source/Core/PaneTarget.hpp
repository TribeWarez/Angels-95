#pragma once
// ============================================================================
// Core/PaneTarget.hpp — where a viewport pane renders
//
// The editor renders every pane straight into the main window with
// BeginScissorMode + rlViewport. A DETACHED pane needs its own framebuffer
// (a RenderTexture2D) so it can be presented in a separate OS window. This
// header is the abstraction between the two: the render seam asks
// PaneRenderRect() for the rect to frame (its screen rect, or the target's own
// size) and PaneIsDetached() says whether to wrap the pass in
// BeginTextureMode/EndTextureMode instead of a scissor.
//
// Header-only and editor-only on purpose: one TU (Main.cpp) includes it, so it
// adds no Makefile / ci.yml object. The Win32 window plumbing lives in
// Core/DetachedPane.cpp (also a Main.cpp fragment).
// ============================================================================

#include "raylib.h"
#include "Viewport.hpp"
#include <vector>

struct PaneTarget {
    bool detached = false;
    RenderTexture2D rt{};   // valid while detached; recreated on resize
    void* hwnd = nullptr;   // HWND of the detached window (opaque here)
    bool rtDirty = false;   // set by WM_SIZE, consumed by EnsurePaneRT
    double lastPresent = 0.0; // presentation cadence (the readback is not free)
    // Tier B: a shared WGL context for this window. When sharedOk, the pane's
    // colour texture is sampled directly (no readback); otherwise the GDI path is
    // used. Kept opaque so this header needs no GL headers.
    void* glctx = nullptr;  // HGLRC
    void* gldc = nullptr;   // HDC (released on attach)
    bool sharedOk = false;
};

inline PaneTarget g_paneTargets[4];

// Per-pane cameras. The active pane's camera is mirrored into
// OTEditor.MainCamera for the existing input/preset paths; a detached window
// edits its own entry directly (Tier C camera navigation).
inline Camera3D g_paneCams[4];

// The rect a pane's 3D pass should frame: the target's own size when detached
// (0,0,rt.w,rt.h), otherwise its screen rect.
inline ed::viewport::VpRect PaneRenderRect(int i,
                                           const std::vector<ed::viewport::VpRect>& rects) {
    if (i >= 0 && i < 4) {
        const PaneTarget& t = g_paneTargets[i];
        if (t.detached && t.rt.texture.width > 0 && t.rt.texture.height > 0)
            return { 0.0f, 0.0f, (float)t.rt.texture.width, (float)t.rt.texture.height };
    }
    if (i >= 0 && i < (int)rects.size()) return rects[i];
    return ed::viewport::VpRect{};
}

inline bool PaneIsDetached(int i) {
    return i >= 0 && i < 4 && g_paneTargets[i].detached;
}
