// ============================================================================
// TexturePanel.cpp
//
// FRAGMENT - not a standalone translation unit. Included by ../UiShell.cpp,
// which is the single TU for the whole UI layer. That is deliberate: see
// UiShell.cpp for why the layer is built as a unity build rather than as 16
// separate objects.
// ============================================================================

// =====================================================================
// Texture Manager v2 ï¿½ï¿½ï¿½ oztex integration, preview, source info
// =====================================================================
static const int ID_TEX_LIST       = 101;
static const int ID_TEX_REFRESH    = 102;
static const int ID_TEX_CLOSE      = 103;
static const int ID_TEX_APPLY      = 105;
static const int ID_TEX_PREVIEW    = 106;
static const int ID_TEX_DIMS_LABEL = 107;
static const int ID_TEX_SRC_LABEL  = 108;
static const int ID_TEX_ADDPKG     = 110;
static const int ID_TEX_IMPORT     = 111;
static const int ID_TEX_SCOPE      = 112;   // scope tree
static const int ID_TEX_SEARCH     = 113;   // search edit box

// =====================================================================
// Asset scoping moved to Resources/AssetScope.{hpp,cpp} in R6.
//
// BuildAssetScope used to live here, yet ModelPanel.cpp calls it too - it only worked
// because the whole UI layer is one translation unit. It is now its own real object
// and is unit-tested headlessly in tests/AssetScope.test.cpp.
// =====================================================================
#ifdef _WIN32
void CollectScopeLeavesUnder(HWND tree, void* node, std::vector<int>& out) {
    if (!tree) return;
    std::vector<HTREEITEM> stack;
    if (node == TVI_ROOT) {
        for (HTREEITEM it = (HTREEITEM)SendMessage(tree, TVM_GETNEXTITEM, TVGN_ROOT, 0);
             it; it = (HTREEITEM)SendMessage(tree, TVM_GETNEXTITEM, TVGN_NEXT, (LPARAM)it))
            stack.push_back(it);
    } else {
        stack.push_back((HTREEITEM)node);
    }
    while (!stack.empty()) {
        HTREEITEM it = stack.back();
        stack.pop_back();
        TVITEMW tvi = {};
        tvi.mask = TVIF_PARAM;
        tvi.hItem = it;
        if (SendMessage(tree, TVM_GETITEMW, 0, (LPARAM)&tvi) && tvi.lParam >= 0) {
            out.push_back((int)tvi.lParam);
            continue;   // a leaf has no children
        }
        // Folder: push children in reverse so they pop in document order.
        std::vector<HTREEITEM> kids;
        for (HTREEITEM k = (HTREEITEM)SendMessage(tree, TVM_GETNEXTITEM, TVGN_CHILD, (LPARAM)it);
             k; k = (HTREEITEM)SendMessage(tree, TVM_GETNEXTITEM, TVGN_NEXT, (LPARAM)k))
            kids.push_back(k);
        for (auto it2 = kids.rbegin(); it2 != kids.rend(); ++it2)
            stack.push_back(*it2);
    }
}
#endif


// Texture grid view constants
static const int TEX_THUMB_SIZE = 64;
static const int TEX_CELL_W     = TEX_THUMB_SIZE + 12;
static const int TEX_CELL_H     = TEX_THUMB_SIZE + 22;
static const int TEX_GRID_GAP   = 4;

void ShowTextureManager(bool show) {
    g_editorPanels.showTextureMgr = show;
    if (g_editorPanels.hTextureMgr)
        ShowWindow((HWND)g_editorPanels.hTextureMgr, show ? SW_SHOW : SW_HIDE);
}

// Build one 64x64 thumbnail HBITMAP on demand. Returns nullptr when the image cannot
// be loaded, in which case the caller simply draws no preview.
//
// LAZY ON PURPOSE. This used to run once per texture in GameData on every rescan,
// building a DIB section for each whether or not it was ever displayed. On a full tree
// that is thousands of GDI bitmaps and a visible memory spike at panel-open time. Now
// only the cells that actually paint pay for one.
static HBITMAP BuildTextureThumbnail(HWND hwnd, const ResourceEntry& tex) {
    Image img = LoadImageWithFallback(tex.path.c_str());
    if (!img.data) {
        // Silent before: a cell with no art and no log is indistinguishable from
        // "this texture does not exist". Name the path so a bad import/format is
        // diagnosable. (The UI layer cannot see EditorLog - it lives in the Main
        // TU - so this goes to stderr, which the editor's console shows.)
        fprintf(stderr, "[TextureManager] thumbnail failed to load '%s'\n", tex.path.c_str());
        return nullptr;
    }

    ImageResize(&img, TEX_THUMB_SIZE, TEX_THUMB_SIZE);
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    unsigned char* px = (unsigned char*)img.data;
    for (int i = 0; i < img.width * img.height; i++) {
        unsigned char tmp = px[i*4];
        px[i*4] = px[i*4+2];
        px[i*4+2] = tmp;
    }
    HDC hdc = GetDC(hwnd);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = img.width;
    bmi.bmiHeader.biHeight = -img.height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (bits && img.data) memcpy(bits, img.data, img.width * img.height * 4);
    ReleaseDC(hwnd, hdc);
    UnloadImage(img);
    return bits ? bmp : nullptr;
}

