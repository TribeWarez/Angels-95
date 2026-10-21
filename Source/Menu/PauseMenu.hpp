#pragma once
// ---------------------------------------------------------------------------
// Menu/PauseMenu.hpp
//
// The in-game pause overlay. This is deliberately NOT the old fixed-size,
// mouse-only block that lived inside Main.cpp: it shares MenuMetrics and
// MenuStyle with the title menu, supports the keyboard, and returns an action
// instead of calling CloseWindow()/LoadWorld() from inside a draw pass.
//
// The caller owns the last game frame (Target.texture) and draws it first; this
// file only dims it and paints the panel.
// ---------------------------------------------------------------------------
#include "raylib.h"
#include "MenuStyle.hpp"
#include "MenuMetrics.hpp"
#include "../Audio/SoundManager.hpp"
#include <algorithm>
#include <cmath>

namespace angelsmenu {

enum class PauseAction : int {
    None = 0, Resume, Settings, SaveGame, MainMenu, Quit
};

inline PauseAction DrawPauseMenu(const MenuMetrics& m, float dt) {
    static int focus = 0;
    static float savedTimer = 0.0f;
    if (savedTimer > 0.0f) savedTimer -= dt;

    // Dim the frozen frame. The caller already drew it at screen size.
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), (Color){0, 0, 0, 170});

    const float w = std::min(m.screenW - m.pad * 4.0f, (float)std::round(460.0f * m.scale));
    const float btnH = std::round(42.0f * m.scale);
    const float gap = std::round(10.0f * m.scale);
    const char* labels[] = {"Resume", "Settings", "Save Game", "Main Menu", "Quit"};
    const int count = 5;
    const float titleH = std::round(86.0f * m.scale);
    const float panelH = titleH + count * btnH + (count - 1) * gap + std::round(30.0f * m.scale);
    Rectangle panel = {(m.screenW - w) * 0.5f, (m.screenH - panelH) * 0.5f, w, panelH};

    DrawRectangleRec({panel.x + 4, panel.y + 4, panel.width, panel.height},
                     (Color){0, 0, 0, 90});
    DrawRectangleRec(panel, angelsui::kPanelBg);
    DrawRectangleLinesEx(panel, 1, angelsui::kBorder);
    DrawRectangleRec({panel.x, panel.y, panel.width, std::round(34.0f * m.scale)},
                     angelsui::kTitleBar);
    DrawLine((int)panel.x, (int)(panel.y + std::round(34.0f * m.scale)),
             (int)(panel.x + panel.width), (int)(panel.y + std::round(34.0f * m.scale)),
             angelsui::kBorder);
    DrawText("PAUSED", (int)(panel.x + std::round(16.0f * m.scale)),
             (int)(panel.y + std::round(9.0f * m.scale)),
             (int)std::round(15.0f * m.scale), angelsui::kText);

    // Keyboard navigation.
    if (IsKeyPressed(KEY_DOWN)) focus = (focus + 1) % count;
    if (IsKeyPressed(KEY_UP)) focus = (focus + count - 1) % count;
    if (IsKeyPressed(KEY_ESCAPE)) return PauseAction::Resume;

    PauseAction chosen = PauseAction::None;
    float y = panel.y + titleH;
    for (int i = 0; i < count; ++i) {
        Rectangle r = {panel.x + std::round(18.0f * m.scale), y,
                       panel.width - std::round(36.0f * m.scale), btnH};
        const bool hover = CheckCollisionPointRec(GetMousePosition(), r);
        if (hover) focus = i;
        const bool active = (focus == i);
        DrawRectangleRec(r, active ? angelsui::kBtnActive
                                   : (hover ? angelsui::kBtnHover : angelsui::kBtnNormal));
        DrawRectangleLinesEx(r, 1, active ? angelsui::kEdge : angelsui::kBorder);
        const int fs = (int)std::round(14.0f * m.scale);
        const int tw = MeasureText(labels[i], fs);
        DrawText(labels[i], (int)(r.x + (r.width - tw) * 0.5f),
                 (int)(r.y + (r.height - fs) * 0.5f), fs,
                 active ? angelsui::kText : angelsui::kTextSoft);
        if (hover && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            SoundManager::Instance().PlayUIClick();
            chosen = (PauseAction)(i + 1);
        }
        y += btnH + gap;
    }
    if (IsKeyPressed(KEY_ENTER)) chosen = (PauseAction)(focus + 1);

    if (savedTimer > 0.0f) {
        const char* msg = "Saved";
        const int fs = (int)std::round(12.0f * m.scale);
        DrawText(msg, (int)(panel.x + (panel.width - MeasureText(msg, fs)) * 0.5f),
                 (int)(panel.y + panel.height - std::round(24.0f * m.scale)), fs,
                 (Color){140, 230, 170, 240});
    } else {
        const char* hint = "Esc resumes  |  Up/Down + Enter";
        const int fs = (int)std::round(10.0f * m.scale);
        DrawText(hint, (int)(panel.x + (panel.width - MeasureText(hint, fs)) * 0.5f),
                 (int)(panel.y + panel.height - std::round(24.0f * m.scale)), fs,
                 angelsui::kTextDim);
    }

    if (chosen == PauseAction::SaveGame) savedTimer = 2.0f;
    return chosen;
}

} // namespace angelsmenu
