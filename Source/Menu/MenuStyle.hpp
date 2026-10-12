#pragma once
// ============================================================================
// Menu/MenuStyle.hpp
//
// The "Angels95 menu look" as reusable primitives, so the in-game overlays
// (Settings, etc.) can match the TitleMenu instead of falling back to raygui's
// default/dark widgets.
//
// Header-only and raygui-free on purpose: it needs nothing but raylib, and a
// header means no Makefile/ci.yml entry (see AGENTS.md on build wiring). The
// palette and geometry here mirror Source/Menu/TitleMenu.hpp - the navy title
// bar, the steel borders and the hover/active button fills - so the two read the
// same on screen. Keep them in step if the TitleMenu palette changes.
// ============================================================================
#include "raylib.h"

namespace angelsui {

// Palette - sampled from TitleMenu's title bar, tab bar and frames.
inline constexpr Color kPanelBg    = {6, 12, 30, 240};
inline constexpr Color kTitleBar   = {0, 16, 48, 255};
inline constexpr Color kBorder     = {60, 100, 160, 255};
inline constexpr Color kBorderDim  = {40, 60, 100, 200};
inline constexpr Color kBtnNormal  = {20, 30, 60, 255};
inline constexpr Color kBtnHover   = {40, 80, 140, 255};
inline constexpr Color kBtnActive  = {50, 110, 190, 255};
inline constexpr Color kTrack      = {14, 22, 44, 255};
inline constexpr Color kText       = {225, 235, 245, 255};
inline constexpr Color kTextDim    = {150, 172, 196, 255};
inline constexpr Color kTextSoft   = {200, 212, 228, 255};
inline constexpr Color kClose      = {140, 30, 30, 255};
inline constexpr Color kCloseHover = {190, 50, 50, 255};
inline constexpr Color kEdge       = {120, 180, 240, 255};

inline bool Hover(Rectangle r) {
    return CheckCollisionPointRec(GetMousePosition(), r);
}

// The TitleMenu's square window buttons (minimise / maximise / close).
inline bool WinButton(Rectangle r, int type) {
    bool hover = Hover(r);
    Color bg = hover ? (type == 2 ? kCloseHover : kBtnHover)
                     : (type == 2 ? kClose : kBtnNormal);
    DrawRectangleRec(r, bg);
    DrawRectangleLinesEx(r, 1, kBorder);
    int cx = (int)(r.x + r.width / 2), cy = (int)(r.y + r.height / 2);
    if (type == 0)      DrawRectangle(cx - 3, cy, 6, 1, kText);
    else if (type == 1) DrawRectangleLines(cx - 3, cy - 3, 6, 6, kText);
    else { DrawLine(cx - 3, cy - 3, cx + 3, cy + 3, kText);
           DrawLine(cx + 3, cy - 3, cx - 3, cy + 3, kText); }
    return hover && IsMouseButtonPressed(MOUSE_LEFT_BUTTON);
}

// Framed panel with a navy title bar and a close box. Returns true when the
// close box is clicked (the caller decides what "close" means).
inline bool Panel(Rectangle r, const char* title) {
    DrawRectangleRec({r.x + 3, r.y + 3, r.width, r.height}, (Color){0, 0, 0, 90});
    DrawRectangleRec(r, kPanelBg);
    Rectangle tb = {r.x, r.y, r.width, 30};
    DrawRectangleRec(tb, kTitleBar);
    DrawRectangleLinesEx(r, 1, kBorder);
    DrawLine((int)tb.x, (int)(tb.y + tb.height),
             (int)(r.x + r.width), (int)(tb.y + tb.height), kBorder);
    DrawText(title, (int)(tb.x + 12), (int)(tb.y + 8), 14, kText);
    return WinButton({r.x + r.width - 24, r.y + 6, 18, 18}, 2);
}

// A titled group box (the menu's section framing) drawn with primitives, so a
// failed/absent 9-slice texture is not a prerequisite.
inline void Group(Rectangle r, const char* title) {
    DrawRectangleLinesEx(r, 1, kBorderDim);
    int tw = MeasureText(title, 11);
    DrawRectangle((int)r.x, (int)r.y, tw + 12, 16, kPanelBg);
    DrawText(title, (int)(r.x + 6), (int)(r.y + 3), 11, kTextDim);
}

inline void Label(Rectangle r, const char* text, int size = 12) {
    DrawText(text, (int)r.x, (int)(r.y + (r.height - size) / 2), size, kTextDim);
}

// Button with the menu hover/active fills. `active` shows a latched toggle.
inline bool Button(Rectangle r, const char* label, bool active = false, int size = 12) {
    bool hover = Hover(r);
    Color bg = active ? kBtnActive : (hover ? kBtnHover : kBtnNormal);
    DrawRectangleRec(r, bg);
    DrawRectangleLinesEx(r, 1, active ? kEdge : kBorder);
    int tw = MeasureText(label, size);
    DrawText(label, (int)(r.x + (r.width - tw) / 2),
             (int)(r.y + (r.height - size) / 2), size,
             (active || hover) ? kText : kTextSoft);
    return hover && IsMouseButtonPressed(MOUSE_LEFT_BUTTON);
}

// Click-and-drag slider. `label` is drawn inside on the left; the caller is
// expected to bake the value into it (e.g. TextFormat("Size %.0f", ...)).
inline float Slider(Rectangle r, const char* label, float value, float lo, float hi,
                    int size = 11) {
    bool hover = Hover(r);
    DrawRectangleRec(r, kTrack);
    float span = (hi > lo) ? (hi - lo) : 1.0f;
    float t = (value - lo) / span;
    if (t < 0) t = 0; if (t > 1) t = 1;
    DrawRectangleRec({r.x, r.y, r.width * t, r.height}, kBtnActive);
    if (hover && IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
        float nt = (GetMousePosition().x - r.x) / r.width;
        if (nt < 0) nt = 0; if (nt > 1) nt = 1;
        value = lo + nt * span;
        t = nt;
    }
    DrawRectangleLinesEx(r, 1, hover ? kEdge : kBorder);
    DrawRectangle((int)(r.x + r.width * t - 1), (int)r.y, 2, (int)r.height,
                  (Color){220, 235, 255, 255});
    if (label)
        DrawText(label, (int)(r.x + 6), (int)(r.y + (r.height - size) / 2), size,
                 (Color){235, 242, 250, 235});
    return value;
}

// Click-to-cycle chooser (a dropdown without the popup), so the settings stay
// fully custom-drawn. Returns the (possibly advanced) index.
inline int Cycle(Rectangle r, const char** opts, int count, int index) {
    if (count <= 0) return index;
    if (index < 0 || index >= count) index = 0;
    bool hover = Hover(r);
    DrawRectangleRec(r, hover ? kBtnHover : kBtnNormal);
    DrawRectangleLinesEx(r, 1, hover ? kEdge : kBorder);
    DrawText(opts[index], (int)(r.x + 8), (int)(r.y + (r.height - 12) / 2), 12, kText);
    int cx = (int)(r.x + r.width - 14), cy = (int)(r.y + r.height / 2);
    DrawTriangle((Vector2){(float)cx - 4, (float)cy - 2},
                 (Vector2){(float)cx + 4, (float)cy - 2},
                 (Vector2){(float)cx, (float)cy + 3}, kText);
    if (hover && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) index = (index + 1) % count;
    return index;
}

} // namespace angelsui
