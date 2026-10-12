#pragma once
// ============================================================================
// Renderer/Crosshair.hpp
//
// The crosshair, as its own renderer. It was four hardcoded DrawLine calls
// inline in Main.cpp's frame loop with a fixed length, a fixed gap and a fixed
// white colour, so "change the crosshair" meant editing the frame loop.
//
// Header-only and raylib-only on purpose: a new .cpp would mean touching the
// root Makefile, AngelEd/Makefile AND the inline g++ list in ci.yml (see
// AGENTS.md on build wiring), and this has no state worth owning.
//
// Style/size/gap/colour come from Settings.hpp (persisted in
// System/Angels95.ini by ClientSettings.hpp). ADS and bloom are passed in
// rather than read from WeaponBehaviour, so this stays decoupled: the caller
// already has both, and a test can drive it without the weapon system.
// ============================================================================
#include "raylib.h"
#include "../Settings.hpp"

namespace oz {

// Resolved geometry for one frame. ADS shrinks the gap and damps the bloom
// kick so the crosshair tightens when aiming, which is what it did before this
// became configurable - the behaviour is preserved, only the inputs are now
// settings rather than constants.
struct CrosshairFrame {
    bool ads = false;
    float bloom = 0.0f;   // WeaponBehaviour::CrosshairBloom()
};

// The effective gap for this frame: the authored gap, tightened while aiming
// and pushed out by the bloom kick on firing.
inline int CrosshairEffectiveGap(const CrosshairFrame& f) {
    const float base = f.ads ? 2.0f : (float)CrosshairGap;
    return (int)(base + f.bloom * (f.ads ? 0.3f : 1.0f));
}

// Draw at the centre of the current screen. No-op when hidden.
inline void DrawCrosshair(const CrosshairFrame& f) {
    if (!CrosshairVisible) return;

    const int cx = GetScreenWidth() / 2;
    const int cy = GetScreenHeight() / 2;
    const int gap = CrosshairEffectiveGap(f);
    const int len = CrosshairSize;
    const float thick = CrosshairThickness;
    const Color col = CrosshairColor();

    switch (CrosshairStyleSel) {
        case CROSSHAIR_DOT: {
            const int r = (int)(thick);  // a dot is a zero-length tick
            DrawRectangle(cx - r, cy - r, r * 2, r * 2, col);
            break;
        }
        case CROSSHAIR_CIRCLE:
        case CROSSHAIR_CIRCLE_DOT: {
            // Outlined circle; radius tracks the size setting.
            const float radius = (float)len;
            for (int i = 0; i < 72; i++) {
                const float a0 = (float)i * (2.0f * PI / 72.0f);
                const float a1 = (float)(i + 1) * (2.0f * PI / 72.0f);
                DrawLineEx({cx + cosf(a0) * radius, cy + sinf(a0) * radius},
                           {cx + cosf(a1) * radius, cy + sinf(a1) * radius},
                           thick, col);
            }
            if (CrosshairStyleSel == CROSSHAIR_CIRCLE_DOT) {
                const int r = (int)thick;
                DrawRectangle(cx - r, cy - r, r * 2, r * 2, col);
            }
            break;
        }
        case CROSSHAIR_CROSS:
        default: {
            // Four ticks, one per arm. Uses DrawLineEx so thickness is a real
            // setting rather than hardcoded to 1px.
            DrawLineEx({(float)(cx - gap - len), (float)cy}, {(float)(cx - gap), (float)cy}, thick, col);
            DrawLineEx({(float)(cx + gap), (float)cy}, {(float)(cx + gap + len), (float)cy}, thick, col);
            DrawLineEx({(float)cx, (float)(cy - gap - len)}, {(float)cx, (float)(cy - gap)}, thick, col);
            DrawLineEx({(float)cx, (float)(cy + gap)}, {(float)cx, (float)(cy + gap + len)}, thick, col);
            break;
        }
    }
}

} // namespace oz
