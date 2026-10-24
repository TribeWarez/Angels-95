// ============================================================================
// UiShell.cpp - the single translation unit for AngelEd's UI layer.
//
// WHY A UNITY BUILD
//
// Splitting a 6,600-line .cpp into 16 files normally means 16 compile rules in
// AngelEd/Makefile AND 16 `g++ -c` lines plus 16 link entries in
// .github/workflows/ci.yml, because CI builds the editor by hand rather than
// through the Makefile. That is the "added in one place, CI breaks silently" trap
// from AGENTS.md multiplied by sixteen, and the two lists can drift without any
// signal.
//
// So the panels are FRAGMENTS: real, separately-editable files that are #included
// here. One object, one Makefile rule, one CI line.
//
// The cost is honest and worth stating: a fragment can rely on an include it does
// not state, because they all share this file scope. That is why every fragment
// starts with a banner saying it is not a standalone TU. Panel code is UI-facing
// and its dependencies are the panels, so in practice the coupling is along
// lines the layer rule already accepts: UI -> Subsystems/Resources, plus the one
// deliberate back-edge, Core/EditorEventBus.hpp, for the shared ed:: type vocabulary.
//
// FRAGMENTS ARE NOT INCLUDED IN ANY ORDER-SENSITIVE WAY beyond needing the
// preamble: every declaration they share lives in UiCommon.hpp, which is included
// first.
// ============================================================================

#include "UiCommon.hpp"

// ---------------------------------------------------------------------------
// Panels. Each is one window: its control IDs, its window procedure, and its
// Show*/Refresh* entry points.
// ---------------------------------------------------------------------------
#include "Panels/SoundPanel.cpp"
#include "Panels/TexturePanel.cpp"
#include "Panels/LevelState.cpp"
#include "Panels/PickupPanel.cpp"
#include "Panels/NodePanel.cpp"
#include "Panels/HeightmapPanel.cpp"
#include "Panels/ModelPanel.cpp"
#include "Panels/PawnPanel.cpp"
#include "Panels/ScriptPanel.cpp"
#include "Panels/WorldGraphPanel.cpp"
#include "Panels/LevelListPanel.cpp"
#include "Panels/PropsPanel.cpp"
#include "Panels/StatsSidebar.cpp"
#include "Panels/AnimPanel.cpp"
#include "Panels/MaterialPanel.cpp"
#include "Panels/SurfacePropsPanel.cpp"
#include "Panels/KeymapPanel.cpp"

// ============================================================================
// CreateAllEditorWindows / DestroyAllEditorWindows
//
// These stay HERE rather than in a fragment because they name every panel class,
// which makes this the one place that shows the layer's full surface. If a panel is
// added without being registered here it will not appear, and this function is
// where you look.
// ============================================================================

static const char* const kEditorIniPath = "System/AngelEd.ini";

struct PanelLayoutBinding {
    const char* key;
    void** handle;
    EditorPanelState::WinPos* pos;
};

static PanelLayoutBinding kLayoutBindings[] = {
    {"sound",    &g_editorPanels.hSoundMgr,       &g_editorPanels.soundMgrPos},
    {"texture",  &g_editorPanels.hTextureMgr,     &g_editorPanels.textureMgrPos},
    {"pawn",     &g_editorPanels.hPawnMgr,        &g_editorPanels.pawnMgrPos},
    {"script",   &g_editorPanels.hScriptMgr,      &g_editorPanels.scriptMgrPos},
    {"model",    &g_editorPanels.hModelBrowser,   &g_editorPanels.modelBrwPos},
    {"pickup",   &g_editorPanels.hPickupPanel,    &g_editorPanels.pickPanelPos},
    {"node",     &g_editorPanels.hNodePanel,      &g_editorPanels.nodePanelPos},
    {"heightmap",&g_editorPanels.hHeightmapEditor,&g_editorPanels.heightmapEditorPos},
    {"worldgraph",&g_editorPanels.hWorldGraph,    &g_editorPanels.worldGraphPos},
    {"props",    &g_editorPanels.hPropsPanel,     &g_editorPanels.propsPanelPos},
    {"level",    &g_editorPanels.hLevelList,      &g_editorPanels.levelListPos},
    {"anim",     &g_editorPanels.hAnimPanel,      &g_editorPanels.animPanelPos},
    {"material", &g_editorPanels.hMaterialPanel,  &g_editorPanels.materialPanelPos},
    {"surface",  &g_editorPanels.hSurfaceProps,   &g_editorPanels.surfacePropsPos},
    {"keymap",   &g_editorPanels.hKeymap,         &g_editorPanels.keymapPos},
};
static const int kLayoutBindingCount =
    (int)(sizeof(kLayoutBindings) / sizeof(kLayoutBindings[0]));

