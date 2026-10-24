// ============================================================================
// MaterialPanel.cpp
//
// Material Graph Explorer. FRAGMENT - included by ../UiShell.cpp, the single TU
// for the UI layer (see UiShell.cpp for why).
//
// Browsers GameData/Materials/*.ozmat, visualises the DAG and edits it:
//   - drag a node to move it (position persisted as `pos <id> <x> <y>`)
//   - drag from a node's right port to an input port / gate to rewire
//   - right-click: add a node, delete a node, edit a constant, pick a texture
//   - double-click a texture node to choose its image
//   - Bake / Reload / Edit Text / Close
//
// Every mutation is validated by re-parsing the serialized graph; a rejected
// edit reverts, so the file on disk is always loadable.
// ============================================================================

#include "../../../../Source/Material/OzMaterial.hpp"
#include "../../Resources/MaterialBake.hpp"
#include "../../../../Source/Material/MaterialRegistry.hpp"
#include <algorithm>
#include <commdlg.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <windowsx.h>
#include <shellapi.h>

static const int ID_MAT_LIST   = 100;
static const int ID_MAT_BAKE   = 101;
static const int ID_MAT_EDIT   = 102;
static const int ID_MAT_RELOAD = 103;
static const int ID_MAT_CLOSE  = 104;
static const int ID_MAT_STATUS = 105;

static const int kBoxW = 150, kBoxH = 40, kGapX = 40, kGapY = 18, kPad = 16;
static const int kStrideX = kBoxW + kGapX;
static const int kStrideY = kBoxH + kGapY;

static std::vector<std::string> g_matPaths;
static ozmat::Material g_matGraph;
static bool g_matValid = false;
static std::string g_matPath;
static HWND g_matCanvas = nullptr;

static int  g_matSelected = -1;
static int  g_matDrag = -1;
static POINT g_matDragOffset{0, 0};
static int  g_matLinkFrom = -1;      // node whose OUTPUT is being dragged
static POINT g_matLinkEnd{0, 0};

static const char* kMaterialDir = "GameData/Materials";

// ---------------------------------------------------------------------------
// Layout / hit-testing (shared by drawing and mouse handling)
// ---------------------------------------------------------------------------
static int MatDepth(const ozmat::Material& m, const std::string& id, int guard) {
    if (guard > 40) return 0;
    const ozmat::Node* n = m.FindNode(id);
    if (!n) return 0;
    switch (n->type) {
        case ozmat::NodeType::Texture:
        case ozmat::NodeType::Constant:
            return 0;
        case ozmat::NodeType::Multiply:
            return 1 + std::max(MatDepth(m, n->a, guard + 1), MatDepth(m, n->b, guard + 1));
        case ozmat::NodeType::Normal:
            return 1 + MatDepth(m, n->source, guard + 1);
    }
    return 0;
}

static int MatMaxDepth() {
    int d = 0;
    for (const ozmat::Node& n : g_matGraph.nodes)
        d = std::max(d, MatDepth(g_matGraph, n.id, 0));
    return d;
}

// Assign px/py to nodes that have none. Nodes with a saved position keep it, so
// a hand-arranged graph is never re-shuffled.
static void MatEnsureLayout() {
    std::vector<int> nextY((size_t)MatMaxDepth() + 1, kPad);
    for (const ozmat::Node& n : g_matGraph.nodes) {
        if (n.px < 0 || n.py < 0) continue;
        const int d = MatDepth(g_matGraph, n.id, 0);
        nextY[(size_t)d] = std::max(nextY[(size_t)d], n.py + kStrideY);
    }
    for (ozmat::Node& n : g_matGraph.nodes) {
        if (n.px >= 0 && n.py >= 0) continue;
        const int d = MatDepth(g_matGraph, n.id, 0);
        n.px = kPad + d * kStrideX;
        n.py = nextY[(size_t)d];
        nextY[(size_t)d] += kStrideY;
    }
}

static RECT MatNodeRect(const ozmat::Node& n) {
    return RECT{n.px, n.py, n.px + kBoxW, n.py + kBoxH};
}

