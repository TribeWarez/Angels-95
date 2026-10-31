#include "UiHandler.hpp"

#ifdef _WIN32
#include "../WindowsCompat.hpp"
#include "../Version.hpp"
#include "raylib.h"

namespace oz::ui {
namespace {

constexpr UINT kIdmFileLoad = 1001;
constexpr UINT kIdmFileSave = 1002;
constexpr UINT kIdmFileQuit = 1003;
constexpr UINT kIdmSettings = 2001;
constexpr UINT kIdmAdvancedSettings = 2002;
constexpr UINT kIdmAbout    = 3001;
constexpr UINT kIdmPerfOverlay = 4001;
constexpr UINT kIdmPerfDump    = 4002;

WNDPROC g_originalWndProc = nullptr;
const MenuBarCallbacks* g_callbacks = nullptr;

LRESULT CALLBACK ClientWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_COMMAND && g_callbacks) {
        switch (LOWORD(wParam)) {
            case kIdmFileLoad:
                if (g_callbacks->onLoadWorld) g_callbacks->onLoadWorld();
                return 0;
            case kIdmFileSave:
                if (g_callbacks->onSaveGame) g_callbacks->onSaveGame();
                return 0;
            case kIdmFileQuit:
                if (g_callbacks->onQuit) g_callbacks->onQuit();
                return 0;
            case kIdmSettings:
                if (g_callbacks->onToggleSettings) g_callbacks->onToggleSettings();
                return 0;
            case kIdmAdvancedSettings:
                if (g_callbacks->onToggleAdvancedSettings) g_callbacks->onToggleAdvancedSettings();
                return 0;
            case kIdmAbout:
                if (g_callbacks->onAbout) g_callbacks->onAbout();
                return 0;
            case kIdmPerfOverlay:
                if (g_callbacks->onTogglePerfOverlay) g_callbacks->onTogglePerfOverlay();
                return 0;
            case kIdmPerfDump:
                if (g_callbacks->onDumpPerf) g_callbacks->onDumpPerf();
                return 0;
        }
    }
    return CallWindowProc(g_originalWndProc, hWnd, msg, wParam, lParam);
}

} // namespace

void CreateNativeMenuBar(const MenuBarCallbacks& callbacks) {
    g_callbacks = &callbacks;

    HWND hWnd = (HWND)GetWindowHandle();
    if (!hWnd) return;

    // Subclass the raylib window so we intercept WM_COMMAND from menus.
    g_originalWndProc = (WNDPROC)SetWindowLongPtr(hWnd, GWLP_WNDPROC, (LONG_PTR)ClientWndProc);

    HMENU hMenuBar = CreateMenu();
    HMENU hFileMenu = CreatePopupMenu();
    AppendMenuA(hFileMenu, MF_STRING, kIdmFileLoad, "&Restart World");
    AppendMenuA(hFileMenu, MF_STRING, kIdmFileSave, "&Save Game");
    AppendMenuA(hFileMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hFileMenu, MF_STRING, kIdmFileQuit, "&Quit");
    AppendMenuA(hMenuBar, MF_POPUP, (UINT_PTR)hFileMenu, "&File");
    HMENU hSettingsMenu = CreatePopupMenu();
    AppendMenuA(hSettingsMenu, MF_STRING, kIdmSettings, "&Settings...");
    // The Advanced Options tree lives next to the plain Settings panel rather
    // than as its own top-level menu: it is the same surface (player settings),
    // just the full tree. This item used to be appended to the MENU BAR with a
    // popup handle that was never created, which did not compile.
    AppendMenuA(hSettingsMenu, MF_STRING, kIdmAdvancedSettings, "&Advanced Options...");
    AppendMenuA(hMenuBar, MF_POPUP, (UINT_PTR)hSettingsMenu, "&Settings");
    // Perf menu. The client has no other discoverable entry point for the
    // FrameProfiler (F9 and `/perf` are the other two, and both drive the same
    // flag), so it lives at the top level rather than under Settings: it is a
    // diagnostic surface, not a player preference.
    HMENU hPerfMenu = CreatePopupMenu();
    AppendMenuA(hPerfMenu, MF_STRING, kIdmPerfOverlay, "&Toggle Perf Overlay\tF9");
    AppendMenuA(hPerfMenu, MF_STRING, kIdmPerfDump, "&Dump Perf Report");
    AppendMenuA(hMenuBar, MF_POPUP, (UINT_PTR)hPerfMenu, "&Perf");
    HMENU hAboutMenu = CreatePopupMenu();
    AppendMenuA(hAboutMenu, MF_STRING, kIdmAbout, "&About Angels95...");
    AppendMenuA(hMenuBar, MF_POPUP, (UINT_PTR)hAboutMenu, "&?");
    SetMenu(hWnd, hMenuBar);
}

void ShowAboutDialog() {
    MessageBoxA(NULL,
        TextFormat("Angels95 %s\nAngelTech Engine (OzWorld)\nraylib 5.5  //  C++20\nTribeWarez 2026",
                   oz::kBuildVersion),
        "About Angels95", MB_OK | MB_ICONINFORMATION);
}

} // namespace oz::ui

#else

namespace oz::ui {
void CreateNativeMenuBar(const MenuBarCallbacks&) {}
void ShowAboutDialog() {}
} // namespace oz::ui

#endif