void ScanTextureBrowserFiles() {
    // Free the previous thumbnails BEFORE clearing the vector. g_textureFiles owns an
    // HBITMAP per entry and this used to be a bare clear(), so every rescan leaked one
    // GDI bitmap per texture - and a rescan runs on every window activation.
    for (auto& tex : g_textureFiles) {
        if (tex.thumbnail) { DeleteObject(tex.thumbnail); tex.thumbnail = nullptr; }
    }
    g_textureFiles.clear();
    // Every shipped texture is now PNG (tools/convert_dds.py converted the .dds
    // library), so the sky panoramas in Tex/Global/Sky are selectable from the same
    // list as everything else.
    const std::vector<std::string> exts = { ".png", ".tga", ".bmp", ".jpg", ".jpeg" };

    // Enumeration belongs to Resources/AssetScan - it is the same walk the sound browser
    // and the Model Browser use, and this file was carrying a third copy of it.
    // ScanAssets also sorts by name and resolves same-named duplicates in favour of the
    // real file (a package copy like "Models/Skybox.png" would otherwise shadow the
    // world's actual file and be unusable as an editable source). Both used to be
    // re-derived here, the dedup from !IsPathFile().
    const std::vector<AssetScopeItem> found = ScanAssets("", exts);
    g_textureFiles.reserve(found.size());
    for (const auto& it : found) {
        ResourceEntry e;
        e.name        = it.name;
        e.path        = it.path;
        e.fromPackage = it.fromPackage;
        g_textureFiles.push_back(std::move(e));
    }

    // Entry indices changed underneath any scope selection, so drop it; the
    // tree is rebuilt below and re-selects the scope by path.
    g_textureVisible.clear();

    if (g_editorPanels.hTextureMgr)
        SendMessage((HWND)g_editorPanels.hTextureMgr, WM_USER + 50, 0, 0);
}

void UpdateTextureManagerList() {
    ScanTextureBrowserFiles();
}

// ---------------------------------------------------------------------
// Import helpers â€” every import is PACKED immediately (Phase F).
//
// Imports used to drop loose files into GameData/, which meant the asset only
// became visible after a rescan and shipped builds (which run from packed
// System/Data/*.oz*) could not see it at all. Now the bytes go straight into a
// .oz* package in System/Data/ and the package is hot-loaded, so the new entry
// shows up under the (Packages) scope immediately and a rebuild needs no extra
// step.
//
// The package is APPENDED to, not overwritten: existing entries are read back
// and rewritten so repeated imports accumulate.
// ---------------------------------------------------------------------
static bool ReadWholeFile(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) return false;
    f.seekg(0, std::ios::end);
    std::streamoff n = f.tellg();
    if (n < 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize((size_t)n);
    if (n > 0) f.read((char*)out.data(), n);
    return (bool)f || n == 0;
}
// Pack / hot-load moved to Resources/PackageIO.{hpp,cpp} in R6. Both import paths
// (texture here, model in ModelPanel.cpp) share it; it used to be `static` in this
// file and reachable from ModelPanel only because the UI layer is one translation
// unit.

// ---------------------------------------------------------------------
// Import Textures â€” pack image file(s) straight into System/Data.
//
// These are FREELY PLACEABLE assets (Phase G): they can be assigned with `tex=`
// on Mesh.Static / Mesh.Skeletal entities and as a model's texture, but they are
// NOT tileset entries, so they cannot be used as a brush `texSlot`. The user is
// told this loudly, because silently getting texSlot 0/auto-selection back is a
// confusing authoring bug.
// ---------------------------------------------------------------------
static const char* kTexSlotWarning =
    "NOTE: packed textures are FREE-PLACEMENT assets, not tileset entries.\n"
    "\n"
    "Use them with  tex=<path>  on Mesh.Static / Mesh.Skeletal entities, or as\n"
    "a model's texture. They CANNOT be used as a brush texSlot: that argument is\n"
    "a BARE POSITIONAL float selecting a tileset slot (1-based, ordered by\n"
    "filename in <world>/oztex/tileset/), and only textures in that folder count.\n"
    "\n"
    "To make this texture a tileset slot instead, add the file to the world's\n"
    "oztex/tileset/ folder on disk and reopen the world.";