static int MatNodeAt(int x, int y) {
    for (int i = (int)g_matGraph.nodes.size() - 1; i >= 0; --i) {
        RECT r = MatNodeRect(g_matGraph.nodes[(size_t)i]);
        if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) return i;
    }
    return -1;
}

struct MatGateBox { ozmat::Gate gate; RECT rect; };
static std::vector<MatGateBox> MatGateBoxes() {
    std::vector<MatGateBox> out;
    const int gx = kPad + (MatMaxDepth() + 1) * kStrideX;
    int row = 0;
    for (int g = 0; g < (int)ozmat::Gate::Count; ++g) {
        const ozmat::Output* o = g_matGraph.FindOutput((ozmat::Gate)g);
        if (!o) continue;
        out.push_back({(ozmat::Gate)g,
                       RECT{gx, kPad + row * kStrideY, gx + kBoxW, kPad + row * kStrideY + kBoxH}});
        ++row;
    }
    return out;
}

static int MatGateAt(int x, int y) {
    const std::vector<MatGateBox> boxes = MatGateBoxes();
    for (size_t i = 0; i < boxes.size(); ++i) {
        const RECT& r = boxes[i].rect;
        if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) return (int)i;
    }
    return -1;
}

// Input slots a node type accepts. Multiply: 2 (a,b); Normal: 1 (source);
// Texture/Constant: none.
static int MatInputCount(const ozmat::Node& n) {
    switch (n.type) {
        case ozmat::NodeType::Multiply: return 2;
        case ozmat::NodeType::Normal:   return 1;
        default:                        return 0;
    }
}

