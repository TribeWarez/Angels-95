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

// Weapon hand for the first-person view-model: 0 = right, 1 = left. Mirrors the
// view-model across the camera's YZ plane.
inline int g_weaponHand = 0;
