#pragma once
// ============================================================================
// Menu/AdvancedOptions.hpp
//
// "Advanced Options" - a UT99-style settings dialog: a category tree on the
// left, the selected category's properties on the right, all live-applied.
//
// Two rules shaped this file, both from AGENTS.md:
//
//   1. **A menu entry is either functional or hidden.** Every row here points at
//      a setting that something actually reads. The UT99 tree this was modelled
//      on has leaves this engine cannot honour - Shadow Quality (there are no
//      shadows at all), Detail Textures (the uniform exists but is gated off and
//      no asset ships), LOD/Decal bias, Netspeed/Client Rate/Actor Replication
//      Rate (UDP is event-driven here, there is no send-rate system), Simulated
//      Lag/Loss and Predict Loss. They are deliberately ABSENT rather than
//      present-and-inert; adding one means adding its consumer first.
//
//   2. **Settings live in one place.** Every pointer below is the SAME variable
//      the in-game Settings panel and System/Angels95.ini use, so the two UIs
//      cannot disagree. Cross-TU mirrors (g_goreEnabled, g_weaponHand, ...) are
//      updated through each row's `after` hook.
//
// The dialog is drawn on top of everything (raylib), so it works in the title
// menu and in game. It is opened from the native menu bar's Settings >
// "Advanced Options..." item, the console's `/advanced`, or a console command.
// Round-tripping the persisted keys is covered by tests/ClientSettings.test.cpp.
// ============================================================================
#include "../Settings.hpp"
#include "../GameplayFlags.hpp"
#include "MenuStyle.hpp"
#include <cstdio>
#include <cstring>
#include <vector>

