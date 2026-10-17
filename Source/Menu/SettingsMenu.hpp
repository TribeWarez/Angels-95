#pragma once

// Settings / developer-settings UI panels. Split out of Settings.hpp so the
// settings *state* header stays free of raygui panel code.

#include "../Settings.hpp"
#include "../GameplayFlags.hpp"
#include "../Audio/SoundManager.hpp"
#include "../Renderer/raygui/raygui.h"
#include "MenuStyle.hpp"

void ToggleSettings() {
    if (ShowSettings) {
        ShowSettings = false;
        HideCursor();
        DisableCursor();
    } else {
        ShowSettings = true;
        ShowCursor();
        EnableCursor();
    }
}

void UpdateSettings() {
    if (!ShowSettings) return;

    // Tall enough for every group including the crosshair section. The content
    // is laid out top-down from a fixed cursor, so on a short screen the bottom
    // (the crosshair group) is simply not reached rather than overlapping the
    // hotbar. The cap is generous so a 1080p screen shows the whole panel - the
    // Audio/Gameplay/Window groups sit near the top and are always visible.
    const float W = 380.0f;
    const float H = fminf(1000.0f, (float)GetScreenHeight() - 40.0f);
    Rectangle win = { 20, 20, W, H };
    if (angelsui::Panel(win, "Angels95 Settings")) {
        ShowSettings = false;
        HideCursor();
        DisableCursor();
        return;
    }

    const float x0 = win.x + 14, x1 = win.x + W - 14;
    const float gap = 8, w3 = (x1 - x0 - gap * 2) / 3;
    float y = win.y + 42;

    // Latched toggles: the button shows ON/OFF and lights up when set.
    auto tgl = [&](int col, const char* label, bool on) {
        return angelsui::Button({ x0 + col * (w3 + gap), y, w3, 26 },
                                TextFormat("%s %s", label, on ? "ON" : "OFF"), on);
    };
    if (tgl(0, "Debug",   Debug))       Debug = !Debug;
    if (tgl(1, "H.Bob",   HeadBob))     HeadBob = !HeadBob;
    if (tgl(2, "P.Shad",  PixelShader)) PixelShader = !PixelShader;
    y += 34;
    if (tgl(0, "Particles", ParticlesEnabled)) ParticlesEnabled = !ParticlesEnabled;
    if (tgl(1, "FPS",       FPSEnabled))       FPSEnabled = !FPSEnabled;
    if (tgl(2, "Log",       ShowLogWindow))    ShowLogWindow = !ShowLogWindow;
    y += 34;
    if (tgl(0, "VSync",  VSYNCToggle)) {
        VSYNCToggle = !VSYNCToggle;
        // SetWindowState/ClearWindowState. SetConfigFlags after InitWindow does not
        // touch the live window, so the old toggle silently did nothing.
        if (VSYNCToggle) SetWindowState(FLAG_VSYNC_HINT);
        else             ClearWindowState(FLAG_VSYNC_HINT);
    }
    if (tgl(1, "Jitter", JitterEnabled)) JitterEnabled = !JitterEnabled;
    if (tgl(2, "Fog",    FogEnabled))    FogEnabled = !FogEnabled;
    y += 42;

    // Audio: master plus the two per-category gains. raylib has only a global
    // master volume, so music and SFX are separate gains on SoundManager.
    angelsui::Group({ x0, y, x1 - x0, 96 }, "Audio");
    {
        float mv = angelsui::Slider({ x0 + 8, y + 22, x1 - x0 - 16, 20 },
                                    TextFormat("Master  %.0f", AudioSlider), AudioSlider, 0.0f, 100.0f);
        if (mv != AudioSlider) { AudioSlider = mv; ApplyMasterVolume(); }

        float mus = angelsui::Slider({ x0 + 8, y + 46, x1 - x0 - 16, 20 },
                                     TextFormat("Music  %.0f", MusicVolume), MusicVolume, 0.0f, 100.0f);
        if (mus != MusicVolume) { MusicVolume = mus; SoundManager::Instance().SetMusicGain(MusicVolume / 100.0f); }

        float sfx = angelsui::Slider({ x0 + 8, y + 70, x1 - x0 - 16, 20 },
                                     TextFormat("Sound  %.0f", SfxVolume), SfxVolume, 0.0f, 100.0f);
        if (sfx != SfxVolume) { SfxVolume = sfx; SoundManager::Instance().SetSfxGain(SfxVolume / 100.0f); }
    }
    y += 108;

    // Gameplay: weapon hand + gore. Both mirror into GameplayFlags.hpp so the
    // gameplay TUs (ViewModel, OzPawnSystem) read what the player chose.
    angelsui::Group({ x0, y, x1 - x0, 60 }, "Gameplay");
    {
        const float half = (x1 - x0 - 24) / 2.0f;
        if (angelsui::Button({ x0 + 8, y + 24, half, 24 },
                             WeaponHand ? "Hand: LEFT" : "Hand: RIGHT", WeaponHand != 0)) {
            WeaponHand = WeaponHand ? 0 : 1;
            g_weaponHand = WeaponHand;
        }
        if (angelsui::Button({ x0 + 16 + half, y + 24, half, 24 },
                             GoreEnabled ? "Gore: ON" : "Gore: OFF", GoreEnabled)) {
            GoreEnabled = !GoreEnabled;
            g_goreEnabled = GoreEnabled;
        }
    }
    y += 72;

    // Window: borderless (Alt+Left-drag to move) or a decorated window with a
    // title bar. Applies live.
    angelsui::Group({ x0, y, x1 - x0, 60 }, "Window");
    if (angelsui::Button({ x0 + 8, y + 24, x1 - x0 - 16, 24 },
                         BorderlessWindow ? "Borderless (Alt+Drag to move)"
                                          : "Windowed (title bar)",
                         BorderlessWindow)) {
        BorderlessWindow = !BorderlessWindow;
        if (BorderlessWindow) SetWindowState(FLAG_WINDOW_UNDECORATED);
        else                  ClearWindowState(FLAG_WINDOW_UNDECORATED);
    }
    y += 72;

    // Texture filter (click-to-cycle, so no raygui dropdown).
    angelsui::Group({ x0, y, x1 - x0, 60 }, "Texture Filter");
    angelsui::Label({ x0 + 8, y + 23, 46, 20 }, "Mode");
    TextureFilterMode = angelsui::Cycle({ x0 + 56, y + 22, x1 - x0 - 64, 24 },
                                        FilterNames, 6, TextureFilterMode);
    y += 74;

    // Pixelation.
    angelsui::Group({ x0, y, x1 - x0, 56 }, "Pixelation");
    PixelSize = angelsui::Slider({ x0 + 8, y + 22, x1 - x0 - 16, 22 },
                                 TextFormat("Size  %.0f", PixelSize), PixelSize, 1.0f, 32.0f);
    y += 70;

    // Vertex jitter.
    angelsui::Group({ x0, y, x1 - x0, 56 }, "Vertex Jitter");
    JitterIntensity = angelsui::Slider({ x0 + 8, y + 22, x1 - x0 - 16, 22 },
                                       TextFormat("Intensity  %.1f", JitterIntensity),
                                       JitterIntensity, 0.0f, 10.0f);
    y += 70;

    // Resolution scale.
    angelsui::Group({ x0, y, x1 - x0, 84 }, "Resolution Scale");
    ResolutionScale = angelsui::Slider({ x0 + 8, y + 22, x1 - x0 - 16, 22 },
                                       TextFormat("Scale  %.2f", ResolutionScale),
                                       ResolutionScale, 0.0f, 3.0f);
    if (angelsui::Button({ x0 + 8, y + 50, 140, 24 }, "Set Resolution")) {
        UnloadRenderTexture(Target);
        Target = LoadRenderTexture(int(1280 * ResolutionScale), int(720 * ResolutionScale));
    }
    y += 98;

    // Window size.
    angelsui::Group({ x0, y, x1 - x0, 60 }, "Window Size");
    static const char* resNames[5] = { "480p", "720p", "1080p", "1440p", "4K" };
    static const int   resW[5] = { 640, 1280, 1920, 2560, 3840 };
    static const int   resH[5] = { 480, 720, 1080, 1440, 2160 };
    const float bw = (x1 - x0 - 16 - 4 * 6) / 5;
    for (int i = 0; i < 5; i++) {
        if (angelsui::Button({ x0 + 8 + i * (bw + 6), y + 24, bw, 24 }, resNames[i],
                             false, 11)) {
            ConfigWindowWidth = resW[i];
            ConfigWindowHeight = resH[i];
            SetWindowSize(ConfigWindowWidth, ConfigWindowHeight);
        }
    }
    y += 68;

    // Crosshair. Everything here is persisted to System/Angels95.ini by
    // SaveClientSettings and read back by LoadClientSettings, so it survives a
    // restart. Drawn by Renderer/Crosshair.hpp.
    angelsui::Group({ x0, y, x1 - x0, 132 }, "Crosshair");
    {
        const float gx = x0 + 8, gw = x1 - x0 - 16, half = (gw - 8) / 2;

        angelsui::Label({ gx, y + 22, 46, 20 }, "Style");
        CrosshairStyleSel = angelsui::Cycle({ gx + 50, y + 22, gw - 50, 24 },
                                             kCrosshairStyleNames,
                                             CROSSHAIR_STYLE_COUNT, CrosshairStyleSel);

        CrosshairSize = (int)(angelsui::Slider({ gx, y + 52, half, 22 },
                                                TextFormat("Size  %.0f", CrosshairSize),
                                                (float)CrosshairSize, 2.0f, 48.0f) + 0.5f);
        CrosshairGap = (int)(angelsui::Slider({ gx + half + 8, y + 52, half, 22 },
                                               TextFormat("Gap  %.0f", CrosshairGap),
                                               (float)CrosshairGap, 0.0f, 48.0f) + 0.5f);

        CrosshairThickness = angelsui::Slider({ gx, y + 82, half, 22 },
                                              TextFormat("Width  %.1f", CrosshairThickness),
                                              CrosshairThickness, 1.0f, 6.0f);
        CrosshairA = (int)(angelsui::Slider({ gx + half + 8, y + 82, half, 22 },
                                             TextFormat("Alpha  %.0f", CrosshairA),
                                             (float)CrosshairA, 0.0f, 255.0f) + 0.5f);

        // Colour presets rather than three more sliders: a crosshair is either
        // one of a handful of colours, or the player is tuning it by hand.
        struct Preset { const char* name; int r, g, b; };
        static const Preset kPresets[] = {
            { "White", 255, 255, 255 }, { "Green", 80, 255, 80 },
            { "Red",   255, 80,  80 },   { "Amber", 255, 200, 60 },
            { "Cyan",  80, 220, 255 },
        };
        const int nPresets = (int)(sizeof(kPresets) / sizeof(kPresets[0]));
        const float pw = (gw - (nPresets - 1) * 6) / nPresets;
        for (int i = 0; i < nPresets; i++) {
            const bool sel = (CrosshairR == kPresets[i].r && CrosshairG == kPresets[i].g &&
                              CrosshairB == kPresets[i].b);
            if (angelsui::Button({ gx + i * (pw + 6), y + 108, pw, 20 }, kPresets[i].name, sel, 10)) {
                CrosshairR = kPresets[i].r;
                CrosshairG = kPresets[i].g;
                CrosshairB = kPresets[i].b;
            }
        }
    }
    y += 140;

    // Visibility toggle sits below the group so it stays reachable (and so a
    // player who turned the crosshair off can find the switch again).
    if (angelsui::Button({ x0, y, x1 - x0 - 8, 26 },
                         CrosshairVisible ? "Crosshair: ON" : "Crosshair: OFF",
                         CrosshairVisible)) {
        CrosshairVisible = !CrosshairVisible;
    }

    // Log output, in the same framing.
    if (ShowLogWindow) {
        Rectangle log = { win.x + W + 10, win.y, 500, 500 };
        if (angelsui::Panel(log, "Log Output")) ShowLogWindow = false;
        angelsui::Group({ log.x + 8, log.y + 38, log.width - 16, log.height - 46 }, "Output");
        int ly = (int)log.y + 60;
        for (const auto& ll : Log::get_buffer()) {
            if (ly > log.y + log.height - 16) break;
            DrawText(ll.text, (int)log.x + 16, ly, 12, angelsui::kTextDim);
            ly += 16;
        }
    }
}

