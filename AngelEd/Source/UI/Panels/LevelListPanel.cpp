// ============================================================================
// LevelListPanel.cpp
//
// FRAGMENT - not a standalone translation unit. Included by ../UiShell.cpp,
// which is the single TU for the whole UI layer. That is deliberate: see
// UiShell.cpp for why the layer is built as a unity build rather than as 16
// separate objects.
// ============================================================================

// =====================================================================
// LevelList / Campaign panel â€” worlds + portal connections
// =====================================================================
static const int ID_LL_LIST   = 500;
static const int ID_LL_OPEN   = 501;
static const int ID_LL_LINK   = 502;
static const int ID_LL_REFRESH= 503;
static const int ID_LL_CLOSE  = 504;

struct LevelListEntry {
    std::string world;
    std::string format;      // "WDL" or "OZONE"
    int portalsOut = 0;
    int portalsIn = 0;
    bool isCurrent = false;
};

static std::vector<LevelListEntry> g_levelList;

// Extract portal target-world names from a world file (light text scan)
static void ScanPortalTargets(const fs::path& worldFile, std::vector<std::string>& out) {
    out.clear();
    std::ifstream f(worldFile);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        // trim leading whitespace
        size_t s = line.find_first_not_of(" \t\r\n");
        if (s == std::string::npos) continue;
        line = line.substr(s);
        if (line.rfind("Portal:", 0) == 0) {                    // WDL
            size_t c1 = line.find(':', 7);
            size_t c2 = (c1 != std::string::npos) ? line.find(':', c1 + 1) : std::string::npos;
            if (c1 != std::string::npos && c2 != std::string::npos)
                out.push_back(line.substr(c1 + 1, c2 - c1 - 1));
        } else if (line.rfind("portal ", 0) == 0) {             // OZONE
            size_t sp = line.find(' ', 7);
            if (sp != std::string::npos)
                out.push_back(line.substr(7, sp - 7));
        }
    }
}

static void BuildLevelList() {
    g_levelList.clear();
    ScanAvailableWorlds();
    extern std::string Editor_GetCurrentWorldName();
    std::string current = Editor_GetCurrentWorldName();

    // First pass: outgoing portal counts
    std::vector<std::vector<std::string>> targets(g_availableWorlds.size());
    for (size_t i = 0; i < g_availableWorlds.size(); i++) {
        LevelListEntry e;
        e.world = g_availableWorlds[i];
        e.isCurrent = (e.world == current);
        fs::path ozone = fs::path("GameData/Worlds") / e.world / "World.ozone";
        if (!fs::exists(ozone)) ozone = fs::path("../GameData/Worlds") / e.world / "World.ozone";
        if (fs::exists(ozone)) { e.format = "OZONE"; ScanPortalTargets(ozone, targets[i]); }
        e.portalsOut = (int)targets[i].size();
        g_levelList.push_back(e);
    }
    // Second pass: incoming counts
    for (auto& e : g_levelList) {
        e.portalsIn = 0;
        for (size_t i = 0; i < g_levelList.size(); i++) {
            if (&g_levelList[i] == &e) continue;
            for (const auto& t : targets[i])
                if (t == e.world) { e.portalsIn++; break; }
        }
    }
}

static void PopulateLevelList(HWND hList) {
    ListView_DeleteAllItems(hList);
    for (size_t i = 0; i < g_levelList.size(); i++) {
        auto& e = g_levelList[i];
        wchar_t buf[64];
        auto setItem = [&](int col, const wchar_t* txt) {
            LVITEMW lvi = {};
            lvi.mask = LVIF_TEXT;
            lvi.iItem = (int)i;
            lvi.iSubItem = col;
            lvi.pszText = const_cast<wchar_t*>(txt);
            if (col == 0) ListView_InsertItem(hList, &lvi);
            else ListView_SetItem(hList, &lvi);
        };
        std::wstring wname(e.world.begin(), e.world.end());
        std::wstring wfmt(e.format.begin(), e.format.end());
        _snwprintf(buf, 63, L"%d", e.portalsOut); buf[63] = 0;
        std::wstring wout(buf);
        _snwprintf(buf, 63, L"%d", e.portalsIn); buf[63] = 0;
        std::wstring win(buf);
        setItem(0, wname.c_str());
        setItem(1, wfmt.c_str());
        setItem(2, wout.c_str());
        setItem(3, win.c_str());
        setItem(4, e.isCurrent ? L"< current >" : L"");
    }
}