namespace advopts {

// One instance across TUs (the overlay is header-only; Main.cpp and TitleMenu
// can both include it).
inline bool g_open = false;

// Live network facts, refreshed by Main.cpp each frame while the dialog is open.
// The menu header deliberately does not include Client/Network: the panel shows
// what the caller tells it.
struct NetInfo {
    bool connected = false;
    int  pingMs = -1;          // -1 = no measurement yet
    int  serverWorld = -1;     // what the server says we are in
    int  localWorld = -1;      // what the client renders
    int  players = 0;
    int  protocol = 0;
    const char* server = "";
    const char* build = "";
};
inline NetInfo g_net;

enum class Kind { Bool, Byte, Float, Int, Enum, Action, Info };

struct Row {
    int node = 0;              // tree node whose property list owns this row
    Kind kind = Kind::Info;
    const char* label = "";
    bool*  b = nullptr;
    unsigned char* u = nullptr;
    float* f = nullptr;
    int*   i = nullptr;
    float lo = 0.0f, hi = 1.0f;
    const char* const* items = nullptr;
    int count = 0;
    const char* suffix = "";
    void (*action)() = nullptr;
    void (*after)() = nullptr;         // push cross-TU mirrors / apply at once
    const char* (*info)() = nullptr;   // Kind::Info dynamic value
    const char* note = "";
};

struct Node {
    const char* label;
    int parent;                // -1 = root
};

// Node ids. Named so inserting a branch does not renumber every Row::node literal
// (they were magic numbers, and adding Lighting between Rendering and Fog would
// have silently shifted ten of them).
enum NodeId : int {
    N_Display = 0,
    N_Rendering,
    N_Lighting,
    N_Fog,
    N_Effects,
    N_Quality,
    N_Window,
    N_Audio,
    N_Input,
    N_Network,
    N_Game,
    N_Gameplay,
    N_Hud,
    N_Debug,
    N_Count
};

// The tree. Order here is DRAW order: a child must follow its parent.
inline const Node kNodes[] = {
    /* Display */   { "Display",      -1 },
    /* Rendering */ { "Rendering",     N_Display },
    /* Lighting */  { "Lighting",      N_Rendering },
    /* Fog */       { "Fog",           N_Rendering },
    /* Effects */   { "Effects",       N_Rendering },
    /* Quality */   { "Quality",       N_Rendering },
    /* Window */    { "Window",        N_Display },
    /* Audio */     { "Audio",        -1 },
    /* Input */     { "Input",        -1 },
    /* Network */   { "Network",      -1 },
    /* Game */      { "Game",         -1 },
    /* Gameplay */  { "Gameplay",      N_Game },
    /* HUD */       { "HUD",           N_Game },
    /* Debug */     { "Debug",        -1 },
};
static_assert(N_Count == (int)(sizeof(kNodes) / sizeof(kNodes[0])),
              "NodeId and kNodes must list the same nodes in the same order");
inline constexpr int kNodeCount = N_Count;

// Enum vocabularies that Settings.hpp/Crosshair do not name for us.
inline const char* const kHandNames[2] = { "Right", "Left" };

inline int DepthOf(int n) {
    int d = 0;
    for (int p = kNodes[n].parent; p >= 0 && d < 16; p = kNodes[p].parent) d++;
    return d;
}

// ---------------------------------------------------------------------------
// Apply hooks. Anything that has a cross-TU mirror or needs a live window call
// gets one, so an edit here and an edit in the in-game panel behave identically.
// ---------------------------------------------------------------------------
inline void ApplyVSync() {
    if (VSYNCToggle) SetWindowState(FLAG_VSYNC_HINT);
    else             ClearWindowState(FLAG_VSYNC_HINT);
}
inline void ApplyGore()  { g_goreEnabled = GoreEnabled; }
inline void ApplyHand()  { g_weaponHand  = WeaponHand; }
inline void ApplyScale() { g_hudScale    = UiScale; }

inline void SetWindowSizeNow(int w, int h) {
    ConfigWindowWidth = w;
    ConfigWindowHeight = h;
    SetWindowSize(w, h);
}
inline void Res1280x720()  { SetWindowSizeNow(1280, 720); }
inline void Res1600x900()  { SetWindowSizeNow(1600, 900); }
inline void Res1920x1080() { SetWindowSizeNow(1920, 1080); }
inline void Res2560x1440() { SetWindowSizeNow(2560, 1440); }

// Dynamic info values (static buffer: drawn immediately, never stored).
inline const char* InfoResolution() {
    static char buf[40];
    snprintf(buf, sizeof buf, "%d x %d", GetScreenWidth(), GetScreenHeight());
    return buf;
}
inline const char* InfoGameWindow() {
    static char buf[40];
    snprintf(buf, sizeof buf, "%d x %d", ConfigWindowWidth, ConfigWindowHeight);
    return buf;
}
inline const char* InfoNetStatus() {
    return g_net.connected ? "Connected" : "Offline";
}
inline const char* InfoNetServer() {
    return (g_net.connected && g_net.server && g_net.server[0]) ? g_net.server : "(none)";
}
inline const char* InfoNetPing() {
    static char buf[32];
    if (!g_net.connected) return "-";
    if (g_net.pingMs < 0) return "measuring...";
    snprintf(buf, sizeof buf, "%d ms", g_net.pingMs);
    return buf;
}
inline const char* InfoNetWorld() {
    static char buf[48];
    if (!g_net.connected) return "-";
    snprintf(buf, sizeof buf, "server %d / local %d", g_net.serverWorld, g_net.localWorld);
    return buf;
}
inline const char* InfoNetIdentity() {
    static char buf[64];
    snprintf(buf, sizeof buf, "protocol %d, build %s", g_net.protocol,
             (g_net.build && g_net.build[0]) ? g_net.build : "?");
    return buf;
}
inline const char* InfoKeybinds() { return "Settings > Controls"; }

// Row factories: keep the table readable and the field set in one place.
inline Row BoolRow(int node, const char* label, bool* p,
                   void (*after)() = nullptr, const char* note = "") {
    Row r; r.node = node; r.kind = Kind::Bool; r.label = label; r.b = p;
    r.after = after; r.note = note; return r;
}
inline Row FloatRow(int node, const char* label, float* p, float lo, float hi,
                    const char* suffix = "", void (*after)() = nullptr,
                    const char* note = "") {
    Row r; r.node = node; r.kind = Kind::Float; r.label = label; r.f = p;
    r.lo = lo; r.hi = hi; r.suffix = suffix; r.after = after; r.note = note;
    return r;
}
inline Row IntRow(int node, const char* label, int* p, float lo, float hi,
                  const char* suffix = "", void (*after)() = nullptr,
                  const char* note = "") {
    Row r; r.node = node; r.kind = Kind::Int; r.label = label; r.i = p;
    r.lo = lo; r.hi = hi; r.suffix = suffix; r.after = after; r.note = note;
    return r;
}
// A 0..255 channel. Color's members are `unsigned char`, so an Int row cannot
// alias them without writing four bytes through a one-byte pointer.
inline Row ByteRow(int node, const char* label, unsigned char* p,
                   void (*after)() = nullptr, const char* note = "") {
    Row r; r.node = node; r.kind = Kind::Byte; r.label = label; r.u = p;
    r.lo = 0.0f; r.hi = 255.0f; r.after = after; r.note = note;
    return r;
}
inline Row EnumRow(int node, const char* label, int* p, const char* const* items,
                   int count, void (*after)() = nullptr, const char* note = "") {
    Row r; r.node = node; r.kind = Kind::Enum; r.label = label; r.i = p;
    r.items = items; r.count = count; r.after = after; r.note = note;
    return r;
}
inline Row ActionRow(int node, const char* label, void (*action)()) {
    Row r; r.node = node; r.kind = Kind::Action; r.label = label; r.action = action;
    return r;
}
inline Row InfoRow(int node, const char* label, const char* (*info)()) {
    Row r; r.node = node; r.kind = Kind::Info; r.label = label; r.info = info;
    return r;
}

inline std::vector<Row>& Rows() {
    static std::vector<Row> rows;
    if (!rows.empty()) return rows;

    // --- Display > Rendering > Lighting -------------------------------------
    // Uniform multipliers into the shared lighting model (LitFog.fs + Surface.fs
    // + the Wind program). The defaults are the authored look: 1/1/0.
    rows.push_back(FloatRow(N_Lighting, "Ambient Boost", &g_lightAmbientBoost, 0.0f, 3.0f,
                            "", nullptr,
                            "Multiplies the ambient term. 1.0 is the authored level; "
                            "raise it to lift unlit corners."));
    rows.push_back(FloatRow(N_Lighting, "Specular Scale", &g_lightSpecularScale, 0.0f, 3.0f,
                            "", nullptr,
                            "Blinn-Phong highlight strength. 0 removes the sheen."));
    rows.push_back(FloatRow(N_Lighting, "Light Softness", &g_lightSoftness, 0.0f, 1.0f,
                            "", nullptr,
                            "Widens the distance falloff: 0 is the authored curve, 1 lets "
                            "lights reach about twice as far."));
    rows.push_back(BoolRow(N_Lighting, "Dynamic Lights", &g_dynamicLights, nullptr,
                           "Torch/fire/lamp flicker. Off uploads period 0, which the "
                           "shader already treats as static."));

    // --- Display > Rendering > Fog -----------------------------------------
    // Two different things live here and the labels say which is which:
    //   * "Screen Fog" is the post-process overlay in [AngelTech.Render].
    //   * "Level Fog" is the skyzone's authored atmospheric fog. The density
    //     multiplier always applies; the rest only replaces the authored values
    //     when Override is on, so a level's art direction is never silently
    //     retuned by a stray slider.
    rows.push_back(BoolRow(N_Fog, "Screen Fog", &FogEnabled));
    rows.push_back(FloatRow(N_Fog, "Screen Fog Blend", &FogIntensity, 0.0f, 1.0f, "", nullptr,
                            "Post-process fog blend amount."));
    rows.push_back(ByteRow(N_Fog, "Screen Tint Red", &FogTint.r));
    rows.push_back(ByteRow(N_Fog, "Screen Tint Green", &FogTint.g));
    rows.push_back(ByteRow(N_Fog, "Screen Tint Blue", &FogTint.b));
    rows.push_back(FloatRow(N_Fog, "Level Fog Density", &g_fogDensityScale, 0.0f, 3.0f,
                            "x", nullptr,
                            "Multiplies whatever haze the level authored (always applied)."));
    rows.push_back(BoolRow(N_Fog, "Override Level Fog", &g_fogOverride, nullptr,
                           "Replace the skyzone's fog with the values below."));
    rows.push_back(FloatRow(N_Fog, "Fog Start", &g_fogStart, 0.0f, 500.0f, " u"));
    rows.push_back(FloatRow(N_Fog, "Fog End", &g_fogEnd, 10.0f, 2000.0f, " u"));
    rows.push_back(FloatRow(N_Fog, "Fog Density", &g_fogDensity, 0.0f, 0.05f, "",
                            nullptr, "Exponential rate per world unit (shipped levels: "
                                     "0.003-0.007)."));
    rows.push_back(ByteRow(N_Fog, "Fog Red", &g_fogColorR));
    rows.push_back(ByteRow(N_Fog, "Fog Green", &g_fogColorG));
    rows.push_back(ByteRow(N_Fog, "Fog Blue", &g_fogColorB));

    // --- Display > Rendering > Effects -------------------------------------
    rows.push_back(BoolRow(N_Effects, "Particles", &ParticlesEnabled, nullptr,
                           "Ambient emitters and weather. Gameplay bursts (impacts, blood) "
                           "still play."));
    rows.push_back(BoolRow(N_Effects, "Gore", &GoreEnabled, &ApplyGore,
                           "Blood decals and death gore."));
    rows.push_back(BoolRow(N_Effects, "Post FX", &PostFXEnabled, nullptr,
                           "HDR bloom + display grade."));
    rows.push_back(FloatRow(N_Effects, "Exposure", &PostExposure, 0.25f, 3.0f));
    rows.push_back(FloatRow(N_Effects, "Bloom", &PostBloom, 0.0f, 3.0f));
    rows.push_back(FloatRow(N_Effects, "Vignette", &PostVignette, 0.0f, 0.8f));
    rows.push_back(BoolRow(N_Effects, "Pixel Shader", &PixelShader));
    rows.push_back(FloatRow(N_Effects, "Pixel Size", &PixelSize, 1.0f, 32.0f, " px"));
    rows.push_back(BoolRow(N_Effects, "Vertex Jitter", &JitterEnabled));
    rows.push_back(FloatRow(N_Effects, "Jitter Amount", &JitterIntensity, 0.0f, 10.0f));

    // --- Display > Rendering > Quality -------------------------------------
    rows.push_back(EnumRow(N_Quality, "Texture Filter", &TextureFilterMode,
                           FilterNames, 6, nullptr,
                           "Applies to textures loaded after the change."));
    rows.push_back(BoolRow(N_Quality, "MSAA", &MXAAToggle, nullptr,
                           "4x MSAA. Takes effect on the next launch."));
    rows.push_back(FloatRow(N_Quality, "Resolution Scale", &ResolutionScale, 0.25f, 4.0f));
    rows.push_back(FloatRow(N_Quality, "UI Scale", &UiScale, 0.75f, 2.0f, "", &ApplyScale));

    // --- Display > Window ---------------------------------------------------
    rows.push_back(InfoRow(N_Window, "Current", &InfoResolution));
    rows.push_back(ActionRow(N_Window, "1280 x 720", &Res1280x720));
    rows.push_back(ActionRow(N_Window, "1600 x 900", &Res1600x900));
    rows.push_back(ActionRow(N_Window, "1920 x 1080", &Res1920x1080));
    rows.push_back(ActionRow(N_Window, "2560 x 1440", &Res2560x1440));
    rows.push_back(InfoRow(N_Window, "Configured", &InfoGameWindow));
    rows.push_back(BoolRow(N_Window, "Borderless", &BorderlessWindow, nullptr,
                           "Off gives the OS title bar back."));
    rows.push_back(BoolRow(N_Window, "VSync", &VSYNCToggle, &ApplyVSync));

    // --- Audio --------------------------------------------------------------
    rows.push_back(BoolRow(N_Audio, "Mute", &MuteToggle));
    rows.push_back(FloatRow(N_Audio, "Master Volume", &AudioSlider, 0.0f, 100.0f, "%",
                            nullptr, "Applies immediately."));
    rows.push_back(FloatRow(N_Audio, "Music Volume", &MusicVolume, 0.0f, 100.0f, "%"));
    rows.push_back(FloatRow(N_Audio, "Effects Volume", &SfxVolume, 0.0f, 100.0f, "%"));

    // --- Input --------------------------------------------------------------
    rows.push_back(FloatRow(N_Input, "Mouse Sensitivity", &g_mouseSensitivity, 0.1f, 5.0f));
    rows.push_back(BoolRow(N_Input, "Invert Mouse", &g_invertY));
    rows.push_back(InfoRow(N_Input, "Key Bindings", &InfoKeybinds));

    // --- Network ------------------------------------------------------------
    // Only what the client can actually drive today: the PING cadence and the
    // stats overlay. Netspeed / client rate / actor replication rate / simulated
    // lag / packet loss do not exist in this engine's UDP path, so they are not
    // offered (see the file header).
    rows.push_back(FloatRow(N_Network, "Ping Interval", &g_netPingInterval, 1.0f, 30.0f, " s",
                            nullptr, "How often the client samples round-trip time."));
    rows.push_back(BoolRow(N_Network, "Show Network Stats", &g_showNetStats, nullptr,
                           "Compact ping/world/player readout while connected."));
    rows.push_back(InfoRow(N_Network, "Status", &InfoNetStatus));
    rows.push_back(InfoRow(N_Network, "Server", &InfoNetServer));
    rows.push_back(InfoRow(N_Network, "Ping", &InfoNetPing));
    rows.push_back(InfoRow(N_Network, "World", &InfoNetWorld));
    rows.push_back(InfoRow(N_Network, "Identity", &InfoNetIdentity));

    // --- Game > Gameplay ----------------------------------------------------
    rows.push_back(BoolRow(N_Gameplay, "Head Bob", &HeadBob, nullptr,
                           "Off also disables the low-health HUD pulse (reduced motion)."));
    rows.push_back(EnumRow(N_Gameplay, "Weapon Hand", &WeaponHand, kHandNames, 2, &ApplyHand));

    // --- Game > HUD ---------------------------------------------------------
    rows.push_back(BoolRow(N_Hud, "Crosshair", &CrosshairVisible));
    rows.push_back(EnumRow(N_Hud, "Crosshair Style", &CrosshairStyleSel,
                           kCrosshairStyleNames, CROSSHAIR_STYLE_COUNT));
    rows.push_back(IntRow(N_Hud, "Crosshair Size", &CrosshairSize, 2, 48, " px"));
    rows.push_back(IntRow(N_Hud, "Crosshair Gap", &CrosshairGap, 0, 48, " px"));
    rows.push_back(FloatRow(N_Hud, "Crosshair Thickness", &CrosshairThickness, 1.0f, 6.0f, " px"));
    rows.push_back(IntRow(N_Hud, "Crosshair Red", &CrosshairR, 0, 255));
    rows.push_back(IntRow(N_Hud, "Crosshair Green", &CrosshairG, 0, 255));
    rows.push_back(IntRow(N_Hud, "Crosshair Blue", &CrosshairB, 0, 255));
    rows.push_back(IntRow(N_Hud, "Crosshair Alpha", &CrosshairA, 0, 255));

    // --- Debug --------------------------------------------------------------
    rows.push_back(BoolRow(N_Debug, "Show Lighting Gizmos", &Debug, nullptr,
                           "Draws light/zone/emitter markers in the world."));
    rows.push_back(BoolRow(N_Debug, "Show FPS", &FPSEnabled));
    rows.push_back(BoolRow(N_Debug, "Show Log Window", &ShowLogWindow));
    return rows;
}

// Two enums that live in Settings.hpp but are not named there.
inline const char* const kHandNamesLocal[2] = { "Right", "Left" };
inline const char* const* const kHandNamesPtr = kHandNamesLocal;

// ---------------------------------------------------------------------------
// UI state
// ---------------------------------------------------------------------------
inline bool g_expanded[64] = {};
inline bool g_uiInit = false;
inline int  g_selected = 2;
inline float g_scroll = 0.0f;

inline void Toggle() { g_open = !g_open; }
inline void Close()  { g_open = false; }

inline void DrawRowControl(const Row& r, Rectangle ctl) {
    switch (r.kind) {
        case Kind::Bool: {
            bool on = r.b && *r.b;
            if (angelsui::Button(ctl, on ? "ON" : "OFF", on, 11) && r.b) {
                *r.b = !*r.b;
                if (r.after) r.after();
            }
            break;
        }
        case Kind::Float: {
            if (!r.f) break;
            float v = angelsui::Slider(ctl, TextFormat("%.2f%s", *r.f, r.suffix),
                                       *r.f, r.lo, r.hi, 11);
            if (v != *r.f) {
                *r.f = v;
                if (r.after) r.after();
            }
            break;
        }
        case Kind::Byte: {
            if (!r.u) break;
            float v = angelsui::Slider(ctl, TextFormat("%d", (int)*r.u),
                                       (float)*r.u, r.lo, r.hi, 11);
            int iv = (int)(v + 0.5f);
            if (iv != (int)*r.u) {
                *r.u = (unsigned char)(iv < 0 ? 0 : (iv > 255 ? 255 : iv));
                if (r.after) r.after();
            }
            break;
        }
        case Kind::Int: {
            if (!r.i) break;
            float v = angelsui::Slider(ctl, TextFormat("%d%s", *r.i, r.suffix),
                                       (float)*r.i, r.lo, r.hi, 11);
            int iv = (int)(v + (v < 0.0f ? -0.5f : 0.5f));
            if (iv != *r.i) {
                *r.i = iv;
                if (r.after) r.after();
            }
            break;
        }
        case Kind::Enum: {
            if (!r.i || !r.items || r.count <= 0) break;
            int idx = angelsui::Cycle(ctl, r.items, r.count, *r.i);
            if (idx != *r.i) {
                *r.i = idx;
                if (r.after) r.after();
            }
            break;
        }
        case Kind::Action: {
            if (angelsui::Button(ctl, r.label, false, 11) && r.action) r.action();
            break;
        }
        case Kind::Info: {
            const char* v = r.info ? r.info() : "-";
            int tw = MeasureText(v, 11);
            DrawText(v, (int)(ctl.x + ctl.width - tw), (int)(ctl.y + (ctl.height - 11) / 2),
                     11, angelsui::kTextSoft);
            break;
        }
    }
}

inline void Draw() {
    if (!g_open) return;
    if (!g_uiInit) {
        for (int i = 0; i < kNodeCount; ++i) g_expanded[i] = (DepthOf(i) < 2);
        g_uiInit = true;
    }

    const int sw = GetScreenWidth(), sh = GetScreenHeight();
    const float w = fminf(1000.0f, (float)sw - 60.0f);
    const float h = fminf(660.0f, (float)sh - 60.0f);
    Rectangle win = { (sw - w) * 0.5f, (sh - h) * 0.5f, w, h };
    if (angelsui::Panel(win, "Angels95 Advanced Options")) { g_open = false; return; }

    const float treeW = fminf(310.0f, w * 0.34f);
    Rectangle tree = { win.x + 12, win.y + 42, treeW, h - 96 };
    Rectangle prop = { tree.x + tree.width + 12, tree.y,
                       win.width - treeW - 36, tree.height };

    // ---- category tree -----------------------------------------------------
    DrawRectangleRec(tree, (Color){ 10, 16, 30, 190 });
    DrawRectangleLinesEx(tree, 1, angelsui::kBorderDim);

    float ty = tree.y + 6;
    int hoveredNode = -1;
    for (int i = 0; i < kNodeCount; ++i) {
        // Hidden when any ancestor is collapsed.
        bool visible = true;
        for (int p = kNodes[i].parent; p >= 0; p = kNodes[p].parent)
            if (!g_expanded[p]) { visible = false; break; }
        if (!visible) continue;

        const int depth = DepthOf(i);
        const bool hasKids = (i + 1 < kNodeCount && kNodes[i + 1].parent == i);
        Rectangle row = { tree.x + 2, ty, tree.width - 4, 22 };
        bool hover = CheckCollisionPointRec(GetMousePosition(), row);
        if (hover) hoveredNode = i;
        if (i == g_selected)
            DrawRectangleRec(row, (Color){ 40, 80, 140, 200 });
        else if (hover)
            DrawRectangleRec(row, (Color){ 26, 40, 70, 200 });

        const float ind = 8.0f + depth * 14.0f;
        if (hasKids) {
            // Expand/collapse caret, click target is the caret box only so the
            // row itself stays a selection like UT's tree.
            Rectangle caret = { row.x + ind - 2, row.y + 5, 12, 12 };
            DrawRectangleLines((int)caret.x, (int)caret.y, (int)caret.width,
                               (int)caret.height, angelsui::kBorder);
            DrawRectangle((int)caret.x + 3, (int)(caret.y + 5), 6, 2, angelsui::kText);
            if (g_expanded[i])
                DrawRectangle((int)caret.x + 6, (int)caret.y + 3, 2, 6, angelsui::kText);
            if (CheckCollisionPointRec(GetMousePosition(), caret) &&
                IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                g_expanded[i] = !g_expanded[i];
            }
        }
        DrawText(kNodes[i].label, (int)(row.x + ind + 16),
                 (int)(row.y + 6), 12,
                 (i == g_selected || hover) ? angelsui::kText : angelsui::kTextSoft);
        if (hover && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            g_selected = i;
            g_scroll = 0.0f;
        }
        ty += 22;
    }

    // ---- property pane -----------------------------------------------------
    DrawRectangleRec(prop, (Color){ 8, 13, 26, 200 });
    DrawRectangleLinesEx(prop, 1, angelsui::kBorderDim);

    // Breadcrumb header: Display > Rendering > Fog
    {
        char crumb[256] = {0};
        int chain[16], n = 0;
        for (int p = g_selected; p >= 0 && n < 16; p = kNodes[p].parent) chain[n++] = p;
        for (int i = n - 1; i >= 0; --i) {
            strncat(crumb, kNodes[chain[i]].label, sizeof(crumb) - strlen(crumb) - 1);
            if (i) strncat(crumb, " > ", sizeof(crumb) - strlen(crumb) - 1);
        }
        DrawText(crumb, (int)(prop.x + 10), (int)(prop.y + 8), 13, angelsui::kText);
        DrawLine((int)prop.x + 8, (int)prop.y + 28,
                 (int)(prop.x + prop.width) - 8, (int)prop.y + 28, angelsui::kBorderDim);
    }

    // Rows for the selected category. Groups with no rows of their own (Display,
    // Rendering, Game) say so instead of showing an empty pane.
    const float rowH = 27.0f;
    const float pad = 10.0f;
    const float lx = prop.x + pad;
    const float ctlW = fminf(230.0f, prop.width * 0.42f);
    const float ctlX = prop.x + prop.width - pad - ctlW;
    const float labelW = ctlX - lx - 12.0f;

    Rectangle list = { prop.x + 1, prop.y + 32, prop.width - 2, prop.height - 74 };
    int rowCount = 0;
    for (const Row& r : Rows()) if (r.node == g_selected) rowCount++;
    float contentH = rowCount * rowH + 8.0f;
    float maxScroll = fmaxf(0.0f, contentH - list.height);
    if (CheckCollisionPointRec(GetMousePosition(), list)) {
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) g_scroll -= wheel * 40.0f;
    }
    g_scroll = fmaxf(0.0f, fminf(g_scroll, maxScroll));

