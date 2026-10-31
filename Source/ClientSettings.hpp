#pragma once

// Client settings persistence. Reads the REAL config file
// System/Angels95.ini at startup (before window creation so window size,
// VSync and MSAA take effect in InitWindow) and writes it back on exit.
//
// The file is grouped by CONCERN - one [AngelTech.*] section each - instead of
// the single flat [Settings] block it used to be:
//
//   [AngelTech.Window]    window size, borderless
//   [AngelTech.Render]    vsync / msaa / filter / pixel / jitter / fog / fps
//   [AngelTech.Audio]     master / music / sfx volume, mute
//   [AngelTech.Gameplay]  head bob, gore, weapon hand
//   [AngelTech.Input]     mouse sensitivity, invert-Y  (bindings: [Keybinds])
//   [AngelTech.HUD]       UI scale, crosshair
//   [AngelTech.Debug]     debug toggle
//   [Keybinds]            the action table (Input/Keybinds.hpp)
//   [MasterServers]       master URL list (Client/MasterList.hpp, hand-edited)
//
// The names are named constants because Load and Save must not drift: a
// mismatched pair reads defaults and then OVERWRITES the player's file with
// them, which looks like "my settings reset for no reason".
//
// A legacy flat [Settings] block is still read, per key, so an existing install
// migrates on its first save instead of losing every option.

#include "Settings.hpp"
#include "GameplayFlags.hpp"
#include "Input/Keybinds.hpp"
#include "IniConfig.hpp"
#include "ClientSettingsSchema.hpp"
#include <string>
#include <vector>

static const char* kClientIniPath = "System/Angels95.ini";