static void ImportTexturesIntoWorld(HWND hwnd) {
    std::vector<wchar_t> buf(32768, 0);
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = (DWORD)buf.size();
    ofn.lpstrFilter = L"Images (*.png;*.jpg;*.jpeg;*.bmp;*.tga)\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0All Files (*.*)\0*.*\0";
    ofn.lpstrTitle = L"Import Textures";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                OFN_ALLOWMULTISELECT | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return;

    // Multi-select buffer layout: "dir\0file1\0file2\0\0", or a single
    // selection which is a direct "fullpath\0".
    std::vector<std::wstring> files;
    std::wstring first(buf.data());
    if (first.empty()) return;
    const wchar_t* p = buf.data() + first.size() + 1;
    if (*p == L'\0') {
        files.push_back(first);
    } else {
        fs::path dir(first);
        while (*p) {
            std::wstring name(p);
            files.push_back((dir / name).wstring());
            p += name.size() + 1;
        }
    }
    if (files.empty()) return;

    // Pack into System/Data/imported_textures.oztex (OZTX). Entry keys are
    // "Textures/<file>" â€” PackageAssetLoader strips the first segment when
    // resolving a bare name, matching build-data.ps1's per-subdirectory packs.
    fs::path pkg = fs::current_path() / "System" / "Data" / "imported_textures.oztex";
    std::vector<std::pair<std::string, std::vector<uint8_t>>> add;
    std::vector<std::string> names;
    int failed = 0;
    for (const auto& f : files) {
        fs::path src(f);
        std::vector<uint8_t> data;
        if (!ReadWholeFile(src, data) || data.empty()) { failed++; continue; }
        names.push_back(src.filename().string());
        add.push_back({"Textures/" + src.filename().string(), std::move(data)});
    }
    if (add.empty()) {
        MessageBoxA(hwnd, "None of the selected files could be read.",
                    "Import Textures", MB_OK | MB_ICONERROR);
        return;
    }

    std::string err;
    if (!PackIntoPackage(pkg, OZ_PACKAGE_MAGIC_TX, add, err)) {
        MessageBoxA(hwnd, err.c_str(), "Import Textures", MB_OK | MB_ICONERROR);
        return;
    }
    if (!HotLoadPackage(pkg, err)) {
        MessageBoxA(hwnd, err.c_str(), "Import Textures", MB_OK | MB_ICONERROR);
        return;
    }

    ScanTextureBrowserFiles();

    std::string msg = "Packed " + std::to_string(add.size()) + " texture(s) into:\n" +
                      pkg.string() + "\n\nAvailable as:\n  tex=Textures/" +
                      (names.empty() ? std::string() : names[0]);
    if (names.size() > 1) msg += "\n  ... and " + std::to_string(names.size() - 1) + " more";
    if (failed) msg += "\n\n" + std::to_string(failed) + " file(s) failed to read.";
    msg += "\n\n";
    msg += kTexSlotWarning;
    MessageBoxA(hwnd, msg.c_str(), "Import Textures (packed)", MB_OK | MB_ICONINFORMATION);
}

// =====================================================================
// Texture Grid View â€” custom control drawing thumbnails in a responsive grid
// =====================================================================
struct TextureGridState {
    int selectedIdx = -1;   // grid position, NOT a g_textureFiles index
    int columns = 1;
    int totalHeight = 0;
};