    BeginScissorMode((int)list.x, (int)list.y, (int)list.width, (int)list.height);
    float ry = list.y + 4 - g_scroll;
    const Row* hovered = nullptr;
    for (const Row& r : Rows()) {
        if (r.node != g_selected) { continue; }
        Rectangle row = { list.x + 4, ry, list.width - 8, rowH - 3 };
        bool hover = CheckCollisionPointRec(GetMousePosition(), row) &&
                     CheckCollisionPointRec(GetMousePosition(), list);
        if (hover) {
            hovered = &r;
            DrawRectangleRec(row, (Color){ 24, 38, 66, 160 });
        }

        if (r.kind == Kind::Action) {
            // Actions are the whole row: a menu-style button.
            Rectangle btn = { row.x + 4, row.y + 2, row.width - 8, row.height - 4 };
            if (angelsui::Button(btn, r.label, false, 11) && r.action) r.action();
        } else {
            DrawText(r.label, (int)(row.x + 4),
                     (int)(row.y + (row.height - 11) / 2), 11, angelsui::kTextSoft);
            Rectangle ctl = { ctlX, row.y + 2, ctlW, row.height - 4 };
            DrawRowControl(r, ctl);
        }
        ry += rowH;
    }
    EndScissorMode();

    if (rowCount == 0) {
        DrawText("Pick a category on the left.", (int)lx, (int)(list.y + 10), 12,
                 angelsui::kTextDim);
    }
    if (maxScroll > 0.0f) {
        // Slim scroll indicator on the right edge of the list.
        float t = (contentH > 0.0f) ? (list.height / contentH) : 1.0f;
        float barH = fmaxf(20.0f, list.height * t);
        float barY = list.y + (list.height - barH) * (g_scroll / maxScroll);
        DrawRectangle((int)(list.x + list.width - 3), (int)barY, 3, (int)barH,
                      angelsui::kBorder);
    }

