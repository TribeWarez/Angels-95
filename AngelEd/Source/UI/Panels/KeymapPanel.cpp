// ============================================================================
// KeymapPanel.cpp
//
// FRAGMENT - not a standalone translation unit. Included by ../UiShell.cpp,
// which is the single TU for the whole UI layer.
//
// Help -> Keyboard Shortcuts. The shortcut table and its persistence live in
// EditorKeys.hpp; this is only the editor for it. Capture is synchronous because
// the panel owns the Win32 focus, so raylib's IsKeyPressed cannot see the
// keystroke; GetAsyncKeyState is the only source that works while a native
// window has focus.
// ============================================================================

#include "../../EditorKeys.hpp"

static const int ID_KM_LIST   = 960;
static const int ID_KM_REBIND = 961;
static const int ID_KM_RESET  = 962;
static const int ID_KM_CLOSE  = 963;
static const int ID_KM_STATUS = 964;

static HWND g_kmList = nullptr;
static HWND g_kmStatus = nullptr;
static HWND g_kmRebind = nullptr;
static HWND g_kmReset = nullptr;
static HWND g_kmClose = nullptr;

static void KmPopulate() {
    if (!g_kmList) return;
    const int sel = (int)SendMessageW(g_kmList, LB_GETCURSEL, 0, 0);
    SendMessageW(g_kmList, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < EdKeys::kCount; i++) {
        std::string line = EdKeys::kDefs[i].label;
        line += "    [";
        line += EdKeys::ShortcutText(EdKeys::kDefs[i].action);
        line += "]";
        std::wstring w(line.begin(), line.end());
        SendMessageW(g_kmList, LB_ADDSTRING, 0, (LPARAM)w.c_str());
    }
    if (sel >= 0 && sel < EdKeys::kCount)
        SendMessageW(g_kmList, LB_SETCURSEL, sel, 0);
    else if (EdKeys::kCount > 0)
        SendMessageW(g_kmList, LB_SETCURSEL, 0, 0);
}

static void KmLayout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int m = 8, btnH = 26, statusH = 20;
    if (g_kmList)
        MoveWindow(g_kmList, m, 32, rc.right - 2 * m,
                   rc.bottom - 32 - statusH - btnH - 3 * m, TRUE);
    if (g_kmStatus)
        MoveWindow(g_kmStatus, m, rc.bottom - m - btnH - statusH, rc.right - 2 * m,
                   statusH, TRUE);
    const int by = rc.bottom - m - btnH;
    int bx = m;
    if (g_kmRebind) { MoveWindow(g_kmRebind, bx, by, 120, btnH, TRUE); bx += 126; }
    if (g_kmReset)  { MoveWindow(g_kmReset, bx, by, 120, btnH, TRUE); }
    if (g_kmClose)  { MoveWindow(g_kmClose, rc.right - m - 90, by, 90, btnH, TRUE); }
}

static void KmStatus(const char* text) {
    if (!g_kmStatus) return;
    std::wstring w(text, text + strlen(text));
    SetWindowTextW(g_kmStatus, w.c_str());
}

// Returns a Virtual-Key code, VK_ESCAPE for cancel, or 0 on timeout.
static int KmCaptureKey() {
    bool prev[256] = {};
    KmStatus("Press a key (Escape cancels)...");
    for (int waited = 0; waited < 500; waited++) {
        for (int vk = 8; vk < 256; vk++) {
            if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU ||
                vk == VK_LWIN || vk == VK_RWIN)
                continue;
            const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
            if (down && !prev[vk]) return vk;
            prev[vk] = down;
        }
        Sleep(10);
        MSG peek;
        while (PeekMessage(&peek, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&peek);
            DispatchMessage(&peek);
        }
        if (!IsWindow(g_kmList)) return VK_ESCAPE;
    }
    return 0;
}

static LRESULT CALLBACK KeymapProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        CreateLabel(hwnd, L"Click Rebind, then press the new key. Modifiers stay as shown.",
                    8, 8, 460, 18, 0);
        g_kmList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY,
            8, 32, 460, 280, hwnd, (HMENU)(INT_PTR)ID_KM_LIST, g_hInst, nullptr);
        g_kmRebind = CreateButton(hwnd, L"Rebind...", 8, 330, 120, 26, ID_KM_REBIND);
        g_kmReset  = CreateButton(hwnd, L"Reset Defaults", 134, 330, 120, 26, ID_KM_RESET);
        g_kmClose  = CreateButton(hwnd, L"Close", 380, 330, 90, 26, ID_KM_CLOSE);
        g_kmStatus = CreateLabel(hwnd, L"", 8, 306, 460, 20, ID_KM_STATUS);
        KmPopulate();
        KmLayout(hwnd);
        break;
    }
    case WM_SIZE:
        KmLayout(hwnd);
        break;
    case WM_GETMINMAXINFO:
        EnforceMinTrackSize(l, 500, 380);
        break;
    case WM_COMMAND: {
        const int id = LOWORD(w);
        if (id == ID_KM_CLOSE) {
            ShowKeymapPanel(false);
        } else if (id == ID_KM_REBIND) {
            const int sel = (int)SendMessageW(g_kmList, LB_GETCURSEL, 0, 0);
            if (sel < 0 || sel >= EdKeys::kCount) break;
            const int vk = KmCaptureKey();
            if (vk == VK_ESCAPE) {
                KmStatus("Cancelled.");
            } else if (vk == 0) {
                KmStatus("No key pressed (timed out).");
            } else {
                const int key = EdKeys::FromVirtualKey(vk);
                if (key < 0) {
                    KmStatus("That key is not supported.");
                } else {
                    const EdAction a = EdKeys::kDefs[sel].action;
                    const int swapped = EdKeys::Assign(a, key);
                    EdKeys::Save();
                    KmPopulate();
                    std::string text = "Set " + std::string(EdKeys::kDefs[sel].label) +
                                       " to " + EdKeys::ShortcutText(a);
                    if (swapped >= 0)
                        text += " (swapped with " + std::string(EdKeys::kDefs[swapped].label) + ")";
                    KmStatus(text.c_str());
                }
            }
        } else if (id == ID_KM_RESET) {
            EdKeys::ResetDefaults();
            EdKeys::Save();
            KmPopulate();
            KmStatus("Defaults restored.");
        }
        break;
    }
    case WM_CLOSE:
        ShowKeymapPanel(false);
        break;
    case WM_DESTROY:
        g_kmList = nullptr;
        g_kmStatus = nullptr;
        g_kmRebind = nullptr;
        g_kmReset = nullptr;
        g_kmClose = nullptr;
        g_editorPanels.hKeymap = nullptr;
        break;
    default:
        return DefWindowProc(hwnd, msg, w, l);
    }
    return 0;
}

void ShowKeymapPanel(bool show) {
    g_editorPanels.showKeymap = show;
    if (!g_editorPanels.hKeymap) return;
    ShowWindow((HWND)g_editorPanels.hKeymap, show ? SW_SHOW : SW_HIDE);
    if (show) {
        KmPopulate();
        KmStatus("Shortcut changes apply immediately and are saved.");
    }
}
