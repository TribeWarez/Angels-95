// ============================================================================
// ModelPanel.cpp
//
// FRAGMENT - not a standalone translation unit. Included by ../UiShell.cpp,
// which is the single TU for the whole UI layer. That is deliberate: see
// UiShell.cpp for why the layer is built as a unity build rather than as 16
// separate objects.
// ============================================================================

// =====================================================================
// Model / Mesh Browser
// =====================================================================
static const int ID_MDL_REFRESH = 101;
static const int ID_MDL_PLACE   = 102;
static const int ID_MDL_CLOSE   = 103;
static const int ID_MDL_PREVIEW = 104;
static const int ID_MDL_IMPORT  = 105;
static const int ID_MDL_EXPORT  = 106;
static const int ID_MDL_SCOPE   = 107;   // scope tree
static const int ID_MDL_SEARCH  = 108;   // search edit box

static const UINT WM_MODEL_SELECTED = WM_USER + 100;
static const UINT WM_PREVIEW_READY  = WM_USER + 101;

void ShowModelBrowser(bool show) {
    g_editorPanels.showModelBrowser = show;
    if (g_editorPanels.hModelBrowser)
        ShowWindow((HWND)g_editorPanels.hModelBrowser, show ? SW_SHOW : SW_HIDE);
}

// Adapter over Resources/AssetScan.cpp - the enumeration itself (filesystem walk,
// package walk, sort, dedupe) is shared with the sound browser. What stays here is the
// part that belongs to this panel: it owns g_editorPanels.modelEntries, and it has to
// poke the treeview afterwards.
void ScanModelBrowserFiles() {
    g_editorPanels.modelEntries.clear();
    g_editorPanels.selectedModel = -1;

    const std::vector<AssetScopeItem> found =
        ScanAssets("", { ".obj", ".gltf", ".glb", ".iqm", ".vox", ".m3d" });
    g_editorPanels.modelEntries.reserve(found.size());
    for (const auto& it : found) {
        ModelBrowserEntry mbe;
        mbe.name = it.name;
        mbe.path = it.path;
        g_editorPanels.modelEntries.push_back(mbe);
    }

    if (g_editorPanels.hModelBrowser)
        SendMessage((HWND)g_editorPanels.hModelBrowser, WM_USER + 50, 0, 0);
}

void UpdateModelPreview(void* hBmp, int w, int h) {
    if (g_editorPanels.hPreviewBitmap)
        DeleteObject((HGDIOBJ)g_editorPanels.hPreviewBitmap);
    g_editorPanels.hPreviewBitmap = hBmp;
    g_editorPanels.previewW = w;
    g_editorPanels.previewH = h;
    if (g_editorPanels.hModelBrowser) {
        PostMessage((HWND)g_editorPanels.hModelBrowser, WM_PREVIEW_READY,
                    (WPARAM)hBmp, MAKELPARAM(w, h));
    }
}

static void LayoutModelBrowser(HWND hwnd, HWND hList, HWND hPreview, HWND hInfo,
                               HWND hRefresh, HWND hPlace, HWND hClose) {
    if (!hwnd) return;
    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right - rc.left, H = rc.bottom - rc.top;
    int top = 32, bottomPad = 34;
    int listW = (int)(W * 0.42f); if (listW < 160) listW = 160;
    int listH = H - top - bottomPad; if (listH < 80) listH = 80;
    int rx = listW + 18;
    int rw = W - rx - 8; if (rw < 120) rw = 120;
    int previewH = (H - top - bottomPad) / 2; if (previewH < 120) previewH = 120;
    if (hList)    MoveWindow(hList, 8, top, listW, listH, TRUE);
    if (hPreview) MoveWindow(hPreview, rx, top, rw, previewH, TRUE);
    if (hInfo)    MoveWindow(hInfo, rx, top + previewH + 6, rw, 56, TRUE);
    if (hRefresh) MoveWindow(hRefresh, 8, 4, 80, 22, TRUE);
    if (hPlace)   MoveWindow(hPlace, rx, top + previewH + 68, 130, 24, TRUE);
    if (hClose)   MoveWindow(hClose, W - 8 - 72, H - 8 - 24, 72, 24, TRUE);
}