    // Hovered row's explanation, then the footer.
    if (hovered && hovered->note && hovered->note[0]) {
        DrawText(hovered->note, (int)lx, (int)(prop.y + prop.height - 46), 11,
                 angelsui::kTextDim);
    }
    DrawText("Changes apply immediately and are saved on exit.",
             (int)lx, (int)(prop.y + prop.height - 28), 11, angelsui::kTextDim);

    Rectangle closeBtn = { win.x + win.width - 118, win.y + win.height - 36, 100, 24 };
    if (angelsui::Button(closeBtn, "Close", false, 12)) g_open = false;
}

// Compact network readout, drawn by the caller when g_showNetStats is on.
inline void DrawNetStats(const NetInfo& n) {
    if (!n.connected) return;
    char ping[32];
    if (n.pingMs < 0) snprintf(ping, sizeof ping, "--");
    else              snprintf(ping, sizeof ping, "%d ms", n.pingMs);

    const int w = 300, h = 40;
    const int x = GetScreenWidth() - w - 12, y = 12;
    DrawRectangle(x, y, w, h, (Color){ 6, 12, 30, 170 });
    DrawRectangleLines(x, y, w, h, angelsui::kBorderDim);
    DrawText(TextFormat("NET  ping %s", ping), x + 8, y + 5, 12, angelsui::kTextSoft);
    DrawText(TextFormat("world %d / %d   players %d", n.serverWorld, n.localWorld,
                        n.players), x + 8, y + 21, 12, angelsui::kTextDim);
}

} // namespace advopts