static void LevelListLayout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int m = 6, btnH = 26, btnW = 110;
    HWND list = GetDlgItem(hwnd, ID_LL_LIST);
    if (list) {
        int h = rc.bottom - btnH - m * 3;
        if (h < 40) h = 40;
        MoveWindow(list, m, m, rc.right - 2 * m, h, TRUE);
    }
    const int by = rc.bottom - m - btnH;
    int x = m;
    const int ids[] = {ID_LL_OPEN, ID_LL_LINK, ID_LL_REFRESH};
    for (int id : ids) {
        if (HWND h = GetDlgItem(hwnd, id)) MoveWindow(h, x, by, btnW, btnH, TRUE);
        x += btnW + 6;
    }
    if (HWND h = GetDlgItem(hwnd, ID_LL_CLOSE))
        MoveWindow(h, rc.right - m - btnW, by, btnW, btnH, TRUE);
}

static LRESULT CALLBACK LevelListProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    static HWND hList;
    switch (msg) {
    case WM_CREATE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int bw = 110, margin = 6;
        int listH = rc.bottom - bw - margin * 3;

        hList = CreateWindowEx(0, WC_LISTVIEW, L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | WS_TABSTOP,
            margin, margin, rc.right - margin * 2, listH,
            hwnd, (HMENU)ID_LL_LIST, g_hInst, nullptr);
        ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        LVCOLUMNW lvc = {};
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        lvc.fmt = LVCFMT_LEFT;
        const wchar_t* headers[] = {L"World", L"Format", L"Links Out", L"Links In", L""};
        int widths[] = {160, 70, 80, 80, 90};
        for (int i = 0; i < 5; i++) {
            lvc.cx = widths[i];
            lvc.pszText = const_cast<wchar_t*>(headers[i]);
            ListView_InsertColumn(hList, i, &lvc);
        }

        CreateButton(hwnd, L"Open World",   margin, listH + margin * 2, bw, 26, ID_LL_OPEN);
        CreateButton(hwnd, L"Add Link ->",  margin + bw + 6, listH + margin * 2, bw, 26, ID_LL_LINK);
        CreateButton(hwnd, L"Refresh",      margin + (bw + 6) * 2, listH + margin * 2, bw, 26, ID_LL_REFRESH);
        CreateButton(hwnd, L"Close",        rc.right - bw - margin, listH + margin * 2, bw, 26, ID_LL_CLOSE);

        BuildLevelList();
        PopulateLevelList(hList);
        LevelListLayout(hwnd);
        break;
    }
    case WM_SIZE:
        LevelListLayout(hwnd);
        break;
    case WM_GETMINMAXINFO:
        EnforceMinTrackSize(l, 500, 320);
        break;
    case WM_USER + 51:
        BuildLevelList();
        PopulateLevelList(hList);
        break;
    case WM_NOTIFY: {
        NMHDR* nm = (NMHDR*)l;
        if (nm->idFrom == ID_LL_LIST && nm->code == NM_DBLCLK) {
            int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
            if (sel >= 0 && sel < (int)g_levelList.size())
                ed::EventBus::instance().post(ed::Ev::OpenWorld, g_levelList[sel].world);
        }
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(w);
        int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
        if (id == ID_LL_CLOSE) { ShowLevelList(false); break; }
        if (id == ID_LL_REFRESH) {
            BuildLevelList();
            PopulateLevelList(hList);
            break;
        }
        if (sel < 0 || sel >= (int)g_levelList.size()) break;
        if (id == ID_LL_OPEN)
            ed::EventBus::instance().post(ed::Ev::OpenWorld, g_levelList[sel].world);
        if (id == ID_LL_LINK && !g_levelList[sel].isCurrent)
            ed::EventBus::instance().post(ed::Ev::LinkWorld, g_levelList[sel].world);
        break;
    }
    case WM_CLOSE: ShowLevelList(false); break;
    case WM_DESTROY: g_editorPanels.hLevelList = nullptr; break;
    default: return DefWindowProc(hwnd, msg, w, l);
    }
    return 0;
}

void ShowLevelList(bool show) {
    g_editorPanels.showLevelList = show;
    if (g_editorPanels.hLevelList)
        ShowWindow((HWND)g_editorPanels.hLevelList, show ? SW_SHOW : SW_HIDE);
}

void RefreshLevelList() {
    if (g_editorPanels.hLevelList)
        SendMessage((HWND)g_editorPanels.hLevelList, WM_USER + 51, 0, 0);
}