static LRESULT CALLBACK TextureGridProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    auto* state = (TextureGridState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        state = new TextureGridState();
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)state);
        break;
    }
    case WM_NCDESTROY: {
        delete state;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
        break;
    }
    case WM_SIZE: {
        if (!state) break;
        int cw = LOWORD(l);
        int ch = HIWORD(l);
        if (cw < 1) cw = 1;
        if (ch < 1) ch = 1;
        state->columns = (cw - 8) / (TEX_CELL_W + TEX_GRID_GAP);
        if (state->columns < 1) state->columns = 1;
        int rows = (TexVisibleCount() + state->columns - 1) / state->columns;
        state->totalHeight = rows * (TEX_CELL_H + TEX_GRID_GAP) + 8;
        SCROLLINFO si = { sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE, 0, state->totalHeight, ch, 0 };
        SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
        InvalidateRect(hwnd, NULL, TRUE);
        break;
    }
    case WM_PAINT: {
        if (!state) break;
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        int scrollPos = GetScrollPos(hwnd, SB_VERT);
        HDC hdcMem = CreateCompatibleDC(hdc);

        FillRect(hdc, &ps.rcPaint, GetSysColorBrush(COLOR_WINDOW));

        const int visCount = TexVisibleCount();
        for (int i = 0; i < visCount; i++) {
            const int fi = TexEntryAt(i);
            if (fi < 0) continue;
            int col = (i % state->columns);
            int row = (i / state->columns);
            int x = 4 + col * (TEX_CELL_W + TEX_GRID_GAP);
            int y = 4 + row * (TEX_CELL_H + TEX_GRID_GAP) - scrollPos;

            if (y + TEX_CELL_H < 0 || y > rc.bottom) continue;

            // Selection highlight
            if (i == state->selectedIdx) {
                RECT sel = { x - 2, y - 2, x + TEX_CELL_W + 2, y + TEX_CELL_H + 2 };
                HBRUSH hBrush = CreateSolidBrush(RGB(60, 80, 120));
                FillRect(hdc, &sel, hBrush);
                DeleteObject(hBrush);
            }

            // Thumbnail, built on first paint. Off-screen cells are `continue`d
            // above, so scrolling never pays for what it cannot show.
            if (!g_textureFiles[fi].thumbnail)
                g_textureFiles[fi].thumbnail = BuildTextureThumbnail(hwnd, g_textureFiles[fi]);
            if (g_textureFiles[fi].thumbnail) {
                SelectObject(hdcMem, g_textureFiles[fi].thumbnail);
                StretchBlt(hdc, x + (TEX_CELL_W - TEX_THUMB_SIZE) / 2, y + 2,
                           TEX_THUMB_SIZE, TEX_THUMB_SIZE, hdcMem, 0, 0, TEX_THUMB_SIZE, TEX_THUMB_SIZE, SRCCOPY);
            }

            // Name below thumbnail
            RECT tr = { x, y + TEX_THUMB_SIZE + 4, x + TEX_CELL_W, y + TEX_CELL_H };
            SetTextColor(hdc, RGB(200, 200, 200));
            SetBkMode(hdc, TRANSPARENT);
            DrawTextA(hdc, g_textureFiles[fi].name.c_str(), -1, &tr, DT_CENTER | DT_SINGLELINE | DT_WORD_ELLIPSIS);
        }
        DeleteDC(hdcMem);
        EndPaint(hwnd, &ps);
        break;
    }
    case WM_LBUTTONDOWN: {
        if (!state) break;
        int mx = LOWORD(l), my = HIWORD(l);
        int scrollPos = GetScrollPos(hwnd, SB_VERT);
        int col = (mx - 4) / (TEX_CELL_W + TEX_GRID_GAP);
        int row = (my + scrollPos - 4) / (TEX_CELL_H + TEX_GRID_GAP);
        int idx = row * state->columns + col;
        if (idx >= 0 && idx < TexVisibleCount() && col < state->columns && mx >= 4) {
            state->selectedIdx = idx;
            InvalidateRect(hwnd, NULL, TRUE);
            PostMessage(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(ID_TEX_LIST, 1), 0);
        }
        break;
    }
    case WM_LBUTTONDBLCLK: {
        if (!state) break;
        int mx = LOWORD(l), my = HIWORD(l);
        int scrollPos = GetScrollPos(hwnd, SB_VERT);
        int col = (mx - 4) / (TEX_CELL_W + TEX_GRID_GAP);
        int row = (my + scrollPos - 4) / (TEX_CELL_H + TEX_GRID_GAP);
        int idx = row * state->columns + col;
        if (idx >= 0 && idx < TexVisibleCount() && col < state->columns && mx >= 4) {
            state->selectedIdx = idx;
            InvalidateRect(hwnd, NULL, TRUE);
            PostMessage(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(ID_TEX_LIST, 2), 0);
        }
        break;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(w);
        int scrollPos = GetScrollPos(hwnd, SB_VERT);
        scrollPos -= delta / 120 * (TEX_CELL_H + TEX_GRID_GAP);
        if (scrollPos < 0) scrollPos = 0;
        SCROLLINFO si = { sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE };
        GetScrollInfo(hwnd, SB_VERT, &si);
        int maxPos = si.nMax - (int)si.nPage;
        if (maxPos < 0) maxPos = 0;
        if (scrollPos > maxPos) scrollPos = maxPos;
        SetScrollPos(hwnd, SB_VERT, scrollPos, TRUE);
        InvalidateRect(hwnd, NULL, TRUE);
        break;
    }
    case WM_VSCROLL: {
        SCROLLINFO si = { sizeof(SCROLLINFO), SIF_ALL };
        GetScrollInfo(hwnd, SB_VERT, &si);
        int pos = si.nPos;
        switch (LOWORD(w)) {
        case SB_LINEUP:        pos -= TEX_CELL_H / 4; break;
        case SB_LINEDOWN:      pos += TEX_CELL_H / 4; break;
        case SB_PAGEUP:        pos -= (int)si.nPage; break;
        case SB_PAGEDOWN:      pos += (int)si.nPage; break;
        case SB_THUMBTRACK:    pos = si.nTrackPos; break;
        }
        if (pos < 0) pos = 0;
        int maxPos = si.nMax - (int)si.nPage;
        if (maxPos < 0) maxPos = 0;
        if (pos > maxPos) pos = maxPos;
        si.nPos = pos;
        SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
        InvalidateRect(hwnd, NULL, TRUE);
        break;
    }
    case WM_GETDLGCODE: {
        // Let the grid receive arrow keys if needed
        return DLGC_WANTARROWS;
    }
    default:
        return DefWindowProc(hwnd, msg, w, l);
    }
    return 0;
}

