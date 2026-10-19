#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include "../../../Source/WindowsCompat.hpp"
#include "../../../Source/Package/PackageAssetLoader.hpp"
#include "../../../Source/Pawn/OzPawnSystem.hpp"
#include "../../../Source/Script/LightningEntityRegistry.hpp"
#include "../../../Source/World/OzOzoneLoader.hpp"
#include "UiPanels.hpp"
#include "../Resources/AssetScan.hpp"
#include "../Resources/PackageIO.hpp"
#include "../SelType.hpp"
#include "../Core/EditorEventBus.hpp"
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <functional>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

// =====================================================================
// Globals
// =====================================================================

// Extern accessors from Main.cpp for WorldGraph model data
extern int WorldGraph_GetModelCount();
extern void WorldGraph_GetModelData(int index, float& outX, float& outY, float& outZ, float& outR, float& outS);
extern const char* WorldGraph_GetModelName(int index);

// Read-only selection capture for selection-scoped posts from this (modeless)
// layer. g_sel lives in Core/EditorState.hpp, which this TU cannot include
// because that header pulls Editor.hpp (a Main-TU header); these are defined
// in Subsystems/Selection.cpp, beside the picker that maintains g_sel.
extern int EdSel_CurrentType();   // SelType as int
extern int EdSel_CurrentIndex();
// Selection accessors
extern int Editor_GetSelectedType();
extern int Editor_GetSelectedIndex();
// Editor state accessors for toolbox sidebar
extern int Editor_GetCsgOperation();
extern void Editor_SetCsgOperation(int op);
extern int Editor_GetPlaceMode();
extern void Editor_SetPlaceMode(int mode);
// Current world directory (absolute) â€” used by the Texture Manager import feature
extern std::string Editor_GetCurrentWorldDir();
extern std::string Editor_GetCurrentWorldName();

static HINSTANCE g_hInst = nullptr;
static HWND g_hRaylibWnd = nullptr;
static HWND g_hMainWnd = nullptr; // alias for g_hRaylibWnd in message handling

static const wchar_t* CLASS_SOUNDMGR    = L"OzSoundMgr";
static const wchar_t* CLASS_TEXTUREMGR  = L"OzTextureMgr";
static const wchar_t* CLASS_TEXTURE_GRID = L"OzTextureGrid";
static const wchar_t* CLASS_PAWNNMGR    = L"OzPawnMgr";
static const wchar_t* CLASS_SCRIPTMGR   = L"OzScriptMgr";
static const wchar_t* CLASS_MODELBRW    = L"OzModelBrw";
static const wchar_t* CLASS_PICKUPPANEL = L"OzPickupPanel";
static const wchar_t* CLASS_NODEPANEL   = L"OzNodePanel";
static const wchar_t* CLASS_HMEDITOR   = L"OzHmEditor";
static const wchar_t* CLASS_WORLDGRAPH = L"OzWorldGraph";
static const wchar_t* CLASS_PROPSPANEL = L"OzPropsPanel";
static const wchar_t* CLASS_STATSSIDEBAR = L"OzStatsSidebar";
static const wchar_t* CLASS_LEVELLIST = L"OzLevelList";
static const wchar_t* CLASS_ANIMPANEL  = L"OzAnimPanel";
static const int STATS_SIDEBAR_W = 200;


// Entry structures for dynamic resource browsers
struct ResourceEntry {
    std::string name;
    std::string path;
    HBITMAP thumbnail = nullptr; // cached 64x64 preview, built LAZILY on first paint
    // Set by the scanner, which knows which list an entry came from. Both browsers used
    // to infer it with !IsPathFile(path) - a filesystem hit test that re-derives
    // something already known, and that AssetScopeItem::fromPackage exists precisely to
    // state instead.
    bool fromPackage = false;
};

static std::vector<ResourceEntry> g_textureFiles;
static std::vector<ResourceEntry> g_soundFiles;

// Indices into g_textureFiles for the leaves currently in scope (the subtree
// selected in the Texture Manager's scope tree, or everything when a search is
// active). Empty means "no scope filter" and the grid shows every texture,
// which is the grid's original behaviour.
static std::vector<int> g_textureVisible;

static int TexVisibleCount() {
    return g_textureVisible.empty() ? (int)g_textureFiles.size()
                                    : (int)g_textureVisible.size();
}

// Map a grid position to a g_textureFiles index, or -1 when out of range.
static int TexEntryAt(int pos) {
    if (g_textureVisible.empty()) {
        return (pos >= 0 && pos < (int)g_textureFiles.size()) ? pos : -1;
    }
    if (pos < 0 || pos >= (int)g_textureVisible.size()) return -1;
    return g_textureVisible[pos];
}

// Pawn defs managed by PawnSystem (shared with runtime)

