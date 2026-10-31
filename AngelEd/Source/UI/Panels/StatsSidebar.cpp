// ============================================================================
// StatsSidebar.cpp
//
// FRAGMENT - not a standalone translation unit. Included by ../UiShell.cpp,
// which is the single TU for the whole UI layer. That is deliberate: see
// UiShell.cpp for why the layer is built as a unity build rather than as 16
// separate objects.
// ============================================================================

#include <string>   // std::wstring caches for the label change detection

// =====================================================================
// Stats Sidebar Ã¢â‚¬â€ docked native left panel (stats + toolbox)
// =====================================================================
static const int ID_SB_TITLE   = 900;
static const int ID_SB_POS     = 901;
static const int ID_SB_SIZE    = 902;
static const int ID_SB_ROT     = 903;
static const int ID_SB_COLL    = 904;
static const int ID_SB_CHUNKS  = 905;
static const int ID_SB_MODE    = 906;
static const int ID_SB_CAM_L   = 907;
static const int ID_SB_CAM     = 908;
static const int ID_SB_SEP1    = 909;
static const int ID_SB_SEP2    = 910;
static const int ID_SB_WORLD   = 911;   // world size: brushes/meshes/npcs
// Toolbox button IDs
static const int ID_TB_CSG_BOX    = 920;
static const int ID_TB_CSG_CYL    = 921;
static const int ID_TB_CSG_SPH    = 922;
static const int ID_TB_CSG_PYR    = 923;
static const int ID_TB_CSG_PLN    = 924;
static const int ID_TB_OP_SOLID   = 925;
static const int ID_TB_OP_ADD     = 926;
static const int ID_TB_OP_SUB     = 927;
static const int ID_TB_OP_INTER   = 928;
static const int ID_TB_MODE_CAM   = 929;
static const int ID_TB_MODE_MOVE  = 930;
static const int ID_TB_MODE_SCALE = 931;
static const int ID_TB_MODE_ROT   = 932;
static const int ID_TB_SHOW_COLLISION = 933;
// New-brush size fields (W/H/L). Placed before the primitives so the next placed
// brush spawns at this size instead of the old hard-coded 4x4x4.
static const int ID_SB_NEWW = 954;
static const int ID_SB_NEWH = 955;
static const int ID_SB_NEWL = 956;

static HWND g_sbPos = nullptr, g_sbSize = nullptr, g_sbRot = nullptr;
static HWND g_sbColl = nullptr, g_sbChunks = nullptr, g_sbMode = nullptr;
static HWND g_sbCam = nullptr;
static HWND g_sbWorld = nullptr;
static HWND g_sbNewW = nullptr, g_sbNewH = nullptr, g_sbNewL = nullptr;
static HBRUSH g_sbBgBrush = nullptr;

int GetStatsSidebarWidth() { return STATS_SIDEBAR_W; }