static inline int CfgClampI(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float CfgClampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Call BEFORE SetConfigFlags/InitWindow.
static inline void LoadClientSettings() {
    IniConfig cfg;
    if (!cfg.Load(kClientIniPath)) return; // missing/malformed -> defaults
    using namespace clientcfg;

    const char* W = kWindow;
    ConfigWindowWidth  = CfgClampI(Int(cfg, W, "window_width",  ConfigWindowWidth),  320, 7680);
    ConfigWindowHeight = CfgClampI(Int(cfg, W, "window_height", ConfigWindowHeight), 200, 4320);
    BorderlessWindow   = Bool(cfg, W, "borderless", BorderlessWindow);

    const char* R = kRender;
    VSYNCToggle        = Bool(cfg, R, "vsync",  VSYNCToggle);
    MXAAToggle         = Bool(cfg, R, "msaa",   MXAAToggle);
    ResolutionScale    = CfgClampF(Float(cfg, R, "resolution_scale",  ResolutionScale),  0.25f, 4.0f);
    TextureFilterMode  = CfgClampI(Int(cfg, R,  "texture_filter",    TextureFilterMode), 0, 5);
    PixelSize          = CfgClampF(Float(cfg, R, "pixel_size", PixelSize), 1.0f, 32.0f);
    PixelShader        = Bool(cfg, R, "pixel_shader",  PixelShader);
    JitterEnabled      = Bool(cfg, R, "jitter",      JitterEnabled);
    JitterIntensity    = CfgClampF(Float(cfg, R, "jitter_intensity", JitterIntensity), 0.0f, 10.0f);
    FogEnabled         = Bool(cfg, R, "fog",         FogEnabled);
    FogIntensity       = CfgClampF(Float(cfg, R, "fog_intensity", FogIntensity), 0.0f, 1.0f);
    int fr = CfgClampI(Int(cfg, R, "fog_tint_r", FogTint.r), 0, 255);
    int fg = CfgClampI(Int(cfg, R, "fog_tint_g", FogTint.g), 0, 255);
    int fb = CfgClampI(Int(cfg, R, "fog_tint_b", FogTint.b), 0, 255);
    FogTint = (Color){(unsigned char)fr, (unsigned char)fg, (unsigned char)fb, 255};

    // HDR bloom + grade. Clamped here so a hand-edited ini cannot push exposure
    // to a value that blows the whole frame to white.
    PostFXEnabled = Bool(cfg, R, "post_fx", PostFXEnabled);
    PeriodRenderProfile = Bool(cfg, R, "period_profile", PeriodRenderProfile);
    PostExposure  = CfgClampF(Float(cfg, R, "post_exposure", PostExposure), 0.25f, 3.0f);
    PostBloom     = CfgClampF(Float(cfg, R, "post_bloom", PostBloom), 0.0f, 3.0f);
    PostVignette  = CfgClampF(Float(cfg, R, "post_vignette", PostVignette), 0.0f, 0.8f);

    // Lighting + level-fog tuning (Advanced Options > Display > Rendering).
    // Uniform multipliers only, so the defaults keep the authored look.
    g_lightAmbientBoost  = CfgClampF(Float(cfg, R, "light_ambient_boost",  g_lightAmbientBoost),  0.0f, 3.0f);
    g_lightSpecularScale = CfgClampF(Float(cfg, R, "light_specular_scale", g_lightSpecularScale), 0.0f, 3.0f);
    g_lightSoftness      = CfgClampF(Float(cfg, R, "light_softness",      g_lightSoftness),      0.0f, 1.0f);
    g_dynamicLights      = Bool(cfg, R, "dynamic_lights", g_dynamicLights);
    g_fogOverride        = Bool(cfg, R, "fog_override",   g_fogOverride);
    g_fogDensityScale    = CfgClampF(Float(cfg, R, "fog_density_scale", g_fogDensityScale), 0.0f, 3.0f);
    g_fogStart           = CfgClampF(Float(cfg, R, "fog_start",   g_fogStart),   0.0f, 500.0f);
    g_fogEnd             = CfgClampF(Float(cfg, R, "fog_end",     g_fogEnd),    10.0f, 2000.0f);
    g_fogDensity         = CfgClampF(Float(cfg, R, "fog_density", g_fogDensity), 0.0f, 0.05f);
    g_fogColorR = (unsigned char)CfgClampI(Int(cfg, R, "fog_color_r", g_fogColorR), 0, 255);
    g_fogColorG = (unsigned char)CfgClampI(Int(cfg, R, "fog_color_g", g_fogColorG), 0, 255);
    g_fogColorB = (unsigned char)CfgClampI(Int(cfg, R, "fog_color_b", g_fogColorB), 0, 255);
    // `particles` is DELIBERATELY no longer read.
    //
    // It was persisted while a bug forced ParticlesEnabled=false on EVERY world
    // load (Core.hpp's LoadWorld reset), and SaveClientSettings then wrote that
    // artifact to the INI on exit. So on any install that has quit the game, the
    // stored value is the bug's, not the player's - and honouring it leaves every
    // OZONE emitter and every `particles` weather field invisible, in the shipped
    // maps too. A new key is the only unambiguous migration: absent it means
    // "pre-fix file" and defaults back to ON, and once this build has saved, the
    // value is genuinely user-controlled. Do not read the old key again.
    if (!cfg.Has(R, "particles_enabled")) ParticlesEnabled = true;
    ParticlesEnabled = Bool(cfg, R, "particles_enabled", ParticlesEnabled);
    FPSEnabled       = Bool(cfg, R, "fps",          FPSEnabled);

    const char* A = kAudio;
    MuteToggle  = Bool(cfg, A, "mute",         MuteToggle);
    AudioSlider = CfgClampF(Float(cfg, A, "audio_volume", AudioSlider), 0.0f, 100.0f);
    MusicVolume = CfgClampF(Float(cfg, A, "music_volume", MusicVolume), 0.0f, 100.0f);
    SfxVolume   = CfgClampF(Float(cfg, A, "sfx_volume",   SfxVolume),   0.0f, 100.0f);

    // Gameplay. Mirror the cross-TU flags gameplay code actually reads
    // (GameplayFlags.hpp); the statics above are this TU's persisted UI state.
    const char* G = kGameplay;
    HeadBob     = Bool(cfg, G, "head_bob", HeadBob);
    GoreEnabled = Bool(cfg, G, "gore", GoreEnabled);
    WeaponHand  = CfgClampI(Int(cfg, G, "weapon_hand", WeaponHand), 0, 1);
    g_goreEnabled = GoreEnabled;
    g_weaponHand  = WeaponHand;

    const char* I = kInput;
    g_mouseSensitivity = CfgClampF(Float(cfg, I, "mouse_sensitivity", g_mouseSensitivity), 0.1f, 5.0f);
    g_invertY          = Bool(cfg, I, "invert_y", g_invertY);
    Keybinds::Load(cfg);   // [Keybinds] - unchanged by the scoping pass

    const char* H = kHud;
    UiScale            = CfgClampF(Float(cfg, H, "ui_scale", UiScale), 0.75f, 2.0f);
    CrosshairStyleSel  = CfgClampI(Int(cfg, H, "crosshair_style", CrosshairStyleSel),
                                   0, CROSSHAIR_STYLE_COUNT - 1);
    CrosshairVisible   = Bool(cfg, H, "crosshair_visible", CrosshairVisible);
    CrosshairSize      = CfgClampI(Int(cfg, H, "crosshair_size", CrosshairSize), 2, 48);
    CrosshairGap       = CfgClampI(Int(cfg, H, "crosshair_gap",  CrosshairGap),  0, 48);
    CrosshairThickness = CfgClampF(Float(cfg, H, "crosshair_thickness", CrosshairThickness), 1.0f, 6.0f);
    CrosshairR = CfgClampI(Int(cfg, H, "crosshair_r", CrosshairR), 0, 255);
    CrosshairG = CfgClampI(Int(cfg, H, "crosshair_g", CrosshairG), 0, 255);
    CrosshairB = CfgClampI(Int(cfg, H, "crosshair_b", CrosshairB), 0, 255);
    CrosshairA = CfgClampI(Int(cfg, H, "crosshair_a", CrosshairA), 0, 255);

    // Network. These live as cross-TU inline globals (GameplayFlags.hpp) because
    // the client applies the ping cadence and the HUD draws the stats readout
    // from other translation units.
    const char* N = kNetwork;
    g_netPingInterval = CfgClampF(Float(cfg, N, "ping_interval", g_netPingInterval),
                                  1.0f, 30.0f);
    g_showNetStats    = Bool(cfg, N, "show_net_stats", g_showNetStats);

    Debug              = Bool(cfg, kDebug, "debug", Debug);
    PerfOverlay        = Bool(cfg, kDebug, "perf_overlay", PerfOverlay);
    PerfOverlayExpanded = Bool(cfg, kDebug, "perf_overlay_expanded", PerfOverlayExpanded);
    g_debugEnabled = Debug;   // mirror into the cross-TU flag (see DebugFlags.hpp)
}

// Write all settings back to System/Angels95.ini.
static inline void SaveClientSettings() {
    IniConfig cfg;
    // Load FIRST: this file also carries sections the client only READS
    // ([MasterServers], and whatever a later feature adds). Starting from an
    // empty IniConfig and calling Save() writes just this function's keys, so
    // every other section was truncated on each exit - which is why the master
    // URL list kept disappearing from a file that is documented as editable.
    cfg.Load(kClientIniPath);   // missing file is fine: we write a fresh one
    // The legacy block has just been migrated into the scoped sections below.
    // Leaving it behind would keep a stale second copy of every setting.
    cfg.data.erase(clientcfg::kLegacy);

    using namespace clientcfg;

    const char* W = kWindow;
    cfg.SetInt(W, "window_width",  ConfigWindowWidth);
    cfg.SetInt(W, "window_height", ConfigWindowHeight);
    cfg.SetBool(W, "borderless",   BorderlessWindow);

    const char* R = kRender;
    cfg.SetBool(R,  "vsync",  VSYNCToggle);
    cfg.SetBool(R,  "msaa",   MXAAToggle);
    cfg.SetFloat(R, "resolution_scale",  ResolutionScale);
    cfg.SetInt(R,   "texture_filter",    TextureFilterMode);
    cfg.SetFloat(R, "pixel_size",        PixelSize);
    cfg.SetBool(R,  "pixel_shader",  PixelShader);
    cfg.SetBool(R,  "jitter",             JitterEnabled);
    cfg.SetFloat(R, "jitter_intensity",  JitterIntensity);
    cfg.SetBool(R,  "fog",                FogEnabled);
    cfg.SetFloat(R, "fog_intensity",     FogIntensity);
    cfg.SetInt(R,   "fog_tint_r", FogTint.r);
    cfg.SetInt(R,   "fog_tint_g", FogTint.g);
    cfg.SetInt(R,   "fog_tint_b", FogTint.b);
    cfg.SetBool(R,  "post_fx",       PostFXEnabled);
    cfg.SetBool(R,  "period_profile", PeriodRenderProfile);
    cfg.SetFloat(R, "post_exposure", PostExposure);
    cfg.SetFloat(R, "post_bloom",    PostBloom);
    cfg.SetFloat(R, "post_vignette", PostVignette);
    cfg.SetFloat(R, "light_ambient_boost",  g_lightAmbientBoost);
    cfg.SetFloat(R, "light_specular_scale", g_lightSpecularScale);
    cfg.SetFloat(R, "light_softness",       g_lightSoftness);
    cfg.SetBool(R,  "dynamic_lights",       g_dynamicLights);
    cfg.SetBool(R,  "fog_override",         g_fogOverride);
    cfg.SetFloat(R, "fog_density_scale",    g_fogDensityScale);
    cfg.SetFloat(R, "fog_start",            g_fogStart);
    cfg.SetFloat(R, "fog_end",              g_fogEnd);
    cfg.SetFloat(R, "fog_density",          g_fogDensity);
    cfg.SetInt(R,   "fog_color_r",          g_fogColorR);
    cfg.SetInt(R,   "fog_color_g",          g_fogColorG);
    cfg.SetInt(R,   "fog_color_b",          g_fogColorB);
    cfg.SetBool(R,  "particles_enabled", ParticlesEnabled);
    cfg.SetBool(R,  "fps",           FPSEnabled);

    const char* A = kAudio;
    cfg.SetBool(A,  "mute",         MuteToggle);
    cfg.SetFloat(A, "audio_volume", AudioSlider);
    cfg.SetFloat(A, "music_volume", MusicVolume);
    cfg.SetFloat(A, "sfx_volume",   SfxVolume);

    const char* G = kGameplay;
    cfg.SetBool(G, "head_bob",    HeadBob);
    cfg.SetBool(G, "gore",        GoreEnabled);
    cfg.SetInt(G,  "weapon_hand", WeaponHand);

    const char* I = kInput;
    cfg.SetFloat(I, "mouse_sensitivity", g_mouseSensitivity);
    cfg.SetBool(I,  "invert_y",          g_invertY);
    Keybinds::Save(cfg);   // [Keybinds]

    const char* H = kHud;
    cfg.SetFloat(H, "ui_scale",            UiScale);
    cfg.SetInt(H,   "crosshair_style",     CrosshairStyleSel);
    cfg.SetBool(H,  "crosshair_visible",   CrosshairVisible);
    cfg.SetInt(H,   "crosshair_size",      CrosshairSize);
    cfg.SetInt(H,   "crosshair_gap",       CrosshairGap);
    cfg.SetFloat(H, "crosshair_thickness", CrosshairThickness);
    cfg.SetInt(H,   "crosshair_r", CrosshairR);
    cfg.SetInt(H,   "crosshair_g", CrosshairG);
    cfg.SetInt(H,   "crosshair_b", CrosshairB);
    cfg.SetInt(H,   "crosshair_a", CrosshairA);

    const char* N = kNetwork;
    cfg.SetFloat(N, "ping_interval",  g_netPingInterval);
    cfg.SetBool(N,  "show_net_stats", g_showNetStats);

    cfg.SetBool(kDebug, "debug", Debug);
    cfg.SetBool(kDebug, "perf_overlay", PerfOverlay);
    cfg.SetBool(kDebug, "perf_overlay_expanded", PerfOverlayExpanded);

    // Fixed section order = a stable, diffable, hand-editable file. SaveOrdered
    // appends anything not named here, so unknown sections survive the round
    // trip instead of being deleted.
    cfg.SaveOrdered(kClientIniPath, clientcfg::kSectionOrder,
                    "; Angels95 client settings - written by the game on exit; safe to hand-edit.\n"
                    "; Grouped by concern; a legacy flat [Settings] block is migrated here on save.\n"
                    "\n");
}