static void MatInputPort(const ozmat::Node& n, int slot, int* x, int* y) {
    const int count = std::max(1, MatInputCount(n));
    *x = n.px;
    *y = n.py + (int)((slot + 1.0f) / (count + 1.0f) * kBoxH);
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------
static bool MatValidate() {
    ozmat::Material probe;
    std::string err;
    return ozmat::Parse(ozmat::Serialize(g_matGraph), probe, &err);
}

static bool MatSaveGraph() {
    if (g_matPath.empty() || !g_matValid) return false;
    std::ofstream f(g_matPath, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;
    f << ozmat::Serialize(g_matGraph);
    return true;
}

static std::string MatStem(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    const size_t dot = path.find_last_of('.');
    const size_t b = (slash == std::string::npos) ? 0 : slash + 1;
    const size_t e = (dot == std::string::npos || dot < b) ? path.size() : dot;
    return path.substr(b, e - b);
}

static void MatSetStatus(HWND hwnd, const std::string& text) {
    if (HWND h = GetDlgItem(hwnd, ID_MAT_STATUS))
        SetWindowTextA(h, text.c_str());
}

static void MatUpdateStatus(HWND hwnd) {
    if (!g_matValid) return;
    const bool stale = g_matGraph.bakeHash.empty() ||
                       g_matGraph.bakeHash != ozmat::Hash(g_matGraph);
    MatSetStatus(hwnd, g_matGraph.name + " | " + std::to_string(g_matGraph.nodes.size()) +
                           " node(s)" + (stale ? " | STALE (bake me)" : " | baked"));
}

static void MatLoadPreview(HWND hwnd) {
    g_matValid = false;
    g_matGraph = ozmat::Material{};
    g_matSelected = -1;
    if (g_matPath.empty()) {
        MatSetStatus(hwnd, "No material selected");
        if (g_matCanvas) InvalidateRect(g_matCanvas, nullptr, TRUE);
        return;
    }
    std::ifstream f(g_matPath);
    if (!f.is_open()) {
        MatSetStatus(hwnd, "Could not open " + g_matPath);
        if (g_matCanvas) InvalidateRect(g_matCanvas, nullptr, TRUE);
        return;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string err;
    if (!ozmat::Parse(ss.str(), g_matGraph, &err)) {
        MatSetStatus(hwnd, "Parse error: " + err);
        if (g_matCanvas) InvalidateRect(g_matCanvas, nullptr, TRUE);
        return;
    }
    g_matValid = true;
    MatEnsureLayout();
    MatUpdateStatus(hwnd);
    if (g_matCanvas) InvalidateRect(g_matCanvas, nullptr, TRUE);
}

static void MatRefreshList(HWND hwnd) {
    HWND list = GetDlgItem(hwnd, ID_MAT_LIST);
    if (!list) return;
    const std::string keep = g_matPath;
    SendMessageA(list, LB_RESETCONTENT, 0, 0);
    g_matPaths.clear();

    std::error_code ec;
    std::vector<std::filesystem::path> files;
    if (std::filesystem::exists(kMaterialDir, ec)) {
        for (auto& e : std::filesystem::directory_iterator(kMaterialDir, ec))
            if (e.is_regular_file() && e.path().extension() == ".ozmat") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    int select = -1;
    for (const auto& p : files) {
        g_matPaths.push_back(p.generic_string());
        const std::string stem = p.stem().string();
        const int idx = (int)SendMessageA(list, LB_ADDSTRING, 0, (LPARAM)stem.c_str());
        if (p.generic_string() == keep) select = idx;
    }
    if (select < 0 && !g_matPaths.empty()) select = 0;
    if (select >= 0) {
        SendMessageA(list, LB_SETCURSEL, (WPARAM)select, 0);
        g_matPath = g_matPaths[(size_t)select];
    } else {
        g_matPath.clear();
    }
    MatLoadPreview(hwnd);
}

static void MatCascadeDelete(int index) {
    if (index < 0 || index >= (int)g_matGraph.nodes.size()) return;
    std::set<std::string> doomed{g_matGraph.nodes[(size_t)index].id};
    bool changed = true;
    while (changed) {
        changed = false;
        for (const ozmat::Node& n : g_matGraph.nodes) {
            if (doomed.count(n.id)) continue;
            bool refs = false;
            if (n.type == ozmat::NodeType::Multiply)
                refs = doomed.count(n.a) || doomed.count(n.b);
            else if (n.type == ozmat::NodeType::Normal)
                refs = doomed.count(n.source);
            if (refs) { doomed.insert(n.id); changed = true; }
        }
    }
    g_matGraph.nodes.erase(
        std::remove_if(g_matGraph.nodes.begin(), g_matGraph.nodes.end(),
                       [&](const ozmat::Node& n) { return doomed.count(n.id) != 0; }),
        g_matGraph.nodes.end());
    g_matGraph.outputs.erase(
        std::remove_if(g_matGraph.outputs.begin(), g_matGraph.outputs.end(),
                       [&](const ozmat::Output& o) { return doomed.count(o.source) != 0; }),
        g_matGraph.outputs.end());
    g_matSelected = -1;
}

static std::string MatUniqueId(const char* prefix) {
    for (int i = 0; i < 10000; ++i) {
        std::string id = std::string(prefix) + std::to_string(i);
        if (!g_matGraph.FindNode(id)) return id;
    }
    return std::string(prefix) + "x";
}

static void MatAddNode(ozmat::NodeType type, int x, int y) {
    ozmat::Material backup = g_matGraph;
    if (g_matGraph.nodes.empty()) {
        // Every non-source node needs inputs; seed a constant so the first
        // added node is immediately valid.
        ozmat::Node c;
        c.id = MatUniqueId("c");
        c.type = ozmat::NodeType::Constant;
        c.color[0] = c.color[1] = c.color[2] = 0.5f;
        c.color[3] = 1.0f;
        c.px = kPad;
        c.py = kPad;
        g_matGraph.nodes.push_back(c);
    }

    ozmat::Node n;
    n.type = type;
    n.px = x - kBoxW / 2;
    n.py = y - kBoxH / 2;
    switch (type) {
        case ozmat::NodeType::Texture:
            n.id = MatUniqueId("tex");
            break;
        case ozmat::NodeType::Constant:
            n.id = MatUniqueId("c");
            n.color[0] = n.color[1] = n.color[2] = 0.5f;
            n.color[3] = 1.0f;
            break;
        case ozmat::NodeType::Multiply:
            n.id = MatUniqueId("mul");
            n.a = g_matGraph.nodes.front().id;
            n.b = g_matGraph.nodes.front().id;
            break;
        case ozmat::NodeType::Normal:
            n.id = MatUniqueId("nrm");
            n.source = g_matGraph.nodes.front().id;
            break;
    }
    g_matGraph.nodes.push_back(n);
    if (!MatValidate()) g_matGraph = backup;
    g_matSelected = (int)g_matGraph.nodes.size() - 1;
    MatEnsureLayout();
}

static bool MatPromptFloat(HWND parent, const char* title, float current, float* out) {
    struct State { bool done = false; bool ok = false; } st;
    HWND wnd = CreateWindowExA(WS_EX_DLGMODALFRAME, "STATIC", title,
                               WS_POPUP | WS_CAPTION | WS_SYSMENU,
                               CW_USEDEFAULT, CW_USEDEFAULT, 280, 130,
                               parent, nullptr, GetModuleHandle(nullptr), nullptr);
    if (!wnd) return false;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%g", current);
    HWND edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", buf,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                12, 14, 250, 24, wnd, (HMENU)1, GetModuleHandle(nullptr), nullptr);
    CreateWindowExA(0, "BUTTON", "OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
                    110, 56, 70, 26, wnd, (HMENU)IDOK, GetModuleHandle(nullptr), nullptr);
    CreateWindowExA(0, "BUTTON", "Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    190, 56, 70, 26, wnd, (HMENU)IDCANCEL, GetModuleHandle(nullptr), nullptr);
    ShowWindow(wnd, SW_SHOW);
    SetFocus(edit);
    MSG msg;
    while (!st.done && GetMessage(&msg, nullptr, 0, 0)) {
        const int id = (msg.hwnd == wnd && msg.message == WM_COMMAND) ? LOWORD(msg.wParam) : 0;
        if (id == IDOK || (msg.hwnd == edit && msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN)) {
            char text[64] = {0};
            GetWindowTextA(edit, text, sizeof(text));
            char* end = nullptr;
            const double v = std::strtod(text, &end);
            if (end != text) { *out = (float)v; st.ok = true; }
            st.done = true;
        } else if (id == IDCANCEL || (msg.hwnd == wnd && msg.message == WM_CLOSE)) {
            st.done = true;
        } else if (msg.message != WM_KEYDOWN || msg.wParam != VK_ESCAPE) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            st.done = true;
        }
    }
    DestroyWindow(wnd);
    return st.ok;
}

static void MatPickTexture(HWND parent, int nodeIndex) {
    if (nodeIndex < 0 || nodeIndex >= (int)g_matGraph.nodes.size()) return;
    char file[MAX_PATH] = {0};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = parent;
    ofn.lpstrFilter = "Images\0*.png;*.bmp;*.tga;*.jpg;*.jpeg\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle = "Choose a texture";
    if (!GetOpenFileNameA(&ofn)) return;

    std::filesystem::path p(file);
    std::error_code ec;
    auto rel = std::filesystem::relative(p, std::filesystem::current_path(), ec);
    std::string stored = (!ec && !rel.empty()) ? rel.generic_string() : p.generic_string();
    g_matGraph.nodes[(size_t)nodeIndex].path = stored;
    if (!MatValidate()) g_matGraph.nodes[(size_t)nodeIndex].path.clear();
    MatSaveGraph();
    MatLoadPreview(GetParent(g_matCanvas));
}

// Wire `fromNode`'s output into whatever is under (x,y): a gate box or an input
// slot of another node. Reverts if the result would not parse.
static void MatFinishLink(int x, int y) {
    if (g_matLinkFrom < 0 || g_matLinkFrom >= (int)g_matGraph.nodes.size()) return;
    const std::string source = g_matGraph.nodes[(size_t)g_matLinkFrom].id;
    ozmat::Material backup = g_matGraph;

    bool changed = false;
    const int gate = MatGateAt(x, y);
    if (gate >= 0) {
        const std::vector<MatGateBox> boxes = MatGateBoxes();
        for (ozmat::Output& o : g_matGraph.outputs) {
            if (o.gate == boxes[(size_t)gate].gate) {
                o.source = source;
                o.channel = 0;
                changed = true;
            }
        }
    } else {
        const int target = MatNodeAt(x, y);
        if (target >= 0 && target != g_matLinkFrom) {
            ozmat::Node& n = g_matGraph.nodes[(size_t)target];
            const int inputs = MatInputCount(n);
            if (inputs > 0) {
                const int slot = (inputs == 1) ? 0 : ((y < n.py + kBoxH / 2) ? 0 : 1);
                if (n.type == ozmat::NodeType::Multiply) {
                    if (slot == 0) n.a = source; else n.b = source;
                } else {
                    n.source = source;
                }
                changed = true;
            }
        }
    }
    if (changed && !MatValidate()) g_matGraph = backup;
    if (changed) MatSaveGraph();
    MatLoadPreview(GetParent(g_matCanvas));
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static const char* MatTypeName(ozmat::NodeType t) {
    switch (t) {
        case ozmat::NodeType::Texture:  return "texture";
        case ozmat::NodeType::Constant: return "constant";
        case ozmat::NodeType::Multiply: return "multiply";
        case ozmat::NodeType::Normal:   return "normal";
    }
    return "?";
}

static void MatDrawGraph(HDC hdc, const RECT& rc) {
    HBRUSH bg = CreateSolidBrush(RGB(24, 26, 32));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(hdc, TRANSPARENT);

    if (!g_matValid) {
        SetTextColor(hdc, RGB(180, 180, 190));
        const char* hint = "Select a .ozmat material on the left.";
        TextOutA(hdc, 16, 16, hint, (int)std::strlen(hint));
        return;
    }
    MatEnsureLayout();

    HPEN edge = CreatePen(PS_SOLID, 1, RGB(120, 140, 170));
    HPEN oldPen = (HPEN)SelectObject(hdc, edge);
    auto nodeCenterRight = [&](const std::string& id, int* x, int* y) {
        for (const ozmat::Node& n : g_matGraph.nodes)
            if (n.id == id) { *x = n.px + kBoxW; *y = n.py + kBoxH / 2; return true; }
        return false;
    };
    auto drawEdge = [&](int x1, int y1, int x2, int y2) {
        MoveToEx(hdc, x1, y1, nullptr);
        LineTo(hdc, x2, y2);
        POINT pts[3] = {{x2, y2}, {x2 - 6, y2 - 4}, {x2 - 6, y2 + 4}};
        Polyline(hdc, pts, 3);
    };

    for (const ozmat::Node& n : g_matGraph.nodes) {
        int sx = 0, sy = 0;
        if (n.type == ozmat::NodeType::Multiply) {
            if (nodeCenterRight(n.a, &sx, &sy)) drawEdge(sx, sy, n.px, n.py + kBoxH / 3);
            if (nodeCenterRight(n.b, &sx, &sy)) drawEdge(sx, sy, n.px, n.py + (2 * kBoxH) / 3);
        } else if (n.type == ozmat::NodeType::Normal) {
            if (nodeCenterRight(n.source, &sx, &sy)) drawEdge(sx, sy, n.px, n.py + kBoxH / 2);
        }
    }

    for (const MatGateBox& gb : MatGateBoxes()) {
        const ozmat::Output* out = g_matGraph.FindOutput(gb.gate);
        if (!out) continue;
        int sx = 0, sy = 0;
        if (nodeCenterRight(out->source, &sx, &sy))
            drawEdge(sx, sy, gb.rect.left, gb.rect.top + kBoxH / 2);
        std::string label = std::string(ozmat::GateName(gb.gate)) + "  <- " + out->source;
        if (out->channel) label += std::string(".") + out->channel;
        HBRUSH gbBrush = CreateSolidBrush(RGB(42, 46, 58));
        FillRect(hdc, &gb.rect, gbBrush);
        DeleteObject(gbBrush);
        SetTextColor(hdc, RGB(235, 220, 160));
        TextOutA(hdc, gb.rect.left + 8, gb.rect.top + 12, label.c_str(), (int)label.size());
    }

    for (size_t i = 0; i < g_matGraph.nodes.size(); ++i) {
        const ozmat::Node& n = g_matGraph.nodes[i];
        RECT r = MatNodeRect(n);
        HBRUSH nb = CreateSolidBrush(i == (size_t)g_matSelected ? RGB(80, 96, 128) : RGB(52, 58, 72));
        FillRect(hdc, &r, nb);
        DeleteObject(nb);
        FrameRect(hdc, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetTextColor(hdc, RGB(230, 235, 245));
        TextOutA(hdc, r.left + 8, r.top + 5, n.id.c_str(), (int)n.id.size());
        std::string sub = MatTypeName(n.type);
        if (n.type == ozmat::NodeType::Texture) {
            const size_t slash = n.path.find_last_of("/\\");
            sub = slash == std::string::npos ? n.path : n.path.substr(slash + 1);
        } else if (n.type == ozmat::NodeType::Constant) {
            char buf[80];
            std::snprintf(buf, sizeof(buf), "%.2f %.2f %.2f", n.color[0], n.color[1], n.color[2]);
            sub = buf;
        }
        SetTextColor(hdc, RGB(170, 178, 195));
        TextOutA(hdc, r.left + 8, r.top + 21, sub.c_str(), (int)sub.size());

        // Ports: input(s) left, output right.
        HBRUSH port = CreateSolidBrush(RGB(150, 170, 210));
        for (int s = 0; s < MatInputCount(n); ++s) {
            int px = 0, py = 0;
            MatInputPort(n, s, &px, &py);
            RECT pr = {px - 4, py - 4, px + 4, py + 4};
            FillRect(hdc, &pr, port);
        }
        RECT out = {r.right - 4, r.top + kBoxH / 2 - 4, r.right + 4, r.top + kBoxH / 2 + 4};
        FillRect(hdc, &out, port);
        DeleteObject(port);
    }

    if (g_matLinkFrom >= 0 && g_matLinkFrom < (int)g_matGraph.nodes.size()) {
        const ozmat::Node& n = g_matGraph.nodes[(size_t)g_matLinkFrom];
        HPEN link = CreatePen(PS_SOLID, 2, RGB(230, 200, 120));
        HPEN prev = (HPEN)SelectObject(hdc, link);
        MoveToEx(hdc, n.px + kBoxW, n.py + kBoxH / 2, nullptr);
        LineTo(hdc, g_matLinkEnd.x, g_matLinkEnd.y);
        SelectObject(hdc, prev);
        DeleteObject(link);
    }

    SelectObject(hdc, oldPen);
    DeleteObject(edge);
}

// ---------------------------------------------------------------------------
// Canvas window
// ---------------------------------------------------------------------------
static LRESULT CALLBACK MatCanvasProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    const int mx = GET_X_LPARAM(l), my = GET_Y_LPARAM(l);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        MatDrawGraph(hdc, rc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN: {
        SetFocus(hwnd);
        const int node = MatNodeAt(mx, my);
        if (node >= 0) {
            g_matSelected = node;
            const RECT r = MatNodeRect(g_matGraph.nodes[(size_t)node]);
            // Right-edge port starts a link drag.
            if (mx >= r.right - 8) {
                g_matLinkFrom = node;
                g_matLinkEnd = {mx, my};
            } else {
                g_matDrag = node;
                g_matDragOffset = {mx - r.left, my - r.top};
            }
            SetCapture(hwnd);
        } else {
            g_matSelected = -1;
        }
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_matDrag >= 0 && g_matDrag < (int)g_matGraph.nodes.size()) {
            ozmat::Node& n = g_matGraph.nodes[(size_t)g_matDrag];
            n.px = std::max(2, mx - (int)g_matDragOffset.x);
            n.py = std::max(2, my - (int)g_matDragOffset.y);
            InvalidateRect(hwnd, nullptr, TRUE);
        } else if (g_matLinkFrom >= 0) {
            g_matLinkEnd = {mx, my};
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == hwnd) ReleaseCapture();
        if (g_matLinkFrom >= 0) {
            MatFinishLink(mx, my);
            g_matLinkFrom = -1;
        } else if (g_matDrag >= 0) {
            if (MatValidate()) MatSaveGraph();
            g_matDrag = -1;
        }
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    case WM_LBUTTONDBLCLK: {
        const int node = MatNodeAt(mx, my);
        if (node >= 0) {
            const ozmat::Node& n = g_matGraph.nodes[(size_t)node];
            if (n.type == ozmat::NodeType::Texture) {
                MatPickTexture(hwnd, node);
            } else if (n.type == ozmat::NodeType::Constant) {
                float v = n.color[0];
                if (MatPromptFloat(hwnd, "Constant value (grey)", v, &v)) {
                    ozmat::Material backup = g_matGraph;
                    ozmat::Node& c = g_matGraph.nodes[(size_t)node];
                    c.color[0] = c.color[1] = c.color[2] = v;
                    c.color[3] = 1.0f;
                    if (!MatValidate()) g_matGraph = backup;
                    else MatSaveGraph();
                    MatLoadPreview(GetParent(hwnd));
                }
            }
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        const int node = MatNodeAt(mx, my);
        HMENU menu = CreatePopupMenu();
        if (node >= 0) {
            AppendMenuA(menu, MF_STRING, 1, "Delete node");
            if (g_matGraph.nodes[(size_t)node].type == ozmat::NodeType::Texture)
                AppendMenuA(menu, MF_STRING, 2, "Choose texture...");
            if (g_matGraph.nodes[(size_t)node].type == ozmat::NodeType::Constant)
                AppendMenuA(menu, MF_STRING, 3, "Set value...");
        } else {
            AppendMenuA(menu, MF_STRING, 10, "Add Texture");
            AppendMenuA(menu, MF_STRING, 11, "Add Constant");
            AppendMenuA(menu, MF_STRING, 12, "Add Multiply");
            AppendMenuA(menu, MF_STRING, 13, "Add Normal");
        }
        POINT pt = {mx, my};
        ClientToScreen(hwnd, &pt);
        const int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                            pt.x, pt.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
        if (cmd == 1) {
            ozmat::Material backup = g_matGraph;
            MatCascadeDelete(node);
            if (!MatValidate()) g_matGraph = backup;
            else MatSaveGraph();
            MatLoadPreview(GetParent(hwnd));
        } else if (cmd == 2) {
            MatPickTexture(hwnd, node);
        } else if (cmd == 3) {
            float v = g_matGraph.nodes[(size_t)node].color[0];
            if (MatPromptFloat(hwnd, "Constant value (grey)", v, &v)) {
                ozmat::Node& c = g_matGraph.nodes[(size_t)node];
                c.color[0] = c.color[1] = c.color[2] = v;
                c.color[3] = 1.0f;
                if (MatValidate()) MatSaveGraph();
                MatLoadPreview(GetParent(hwnd));
            }
        } else if (cmd >= 10 && cmd <= 13) {
            static const ozmat::NodeType kTypes[4] = {
                ozmat::NodeType::Texture, ozmat::NodeType::Constant,
                ozmat::NodeType::Multiply, ozmat::NodeType::Normal};
            MatAddNode(kTypes[cmd - 10], mx, my);
            MatSaveGraph();
            MatLoadPreview(GetParent(hwnd));
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (w == VK_DELETE && g_matSelected >= 0) {
            ozmat::Material backup = g_matGraph;
            MatCascadeDelete(g_matSelected);
            if (!MatValidate()) g_matGraph = backup;
            else MatSaveGraph();
            MatLoadPreview(GetParent(hwnd));
        }
        return 0;
    default:
        return DefWindowProc(hwnd, msg, w, l);
    }
}

// ---------------------------------------------------------------------------
// Panel
// ---------------------------------------------------------------------------
void ShowMaterialGraph(bool show) {
    g_editorPanels.showMaterialPanel = show;
    if (g_editorPanels.hMaterialPanel)
        ShowWindow((HWND)g_editorPanels.hMaterialPanel, show ? SW_SHOW : SW_HIDE);
    if (show && g_editorPanels.hMaterialPanel)
        MatRefreshList((HWND)g_editorPanels.hMaterialPanel);
}

void RefreshMaterialGraph() {
    if (g_editorPanels.hMaterialPanel)
        MatRefreshList((HWND)g_editorPanels.hMaterialPanel);
}

static void MatLayout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int w = rc.right, h = rc.bottom;
    const int bottom = 76;
    if (HWND list = GetDlgItem(hwnd, ID_MAT_LIST))
        MoveWindow(list, 8, 28, 170, std::max(40, h - bottom - 28), TRUE);
    if (g_matCanvas)
        MoveWindow(g_matCanvas, 186, 8, std::max(80, w - 194), std::max(60, h - bottom - 8), TRUE);
    int by = h - 60;
    if (HWND b = GetDlgItem(hwnd, ID_MAT_BAKE))   MoveWindow(b, 8, by, 90, 26, TRUE);
    if (HWND b = GetDlgItem(hwnd, ID_MAT_EDIT))   MoveWindow(b, 104, by, 90, 26, TRUE);
    if (HWND b = GetDlgItem(hwnd, ID_MAT_RELOAD)) MoveWindow(b, 200, by, 80, 26, TRUE);
    if (HWND b = GetDlgItem(hwnd, ID_MAT_CLOSE))  MoveWindow(b, 286, by, 70, 26, TRUE);
    if (HWND s = GetDlgItem(hwnd, ID_MAT_STATUS)) MoveWindow(s, 8, by + 30, std::max(80, w - 16), 20, TRUE);
}

static LRESULT CALLBACK MaterialPanelProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        g_matCanvas = CreateWindowExW(0, CLASS_MATERIALCANVAS, L"",
                                      WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                      186, 8, 400, 300, hwnd, nullptr,
                                      GetModuleHandle(nullptr), nullptr);
        CreateLabel(hwnd, L"Materials", 8, 8, 150, 18, 0);
        CreateListBox(hwnd, 8, 28, 170, 300, ID_MAT_LIST);
        CreateButton(hwnd, L"Bake", 8, 400, 90, 26, ID_MAT_BAKE);
        CreateButton(hwnd, L"Edit Text", 104, 400, 90, 26, ID_MAT_EDIT);
        CreateButton(hwnd, L"Reload", 200, 400, 80, 26, ID_MAT_RELOAD);
        CreateButton(hwnd, L"Close", 286, 400, 70, 26, ID_MAT_CLOSE);
        CreateLabel(hwnd, L"", 8, 430, 500, 18, ID_MAT_STATUS);
        MatRefreshList(hwnd);
        MatLayout(hwnd);
        break;
    }
    case WM_SIZE:
        MatLayout(hwnd);
        break;
    case WM_COMMAND: {
        const int id = LOWORD(w);
        const int code = HIWORD(w);
        if (id == ID_MAT_LIST && code == LBN_SELCHANGE) {
            HWND list = GetDlgItem(hwnd, ID_MAT_LIST);
            const int sel = (int)SendMessageA(list, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < (int)g_matPaths.size()) {
                g_matPath = g_matPaths[(size_t)sel];
                MatLoadPreview(hwnd);
            }
            break;
        }
        if (id == ID_MAT_BAKE) {
            if (g_matPath.empty()) { MessageBoxA(hwnd, "Select a material first.", "Bake", MB_OK); break; }
            std::string log;
            const bool ok = BakeMaterialFile(g_matPath, &log);
            ozmat::MaterialRegistry::Instance().Reload();
            MatLoadPreview(hwnd);
            MessageBoxA(hwnd, log.empty() ? (ok ? "Baked." : "Bake failed.") : log.c_str(),
                        ok ? "Bake Material" : "Bake Failed",
                        MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
        } else if (id == ID_MAT_EDIT) {
            if (!g_matPath.empty())
                ShellExecuteA(hwnd, "open", g_matPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else if (id == ID_MAT_RELOAD) {
            ozmat::MaterialRegistry::Instance().Reload();
            MatRefreshList(hwnd);
        } else if (id == ID_MAT_CLOSE) {
            ShowMaterialGraph(false);
        }
        break;
    }
    case WM_GETMINMAXINFO:
        EnforceMinTrackSize(l, 420, 260);
        break;
    case WM_CLOSE:
        ShowMaterialGraph(false);
        break;
    case WM_DESTROY:
        g_matCanvas = nullptr;
        g_editorPanels.hMaterialPanel = nullptr;
        break;
    default:
        return DefWindowProc(hwnd, msg, w, l);
    }
    return 0;
}