void ShowMenuSetiings() {
    Rectangle LayoutRecs[15] = {
        (Rectangle){ 72, 85, 120, 24 },
        (Rectangle){ 62, 115, 391, 419 },
        (Rectangle){ 71, 145, 374, 200 },
        (Rectangle){ 94, 186, 120, 24 },
        (Rectangle){ 172, 237, 120, 24 },
        (Rectangle){ 171, 278, 120, 24 },
        (Rectangle){ 311, 258, 120, 24 },
        (Rectangle){ 325, 176, 88, 24 },
        (Rectangle){ 325, 214, 88, 24 },
        (Rectangle){ 336, 399, 88, 24 },
        (Rectangle){ 182, 407, 120, 16 },
        (Rectangle){ 72, 361, 374, 85 },
        (Rectangle){ 73, 452, 120, 24 },
        (Rectangle){ 74, 486, 146, 26 },
        (Rectangle){ 268, 475, 140, 28 },
    };
    GuiWindowBox(LayoutRecs[1], "Settings");
    GuiLabel(LayoutRecs[0], "Angels95 Settings");
    GuiPanel(LayoutRecs[2], "Window");
    if (GuiButton(LayoutRecs[3], "Toggle Fullscreen")) ToggleFullscreen();
    VSYNCToggle = GuiToggle(LayoutRecs[7], "VSync", VSYNCToggle);
    MXAAToggle = GuiToggle(LayoutRecs[8], "MXAA x4", MXAAToggle);

    GuiPanel(LayoutRecs[11], "Audio");
    MuteToggle = GuiToggle(LayoutRecs[9], "Mute Audio", MuteToggle);
    AudioSlider = GuiSlider(LayoutRecs[10], "Audio Volume", NULL, AudioSlider, 0, 100);

    GuiLabel(LayoutRecs[12], "ANGELS95 ");
    GuiLabel(LayoutRecs[13], "TribeWarez 2025");
    GuiLabel(LayoutRecs[14], "@EC, JF , ZT , NC , LC");

    if (VSYNCToggle) {
        SetConfigFlags(FLAG_VSYNC_HINT);
    } else {
        ClearWindowState(FLAG_VSYNC_HINT);
    }

    if (MXAAToggle) {
        SetConfigFlags(FLAG_MSAA_4X_HINT);
    } else {
        ClearWindowState(FLAG_MSAA_4X_HINT);
    }
    ApplyMasterVolume();
}