// --- Model Browser scope tree -------------------------------------------
// Rebuilds the (GameData)/(Packages) tree from modelEntries, honouring the
// search box. Leaves carry their modelEntries index in lParam; folders carry -1.
static void FillModelScopeTree(HWND hTree) {
    if (!hTree) return;
    std::vector<AssetScopeItem> items;
    items.reserve(g_editorPanels.modelEntries.size());
    for (const auto& e : g_editorPanels.modelEntries)
        items.push_back({e.name, e.path, !IsPathFile(e.path.c_str())});

    char search[128] = {};
    GetWindowTextA(GetDlgItem(GetParent(hTree), ID_MDL_SEARCH), search, sizeof(search));
    AssetScopeNode tree = BuildAssetScope(items, search);

    SendMessage(hTree, WM_SETREDRAW, FALSE, 0);
    SendMessage(hTree, TVM_DELETEITEM, 0, (LPARAM)TVI_ROOT);

    std::function<void(const AssetScopeNode&, HTREEITEM, bool)> add =
        [&](const AssetScopeNode& n, HTREEITEM parent, bool isRoot) {
            for (const auto& c : n.children) {
                std::wstring wl(c.label.begin(), c.label.end());
                TVINSERTSTRUCTW ins = {};
                ins.hParent = parent;
                ins.itemex.mask = TVIF_TEXT | TVIF_PARAM;
                ins.itemex.pszText = (LPWSTR)wl.c_str();
                ins.itemex.lParam = (LPARAM)c.entryIndex;
                HTREEITEM h = (HTREEITEM)SendMessage(hTree, TVM_INSERTITEMW, 0, (LPARAM)&ins);
                if (h && c.entryIndex < 0) {
                    add(c, h, false);
                    // While filtering, reveal matches without a click.
                    if (search[0]) SendMessage(hTree, TVM_EXPAND, TVE_EXPAND, (LPARAM)h);
                }
            }
        };
    add(tree, TVI_ROOT, true);

    // With no filter, start with the two roots expanded so the tree is useful
    // immediately; folders stay collapsed so deep hierarchies don't explode.
    if (!search[0]) {
        HTREEITEM r = (HTREEITEM)SendMessage(hTree, TVM_GETNEXTITEM, TVGN_ROOT, 0);
        for (; r; r = (HTREEITEM)SendMessage(hTree, TVM_GETNEXTITEM, TVGN_NEXT, (LPARAM)r))
            SendMessage(hTree, TVM_EXPAND, TVE_EXPAND, (LPARAM)r);
    }
    SendMessage(hTree, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hTree, nullptr, TRUE);
}

// Selected leaf's modelEntries index, or -1 when a folder (or nothing) is picked.
static int SelectedModelFromTree(HWND hTree) {
    if (!hTree) return -1;
    HTREEITEM h = (HTREEITEM)SendMessage(hTree, TVM_GETNEXTITEM, TVGN_CARET, 0);
    if (!h) return -1;
    TVITEMW tvi = {};
    tvi.mask = TVIF_PARAM;
    tvi.hItem = h;
    if (!SendMessage(hTree, TVM_GETITEMW, 0, (LPARAM)&tvi)) return -1;
    if (tvi.lParam < 0) return -1;
    if (tvi.lParam >= (int)g_editorPanels.modelEntries.size()) return -1;
    return (int)tvi.lParam;
}

static void ShowModelInfoFor(HWND hInfo, int idx) {
    if (!hInfo || idx < 0 || idx >= (int)g_editorPanels.modelEntries.size()) {
        if (hInfo) SetWindowTextA(hInfo, "Select a model from the tree");
        return;
    }
    g_editorPanels.selectedModel = idx;
    const auto& e = g_editorPanels.modelEntries[idx];
    char buf[640];
    snprintf(buf, sizeof(buf), "%s   (%d verts, %d tris)\n%s",
             e.name.c_str(), e.vertices, e.triangles, e.path.c_str());
    SetWindowTextA(hInfo, buf);
}