void LayoutStatsSidebar(int clientW, int clientH, int topOffset, int width) {
    (void)clientW;
    HWND hwnd = (HWND)g_editorPanels.hStatsSidebar;
    if (!hwnd) return;
    int w = width > 0 ? width : STATS_SIDEBAR_W;
    int h = clientH - topOffset;
    if (h < 1) h = 1;

    // SetWindowPos with SWP_SHOWWINDOW on every frame invalidated the whole
    // sidebar (and its child STATIC controls) once per frame. The docked panel
    // only changes on a resize, a layout change or a window recreation.
    static HWND s_lastHwnd = nullptr;
    static int s_lastW = -1, s_lastH = -1, s_lastTop = -1;
    if (s_lastHwnd == hwnd && s_lastW == w && s_lastH == h && s_lastTop == topOffset)
        return;
    s_lastHwnd = hwnd;
    s_lastW = w;
    s_lastH = h;
    s_lastTop = topOffset;
    SetWindowPos(hwnd, HWND_TOP, 0, topOffset, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void UpdateStatsSidebar(float posX, float posY, float posZ,
                        float sizeX, float sizeY, float sizeZ,
                        float rot, float scale,
                        int collisionVols, int chunks,
                        const char* mode,
                        float camX, float camY, float camZ,
                        const EditorWorldStats& stats) {
    HWND hwnd = (HWND)g_editorPanels.hStatsSidebar;
    if (!hwnd) return;

    // Only push a label when its text actually changed. This used to call
    // SetWindowTextW nine times per frame; each one invalidates the STATIC and
    // makes GDI repaint the panel, for values that change once per drag.
    static HWND s_lastHwnd = nullptr;
    static std::wstring s_pos, s_size, s_rot, s_coll, s_chunks, s_mode, s_cam, s_world;
    if (s_lastHwnd != hwnd) {
        s_lastHwnd = hwnd;
        s_pos.clear(); s_size.clear(); s_rot.clear(); s_coll.clear();
        s_chunks.clear(); s_mode.clear(); s_cam.clear(); s_world.clear();
    }
    wchar_t buf[128];
    auto setA = [](HWND h, const wchar_t* t, std::wstring& cache) {
        if (h && cache != t) {
            SetWindowTextW(h, t);
            cache = t;
        }
    };

    swprintf(buf, 128, L"Pos: %.1f %.1f %.1f", posX, posY, posZ);
    setA(g_sbPos, buf, s_pos);
    swprintf(buf, 128, L"Size: %.1f x %.1f x %.1f", sizeX, sizeY, sizeZ);
    setA(g_sbSize, buf, s_size);
    swprintf(buf, 128, L"Rot: %.0f  Scale: %.1f", rot, scale);
    setA(g_sbRot, buf, s_rot);
    swprintf(buf, 128, L"Collision: %d vols", collisionVols);
    // A Sub/Intersect brush that produced no volume leaves the brush rendering
    // but the player falling through it, and that is invisible from the
    // viewport. Say so on the counter until the next brush is committed.
    if (g_editorPanels.collisionOpWarning)
        swprintf(buf, 128, L"Collision: %d vols  << NO SOLID", collisionVols);
    setA(g_sbColl, buf, s_coll);
    swprintf(buf, 128, L"Chunks: %d", chunks);
    setA(g_sbChunks, buf, s_chunks);
    {
        wchar_t modeW[32] = L"MODEL";
        if (mode && mode[0])
            MultiByteToWideChar(CP_UTF8, 0, mode, -1, modeW, 32);
        swprintf(buf, 128, L"Mode: %ls", modeW);
        setA(g_sbMode, buf, s_mode);
    }
    swprintf(buf, 128, L"%.1f %.1f %.1f", camX, camY, camZ);
    setA(g_sbCam, buf, s_cam);

    // World size (Phase 5.3): one line, brushes / meshes / NPCs, so the level's
    // weight is visible without expanding the WorldGraph.
    swprintf(buf, 128, L"World: %d br  %d mesh  %d NPC",
             stats.brushes, stats.meshes, stats.npcs);
    setA(g_sbWorld, buf, s_world);
}

static LRESULT CALLBACK StatsSidebarProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        if (!g_sbBgBrush)
            g_sbBgBrush = CreateSolidBrush(RGB(25, 25, 30));

        int x = 10, y = 10, lw = STATS_SIDEBAR_W - 20, bh = 24, gap = 4, bw = (lw - gap) / 2;
        // --- Stats section ---
        CreateLabel(hwnd, L"Stats", x, y, lw, 20, ID_SB_TITLE); y += 28;
        g_sbPos    = CreateLabel(hwnd, L"Pos: 0 0 0", x, y, lw, 18, ID_SB_POS); y += 20;
        g_sbSize   = CreateLabel(hwnd, L"Size: 0 x 0 x 0", x, y, lw, 18, ID_SB_SIZE); y += 20;
        g_sbRot    = CreateLabel(hwnd, L"Rot: 0  Scale: 1.0", x, y, lw, 18, ID_SB_ROT); y += 26;
        CreateLabel(hwnd, L"---", x, y, lw, 16, ID_SB_SEP1); y += 20;
        g_sbColl   = CreateLabel(hwnd, L"Collision: 0 vols", x, y, lw, 18, ID_SB_COLL); y += 20;
        g_sbChunks = CreateLabel(hwnd, L"Chunks: 0", x, y, lw, 18, ID_SB_CHUNKS); y += 26;
        g_sbMode   = CreateLabel(hwnd, L"Mode: MODEL", x, y, lw, 18, ID_SB_MODE); y += 26;
        CreateLabel(hwnd, L"---", x, y, lw, 16, ID_SB_SEP2); y += 20;
        CreateLabel(hwnd, L"Camera:", x, y, lw, 18, ID_SB_CAM_L); y += 20;
        g_sbCam    = CreateLabel(hwnd, L"0.0 0.0 0.0", x, y, lw, 18, ID_SB_CAM); y += 20;
        g_sbWorld  = CreateLabel(hwnd, L"World: 0 br  0 mesh  0 NPC", x, y, lw, 18, ID_SB_WORLD); y += 22;

        // New-brush size: the NEXT primitive placed spawns at these extents, so
        // placing a wall no longer means add-a-4x4-cube then resize. Editable before
        // clicking a primitive (three small fields, all in the tab ring).
        CreateLabel(hwnd, L"New brush size (W H L)", x, y, lw, 18, 954 + 100); y += 20;
        {
            const int fw = 46, gap = 4;
            int fx = x;
            auto mkEdit = [&](int cid, const wchar_t* txt) -> HWND {
                return CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", txt,
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                    fx, y, fw, 20, hwnd, (HMENU)(INT_PTR)cid, g_hInst, nullptr);
            };
            CreateLabel(hwnd, L"W", fx, y + 2, 14, 16, 954 + 101); fx += 14;
            g_sbNewW = mkEdit(ID_SB_NEWW, L"4"); fx += fw + gap;
            CreateLabel(hwnd, L"H", fx, y + 2, 14, 16, 954 + 102); fx += 14;
            g_sbNewH = mkEdit(ID_SB_NEWH, L"4"); fx += fw + gap;
            CreateLabel(hwnd, L"L", fx, y + 2, 14, 16, 954 + 103); fx += 14;
            g_sbNewL = mkEdit(ID_SB_NEWL, L"4");
            y += 28;
        }

        // --- Primitives section (icons) ---
        CreateLabel(hwnd, L"Primitives", x, y, lw, 18, 950); y += 22;
        CreateIconButton(hwnd, L"Cube",     x, y, bw, bh, ID_TB_CSG_BOX, "BBCube");
        CreateIconButton(hwnd, L"Cylinder", x + bw + gap, y, bw, bh, ID_TB_CSG_CYL, "BBCylinder"); y += bh + gap;
        CreateIconButton(hwnd, L"Sphere",   x, y, bw, bh, ID_TB_CSG_SPH, "BBSphere");
        CreateIconButton(hwnd, L"Pyramid",  x + bw + gap, y, bw, bh, ID_TB_CSG_PYR, "BBGeneric"); y += bh + gap;
        CreateIconButton(hwnd, L"Plane",    x, y, bw, bh, ID_TB_CSG_PLN, "BBSheet"); y += bh + 10;

        // --- CSG Op section (icons) ---
        CreateLabel(hwnd, L"CSG Op", x, y, lw, 18, 951); y += 22;
        CreateIconButton(hwnd, L"Solid",    x, y, bw, bh, ID_TB_OP_SOLID, "BBCube");
        CreateIconButton(hwnd, L"Add",      x + bw + gap, y, bw, bh, ID_TB_OP_ADD, "ModeAdd"); y += bh + gap;
        CreateIconButton(hwnd, L"Sub",      x, y, bw, bh, ID_TB_OP_SUB, "ModeSubtract");
        CreateIconButton(hwnd, L"Inter",    x + bw + gap, y, bw, bh, ID_TB_OP_INTER, "ModeIntersect"); y += bh + 10;

// --- Tool Mode section (icons) ---
        CreateLabel(hwnd, L"Tool", x, y, lw, 18, 952); y += 22;
        CreateIconButton(hwnd, L"Cam",      x, y, bw, bh, ID_TB_MODE_CAM, "ModeCamera");
        CreateIconButton(hwnd, L"Move",     x + bw + gap, y, bw, bh, ID_TB_MODE_MOVE, "ModeVertex"); y += bh + gap;
        CreateIconButton(hwnd, L"Scale",    x, y, bw, bh, ID_TB_MODE_SCALE, "ModeScale");
        CreateIconButton(hwnd, L"Rotate",   x + bw + gap, y, bw, bh, ID_TB_MODE_ROT, "ModeRotate"); y += bh + 10;

        // --- View section (icons) ---
        CreateLabel(hwnd, L"View", x, y, lw, 18, 953); y += 22;
        CreateIconButton(hwnd, L"Collision", x, y, bw, bh, ID_TB_SHOW_COLLISION, "BrushClip");
        y += bh + 10;
        break;
    }
    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)l;
        if (dis && dis->CtlType == ODT_BUTTON) return DrawIconButton(dis);
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(w);
        // New-brush size edits: parse on every change and DO NOT fall through to
        // the toolbox focus-return below (that would steal focus from the field
        // mid-keystroke).
        if (id == ID_SB_NEWW || id == ID_SB_NEWH || id == ID_SB_NEWL) {
            wchar_t buf[32] = {};
            GetWindowTextW((HWND)l, buf, 32);
            float v = (float)wcstod(buf, nullptr);
            if (!(v > 0.0f)) v = 0.1f;   // never zero/negative
            if (id == ID_SB_NEWW)      g_editorPanels.newBrushW = v;
            else if (id == ID_SB_NEWH) g_editorPanels.newBrushH = v;
            else                       g_editorPanels.newBrushL = v;
            break;
        }
        switch (id) {
            // --- Primitives ---
            // Both halves post a CsgIntent. They are separate kinds because
            // CsgPlace resets the ghost to a default box at the camera while
            // CsgCommit reads whatever the ghost currently is - collapsing them
            // would either lose the reset or commit a brush nobody positioned.
            case ID_TB_CSG_BOX:  ed::EventBus::instance().post(ed::Ev::CsgPlace, ed::CsgIntent{0, -1, false}); break;
            case ID_TB_CSG_CYL:  ed::EventBus::instance().post(ed::Ev::CsgPlace, ed::CsgIntent{1, -1, false}); break;
            case ID_TB_CSG_SPH:  ed::EventBus::instance().post(ed::Ev::CsgPlace, ed::CsgIntent{2, -1, false}); break;
            case ID_TB_CSG_PYR:  ed::EventBus::instance().post(ed::Ev::CsgPlace, ed::CsgIntent{3, -1, false}); break;
            case ID_TB_CSG_PLN:  ed::EventBus::instance().post(ed::Ev::CsgPlace, ed::CsgIntent{4, -1, false}); break;
            // --- CSG Operations (commit immediately) ---
            case ID_TB_OP_SOLID: Editor_SetCsgOperation(0); Editor_SetPlaceMode(0);
                ed::EventBus::instance().post(ed::Ev::CsgCommit, ed::CsgIntent{-1, 0, true}); break;
            case ID_TB_OP_ADD:   Editor_SetCsgOperation(1); Editor_SetPlaceMode(0);
                ed::EventBus::instance().post(ed::Ev::CsgCommit, ed::CsgIntent{-1, 1, true}); break;
            case ID_TB_OP_SUB:   Editor_SetCsgOperation(2); Editor_SetPlaceMode(0);
                ed::EventBus::instance().post(ed::Ev::CsgCommit, ed::CsgIntent{-1, 2, false}); break;
            case ID_TB_OP_INTER: Editor_SetCsgOperation(3); Editor_SetPlaceMode(0);
                ed::EventBus::instance().post(ed::Ev::CsgCommit, ed::CsgIntent{-1, 3, false}); break;
            // --- Tool Modes ---
            case ID_TB_MODE_CAM:   g_editorPanels.currentToolMode = 0; break;
            case ID_TB_MODE_MOVE:  g_editorPanels.currentToolMode = 1; break;
            case ID_TB_MODE_SCALE: g_editorPanels.currentToolMode = 2; break;
            case ID_TB_MODE_ROT:   g_editorPanels.currentToolMode = 3; break;
            // --- View ---
            case ID_TB_SHOW_COLLISION:
                g_editorPanels.showCollisionBounds = !g_editorPanels.showCollisionBounds;
                OzoneLoader::Instance().SetDrawCollisionProxies(g_editorPanels.showCollisionBounds);
                break;
        }
        // Hand keyboard focus back to the viewport.
        //
        // A Win32 button that keeps focus takes it from the GLFW/raylib window,
        // and raylib's keyboard state freezes with it. That is not cosmetic:
        // Alt+MMB (camera orbit) then reads no Alt and silently degrades to a
        // plain pan, so the camera "rotation stops working" after clicking any
        // mode button (Cam/Move/Scale/Rotate) and coming back - and every
        // viewport shortcut (Home, NumPad presets, Delete...) stops too until
        // the user clicks the viewport again. The sidebar is a toolbar with no
        // text input, so returning focus here is always safe.
        if (g_hRaylibWnd) SetFocus(g_hRaylibWnd);
        break;
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)w;
        SetTextColor(hdc, RGB(180, 200, 220));
        SetBkColor(hdc, RGB(25, 25, 30));
        if (!g_sbBgBrush) g_sbBgBrush = CreateSolidBrush(RGB(25, 25, 30));
        return (LRESULT)g_sbBgBrush;
    }
    case WM_ERASEBKGND: {
        RECT rc; GetClientRect(hwnd, &rc);
        if (!g_sbBgBrush) g_sbBgBrush = CreateSolidBrush(RGB(25, 25, 30));
        FillRect((HDC)w, &rc, g_sbBgBrush);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        if (!g_sbBgBrush) g_sbBgBrush = CreateSolidBrush(RGB(25, 25, 30));
        FillRect(hdc, &rc, g_sbBgBrush);
        // Right edge line
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(60, 60, 70));
        HGDIOBJ old = SelectObject(hdc, pen);
        MoveToEx(hdc, rc.right - 1, 0, nullptr);
        LineTo(hdc, rc.right - 1, rc.bottom);
        SelectObject(hdc, old);
        DeleteObject(pen);
        EndPaint(hwnd, &ps);
        break;
    }
    case WM_DESTROY:
        g_editorPanels.hStatsSidebar = nullptr;
        g_sbPos = g_sbSize = g_sbRot = g_sbColl = g_sbChunks = g_sbMode = g_sbCam = g_sbWorld = nullptr;
        g_sbNewW = g_sbNewH = g_sbNewL = nullptr;
        break;
    default:
        return DefWindowProc(hwnd, msg, w, l);
    }
    return 0;
}