// =====================================================================
// Texture Manager v3 â€” Grid-based texture browser, dynamic resize, package-only
// =====================================================================
// Rebuilds the (GameData)/(Packages) scope tree from g_textureFiles, honouring
// the search box, then republishes the grid's visible set. Mirrors
// FillModelScopeTree in the Model Browser.
// Republish the grid's visible set from the tree's current selection and
// refresh it. Split out from FillTextureScopeTree so a selection change can
// refresh the grid WITHOUT rebuilding the tree â€” rebuilding from inside the
// tree's own TVN_SELCHANGED notification would destroy the very selection that
// triggered it.
static void UpdateTextureScopeSelection(HWND hwnd, HWND hTree, HWND hGrid) {
    // Grid shows the selected scope's leaves; with no selection it shows all
    // (g_textureVisible empty => identity mapping), which is the pre-tree
    // behaviour.
    g_textureVisible.clear();
    HTREEITEM sel = hTree ? (HTREEITEM)SendMessage(hTree, TVM_GETNEXTITEM, TVGN_CARET, 0) : nullptr;
    if (sel) CollectScopeLeavesUnder(hTree, sel, g_textureVisible);

    if (hGrid) {
        RECT rc;
        GetClientRect(hGrid, &rc);
        auto* gs = (TextureGridState*)GetWindowLongPtr(hGrid, GWLP_USERDATA);
        if (gs) gs->selectedIdx = -1;   // grid positions just changed
        SendMessage(hGrid, WM_SIZE, 0, MAKELPARAM(rc.right - rc.left, rc.bottom - rc.top));
        InvalidateRect(hGrid, NULL, TRUE);
    }
    if (hwnd) {
        HWND hSrc = GetDlgItem(hwnd, ID_TEX_SRC_LABEL);
        if (hSrc) {
            if (g_textureVisible.empty())
                SetWindowTextA(hSrc, "Source: all textures (no scope selected)");
            else if (g_textureVisible.size() == 1)
                SetWindowTextA(hSrc, "Source: 1 texture in scope");
            else
                SetWindowTextA(hSrc, TextFormat("Source: %d textures in scope",
                                                (int)g_textureVisible.size()));
        }
    }
}