static LRESULT CALLBACK ModelBrwProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    static HWND hList, hPreview, hInfo, hRefreshBtn, hPlaceBtn, hCloseBtn;
    int id;
    switch (msg) {
    case WM_CREATE: {
        int PW = 540, PH = 500;
        // Scope tree replaces the old flat listbox: (GameData) / (Packages)
        // roots with the real folder hierarchy underneath.
        hList = CreateWindowEx(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
             WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | TVS_HASBUTTONS |
             TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS | WS_TABSTOP,
             8, 32, 230, PH - 100, hwnd, (HMENU)(INT_PTR)ID_MDL_SCOPE,
             g_hInst, nullptr);
        hPreview = CreateWindowEx(WS_EX_STATICEDGE, L"STATIC", L"",
             WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
             248, 32, 260, 220, hwnd, (HMENU)(INT_PTR)ID_MDL_PREVIEW,
             g_hInst, nullptr);
        hInfo    = CreateLabel(hwnd, L"Select a model from the tree", 248, 260, 260, 60, 2);
        hRefreshBtn = CreateButton(hwnd, L"Refresh", 8, 4, 80, 22, ID_MDL_REFRESH);
        CreateIconButton(hwnd, L"Import", 94, 4, 74, 22, ID_MDL_IMPORT, "BBGeneric");
        CreateIconButton(hwnd, L"Export", 172, 4, 74, 22, ID_MDL_EXPORT, "BBSheet");
        CreateLabel(hwnd, L"Search", 252, 4, 44, 22, 2);
        CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"",
             WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
             294, 4, 150, 22, hwnd, (HMENU)(INT_PTR)ID_MDL_SEARCH, g_hInst, nullptr);
        hPlaceBtn   = CreateButton(hwnd, L"Place in World", 248, 340, 120, 24, ID_MDL_PLACE);
        hCloseBtn   = CreateButton(hwnd, L"Close", PW - 72, PH - 28, 64, 22, ID_MDL_CLOSE);
        LayoutModelBrowser(hwnd, hList, hPreview, hInfo, hRefreshBtn, hPlaceBtn, hCloseBtn);
        break;
    }
    case WM_SIZE:
        LayoutModelBrowser(hwnd, hList, hPreview, hInfo, hRefreshBtn, hPlaceBtn, hCloseBtn);
        break;
    case WM_USER + 50: {
        FillModelScopeTree(hList);
        SetWindowTextA(hInfo, "Select a model from the tree");
        break;
    }
    case WM_PREVIEW_READY: {
        HBITMAP hBmp = (HBITMAP)w;
        if (hBmp) {
            SendMessage(hPreview, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)hBmp);
            InvalidateRect(hPreview, nullptr, TRUE);
            UpdateWindow(hPreview);
        }
        break;
    }
    case WM_COMMAND: {
        id = LOWORD(w);
        if (id == ID_MDL_CLOSE) {
            ShowModelBrowser(false);
        } else if (id == ID_MDL_REFRESH) {
            // A DIRTY FLAG, not a user action: the preview must re-read the
            // list. Renamed off the action* prefix so it does not read as a
            // message - see actionAnimRefresh for the same distinction.
            g_editorPanels.refreshModelBrowser = true;
        } else if (id == ID_MDL_SEARCH && HIWORD(w) == EN_CHANGE) {
            // Rebuild the tree from the current search text. modelEntries is
            // already populated, so this does not touch the filesystem.
            FillModelScopeTree(hList);
            SetWindowTextA(hInfo, "Select a model from the tree");
        } else if (id == ID_MDL_PLACE) {
            int sel = SelectedModelFromTree(hList);
            if (sel >= 0) {
                g_editorPanels.selectedModel = sel;
                ed::PlacementRequest pr;
                pr.kind = ed::PlacementRequest::Kind::Model;
                if (sel < (int)g_editorPanels.modelEntries.size())
                    pr.key = g_editorPanels.modelEntries[sel].path;
                ed::EventBus::instance().post(ed::Ev::BeginPlacement, pr);
            }
        } else if (id == ID_MDL_IMPORT) {
            // Import an external model (and its companion texture) by packing it
            // straight into System/Data/imported_models.ozpak (OZPK).
            wchar_t path[MAX_PATH] = L"";
            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFile = path;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrFilter = L"Meshes (*.obj;*.glb;*.gltf;*.iqm;*.vox;*.m3d)\0*.obj;*.glb;*.gltf;*.iqm;*.vox;*.m3d\0All Files (*.*)\0*.*\0\0";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&ofn)) {
                fs::path src(path);
                std::vector<std::pair<std::string, std::vector<uint8_t>>> add;
                std::vector<uint8_t> data;
                if (ReadWholeFile(src, data) && !data.empty())
                    add.push_back({"Models/" + src.filename().string(), std::move(data)});
                if (add.empty()) {
                    MessageBoxA(hwnd, "Could not read the selected file.",
                                "Import Mesh", MB_OK | MB_ICONERROR);
                    break;
                }
                // Companion texture, if one sits next to the mesh.
                std::string stem = src.stem().string();
                for (const char* suf : {"_texture.png", ".png", "_texture.tga", ".tga",
                                        "_texture.bmp", ".bmp"}) {
                    fs::path tp = src.parent_path() / (stem + suf);
                    if (fs::exists(tp) && ReadWholeFile(tp, data) && !data.empty()) {
                        add.push_back({"Models/" + tp.filename().string(), data});
                        break;
                    }
                }

                fs::path pkg = fs::current_path() / "System" / "Data" / "imported_models.ozpak";
                std::string err;
                if (!PackIntoPackage(pkg, OZ_PACKAGE_MAGIC_PK, add, err) ||
                    !HotLoadPackage(pkg, err)) {
                    MessageBoxA(hwnd, err.c_str(), "Import Mesh", MB_OK | MB_ICONERROR);
                    break;
                }
                ScanModelBrowserFiles();
                fprintf(stdout, "Packed mesh '%s' -> %s\n",
                        src.filename().string().c_str(), pkg.string().c_str());
            }
        } else if (id == ID_MDL_EXPORT) {

            int sel = SelectedModelFromTree(hList);
            if (sel < 0) {
                MessageBoxA(hwnd, "Select a model first.", "Export Mesh", MB_OK | MB_ICONINFORMATION);
            } else {
                const auto& e = g_editorPanels.modelEntries[sel];
                std::string ext = ".obj";
                size_t dot = e.path.rfind('.');
                if (dot != std::string::npos) ext = e.path.substr(dot);
                std::string defName = e.name + ext;
                wchar_t path[MAX_PATH] = L"";
                std::wstring wdef(defName.begin(), defName.end());
                wcsncpy(path, wdef.c_str(), MAX_PATH - 1);
                OPENFILENAMEW sfn = {};
                sfn.lStructSize = sizeof(sfn);
                sfn.hwndOwner = hwnd;
                sfn.lpstrFile = path;
                sfn.nMaxFile = MAX_PATH;
                sfn.lpstrFilter = L"Meshes (*.obj;*.glb;*.gltf;*.iqm;*.vox;*.m3d)\0*.obj;*.glb;*.gltf;*.iqm;*.vox;*.m3d\0All Files (*.*)\0*.*\0\0";
                sfn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
                if (GetSaveFileNameW(&sfn)) {
                    std::error_code ec;
                    fs::path dest(path);
                    if (IsPathFile(e.path.c_str())) {
                        fs::copy_file(e.path, dest, fs::copy_options::overwrite_existing, ec);
                    } else {
                        size_t sz = 0;
                        const uint8_t* data =
                            PackageAssetLoader::Instance().ResolvePackageKey(e.path.c_str(), sz);
                        if (data && sz > 0) {
                            std::ofstream out(dest, std::ios::binary);
                            if (out.is_open()) out.write((const char*)data, (std::streamsize)sz);
                        }
                    }
                    fprintf(stdout, "Exported mesh '%s' -> %s\n", e.name.c_str(), dest.string().c_str());
                }
            }
        }
        break;
    }
    case WM_NOTIFY: {
        // Treeview selection arrives as WM_NOTIFY, not WM_COMMAND.
        NMHDR* nh = (NMHDR*)l;
        if (nh && nh->idFrom == ID_MDL_SCOPE &&
            nh->code == TVN_SELCHANGEDW) {
            ShowModelInfoFor(hInfo, SelectedModelFromTree(hList));
            return 0;
        }
        break;
    }
    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)l;
        if (dis->CtlID == ID_MDL_PREVIEW && g_editorPanels.hPreviewBitmap) {
            HDC hdc = dis->hDC;
            RECT r = dis->rcItem;
            BITMAP bm;
            GetObject((HGDIOBJ)g_editorPanels.hPreviewBitmap, sizeof(bm), &bm);
            HDC hdcMem = CreateCompatibleDC(hdc);
            SelectObject(hdcMem, (HGDIOBJ)g_editorPanels.hPreviewBitmap);
            StretchBlt(hdc, r.left, r.top, r.right - r.left, r.bottom - r.top,
                       hdcMem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
            DeleteDC(hdcMem);
            return TRUE;
        }
        if (dis->CtlType == ODT_BUTTON) return DrawIconButton(dis);
        return DefWindowProc(hwnd, msg, w, l);
    }
    case WM_CLOSE:
        ShowModelBrowser(false);
        break;
    case WM_DESTROY:
        g_editorPanels.hModelBrowser = nullptr;
        if (g_editorPanels.hPreviewBitmap) {
            DeleteObject((HGDIOBJ)g_editorPanels.hPreviewBitmap);
            g_editorPanels.hPreviewBitmap = nullptr;
        }
        break;
    default:
        return DefWindowProc(hwnd, msg, w, l);
    }
    return 0;
}

