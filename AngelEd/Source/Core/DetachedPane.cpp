// ============================================================================
// Core/DetachedPane.cpp
//
// Tier A detached viewport: a plain Win32 window with NO GL context that shows a
// pane's RenderTexture2D. The pane is rendered into its own framebuffer by the
// main render seam and uploaded here with LoadImageFromTexture -> StretchDIBits.
// This is the same render-to-texture + GDI blit the Model Browser's preview has
// used for years (Main.cpp), so it needs no second GL context and cannot disturb
// raylib's state.
//
// FRAGMENT - included by Main.cpp (single core TU). Win32 is available because
// Core/EditorShell.hpp includes WindowsCompat.hpp + <windows.h> first.
// ============================================================================

#include "PaneTarget.hpp"

static const wchar_t* kDetachedPaneClass = L"AngelEdDetachedPane";
static bool g_detachedPaneClassRegistered = false;

static LRESULT CALLBACK DetachedPaneProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    PaneTarget* t = (PaneTarget*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_SIZE:
            if (t) t->rtDirty = true;
            return 0;
        case WM_ERASEBKGND:
            return 1;   // Present paints every frame; do not flicker the background
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);
            // Fill black so a frame that has not arrived yet reads as "loading",
            // not as a stale smear of the previous camera.
            FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CLOSE: {
            const int i = t ? (int)(t - g_paneTargets) : -1;
            if (i >= 0 && i < 4) {
                // User closed the pop-out: re-attach the pane to the main layout.
                g_paneTargets[i].detached = false;
                if (g_paneTargets[i].rt.id > 0) {
                    UnloadRenderTexture(g_paneTargets[i].rt);
                    g_paneTargets[i].rt = RenderTexture2D{};
                }
                g_paneTargets[i].hwnd = nullptr;
            }
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_DESTROY:
            if (t) t->hwnd = nullptr;
            return 0;
        default:
            return DefWindowProc(hwnd, msg, w, l);
    }
}

static void DetachedPane_EnsureClass() {
    if (g_detachedPaneClassRegistered) return;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = DetachedPaneProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = kDetachedPaneClass;
    RegisterClassExW(&wc);
    g_detachedPaneClassRegistered = true;
}

// Create/recreate the pane's framebuffer at the detached window's client size.
static void DetachedPane_EnsureRT(int i) {
    PaneTarget& t = g_paneTargets[i];
    if (!t.detached || !t.hwnd) return;
    RECT rc; GetClientRect((HWND)t.hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w < 16 || h < 16) return;
    if (!t.rtDirty && t.rt.id > 0 &&
        t.rt.texture.width == w && t.rt.texture.height == h) return;
    if (t.rt.id > 0) UnloadRenderTexture(t.rt);
    t.rt = LoadRenderTexture(w, h);
    t.rtDirty = false;
}

static void DetachedPane_Detach(int i, int w, int h) {
    if (i < 0 || i > 3) return;
    DetachedPane_EnsureClass();
    PaneTarget& t = g_paneTargets[i];
    if (t.hwnd) return;   // already detached
    if (w < 320) w = 320;
    if (h < 240) h = 240;
    if (w > 1600) w = 1600;   // cap the readback cost
    if (h > 900) h = 900;
    t.detached = true;
    t.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW,
        kDetachedPaneClass, L"AngelEd Viewport",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, w, h,
        nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (!t.hwnd) {
        t.detached = false;
        return;
    }
    SetWindowLongPtr((HWND)t.hwnd, GWLP_USERDATA, (LONG_PTR)&t);
    t.rtDirty = true;
    DetachedPane_EnsureRT(i);
}

static void DetachedPane_Attach(int i) {
    if (i < 0 || i > 3) return;
    PaneTarget& t = g_paneTargets[i];
    t.detached = false;
    if (t.rt.id > 0) {
        UnloadRenderTexture(t.rt);
        t.rt = RenderTexture2D{};
    }
    if (t.hwnd) {
        HWND h = (HWND)t.hwnd;
        t.hwnd = nullptr;
        DestroyWindow(h);
    }
}

// Upload the pane's framebuffer to its window. Called once per frame after the
// 3D pass; a GPU->CPU readback is the price of needing no second GL context.
static void DetachedPane_Present(int i) {
    if (i < 0 || i > 3) return;
    PaneTarget& t = g_paneTargets[i];
    if (!t.detached || !t.hwnd || t.rt.id == 0) return;

    Image img = LoadImageFromTexture(t.rt.texture);
    if (!img.data) return;
    ImageFlipVertical(&img);   // GL textures are bottom-up; GDI wants top-down
    unsigned char* px = (unsigned char*)img.data;
    for (int p = 0; p < img.width * img.height; p++) {
        unsigned char tmp = px[p * 4];
        px[p * 4] = px[p * 4 + 2];
        px[p * 4 + 2] = tmp;
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = img.width;
    bmi.bmiHeader.biHeight = -img.height;   // top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC dc = GetDC((HWND)t.hwnd);
    RECT rc; GetClientRect((HWND)t.hwnd, &rc);
    StretchDIBits(dc, 0, 0, rc.right, rc.bottom,
                  0, 0, img.width, img.height,
                  img.data, &bmi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC((HWND)t.hwnd, dc);
    UnloadImage(img);
}

static void DetachedPane_Shutdown() {
    for (int i = 0; i < 4; i++) DetachedPane_Attach(i);
}