static void FillTextureScopeTree(HWND hwnd, HWND hTree, HWND hGrid) {
    if (!hTree) return;
    std::vector<AssetScopeItem> items;
    items.reserve(g_textureFiles.size());
    for (const auto& e : g_textureFiles)
        items.push_back({e.name, e.path, e.fromPackage});

    char search[128] = {};
    GetWindowTextA(GetDlgItem(hwnd, ID_TEX_SEARCH), search, sizeof(search));
    AssetScopeNode tree = BuildAssetScope(items, search);

    SendMessage(hTree, WM_SETREDRAW, FALSE, 0);
    SendMessage(hTree, TVM_DELETEITEM, 0, (LPARAM)TVI_ROOT);

    std::function<void(const AssetScopeNode&, HTREEITEM)> add =
        [&](const AssetScopeNode& n, HTREEITEM parent) {
            for (const auto& c : n.children) {
                std::wstring wl(c.label.begin(), c.label.end());
                TVINSERTSTRUCTW ins = {};
                ins.hParent = parent;
                ins.itemex.mask = TVIF_TEXT | TVIF_PARAM;
                ins.itemex.pszText = (LPWSTR)wl.c_str();
                ins.itemex.lParam = (LPARAM)c.entryIndex;
                HTREEITEM h = (HTREEITEM)SendMessage(hTree, TVM_INSERTITEMW, 0, (LPARAM)&ins);
                if (h && c.entryIndex < 0) {
                    add(c, h);
                    if (search[0]) SendMessage(hTree, TVM_EXPAND, TVE_EXPAND, (LPARAM)h);
                }
            }
        };
    add(tree, TVI_ROOT);

    if (!search[0]) {
        HTREEITEM r = (HTREEITEM)SendMessage(hTree, TVM_GETNEXTITEM, TVGN_ROOT, 0);
        for (; r; r = (HTREEITEM)SendMessage(hTree, TVM_GETNEXTITEM, TVGN_NEXT, (LPARAM)r))
            SendMessage(hTree, TVM_EXPAND, TVE_EXPAND, (LPARAM)r);
    }
    SendMessage(hTree, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hTree, nullptr, TRUE);

    UpdateTextureScopeSelection(hwnd, hTree, hGrid);
}

