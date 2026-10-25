#pragma once

// Cross-translation-unit gameplay settings.
//
// Settings.hpp declares its state as `static` globals (HeadBob, Debug, ...), so
// every .cpp that includes it would get its own private copy. Only the TU that
// calls LoadClientSettings()/the settings menu (Main.cpp, which also textually
// includes Core.hpp) ever sees the value the player chose. Gameplay code that
// lives in a different TU - OzPawnSystem.cpp reads gore, ViewModel.cpp reads the
// weapon hand - cannot see those copies, so it reads these instead.
//
// `inline` variables give exactly one instance across the program (C++17), the
// same reason DebugFlags.hpp's g_debugEnabled exists. These are written by
// LoadClientSettings() (from the ini) and by the settings menu when a toggle
// changes, and read by the gameplay code below.
//
// This is its own header on purpose: including Settings.hpp from a second .cpp
// produces "multiple definition" link errors (it declares non-static globals
// such as AudioSlider and MuteToggle).

// Blood/gore on enemy hit and death. Default on; the "Gore" gameplay setting.
inline bool g_goreEnabled = true;

// Reduced-motion comfort flag, derived from the existing HeadBob toggle (the
// camera bob is the game's main motion source) and pushed once per frame in
// Core.hpp - same reason ParticlesEnabled is pushed there: Settings.hpp's
// globals are per-TU static copies. Consumed by oz::CombatFeedback so the
// low-health edge cue holds a steady alpha instead of pulsing.
inline bool g_reducedMotion = false;

// Weapon hand for the first-person view-model: 0 = right, 1 = left. Mirrors the
// view-model across the camera's YZ plane.
inline int g_weaponHand = 0;

// Mouse look sensitivity multiplier and Y-inversion. Exposed on the Controls
// page; PlayerMovement multiplies its per-controller MouseSensitivity by this.
inline float g_mouseSensitivity = 1.0f;
inline bool  g_invertY = false;

// In-game HUD / inventory scale. Pushed once per frame from Core.hpp out of the
// SAME persisted value the menu already uses ([AngelTech.HUD] ui_scale ->
// Settings.hpp's UiScale), so there is one UI scale control instead of two that
// can disagree. InventoryBehaviour reads this mirror because it is a separate
// TU and Settings.hpp's globals are per-TU static copies.
inline float g_hudScale = 1.0f;
