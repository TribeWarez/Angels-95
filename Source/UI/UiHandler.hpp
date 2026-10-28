#pragma once

#include <functional>

namespace oz::ui {

// Callbacks invoked by the native menu bar. Main.cpp supplies these so the
// handler stays decoupled from engine globals (scene switching, save, quit).
struct MenuBarCallbacks {
    std::function<void()> onLoadWorld;
    std::function<void()> onSaveGame;
    std::function<void()> onQuit;
    std::function<void()> onToggleSettings;
    std::function<void()> onToggleAdvancedSettings;
    std::function<void()> onAbout;
};

// Installs the native (Win32) menu bar on the active raylib window and
// subclasses its window proc to route WM_COMMAND to `callbacks`.
// No-op on non-Windows platforms.
void CreateNativeMenuBar(const MenuBarCallbacks& callbacks);

// Shows the platform About dialog. No-op on non-Windows platforms.
void ShowAboutDialog();

} // namespace oz::ui
