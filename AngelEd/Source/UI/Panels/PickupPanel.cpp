// ============================================================================
// PickupPanel.cpp
//
// FRAGMENT - not a standalone translation unit. Included by ../UiShell.cpp,
// which is the single TU for the whole UI layer. That is deliberate: see
// UiShell.cpp for why the layer is built as a unity build rather than as 16
// separate objects.
// ============================================================================

// =====================================================================
// Pickups panel - dynamically generated from the LightningScript registry.
//
// This used to create one BUTTON per pickup def in WM_CREATE and never touch the
// list again. WM_CREATE runs before PawnDefs/ozls defs are registered and before
// the world is open, so on a normal launch the panel was permanently empty. It
// also could not scroll, and its Close button was positioned after the row loop,
// which put it below the window once there were more than a handful of defs.
//
// A LISTBOX + explicit Place button fixes all three: the list is refilled every
// time the panel is shown, it scrolls, and the buttons are pinned by a layout
// function that also runs on WM_SIZE.
// =====================================================================
static const int ID_PICK_CLOSE   = 100;
static const int ID_PICK_LIST    = 101;
static const int ID_PICK_PLACE   = 102;
static const int ID_PICK_REFRESH = 103;

static HWND g_pickList = nullptr;
static HWND g_pickPlace = nullptr;
static HWND g_pickClose = nullptr;
static HWND g_pickRefresh = nullptr;
static HWND g_pickLabel = nullptr;
static std::vector<std::string> g_pickupNames;

static void FillPickupList(HWND hwnd) {
    if (!g_pickList) return;
    SendMessageW(g_pickList, LB_RESETCONTENT, 0, 0);
    g_pickupNames.clear();

    std::vector<const EntityDef*> defs;
    LightningEntityRegistry::Instance().FindByType(EntityType::PICKUP, defs);
    for (const EntityDef* d : defs) {
        if (!d) continue;
        g_pickupNames.push_back(d->name);
        std::wstring label(d->name.begin(), d->name.end());
        SendMessageW(g_pickList, LB_ADDSTRING, 0, (LPARAM)label.c_str());
    }
    if (!g_pickupNames.empty())
        SendMessageW(g_pickList, LB_SETCURSEL, 0, 0);

    if (g_pickLabel) {
        wchar_t buf[96];
        swprintf(buf, 96, L"Pickups (%d) - select, then Place", (int)g_pickupNames.size());
        SetWindowTextW(g_pickLabel, buf);
    }
    (void)hwnd;
}

static void PickupLayout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int m = 8, btnH = 26, labelH = 18;
    if (g_pickLabel) MoveWindow(g_pickLabel, m, m, rc.right - 2 * m, labelH, TRUE);
    if (g_pickList) {
        const int top = m + labelH + 4;
        const int h = rc.bottom - top - btnH - 2 * m;
        MoveWindow(g_pickList, m, top, rc.right - 2 * m, (h > 30 ? h : 30), TRUE);
    }
    const int by = rc.bottom - m - btnH;
    int bx = m;
    if (g_pickPlace)   { MoveWindow(g_pickPlace, bx, by, 90, btnH, TRUE); bx += 96; }
    if (g_pickRefresh) { MoveWindow(g_pickRefresh, bx, by, 80, btnH, TRUE); }
    if (g_pickClose)   { MoveWindow(g_pickClose, rc.right - m - 80, by, 80, btnH, TRUE); }
}

static void PlaceSelectedPickup(HWND hwnd) {
    if (!g_pickList) return;
    const int sel = (int)SendMessageW(g_pickList, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)g_pickupNames.size()) return;

    // Resolve the def NAME here, not in the main loop. The list was filled from the
    // same FindByType order, so the lookup is exact at click time.
    ed::PlacementRequest pr;
    pr.kind = ed::PlacementRequest::Kind::Pickup;
    pr.key  = g_pickupNames[sel];
    ed::EventBus::instance().post(ed::Ev::BeginPlacement, pr);
    (void)hwnd;
}

void ShowPickupPanel(bool show) {
    g_editorPanels.showPickupPanel = show;
    if (g_editorPanels.hPickupPanel) {
        ShowWindow((HWND)g_editorPanels.hPickupPanel, show ? SW_SHOW : SW_HIDE);
        // Defs may have been registered (or a script created) since the last open.
        if (show)
            SendMessageW((HWND)g_editorPanels.hPickupPanel, WM_USER + 50, 0, 0);
    }
}

static LRESULT CALLBACK PickupPanelProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        g_pickLabel = CreateLabel(hwnd, L"Pickups", 8, 8, 200, 18, 0);
        g_pickList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY,
            8, 30, 220, 180, hwnd, (HMENU)(INT_PTR)ID_PICK_LIST, g_hInst, nullptr);
        g_pickPlace   = CreateButton(hwnd, L"Place",   8, 220, 90, 26, ID_PICK_PLACE);
        g_pickRefresh = CreateButton(hwnd, L"Refresh", 104, 220, 80, 26, ID_PICK_REFRESH);
        g_pickClose   = CreateButton(hwnd, L"Close",   190, 220, 80, 26, ID_PICK_CLOSE);
        FillPickupList(hwnd);
        PickupLayout(hwnd);
        break;
    }
    case WM_USER + 50:
        FillPickupList(hwnd);
        break;
    case WM_GETMINMAXINFO:
        EnforceMinTrackSize(l, 260, 280);
        break;
    case WM_SIZE:
        PickupLayout(hwnd);
        break;
    case WM_COMMAND: {
        const int id = LOWORD(w);
        if (id == ID_PICK_CLOSE) {
            ShowPickupPanel(false);
        } else if (id == ID_PICK_REFRESH) {
            FillPickupList(hwnd);
        } else if (id == ID_PICK_PLACE) {
            PlaceSelectedPickup(hwnd);
        } else if (id == ID_PICK_LIST && HIWORD(w) == LBN_DBLCLK) {
            PlaceSelectedPickup(hwnd);
        }
        break;
    }
    case WM_CLOSE:
        ShowPickupPanel(false);
        break;
    case WM_DESTROY:
        g_pickList = nullptr;
        g_pickPlace = nullptr;
        g_pickClose = nullptr;
        g_pickRefresh = nullptr;
        g_pickLabel = nullptr;
        g_editorPanels.hPickupPanel = nullptr;
        break;
    default:
        return DefWindowProc(hwnd, msg, w, l);
    }
    return 0;
}
