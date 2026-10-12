#pragma once

// Settings / developer-settings UI panels. Split out of Settings.hpp so the
// settings *state* header stays free of raygui panel code.

#include "../Settings.hpp"
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

    // Sized to fit the content; the old 360x520 clipped the window-size row.
    const float W = 380.0f, H = 584.0f;
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