// Model preview sequence number (for refresh)
static int g_previewSeq = 0;

// =====================================================================
// Helper: scan GameData/ filesystem + packages for matching extensions
// =====================================================================
// Adapter over Resources/AssetScan.cpp. The enumeration (filesystem walk + package
// walk + sort + dedupe) is shared with the Model Browser; all this does is map the
// result into ResourceEntry, which carries an HBITMAP thumbnail and is therefore a UI
// type rather than an asset-tree one.
static void ScanFilesAndPackages(const std::string& subdir,
                                 const std::vector<std::string>& exts,
                                 std::vector<ResourceEntry>& out) {
    const std::vector<AssetScopeItem> found = ScanAssets(subdir, exts);
    out.clear();
    out.reserve(found.size());
    for (const auto& it : found) {
        ResourceEntry e;
        e.name = it.name;
        e.path = it.path;
        e.thumbnail = nullptr;      // filled lazily by the texture grid
        out.push_back(std::move(e));
    }
}

// =====================================================================
// Forward declarations
// =====================================================================
static LRESULT CALLBACK SoundMgrProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
static LRESULT CALLBACK TextureMgrProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
static LRESULT CALLBACK TextureGridProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
static LRESULT CALLBACK PawnMgrProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
static LRESULT CALLBACK ScriptMgrProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
static LRESULT CALLBACK ModelBrwProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
static LRESULT CALLBACK PickupPanelProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
static LRESULT CALLBACK NodePanelProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
static LRESULT CALLBACK HmEditorProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
static LRESULT CALLBACK WorldGraphProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
static LRESULT CALLBACK LevelListProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
static LRESULT CALLBACK StatsSidebarProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);

// =====================================================================
// Helper functions
// =====================================================================
static bool RegisterPanelClass(const wchar_t* className, WNDPROC proc, HINSTANCE hInst) {
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = className;
    return RegisterClassEx(&wc) != 0;
}

static HWND CreateCtrl(HWND hParent, const wchar_t* cls, const wchar_t* text,
                       int x, int y, int w, int h, int id, DWORD extraStyle = 0) {
    return CreateWindowEx(0, cls, text, WS_CHILD | WS_VISIBLE | extraStyle,
                          x, y, w, h, hParent, (HMENU)(INT_PTR)id, g_hInst, nullptr);
}

static HWND CreateButton(HWND hParent, const wchar_t* text, int x, int y, int w, int h, int id) {
    return CreateCtrl(hParent, L"BUTTON", text, x, y, w, h, id, BS_PUSHBUTTON);
}

static HWND CreateLabel(HWND hParent, const wchar_t* text, int x, int y, int w, int h, int id) {
    return CreateCtrl(hParent, L"STATIC", text, x, y, w, h, id, SS_LEFT);
}