// Defaults captured before the INI is applied, so View -> Reset Layout can
// restore the built-in positions without a second hand-maintained table.
static EditorPanelState::WinPos kLayoutDefaults[kLayoutBindingCount];
static bool kLayoutDefaultsCaptured = false;

static void ClampPanelToWorkArea(EditorPanelState::WinPos& p) {
    RECT work = {0, 0, 1920, 1080};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    if (p.w < 200) p.w = 200;
    if (p.h < 140) p.h = 140;
    if (p.x < work.left - 40) p.x = work.left;
    if (p.y < work.top) p.y = work.top;
    if (p.x + 80 > work.right) p.x = work.right - 80;
    if (p.y + 40 > work.bottom) p.y = work.bottom - 40;
}

static void LoadPanelLayouts() {
    if (!kLayoutDefaultsCaptured) {
        for (int i = 0; i < kLayoutBindingCount; i++)
            kLayoutDefaults[i] = *kLayoutBindings[i].pos;
        kLayoutDefaultsCaptured = true;
    }

    IniConfig cfg;
    if (!cfg.Load(kEditorIniPath)) return;

    for (int i = 0; i < kLayoutBindingCount; i++) {
        auto& b = kLayoutBindings[i];
        const std::string base = b.key;
        EditorPanelState::WinPos p = *b.pos;
        p.x = cfg.GetInt("Layout", (base + "_x").c_str(), p.x);
        p.y = cfg.GetInt("Layout", (base + "_y").c_str(), p.y);
        p.w = cfg.GetInt("Layout", (base + "_w").c_str(), p.w);
        p.h = cfg.GetInt("Layout", (base + "_h").c_str(), p.h);
        ClampPanelToWorkArea(p);
        *b.pos = p;
    }
}

static void SavePanelLayouts() {
    IniConfig cfg;
    cfg.Load(kEditorIniPath);   // keep every unrelated section

    for (int i = 0; i < kLayoutBindingCount; i++) {
        auto& b = kLayoutBindings[i];
        HWND hw = b.handle ? (HWND)*b.handle : nullptr;
        if (!hw) continue;
        RECT r;
        GetWindowRect(hw, &r);
        // A maximized panel's "restore" rect is what should be persisted.
        WINDOWPLACEMENT wp = {sizeof(WINDOWPLACEMENT)};
        bool maxed = false;
        if (GetWindowPlacement(hw, &wp)) {
            maxed = (wp.showCmd == SW_SHOWMAXIMIZED);
            if (maxed) r = wp.rcNormalPosition;
        }
        const std::string base = b.key;
        cfg.SetInt("Layout", (base + "_x").c_str(), r.left);
        cfg.SetInt("Layout", (base + "_y").c_str(), r.top);
        cfg.SetInt("Layout", (base + "_w").c_str(), r.right - r.left);
        cfg.SetInt("Layout", (base + "_h").c_str(), r.bottom - r.top);
        cfg.SetBool("Layout", (base + "_max").c_str(), maxed);
    }

    if (g_hRaylibWnd) {
        RECT r;
        GetWindowRect(g_hRaylibWnd, &r);
        WINDOWPLACEMENT wp = {sizeof(WINDOWPLACEMENT)};
        bool maxed = false;
        if (GetWindowPlacement(g_hRaylibWnd, &wp)) {
            maxed = (wp.showCmd == SW_SHOWMAXIMIZED);
            if (maxed) r = wp.rcNormalPosition;
        }
        cfg.SetInt("Layout", "main_x", r.left);
        cfg.SetInt("Layout", "main_y", r.top);
        cfg.SetInt("Layout", "main_w", r.right - r.left);
        cfg.SetInt("Layout", "main_h", r.bottom - r.top);
        cfg.SetBool("Layout", "main_max", maxed);
    }
    cfg.Save(kEditorIniPath);
}

