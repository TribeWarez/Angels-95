#pragma once

// Client settings persistence. Reads the REAL config file
// System/Angels95.ini at startup (before window creation so window size,
// VSync and MSAA take effect in InitWindow) and writes it back on exit.
// The ini is a plain [Settings] key=value file written by SaveClientSettings.

#include "Settings.hpp"
#include "IniConfig.hpp"

static const char* kClientIniPath = "System/Angels95.ini";

static inline int CfgClampI(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float CfgClampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Call BEFORE SetConfigFlags/InitWindow.
static inline void LoadClientSettings() {
    IniConfig cfg;
    if (!cfg.Load(kClientIniPath)) return; // missing/malformed -> defaults
    const char* S = "Settings";
    ConfigWindowWidth  = CfgClampI(cfg.GetInt(S, "window_width",  ConfigWindowWidth),  320, 7680);
    ConfigWindowHeight = CfgClampI(cfg.GetInt(S, "window_height", ConfigWindowHeight), 200, 4320);
    VSYNCToggle        = cfg.GetBool(S, "vsync",  VSYNCToggle);
    MXAAToggle         = cfg.GetBool(S, "msaa",   MXAAToggle);
    ResolutionScale    = CfgClampF(cfg.GetFloat(S, "resolution_scale",  ResolutionScale),  0.25f, 4.0f);
    TextureFilterMode  = CfgClampI(cfg.GetInt(S,  "texture_filter",    TextureFilterMode), 0, 5);
    PixelSize          = CfgClampF(cfg.GetFloat(S, "pixel_size", PixelSize), 1.0f, 32.0f);
    JitterEnabled      = cfg.GetBool(S, "jitter",      JitterEnabled);
    JitterIntensity    = CfgClampF(cfg.GetFloat(S, "jitter_intensity", JitterIntensity), 0.0f, 10.0f);
    FogEnabled         = cfg.GetBool(S, "fog",         FogEnabled);
    FogIntensity       = CfgClampF(cfg.GetFloat(S, "fog_intensity", FogIntensity), 0.0f, 1.0f);
    int fr = CfgClampI(cfg.GetInt(S, "fog_tint_r", FogTint.r), 0, 255);
    int fg = CfgClampI(cfg.GetInt(S, "fog_tint_g", FogTint.g), 0, 255);
    int fb = CfgClampI(cfg.GetInt(S, "fog_tint_b", FogTint.b), 0, 255);
    FogTint = (Color){(unsigned char)fr, (unsigned char)fg, (unsigned char)fb, 255};
    PixelShader      = cfg.GetBool(S, "pixel_shader",  PixelShader);
    ParticlesEnabled = cfg.GetBool(S, "particles",    ParticlesEnabled);
    FPSEnabled       = cfg.GetBool(S, "fps",          FPSEnabled);
    HeadBob          = cfg.GetBool(S, "head_bob",     HeadBob);
    Debug            = cfg.GetBool(S, "debug",        Debug);
    g_debugEnabled   = Debug;   // mirror into the cross-TU flag (see Settings.hpp)
    MuteToggle       = cfg.GetBool(S, "mute",         MuteToggle);
    AudioSlider      = CfgClampF(cfg.GetFloat(S, "audio_volume", AudioSlider), 0.0f, 100.0f);

    CrosshairStyleSel = CfgClampI(cfg.GetInt(S, "crosshair_style", CrosshairStyleSel),
                                  0, CROSSHAIR_STYLE_COUNT - 1);
    CrosshairVisible  = cfg.GetBool(S, "crosshair_visible", CrosshairVisible);
    CrosshairSize     = CfgClampI(cfg.GetInt(S, "crosshair_size", CrosshairSize), 2, 48);
    CrosshairGap      = CfgClampI(cfg.GetInt(S, "crosshair_gap",  CrosshairGap),  0, 48);
    CrosshairThickness = CfgClampF(cfg.GetFloat(S, "crosshair_thickness", CrosshairThickness), 1.0f, 6.0f);
    CrosshairR = CfgClampI(cfg.GetInt(S, "crosshair_r", CrosshairR), 0, 255);
    CrosshairG = CfgClampI(cfg.GetInt(S, "crosshair_g", CrosshairG), 0, 255);
    CrosshairB = CfgClampI(cfg.GetInt(S, "crosshair_b", CrosshairB), 0, 255);
    CrosshairA = CfgClampI(cfg.GetInt(S, "crosshair_a", CrosshairA), 0, 255);
}

// Write all settings back to System/Angels95.ini.
static inline void SaveClientSettings() {
    IniConfig cfg;
    const char* S = "Settings";
    cfg.SetInt(S, "window_width",  ConfigWindowWidth);
    cfg.SetInt(S, "window_height", ConfigWindowHeight);
    cfg.SetBool(S, "vsync",  VSYNCToggle);
    cfg.SetBool(S, "msaa",   MXAAToggle);
    cfg.SetFloat(S, "resolution_scale",  ResolutionScale);
    cfg.SetInt(S,   "texture_filter",    TextureFilterMode);
    cfg.SetFloat(S, "pixel_size",        PixelSize);
    cfg.SetBool(S, "jitter",             JitterEnabled);
    cfg.SetFloat(S, "jitter_intensity",  JitterIntensity);
    cfg.SetBool(S, "fog",                FogEnabled);
    cfg.SetFloat(S, "fog_intensity",     FogIntensity);
    cfg.SetInt(S, "fog_tint_r", FogTint.r);
    cfg.SetInt(S, "fog_tint_g", FogTint.g);
    cfg.SetInt(S, "fog_tint_b", FogTint.b);
    cfg.SetBool(S, "pixel_shader",  PixelShader);
    cfg.SetBool(S, "particles",     ParticlesEnabled);
    cfg.SetBool(S, "fps",           FPSEnabled);
    cfg.SetBool(S, "head_bob",      HeadBob);
    cfg.SetBool(S, "debug",         Debug);
    cfg.SetBool(S, "mute",          MuteToggle);
    cfg.SetFloat(S, "audio_volume", AudioSlider);
    cfg.SetInt(S,   "crosshair_style",     CrosshairStyleSel);
    cfg.SetBool(S,  "crosshair_visible",   CrosshairVisible);
    cfg.SetInt(S,   "crosshair_size",      CrosshairSize);
    cfg.SetInt(S,   "crosshair_gap",       CrosshairGap);
    cfg.SetFloat(S, "crosshair_thickness", CrosshairThickness);
    cfg.SetInt(S,   "crosshair_r", CrosshairR);
    cfg.SetInt(S,   "crosshair_g", CrosshairG);
    cfg.SetInt(S,   "crosshair_b", CrosshairB);
    cfg.SetInt(S,   "crosshair_a", CrosshairA);
    cfg.Save(kClientIniPath);
}