// Bold font for the Entity Properties group headers. A STATIC has no bold style,
// so the weight has to come from the font. Created once (leaked deliberately, like
// the other UI resources here) and deleted never — a HFONT handed to a live control
// must outlive it, and these live for the whole editor session.
static HFONT GetBoldUiFont() {
    static HFONT font = []() -> HFONT {
        return CreateFontW(-11, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    }();
    return font;
}

static HWND CreateListBox(HWND hParent, int x, int y, int w, int h, int id) {
    return CreateCtrl(hParent, L"LISTBOX", L"", x, y, w, h, id,
                      WS_BORDER | WS_VSCROLL | LBS_NOTIFY);
}

// --- Icon buttons (owner-draw) for the native stats sidebar -----------------
static std::unordered_map<std::string, HBITMAP> g_uiBitmaps;
static std::unordered_map<int, HBITMAP> g_uiBtnIcon;

// Button face color icons are pre-composited against. Must match DrawIconButton's
// unselected fill so no seam shows around the glyph.
static const COLORREF UI_BTN_FACE = RGB(45, 45, 50);

// Wrap a raylib Image as a 32-bit top-down DIB, pre-compositing the RGBA over the
// button face. Compositing once at load time lets DrawIconButton keep plain
// StretchBlt/SRCCOPY (no AlphaBlend, so no -lmsimg32).
static HBITMAP MakeUiBitmap(const Image& img) {
    if (!img.data || img.width <= 0 || img.height <= 0) return nullptr;
    int w = img.width, h = img.height;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;                 // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!bmp || !bits) { if (bmp) DeleteObject(bmp); return nullptr; }
    const unsigned bgR = GetRValue(UI_BTN_FACE), bgG = GetGValue(UI_BTN_FACE), bgB = GetBValue(UI_BTN_FACE);
    auto* dst = static_cast<unsigned char*>(bits);
    for (int y = 0; y < h; y++) {
        unsigned char* d = dst + (size_t)y * w * 4;
        for (int x = 0; x < w; x++, d += 4) {
            // GetImageColor rather than reading img.data directly: it normalizes
            // whatever format the decoder produced into a plain Color.
            Color c = GetImageColor(img, x, y);
            unsigned a = c.a;
            if (a == 255) { d[0] = c.b; d[1] = c.g; d[2] = c.r; }
            else {
                d[0] = (unsigned char)(c.b * a / 255 + bgB * (255 - a) / 255);
                d[1] = (unsigned char)(c.g * a / 255 + bgG * (255 - a) / 255);
                d[2] = (unsigned char)(c.r * a / 255 + bgR * (255 - a) / 255);
            }
            d[3] = 255;
        }
    }
    return bmp;
}

// Load a UI icon as an HBITMAP (cached).
//
// NOTE: every file in AngelEd/UI is PNG data carrying a .bmp extension, which made
// LoadImageW(..., IMAGE_BITMAP, ..., LR_LOADFROMFILE) return NULL and left all 18
// icon buttons (13 sidebar + ModelBrowser Import/Export + Heightmap Browse/Generate)
// rendering label-only. raylib's LoadImage sniffs the magic bytes, so it decodes them
// correctly; LoadImageW stays as a fallback for any genuine .bmp added later.
static HBITMAP LoadUiBitmap(const std::string& name) {
    auto it = g_uiBitmaps.find(name);
    if (it != g_uiBitmaps.end()) return it->second;
    const char* prefixes[] = { "AngelEd/UI/", "../AngelEd/UI/" };
    HBITMAP bm = nullptr;
    for (auto* p : prefixes) {
        std::string full = std::string(p) + name + ".bmp";
        Image img = LoadImage(full.c_str());
        if (img.data) { bm = MakeUiBitmap(img); UnloadImage(img); }
        if (!bm) {
            std::wstring w(full.begin(), full.end());
            bm = (HBITMAP)LoadImageW(nullptr, w.c_str(), IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE);
        }
        if (bm) break;
    }
    if (!bm) OZ_WARN("UiBitmap: MISSING '%s' (tried %d prefixes)", name.c_str(), (int)(sizeof(prefixes) / sizeof(prefixes[0])));
    g_uiBitmaps[name] = bm;
    return bm;
}

static HWND CreateIconButton(HWND hParent, const wchar_t* text, int x, int y,
                             int w, int h, int id, const std::string& icon) {
    HWND b = CreateWindowEx(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                            x, y, w, h, hParent, (HMENU)(INT_PTR)id, g_hInst, nullptr);
    HBITMAP bm = LoadUiBitmap(icon);
    if (!bm) OZ_WARN("CreateIconButton: no icon '%s' for control id %d", icon.c_str(), id);
    g_uiBtnIcon[id] = bm;
    return b;
}

// Draw an owner-draw icon button (bitmap on the left, label after it).
static LRESULT DrawIconButton(LPDRAWITEMSTRUCT dis) {
    HBRUSH bg = CreateSolidBrush((dis->itemState & ODS_SELECTED) ? RGB(70, 90, 120) : RGB(45, 45, 50));
    FillRect(dis->hDC, &dis->rcItem, bg);
    DeleteObject(bg);
    FrameRect(dis->hDC, &dis->rcItem, (HBRUSH)GetStockObject(GRAY_BRUSH));

    int x = dis->rcItem.left + 4;
    int cy = (dis->rcItem.top + dis->rcItem.bottom) / 2;
    auto it = g_uiBtnIcon.find((int)dis->CtlID);
    if (it != g_uiBtnIcon.end() && it->second) {
        BITMAP b;
        GetObject(it->second, sizeof(b), &b);
        int ih = (dis->rcItem.bottom - dis->rcItem.top) - 6;
        if (ih > 20) ih = 20;
        if (ih < 8) ih = 8;
        int iw = (b.bmHeight > 0) ? (int)((float)b.bmWidth * ih / b.bmHeight) : ih;
        if (iw > ih) iw = ih;
        HDC mem = CreateCompatibleDC(dis->hDC);
        HGDIOBJ old = SelectObject(mem, it->second);
        SetStretchBltMode(dis->hDC, COLORONCOLOR);
        StretchBlt(dis->hDC, x, cy - ih / 2, iw, ih, mem, 0, 0, b.bmWidth, b.bmHeight, SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);
        x += iw + 5;
    }
    wchar_t txt[64] = {0};
    GetWindowTextW(dis->hwndItem, txt, 64);
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, RGB(215, 225, 240));
    RECT tr = dis->rcItem;
    tr.left = x;
    tr.right -= 3;
    DrawTextW(dis->hDC, txt, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    return TRUE;
}