static LRESULT CALLBACK TextureMgrProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    static HWND hGrid = nullptr, hPreview = nullptr, hDims = nullptr, hSrc = nullptr;
    static HWND hScope = nullptr;
    switch (msg) {
    case WM_CREATE: {
        int x = 10, y = 10, bw = 500;

        // Toolbar: Add Package + Import Textures + Refresh + Close
        CreateButton(hwnd, L"Add Package", x, y, 100, 24, ID_TEX_ADDPKG);
        CreateButton(hwnd, L"Import Textures", x + 106, y, 116, 24, ID_TEX_IMPORT);
        CreateButton(hwnd, L"Refresh", x + 228, y, 70, 24, ID_TEX_REFRESH);
        CreateButton(hwnd, L"Close", x + bw - 80, y, 80, 24, ID_TEX_CLOSE);
        // Search box shares the toolbar row (right of the buttons).
        CreateLabel(hwnd, L"Search", x + 304, y + 2, 44, 20, 2);
        CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            x + 348, y, 150, 24, hwnd, (HMENU)(INT_PTR)ID_TEX_SEARCH, g_hInst, nullptr);
        y += 30;

        // Scope tree (left) + thumbnail grid (right)
        hScope = CreateWindowEx(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | TVS_HASBUTTONS |
            TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS | WS_TABSTOP,
            x, y, 150, 200, hwnd, (HMENU)(INT_PTR)ID_TEX_SCOPE, g_hInst, nullptr);
        hGrid = CreateWindowEx(WS_EX_CLIENTEDGE, CLASS_TEXTURE_GRID, L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL,
            x + 156, y, bw - 156, 200, hwnd, (HMENU)(INT_PTR)ID_TEX_LIST, g_hInst, nullptr);
        y += 206;

        // Source + dimensions info
        hSrc = CreateLabel(hwnd, L"Source: packages", x, y, bw, 16, ID_TEX_SRC_LABEL);
        y += 18;
        hDims = CreateLabel(hwnd, L"Select a texture to preview", x, y, bw, 16, ID_TEX_DIMS_LABEL);
        y += 22;

        // Preview area (static bitmap control)
        hPreview = CreateWindowEx(WS_EX_STATICEDGE, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_BITMAP,
            x + 140, y, 220, 120, hwnd, (HMENU)(INT_PTR)ID_TEX_PREVIEW, g_hInst, nullptr);
        y += 126;

        // Apply target is the viewport selection. The old "Target: Model 1..20"
        // combo fed the legacy WDLModels path, which could never resolve a CSG
        // brush - clicking Apply silently did nothing for every brush.
        CreateLabel(hwnd, L"Applies to the selected brush or model", x, y, 320, 20, 20);
        y += 30;

        CreateButton(hwnd, L"Apply", x, y, 80, 26, ID_TEX_APPLY);

        ScanTextureBrowserFiles();
        break;
    }
    case WM_SIZE: {
        int winW = LOWORD(l), winH = HIWORD(l);
        int x = 10;
        int bw = winW - 20;
        if (bw < 100) bw = 100;

        // Toolbar
        SetWindowPos(GetDlgItem(hwnd, ID_TEX_ADDPKG), NULL, x, 10, 100, 24, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hwnd, ID_TEX_IMPORT), NULL, x + 106, 10, 116, 24, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hwnd, ID_TEX_REFRESH), NULL, x + 228, 10, 70, 24, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hwnd, ID_TEX_CLOSE), NULL, x + bw - 80, 10, 80, 24, SWP_NOZORDER);

        // Grid â€” fill most of the window, to the right of the scope tree
        int gridBot = winH - 280;
        if (gridBot < 60) gridBot = 60;
        int gridH = gridBot - 40;
        if (gridH < 20) gridH = 20;
        int treeW = 150;
        if (bw - treeW - 156 < 80) treeW = 90;   // keep the grid usable when narrow
        if (hScope) {
            SetWindowPos(hScope, NULL, x, 40, treeW, gridH, SWP_NOZORDER);
        }
        if (hGrid) {
            SetWindowPos(hGrid, NULL, x + treeW + 6, 40, bw - treeW - 6, gridH, SWP_NOZORDER);
            SendMessage(hGrid, WM_SIZE, 0, MAKELPARAM(bw - treeW - 6, gridH));
        }

        // Info labels
        int iy = gridBot + 6;
        if (hSrc) SetWindowPos(hSrc, NULL, x, iy, bw, 16, SWP_NOZORDER);
        iy += 18;
        if (hDims) SetWindowPos(hDims, NULL, x, iy, bw, 16, SWP_NOZORDER);
        iy += 22;

        // Preview centered
        int prevW = bw - 40;
        if (prevW > 220) prevW = 220;
        if (prevW < 80) prevW = 80;
        int prevH = 120;
        int prevX = x + (bw - prevW) / 2;
        if (hPreview) SetWindowPos(hPreview, NULL, prevX, iy, prevW, prevH, SWP_NOZORDER);
        iy += prevH + 6;

        // Apply hint + button
        SetWindowPos(GetDlgItem(hwnd, 20), NULL, x, iy, 320, 20, SWP_NOZORDER);
        iy += 26;
        SetWindowPos(GetDlgItem(hwnd, ID_TEX_APPLY), NULL, x, iy, 80, 26, SWP_NOZORDER);
        break;
    }
    case WM_USER + 50: {
        // Free old thumbnails. Loading is NOT done here - TextureGridProc builds each
        // one the first time its cell is actually painted (BuildTextureThumbnail).
        // Building them all up front was the 38 MB spike.
        for (auto& tex : g_textureFiles) {
            if (tex.thumbnail) { DeleteObject(tex.thumbnail); tex.thumbnail = nullptr; }
        }
        // Refresh grid
        FillTextureScopeTree(hwnd, hScope, hGrid);
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(w);
        int notify = HIWORD(w);
        if (id == ID_TEX_CLOSE) {
            ShowTextureManager(false);
        } else if (id == ID_TEX_REFRESH) {
            ScanTextureBrowserFiles();
        } else if (id == ID_TEX_ADDPKG) {
            // File dialog to add a texture package
            wchar_t path[512] = L"";
            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFile = path;
            ofn.nMaxFile = 512;
            ofn.lpstrFilter = L"Texture Packages (*.oztex;*.ozpak)\0*.oztex;*.ozpak\0Oz Packages (*.oz*)\0*.oz*\0";
            ofn.lpstrInitialDir = L"System\\Data";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                char utf8Path[512];
                WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8Path, 512, nullptr, nullptr);
                if (PackageAssetLoader::Instance().LoadPackageFile(utf8Path)) {
                    ScanTextureBrowserFiles();
                } else {
                    MessageBoxA(hwnd, "Failed to load package file.\nThe file may be corrupt or not a valid OzPackage.", "Error", MB_OK | MB_ICONERROR);
                }
            }
        } else if (id == ID_TEX_IMPORT) {
            ImportTexturesIntoWorld(hwnd);
        } else if (id == ID_TEX_SEARCH && notify == EN_CHANGE) {
            // Tree is rebuilt from the existing g_textureFiles (already in
            // memory), so typing does not hit the filesystem.
            FillTextureScopeTree(hwnd, hScope, hGrid);
        } else if (id == ID_TEX_LIST && notify == 1) {
            // Grid selection changed â€” update preview
            if (hGrid) {
                auto* gs = (TextureGridState*)GetWindowLongPtr(hGrid, GWLP_USERDATA);
                int sel = TexEntryAt(gs ? gs->selectedIdx : -1);
                if (sel >= 0 && sel < (int)g_textureFiles.size()) {
                    std::string& p = g_textureFiles[sel].path;
                    g_editorPanels.activeTexturePath = p;
                    SetWindowTextA(hSrc, TextFormat("Source: %s", p.c_str()));
                    Image tmp = LoadImageWithFallback(p.c_str());
                    if (tmp.data) {
                        SetWindowTextA(hDims, TextFormat("%dx%d %s", tmp.width, tmp.height,
                                                         g_textureFiles[sel].fromPackage ? "package" : "file"));
                        RECT pr; GetWindowRect(hPreview, &pr);
                        int pw = pr.right - pr.left - 4, ph = pr.bottom - pr.top - 4;
                        if (pw < 1) pw = 100;
                        if (ph < 1) ph = 60;
                        Image rs = ImageCopy(tmp);
                        ImageResize(&rs, pw, ph);
                        ImageFormat(&rs, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
                        unsigned char* px = (unsigned char*)rs.data;
                        for (int i = 0; i < rs.width * rs.height; i++) {
                            unsigned char t = px[i*4]; px[i*4] = px[i*4+2]; px[i*4+2] = t;
                        }
                        HDC hdc = GetDC(hwnd);
                        BITMAPINFO bmi = {}; bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                        bmi.bmiHeader.biWidth = rs.width; bmi.bmiHeader.biHeight = -rs.height;
                        bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32;
                        bmi.bmiHeader.biCompression = BI_RGB;
                        void* bits = nullptr;
                        HBITMAP hBmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
                        if (bits && rs.data) memcpy(bits, rs.data, rs.width * rs.height * 4);
                        ReleaseDC(hwnd, hdc);
                        if (hBmp) {
                            HBITMAP oldBmp = (HBITMAP)SendMessage(hPreview, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)hBmp);
                            if (oldBmp) DeleteObject(oldBmp);
                        }
                        UnloadImage(tmp);
                        UnloadImage(rs);
                    } else {
                        SetWindowTextA(hDims, "Preview unavailable (see console)");
                        fprintf(stderr, "[TextureManager] preview failed to load '%s'\n", p.c_str());
                    }
                }
            }
        } else if (id == ID_TEX_APPLY || (id == ID_TEX_LIST && notify == 2)) {
            // Apply the grid's selected texture to the CURRENT VIEWPORT
            // SELECTION. The old path posted Ev::ApplyTextureToModel with a
            // "Model 1..20" dropdown index, which resolved through WDLModels
            // and was a guaranteed silent no-op for every CSG brush. The
            // selection is captured into the event: this window is modeless,
            // so the modal-writer argument that keeps actionApplyTextureToSel
            // honest does not apply here.
            int sel = -1;
            if (hGrid) {
                auto* gs = (TextureGridState*)GetWindowLongPtr(hGrid, GWLP_USERDATA);
                sel = TexEntryAt(gs ? gs->selectedIdx : -1);
            }
            if (sel >= 0 && sel < (int)g_textureFiles.size()) {
                ed::TextureSelectionApply apply;
                apply.path   = g_textureFiles[sel].path;
                apply.target = ed::SelRef{ ToBusKind((SelType)EdSel_CurrentType()),
                                           EdSel_CurrentIndex() };
                ed::EventBus::instance().post(ed::Ev::ApplyTextureToSelection, apply);
            }
        }
        break;
    }
    case WM_NOTIFY: {
        // Scope tree selection (WM_NOTIFY, not WM_COMMAND). Refresh the grid
        // from the new selection WITHOUT rebuilding the tree.
        NMHDR* nh = (NMHDR*)l;
        if (nh && nh->idFrom == ID_TEX_SCOPE &&
            nh->code == TVN_SELCHANGEDW) {
            UpdateTextureScopeSelection(hwnd, hScope, hGrid);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        ShowTextureManager(false);
        break;
    case WM_DESTROY:
        g_editorPanels.hTextureMgr = nullptr;
        break;
    default:
        return DefWindowProc(hwnd, msg, w, l);
    }
    return 0;
}

static bool ChooseWorldFile(bool save, std::string& outPath) {
    wchar_t path[MAX_PATH] = {};
    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_hRaylibWnd;
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrFilter = save ? L"OZONE World (*.ozone)\0*.ozone\0\0"
                              : L"OZONE World (*.ozone)\0*.ozone\0\0";
    dialog.lpstrDefExt = save ? L"wdl" : nullptr;
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog))) return false;

    int size = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return false;
    std::vector<char> utf8((size_t)size);
    WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8.data(), size, nullptr, nullptr);
    outPath.assign(utf8.data());
    return true;
}

bool ChooseOpenWorldFile(std::string& outPath) { return ChooseWorldFile(false, outPath); }
bool ChooseSaveWorldFile(std::string& outPath) { return ChooseWorldFile(true, outPath); }