void ResetPanelLayouts() {
    if (!kLayoutDefaultsCaptured) return;
    for (int i = 0; i < kLayoutBindingCount; i++) {
        *kLayoutBindings[i].pos = kLayoutDefaults[i];
        HWND hw = kLayoutBindings[i].handle ? (HWND)*kLayoutBindings[i].handle : nullptr;
        if (!hw) continue;
        const auto& p = *kLayoutBindings[i].pos;
        ShowWindow(hw, SW_RESTORE);
        SetWindowPos(hw, nullptr, p.x, p.y, p.w, p.h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (g_hRaylibWnd) ShowWindow(g_hRaylibWnd, SW_MAXIMIZE);
    SavePanelLayouts();
}

void CreateAllEditorWindows(void* hInst, void* hRaylibWnd) {
    g_hInst = (HINSTANCE)hInst;
    g_hRaylibWnd = (HWND)hRaylibWnd;

    // Panel geometry from the previous session. Loaded before CreateWindowEx so a
    // panel never flashes at its default rect first.
    LoadPanelLayouts();

    // Initialize common controls for TreeView, etc.
    INITCOMMONCONTROLSEX icex = { sizeof(INITCOMMONCONTROLSEX), ICC_TREEVIEW_CLASSES };
    InitCommonControlsEx(&icex);

    // Initialize ListView common controls
    INITCOMMONCONTROLSEX icexLV = { sizeof(INITCOMMONCONTROLSEX), ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icexLV);

    RegisterPanelClass(CLASS_SOUNDMGR, SoundMgrProc, (HINSTANCE)hInst);
    RegisterPanelClass(CLASS_TEXTUREMGR, TextureMgrProc, (HINSTANCE)hInst);

    // Register texture grid custom control
    {
        WNDCLASSEX wc = {};
        wc.cbSize = sizeof(WNDCLASSEX);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = TextureGridProc;
        wc.hInstance = (HINSTANCE)hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = CLASS_TEXTURE_GRID;
        wc.cbWndExtra = sizeof(void*);
        RegisterClassEx(&wc);
    }

    // Material graph canvas (GDI node/edge drawing).
    {
        WNDCLASSEX wc = {};
        wc.cbSize = sizeof(WNDCLASSEX);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = MatCanvasProc;
        wc.hInstance = (HINSTANCE)hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = CLASS_MATERIALCANVAS;
        RegisterClassEx(&wc);
    }

    RegisterPanelClass(CLASS_PAWNNMGR, PawnMgrProc, (HINSTANCE)hInst);
    RegisterPanelClass(CLASS_SCRIPTMGR, ScriptMgrProc, (HINSTANCE)hInst);
    RegisterPanelClass(CLASS_MODELBRW, ModelBrwProc, (HINSTANCE)hInst);
    RegisterPanelClass(CLASS_PICKUPPANEL, PickupPanelProc, (HINSTANCE)hInst);
    RegisterPanelClass(CLASS_NODEPANEL, NodePanelProc, (HINSTANCE)hInst);
    RegisterPanelClass(CLASS_HMEDITOR, HmEditorProc, (HINSTANCE)hInst);
    RegisterPanelClass(CLASS_WORLDGRAPH, WorldGraphProc, (HINSTANCE)hInst);
    RegisterPanelClass(CLASS_PROPSPANEL, PropsPanelProc, (HINSTANCE)hInst);
    RegisterPanelClass(CLASS_ANIMPANEL, AnimPanelProc, (HINSTANCE)hInst);
    RegisterPanelClass(CLASS_MATERIALPANEL, MaterialPanelProc, (HINSTANCE)hInst);
    RegisterPanelClass(CLASS_LEVELLIST, LevelListProc, (HINSTANCE)hInst);
    RegisterPanelClass(CLASS_KEYMAP, KeymapProc, (HINSTANCE)hInst);
    RegisterPanelClass(CLASS_SURFACEPROPS, SurfacePropsProc, (HINSTANCE)hInst);

    // Stats sidebar uses dark background (override default COLOR_BTNFACE)
    {
        WNDCLASSEX wc = {};
        wc.cbSize = sizeof(WNDCLASSEX);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = StatsSidebarProc;
        wc.hInstance = (HINSTANCE)hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(RGB(25, 25, 30));
        wc.lpszClassName = CLASS_STATSSIDEBAR;
        RegisterClassEx(&wc);
    }

    auto create = [&](const wchar_t* cls, const wchar_t* title,
                      EditorPanelState::WinPos& pos, void*& out) {
        HWND hwnd = CreateWindowEx(WS_EX_TOOLWINDOW,
              cls, title,
              WS_OVERLAPPEDWINDOW,
              pos.x, pos.y, pos.w, pos.h,
              nullptr, nullptr, (HINSTANCE)hInst, nullptr);
        out = hwnd;
        if (hwnd) ShowWindow(hwnd, SW_HIDE);
    };

    create(CLASS_SOUNDMGR,    L"Sound Manager",       g_editorPanels.soundMgrPos,   g_editorPanels.hSoundMgr);
    create(CLASS_TEXTUREMGR,  L"Texture Manager",     g_editorPanels.textureMgrPos, g_editorPanels.hTextureMgr);
    create(CLASS_PAWNNMGR,    L"Pawn Manager",        g_editorPanels.pawnMgrPos,    g_editorPanels.hPawnMgr);
    create(CLASS_SCRIPTMGR,   L"Script Manager",      g_editorPanels.scriptMgrPos,  g_editorPanels.hScriptMgr);
    create(CLASS_MODELBRW,    L"Model / Mesh Browser",g_editorPanels.modelBrwPos,   g_editorPanels.hModelBrowser);
    create(CLASS_PICKUPPANEL, L"Pickups",             g_editorPanels.pickPanelPos,  g_editorPanels.hPickupPanel);
    create(CLASS_NODEPANEL,   L"Nodes",               g_editorPanels.nodePanelPos,  g_editorPanels.hNodePanel);
    create(CLASS_HMEDITOR,    L"Heightmap Editor",     g_editorPanels.heightmapEditorPos, g_editorPanels.hHeightmapEditor);
    create(CLASS_WORLDGRAPH,  L"World Hierarchy",      g_editorPanels.worldGraphPos,       g_editorPanels.hWorldGraph);
    create(CLASS_PROPSPANEL,  L"Entity Properties",    g_editorPanels.propsPanelPos,        g_editorPanels.hPropsPanel);
    create(CLASS_ANIMPANEL,   L"Animation",            g_editorPanels.animPanelPos,         g_editorPanels.hAnimPanel);
    create(CLASS_MATERIALPANEL,L"Material Graph",       g_editorPanels.materialPanelPos,     g_editorPanels.hMaterialPanel);
    create(CLASS_LEVELLIST,   L"Level List / Campaign",g_editorPanels.levelListPos,         g_editorPanels.hLevelList);
    create(CLASS_KEYMAP,      L"Keyboard Shortcuts",   g_editorPanels.keymapPos,            g_editorPanels.hKeymap);
    create(CLASS_SURFACEPROPS,L"Surface Properties",    g_editorPanels.surfacePropsPos,     g_editorPanels.hSurfaceProps);

    // Restore a maximized main window. Done after the panels exist so the stats
    // sidebar is sized against the first maximized client rect.
    if (g_hRaylibWnd) {
        IniConfig cfg;
        if (cfg.Load(kEditorIniPath) && cfg.GetBool("Layout", "main_max", false))
            ShowWindow(g_hRaylibWnd, SW_MAXIMIZE);
    }

    // Docked native stats sidebar (child of raylib window)
    {
        // Prevent OpenGL/raylib from painting over child HWNDs
        if (g_hRaylibWnd) {
            LONG_PTR style = GetWindowLongPtr(g_hRaylibWnd, GWL_STYLE);
            SetWindowLongPtr(g_hRaylibWnd, GWL_STYLE, style | WS_CLIPCHILDREN);
        }
        RECT rc = {};
        if (g_hRaylibWnd) GetClientRect(g_hRaylibWnd, &rc);
        int top = 28;
        int h = (rc.bottom - rc.top) - top;
        if (h < 1) h = 400;
        HWND hSb = CreateWindowEx(0, CLASS_STATSSIDEBAR, L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            0, top, STATS_SIDEBAR_W, h,
            g_hRaylibWnd, nullptr, (HINSTANCE)hInst, nullptr);
        g_editorPanels.hStatsSidebar = hSb;
        if (hSb) {
            ShowWindow(hSb, SW_SHOW);
            UpdateWindow(hSb);
        }
    }

    ScanTextureBrowserFiles();
    ScanSoundBrowserFiles();
    ScanModelBrowserFiles();
    ScanScriptFiles();
}

void DestroyAllEditorWindows() {
    // Persist panel geometry (and the main window's restored size) before the
    // windows stop existing. Best-effort: a missing INI is fine.
    SavePanelLayouts();
    auto destroy = [](void*& hwnd) {
        if (hwnd) { DestroyWindow((HWND)hwnd); hwnd = nullptr; }
    };
    destroy(g_editorPanels.hSoundMgr);
    destroy(g_editorPanels.hTextureMgr);
    destroy(g_editorPanels.hPawnMgr);
    destroy(g_editorPanels.hAnimPanel);
    destroy(g_editorPanels.hMaterialPanel);
    destroy(g_editorPanels.hScriptMgr);
    destroy(g_editorPanels.hModelBrowser);
    destroy(g_editorPanels.hPickupPanel);
    destroy(g_editorPanels.hNodePanel);
    destroy(g_editorPanels.hHeightmapEditor);
    destroy(g_editorPanels.hWorldGraph);
    destroy(g_editorPanels.hPropsPanel);
    destroy(g_editorPanels.hLevelList);
    destroy(g_editorPanels.hStatsSidebar);
    // Was missing: the Surface Properties window was created at startup like every
    // other panel but never destroyed here, so it outlived the editor session.
    destroy(g_editorPanels.hSurfaceProps);
    destroy(g_editorPanels.hKeymap);
    if (g_sbBgBrush) { DeleteObject(g_sbBgBrush); g_sbBgBrush = nullptr; }
    if (g_editorPanels.hPreviewBitmap) {
        DeleteObject((HGDIOBJ)g_editorPanels.hPreviewBitmap);
        g_editorPanels.hPreviewBitmap = nullptr;
    }
}

// ============================================================================
// Panel keyboard plumbing (see UiPanels.hpp)
//
// The panels are ordinary modeless top-level windows, not dialogs, so Windows
// gives them none of the dialog keyboard behaviour: Tab does nothing (even with
// WS_TABSTOP set), and a single-line EDIT does not implement Ctrl+A. IsDialogMessage
// is not a good fit here because the Surface Properties controls live on three
// child PAGE windows (g_spfPages), which IsDialogMessage does not traverse.
//
// So the pump asks us first, and this file implements the ring explicitly over
// every visible WS_TABSTOP descendant. Hidden tab pages fail IsWindowVisible, so
// only the active page participates - which is exactly the wanted behaviour.
// ============================================================================

static bool EditorUi_IsPanel(HWND h) {
    if (!h) return false;
    void* const panels[] = {
        g_editorPanels.hSoundMgr, g_editorPanels.hTextureMgr, g_editorPanels.hPawnMgr,
        g_editorPanels.hScriptMgr, g_editorPanels.hModelBrowser, g_editorPanels.hPickupPanel,
        g_editorPanels.hNodePanel, g_editorPanels.hHeightmapEditor, g_editorPanels.hWorldGraph,
        g_editorPanels.hPropsPanel, g_editorPanels.hLevelList, g_editorPanels.hAnimPanel,
        g_editorPanels.hMaterialPanel,
        g_editorPanels.hSurfaceProps, g_editorPanels.hKeymap, g_editorPanels.hStatsSidebar,
    };
    for (void* p : panels)
        if (p && (HWND)p == h) return true;
    return false;
}

// The panel that owns a focused/typed-into window, or nullptr when the window
// belongs to the raylib viewport. The stats sidebar is a CHILD of the raylib
// window, so its controls need the special case.
static HWND EditorUi_PanelRoot(HWND h) {
    if (!h) return nullptr;
    HWND root = GetAncestor(h, GA_ROOT);
    if (EditorUi_IsPanel(root)) return root;
    HWND sb = (HWND)g_editorPanels.hStatsSidebar;
    if (sb && (h == sb || IsChild(sb, h))) return sb;
    return nullptr;
}

static bool EditorUi_IsClass(HWND h, const wchar_t* name) {
    if (!h) return false;
    wchar_t cls[32] = {};
    GetClassNameW(h, cls, 32);
    return _wcsicmp(cls, name) == 0;
}

struct EditorUi_TabStops {
    std::vector<HWND> stops;
};

static BOOL CALLBACK EditorUi_CollectTabStop(HWND h, LPARAM lp) {
    auto* out = (EditorUi_TabStops*)lp;
    if (!IsWindowVisible(h)) return TRUE;              // hidden tab pages drop out
    LONG style = GetWindowLongW(h, GWL_STYLE);
    if (!(style & WS_TABSTOP) || (style & WS_DISABLED)) return TRUE;
    if (EditorUi_IsClass(h, L"STATIC")) return TRUE;   // labels are never stops
    out->stops.push_back(h);
    return TRUE;
}

static void EditorUi_FocusRelative(HWND from, int dir) {
    HWND root = EditorUi_PanelRoot(from);
    if (!root) return;
    EditorUi_TabStops stops;
    EnumChildWindows(root, EditorUi_CollectTabStop, (LPARAM)&stops);
    if (stops.stops.empty()) return;

    const int count = (int)stops.stops.size();
    int idx = -1;
    for (int i = 0; i < count; i++)
        if (stops.stops[i] == from) { idx = i; break; }
    const int next = (idx < 0) ? (dir > 0 ? 0 : count - 1)
                               : ((idx + dir) % count + count) % count;
    SetFocus(stops.stops[next]);
}

// Ctrl+Backspace: delete the word before the caret. With a selection, clear it.
static void EditorUi_WordDelete(HWND edit) {
    int start = 0, end = 0;
    SendMessageW(edit, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
    if (start == end && start > 0) {
        const int len = (int)SendMessageW(edit, WM_GETTEXTLENGTH, 0, 0);
        std::wstring text((size_t)len + 1, L'\0');
        GetWindowTextW(edit, text.data(), len + 1);
        int i = start;
        while (i > 0 && iswspace(text[(size_t)i - 1])) i--;
        while (i > 0 && !iswspace(text[(size_t)i - 1])) i--;
        SendMessageW(edit, EM_SETSEL, (WPARAM)i, (LPARAM)start);
    }
    SendMessageW(edit, WM_CLEAR, 0, 0);
}

bool EditorUi_HandleMessage(void* msgPtr) {
    MSG* msg = (MSG*)msgPtr;
    if (!msg || !msg->hwnd) return false;
    if (!EditorUi_PanelRoot(msg->hwnd)) return false;

    HWND focus = GetFocus();
    HWND msgRoot = EditorUi_PanelRoot(msg->hwnd);
    // Only trust GetFocus when it belongs to the same panel as the message;
    // otherwise keyboard focus is somewhere we do not own.
    if (!focus || EditorUi_PanelRoot(focus) != msgRoot) focus = msg->hwnd;
    const bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool alt   = (GetKeyState(VK_MENU) & 0x8000) != 0;

    if (msg->message != WM_KEYDOWN) return false;

    // Tab / Shift+Tab. Alt+Tab and Ctrl+Tab are left to Windows/the control.
    if (msg->wParam == VK_TAB && !ctrl && !alt) {
        EditorUi_FocusRelative(focus ? focus : msg->hwnd, shift ? -1 : 1);
        return true;
    }

    // Escape: leave a field, then close the panel. A dropped combo consumes it
    // itself (closing the list) and must not also close the window.
    if (msg->wParam == VK_ESCAPE && !ctrl && !alt) {
        if (EditorUi_IsClass(focus, L"COMBOBOX") &&
            SendMessageW(focus, CB_GETDROPPEDSTATE, 0, 0))
            return false;
        HWND root = msgRoot;
        if (focus && focus != root) { SetFocus(root); return true; }
        PostMessageW(root, WM_CLOSE, 0, 0);
        return true;
    }

    const bool isEdit = EditorUi_IsClass(focus, L"EDIT");

    if (ctrl && (msg->wParam == 'A' || msg->wParam == 'a') && isEdit) {
        SendMessageW(focus, EM_SETSEL, 0, (LPARAM)-1);
        return true;
    }

    if (ctrl && msg->wParam == VK_BACK && isEdit) {
        LONG style = GetWindowLongW(focus, GWL_STYLE);
        if (!(style & ES_MULTILINE)) {
            EditorUi_WordDelete(focus);
            return true;
        }
    }

    // Enter in a single-line field = next field (Shift+Enter = previous). This
    // keeps a long Properties form keyboard-drivable without a default button.
    if (msg->wParam == VK_RETURN && !ctrl && !alt && isEdit) {
        LONG style = GetWindowLongW(focus, GWL_STYLE);
        if (!(style & ES_MULTILINE)) {
            EditorUi_FocusRelative(focus, shift ? -1 : 1);
            return true;
        }
    }

    return false;
}

bool EditorUi_ViewportHasFocus() {
    HWND focus = GetFocus();
    if (!focus) return true;                       // startup: no panel focus yet
    if (GetAncestor(focus, GA_ROOT) != g_hRaylibWnd) return false;
    HWND sb = (HWND)g_editorPanels.hStatsSidebar;
    if (sb && (focus == sb || IsChild(sb, focus))) return false;
    return true;
}

