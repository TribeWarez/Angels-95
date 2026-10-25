// ============================================================================
// SurfacePropsPanel.cpp
//
// FRAGMENT - not a standalone translation unit. Included by ../UiShell.cpp,
// which is the single TU for the whole UI layer. That is deliberate: see
// UiShell.cpp for why the layer is built as a unity build rather than as 16
// separate objects.
// ============================================================================

// =====================================================================
// Public API â€” Create / Destroy
// =====================================================================
// =====================================================================
// Surface Properties (UT99-style, per-face)
//
// Right-click a brush FACE in the viewport -> "Surface Properties (N
// Selected)" -> this window. Modelled on the UT99 dialog: a Flags tab of
// checkboxes, an Alignment tab for UV/pan/texture, and a Stats tab for alpha
// and glow.
//
// The face is identified geometrically (dominant axis of the clicked
// triangle's normal) by Main.cpp's PickSurfaceFace, using the same rule the
// renderer's per-face mesh split uses, so the face the user clicks is provably
// the face whose properties are edited here.
//
// A selection can span several faces of one brush; Apply writes to all of them,
// which is why the title reads "(N Selected)".
// =====================================================================
static const wchar_t* CLASS_SURFACEPROPS = L"OzSurfaceProps";

// Flag checkboxes in the three-column layout of the UT99 dialog.
//
// `implemented` is the whole point of this table. Every one of these flags used to
// be a live checkbox whether or not anything consumed it - 14 of them had NO
// consumer anywhere in the engine, so ticking one saved a bit to the .ozone and
// then did nothing, with no warning. That is worse than not offering it: the
// author has no way to tell which half of the dialog works.
//
//   true   - the flag reaches the renderer or the draw pipeline. Live.
//   false  - nothing reads this bit. The row is HIDDEN, not disabled: a greyed row
//            still implies the feature is coming, and there are 14 of them, which
//            is most of a column of checkboxes that can never be ticked.
//
// SURF_MIRROR is the remaining exception: kept VISIBLE but disabled and
// relabelled "(planned)". It is a real feature with a real design (a reflection
// probe) rather than vocabulary nobody filled in, so the dialog should say so
// rather than pretend. SURF_PORTAL used to be in this group and is now LIVE - it
// is the sky portal (the face shows the sky-zone view; see Renderer/SkyPortal.hpp).
//
// SURF_COLLISION_PROXY is absent entirely - AutoConvex generates it, so a
// checkbox would let someone hand-write "invisible but still solid".
//
// SURF_SHADOW_HI / SURF_SHADOW_LO / SURF_DIRTY_SHADOWS are hidden for the same
// reason the flags do nothing: there is no shadow pass. See Wiki/Lighting-Plan.md.
struct SurfaceFlagRow { const wchar_t* label; uint32_t bit; bool implemented; };
static const SurfaceFlagRow kSurfaceFlagRows[] = {
    // --- live: column 1 ---
    { L"Invisible",           SURF_INVISIBLE,       true },
    { L"Masked",              SURF_MASKED,          true },
    { L"Translucent",         SURF_TRANSLUCENT,     true },
    { L"AlphaBlend",          SURF_ALPHABLEND,      true },
    { L"Modulated",           SURF_MODULATED,       true },
    { L"Fake Backdrop",       SURF_FAKEBACKDROP,    true },
    { L"Fake Lit",            SURF_FAKE_LIT,        true },
    { L"Unlit",               SURF_UNLIT,           true },
    { L"Two Sided",           SURF_TWO_SIDED,       true },
    { L"U-Pan",               SURF_PAN_U,           true },
    { L"V-Pan",               SURF_PAN_V,           true },
    { L"Glow",                SURF_GLOW,            true },
    // --- live: column 2 ---
    { L"No Fog",              SURF_NO_FOG,          true },
    { L"No BSP Cuts",         SURF_NO_BSP_CUTS,     true },
    { L"Sky Portal",          SURF_PORTAL,          true },
    // --- planned: shown, disabled ---
    { L"Mirror (planned)",    SURF_MIRROR,          false },
};
static const int kSurfaceFlagCount = (int)(sizeof(kSurfaceFlagRows) / sizeof(kSurfaceFlagRows[0]));

// Rows per flag column. 12 fits a third column at the derived column width.
static const int kSpfFlagRows = 12;

// Control ids. ID_SPF_* is its own block (700+) so it can never collide with
// any other panel's range.
static const int ID_SPF_TAB        = 700;
static const int ID_SPF_APPLY      = 701;
static const int ID_SPF_CLOSE      = 702;
static const int ID_SPF_RESET      = 703;
static const int ID_SPF_SLOTSEL    = 705;
static const int ID_SPF_SCOPESEL   = 706;   // "Selected faces" / "Whole brush"
static const int ID_SPF_FLAG_BASE  = 710;   // + i
static const int ID_SPF_F_BASE     = 760;   // + i  (float fields)
static const int ID_SPF_TEXPATH    = 800;   // the one free-text field
static const int ID_SPF_MATERIAL   = 801;   // baked material name

// Scope choices, kept in the same order as the combo's entries.
enum SpfScope { SPF_SCOPE_FACES = 0, SPF_SCOPE_BRUSH = 1 };

// The face whose properties the controls currently show. With a multi-face
// selection this is the first selected face; Apply still writes to all of them.
static oz::surface::SurfaceFace s_spfShownFace = oz::surface::FACE_PX;
static int  s_spfTab = 0;
static HWND s_hSpfTab = nullptr;
static HWND g_spfPages[3] = {nullptr, nullptr, nullptr};
static oz::surface::SurfaceProps s_spfEdit{};   // working copy shown in the UI

// Numeric fields, in creation order. Kept in a table so read-back cannot drift
// from the controls.
enum SpfField {
    SPF_UV_SCALE_U = 0, SPF_UV_SCALE_V, SPF_UV_OFF_U, SPF_UV_OFF_V,
    SPF_PAN_U, SPF_PAN_V, SPF_ALPHA, SPF_CUTOFF,
    SPF_GLOW_R, SPF_GLOW_G, SPF_GLOW_B, SPF_GLOW_SCALE,
    SPF_FIELD_COUNT
};
static const wchar_t* const kSpfFieldLabel[SPF_FIELD_COUNT] = {
    L"U Scale:",  L"V Scale:",  L"U Offset:", L"V Offset:",
    L"U Pan (t/s):", L"V Pan (t/s):", L"Alpha:", L"Alpha Cutoff:",
    L"Glow R:", L"Glow G:", L"Glow B:", L"Glow Scale:",
};
static const int kSpfRowH = 24;

static float SpfGetField(SpfField f) {
    switch (f) {
        case SPF_UV_SCALE_U:  return s_spfEdit.uvScaleU;
        case SPF_UV_SCALE_V:  return s_spfEdit.uvScaleV;
        case SPF_UV_OFF_U:   return s_spfEdit.uvOffsetU;
        case SPF_UV_OFF_V:   return s_spfEdit.uvOffsetV;
        case SPF_PAN_U:      return s_spfEdit.panU;
        case SPF_PAN_V:      return s_spfEdit.panV;
        case SPF_ALPHA:      return s_spfEdit.alpha;
        case SPF_CUTOFF:     return s_spfEdit.alphaCutoff;
        case SPF_GLOW_R:     return s_spfEdit.glowR;
        case SPF_GLOW_G:     return s_spfEdit.glowG;
        case SPF_GLOW_B:     return s_spfEdit.glowB;
        case SPF_GLOW_SCALE: return s_spfEdit.glowScale;
        default: return 0.0f;
    }
}

static void SpfSetField(SpfField f, float v) {
    switch (f) {
        case SPF_UV_SCALE_U:  s_spfEdit.uvScaleU = v; break;
        case SPF_UV_SCALE_V:  s_spfEdit.uvScaleV = v; break;
        case SPF_UV_OFF_U:   s_spfEdit.uvOffsetU = v; break;
        case SPF_UV_OFF_V:   s_spfEdit.uvOffsetV = v; break;
        case SPF_PAN_U:      s_spfEdit.panU = v; break;
        case SPF_PAN_V:      s_spfEdit.panV = v; break;
        case SPF_ALPHA:      s_spfEdit.alpha = v; break;
        case SPF_CUTOFF:     s_spfEdit.alphaCutoff = v; break;
        case SPF_GLOW_R:     s_spfEdit.glowR = v; break;
        case SPF_GLOW_G:     s_spfEdit.glowG = v; break;
        case SPF_GLOW_B:     s_spfEdit.glowB = v; break;
        case SPF_GLOW_SCALE: s_spfEdit.glowScale = v; break;
        default: break;
    }
}

static void SpfFloatRow(HWND parent, int x, int& y, int id, float value) {
    wchar_t buf[48];
    swprintf(buf, 48, L"%g", value);
    CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", buf,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        x + 104, y, 92, 22, parent, (HMENU)(INT_PTR)id, g_hInst, nullptr);
}

// Load the renderable + face into the working copy, clamping values that the
// renderer would reject anyway.
//
// Brush-wide scope reads surface.def directly; face scope reads the RESOLVED face
// (override, else default) so the controls show what will actually be applied.
static void SpfLoadFromTarget() {
    OzoneRenderable* r = OzoneLoader::Instance().Get(g_editorPanels.surfaceRenderable);
    if (!r) { s_spfEdit = oz::surface::SurfaceProps{}; return; }
    s_spfEdit = g_editorPanels.surfaceBrushWide
                    ? r->surface.def
                    : r->surface.Resolve(s_spfShownFace);
    if (s_spfEdit.alpha < 0.0f) s_spfEdit.alpha = 0.0f;
    if (s_spfEdit.alpha > 1.0f) s_spfEdit.alpha = 1.0f;
    if (s_spfEdit.alphaCutoff < 0.0f) s_spfEdit.alphaCutoff = 0.0f;
    // A zero UV scale is NOT rewritten to 1 here. It used to be, which silently
    // discarded a deliberate collapse to zero - and the mirror image problem is
    // that ApplyRenderableUV divides by nothing, so scale 0 is legal and means
    // "sample one texel". Keep it representable.
}

// Flags tab: a Scope row, then one checkbox per flag in three columns.
//
// Column width is derived from the CLIENT width instead of being hardcoded to 168.
// Three columns of 168 plus the left margin needs 516px, and the default window is
// 520 wide OUTSIDE its frame, i.e. about 504 of client - so the third column was
// clipped and two flags were unreachable. Deriving it means the layout fits
// whatever size the window actually is (see SpfFit).
static void SpfBuildFlagsPage(HWND hwnd) {
    RECT pr; GetClientRect(hwnd, &pr);
    const int x0 = 12, y0 = 8;
    const int colW = ((pr.right - x0 - 8) / 3) > 120 ? ((pr.right - x0 - 8) / 3) : 120;

    CreateLabel(hwnd, L"Apply to:", x0, y0, 70, 20, 0);
    HWND hScope = CreateCtrl(hwnd, L"COMBOBOX", L"", x0 + 74, y0, colW * 2 - 74, 200,
                             ID_SPF_SCOPESEL, CBS_DROPDOWNLIST);
    SendMessage(hScope, CB_ADDSTRING, 0, (LPARAM)L"Selected faces");
    SendMessage(hScope, CB_ADDSTRING, 0, (LPARAM)L"Whole brush (brush default)");
    SendMessage(hScope, CB_SETCURSEL,
                g_editorPanels.surfaceBrushWide ? SPF_SCOPE_BRUSH : SPF_SCOPE_FACES, 0);

    const int fy0 = y0 + kSpfRowH + 8;

    // Two passes over the table, because "hide" and "disable" need different
    // layouts: a hidden row must not occupy a cell, or the live rows stay spread
    // across three sparse columns. The planned rows DO occupy cells (they are
    // visible), so the index counter runs over the same sequence either way - it
    // is a RENDERED-row counter, not a table-index counter, and that is the whole
    // reason the control ids are ID_SPF_FLAG_BASE + <table index> rather than
    // + <rendered row>.
    int col = 0, row = 0;
    for (int i = 0; i < kSurfaceFlagCount; i++) {
        const SurfaceFlagRow& f = kSurfaceFlagRows[i];
        const bool shown = f.implemented || f.bit == SURF_PORTAL || f.bit == SURF_MIRROR;
        if (!shown) continue;
        if (row >= kSpfFlagRows) { row = 0; col++; }

        CreateCtrl(hwnd, L"BUTTON", f.label,
                   x0 + col * colW, fy0 + row * kSpfRowH, colW - 10, 20,
                   ID_SPF_FLAG_BASE + i, BS_AUTOCHECKBOX | WS_TABSTOP);
        HWND c = GetDlgItem(hwnd, ID_SPF_FLAG_BASE + i);
        SendMessage(c, BM_SETCHECK, s_spfEdit.Has(f.bit) ? BST_CHECKED : BST_UNCHECKED, 0);
        // A planned flag keeps its authored value on screen but cannot be toggled,
        // so a level that hand-set the bit does not silently lose it on the next
        // unrelated Apply through this dialog.
        if (!f.implemented) EnableWindow(c, FALSE);
        row++;
    }
}

// Alignment tab: tileset slot, free-placement texture, UV transform and U/V pan.
static void SpfBuildAlignPage(HWND hwnd) {
    int x = 12, y = 8;
    CreateLabel(hwnd, L"Texture slot:", x, y, 100, 20, 0);
    HWND hCombo = CreateCtrl(hwnd, L"COMBOBOX", L"", x + 104, y, 120, 240,
                            ID_SPF_SLOTSEL, CBS_DROPDOWNLIST);
    // 0 = "keep the brush's own texture", then one entry per tileset texture.
    SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)L"(brush default)");
    const int n = OzoneLoader::Instance().TilesetCount();
    for (int i = 1; i <= n; i++) {
        wchar_t buf[32];
        swprintf(buf, 32, L"%d", i);
        SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)buf);
    }
    int sel = s_spfEdit.texSlot;
    if (sel < 0 || sel > n) sel = 0;
    SendMessage(hCombo, CB_SETCURSEL, sel, 0);
    y += kSpfRowH + 4;

    CreateLabel(hwnd, L"Face texture (optional):", x, y, 200, 20, 0);
    y += kSpfRowH - 2;
    {
        std::wstring wp(s_spfEdit.texPath.begin(), s_spfEdit.texPath.end());
        CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", wp.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            x, y, 440, 22, hwnd, (HMENU)(INT_PTR)ID_SPF_TEXPATH, g_hInst, nullptr);
    }
    y += kSpfRowH + 6;

    CreateLabel(hwnd, L"Material (optional):", x, y, 200, 20, 0);
    y += kSpfRowH - 2;
    {
        std::wstring wm(s_spfEdit.material.begin(), s_spfEdit.material.end());
        CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", wm.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            x, y, 440, 22, hwnd, (HMENU)(INT_PTR)ID_SPF_MATERIAL, g_hInst, nullptr);
    }
    y += kSpfRowH + 6;

    for (int i = SPF_UV_SCALE_U; i <= SPF_PAN_V; i++) {
        CreateLabel(hwnd, kSpfFieldLabel[i], x, y, 100, 20, 0);
        SpfFloatRow(hwnd, x, y, ID_SPF_F_BASE + i, SpfGetField((SpfField)i));
        y += kSpfRowH;
    }
}

// Stats tab: alpha, alpha cutoff and the glow colour/scale.
static void SpfBuildStatsPage(HWND hwnd) {
    int x = 12, y = 8;
    for (int i = SPF_ALPHA; i < SPF_FIELD_COUNT; i++) {
        CreateLabel(hwnd, kSpfFieldLabel[i], x, y, 100, 20, 0);
        SpfFloatRow(hwnd, x, y, ID_SPF_F_BASE + i, SpfGetField((SpfField)i));
        y += kSpfRowH;
    }
    y += 8;
    CreateLabel(hwnd,
        L"Flags live on the Flags tab. Apply writes to EVERY selected face.\n"
        L"Masked needs Alpha Cutoff > 0.  Glow needs a non-zero colour.\n"
        L"U-Pan / V-Pan scroll the texture; speed is in texture units per second.",
        x, y, 480, 60, 0);
}

static void SpfShowPage(int page) {
    for (int i = 0; i < 3; i++)
        if (g_spfPages[i]) ShowWindow(g_spfPages[i], (i == page) ? SW_SHOW : SW_HIDE);
    s_spfTab = page;
}

// Find a control that lives on one of the three tab PAGES.
//
// The tab pages are STATIC child windows of the dialog and every checkbox, edit
// and combo is a child of a PAGE, not of the dialog. GetDlgItem(hwnd, id) only
// searches IMMEDIATE children, so reading controls through the dialog handle
// found nothing at all: Apply then re-posted the values loaded when the dialog
// opened, which made every Flags/Alignment/Stats edit silently do nothing.
//
// (Buttons and the tab control ARE direct children of the dialog, so the layout
// code may keep using GetDlgItem on it.)
static HWND SpfFindControl(HWND dialog, int id) {
    if (!dialog) return nullptr;
    if (HWND direct = GetDlgItem(dialog, id)) return direct;
    for (int i = 0; i < 3; i++) {
        if (!g_spfPages[i]) continue;
        if (HWND c = GetDlgItem(g_spfPages[i], id)) return c;
    }
    return nullptr;
}

// Read every control back into the working copy.
static void SpfReadControls(HWND hwnd) {
    for (int i = 0; i < kSurfaceFlagCount; i++) {
        HWND c = SpfFindControl(hwnd, ID_SPF_FLAG_BASE + i);
        if (!c) continue;
        s_spfEdit.Set(kSurfaceFlagRows[i].bit,
                      SendMessage(c, BM_GETCHECK, 0, 0) == BST_CHECKED);
    }
    for (int i = 0; i < SPF_FIELD_COUNT; i++) {
        HWND c = SpfFindControl(hwnd, ID_SPF_F_BASE + i);
        if (!c) continue;
        wchar_t buf[64];
        GetWindowTextW(c, buf, 64);
        // Validation lives here, not in ES_NUMBER: Glow R may legitimately be
        // negative, and a blank field must keep its value rather than become 0.
        wchar_t* end = nullptr;
        const double v = wcstod(buf, &end);
        if (end == buf) continue;
        SpfSetField((SpfField)i, (float)v);
    }
    if (HWND c = SpfFindControl(hwnd, ID_SPF_SLOTSEL)) {
        const int s = (int)SendMessage(c, CB_GETCURSEL, 0, 0);
        s_spfEdit.texSlot = (s > 0) ? s : 0;
    }
    if (HWND c = SpfFindControl(hwnd, ID_SPF_TEXPATH)) {
        wchar_t buf[512];
        GetWindowTextW(c, buf, 512);
        char out[512] = {0};
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, 512, nullptr, nullptr);
        s_spfEdit.texPath = out;
    }
    if (HWND c = SpfFindControl(hwnd, ID_SPF_MATERIAL)) {
        wchar_t buf[256];
        GetWindowTextW(c, buf, 256);
        char out[256] = {0};
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, 256, nullptr, nullptr);
        s_spfEdit.material = out;
    }
    // The Scope combo lives on the Flags page, so it is read here too - otherwise
    // switching to the Stats tab and pressing Apply would silently discard it,
    // exactly like the texPath field used to.
    if (HWND c = SpfFindControl(hwnd, ID_SPF_SCOPESEL)) {
        g_editorPanels.surfaceBrushWide =
            (SendMessage(c, CB_GETCURSEL, 0, 0) == SPF_SCOPE_BRUSH);
    }
}

// Fill the event from the working copy. Every field the dialog can edit travels
// HERE, on the event - none of it is read from live panel state at drain time.
static ed::SurfaceEdit SpfBuildEdit() {
    ed::SurfaceEdit ev;
    ev.renderable = g_editorPanels.surfaceRenderable;
    // The mask is load-bearing: ApplyToSelection is a deliberate no-op on an empty
    // mask so it can never mean "all six faces", and carrying the mask in the event
    // means the handler cannot read a DIFFERENT selection than the one the user was
    // shown. Brush-wide does NOT reuse mask 0 for the same reason.
    ev.faceMask    = g_editorPanels.surfaceFaceMask;
    ev.brushWide   = g_editorPanels.surfaceBrushWide;
    ev.flags       = s_spfEdit.flags;
    ev.glowR       = s_spfEdit.glowR;
    ev.glowG       = s_spfEdit.glowG;
    ev.glowB       = s_spfEdit.glowB;
    ev.glowScale   = s_spfEdit.glowScale;
    ev.alpha       = s_spfEdit.alpha;
    ev.alphaCutoff = s_spfEdit.alphaCutoff;
    ev.uvScaleU    = s_spfEdit.uvScaleU;
    ev.uvScaleV    = s_spfEdit.uvScaleV;
    ev.uvOffsetU   = s_spfEdit.uvOffsetU;
    ev.uvOffsetV   = s_spfEdit.uvOffsetV;
    ev.panU        = s_spfEdit.panU;
    ev.panV        = s_spfEdit.panV;
    ev.texSlot     = s_spfEdit.texSlot;
    ev.texPath     = s_spfEdit.texPath;
    ev.material    = s_spfEdit.material;
    return ev;
}

static void SpfBuildAll(HWND hwnd) {
    for (int i = 0; i < 3; i++) {
        if (g_spfPages[i]) { DestroyWindow(g_spfPages[i]); g_spfPages[i] = nullptr; }
    }
    RECT rc; GetClientRect(hwnd, &rc);
    const int px = 4, py = 32;
    const int pw = rc.right - px * 2, ph = rc.bottom - py - 46;
    for (int i = 0; i < 3; i++) {
        g_spfPages[i] = CreateWindowEx(0, L"STATIC", L"", WS_CHILD, px, py, pw, ph,
                                       hwnd, nullptr, g_hInst, nullptr);
        if (i == 0)      SpfBuildFlagsPage(g_spfPages[i]);
        else if (i == 1) SpfBuildAlignPage(g_spfPages[i]);
        else             SpfBuildStatsPage(g_spfPages[i]);
    }
    SpfShowPage(s_spfTab);
}

// Position the tab control and the three bottom buttons for the CURRENT client
// size. Both sets of controls are hand-placed and there is no layout manager, so
// this is the only thing standing between a resize and a dialog whose Apply button
// has fallen off the bottom. Called once after creation and again on WM_SIZE.
//
// 30px for the tab strip rather than 26: the default 8.4pt tab face wants
// 21 + 6 padding, so at 26 the tab labels were clipped by a few pixels.
static void SpfFitWindow(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    if (s_hSpfTab) MoveWindow(s_hSpfTab, 4, 4, rc.right - 8, 30, TRUE);

    const int bw = 84, bh = 26, by = rc.bottom - bh - 8;
    struct { int id; int x; } btns[3] = {
        { ID_SPF_APPLY, rc.right - bw * 3 - 20 },
        { ID_SPF_RESET, rc.right - bw * 2 - 12 },
        { ID_SPF_CLOSE, rc.right - bw - 8 },
    };
    for (const auto& b : btns)
        if (HWND h = GetDlgItem(hwnd, b.id))
            MoveWindow(h, b.x, by, bw, bh, TRUE);
}

static LRESULT CALLBACK SurfacePropsProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_COMMAND: {
        const int id = LOWORD(w);
        if (id == ID_SPF_APPLY) {
            SpfReadControls(hwnd);
            g_editorPanels.surfaceEdit = s_spfEdit;
            ed::EventBus::instance().post(ed::Ev::ApplySurface, SpfBuildEdit());
            return 0;
        }
        if (id == ID_SPF_RESET) {
            // Reset carries the same target/mask/scope as Apply, so a reset cannot
            // land on faces the user had not selected - nor wipe the brush-wide
            // default when the dialog was showing faces (or vice versa).
            SpfReadControls(hwnd);
            ed::EventBus::instance().post(ed::Ev::ResetSurface, SpfBuildEdit());
            return 0;
        }
        if (id == ID_SPF_CLOSE) {
            ShowSurfaceProps(false, -1, 0);
            return 0;
        }
        return 0;
    }
    case WM_NOTIFY: {
        LPNMHDR nh = (LPNMHDR)l;
        if (nh->idFrom == ID_SPF_TAB && nh->code == TCN_SELCHANGE) {
            // Keep edits made on the page being left, otherwise switching tabs
            // silently discards them.
            SpfReadControls(hwnd);
            s_spfTab = TabCtrl_GetCurSel(s_hSpfTab);
            SpfShowPage(s_spfTab);
        }
        return 0;
    }
    case WM_CLOSE:   ShowSurfaceProps(false, -1, 0); return 0;
    case WM_DESTROY: g_editorPanels.hSurfaceProps = nullptr; return 0;
    case WM_SIZE: {
        // Every child here is positioned once, by hand, in ShowSurfaceProps /
        // SpfBuildAll - there is no layout manager. So resizing used to leave the
        // tab control, the three page frames and the three buttons at their old
        // rects while the window itself moved: the Flags columns stopped lining up
        // with the page and Apply ended up outside the client area on a narrow
        // window. Reflowing on resize is the fix; PropsPanel needed the same lesson
        // for the same reason.
        if (!s_hSpfTab) break;
        SpfReadControls(hwnd);          // keep unsaved edits across the reflow
        SpfFitWindow(hwnd);
        SpfBuildAll(hwnd);
        return 0;
    }
    default: break;
    }
    return DefWindowProc(hwnd, msg, w, l);
}

void ShowSurfaceProps(bool show, int renderable, uint32_t faceMask) {
    ShowSurfacePropsScoped(show, renderable, faceMask, false);
}

// Narrow-scope overload: exactly the pre-existing behaviour (selected faces), kept
// so callers that only ever mean "these faces" - the viewport context menu's
// IDM_SURFACE_RESET - cannot accidentally start writing the brush-wide default.
void ShowSurfacePropsScoped(bool show, int renderable, uint32_t faceMask,
                           bool brushWide) {
    g_editorPanels.showSurfaceProps = show;
    if (!show) {
        if (g_editorPanels.hSurfaceProps)
            ShowWindow((HWND)g_editorPanels.hSurfaceProps, SW_HIDE);
        return;
    }
    if (faceMask == 0) {
        MessageBoxA(nullptr, "No surface face is selected.\n\n"
                     "Right-click directly on a brush face in the viewport.",
                     "Surface Properties", MB_OK | MB_ICONINFORMATION);
        return;
    }
    g_editorPanels.surfaceRenderable = renderable;
    g_editorPanels.surfaceFaceMask = faceMask;
    // Brush-wide still REQUIRES a non-zero mask. The dialog was opened by
    // right-clicking at least one face, and faceMask is what tells it which faces
    // to show; the scope only decides where Apply writes.
    g_editorPanels.surfaceBrushWide = brushWide;

    // Show the first selected face's properties; Apply still covers all of them.
    s_spfShownFace = oz::surface::FACE_PX;
    for (int f = 0; f < oz::surface::FACE_COUNT; f++) {
        if (faceMask & (1u << f)) { s_spfShownFace = (oz::surface::SurfaceFace)f; break; }
    }
    SpfLoadFromTarget();
    if (!g_editorPanels.hSurfaceProps) return;
    HWND hwnd = (HWND)g_editorPanels.hSurfaceProps;

    // Title mirrors the UT99 dialog: "N Surface(s) : px,ny,...".
    int n = 0;
    std::string faceList;
    for (int f = 0; f < oz::surface::FACE_COUNT; f++) {
        if (!(faceMask & (1u << f))) continue;
        if (n++) faceList += ",";
        faceList += oz::surface::FaceName((oz::surface::SurfaceFace)f);
    }
    char title[192];
    if (brushWide)
        snprintf(title, sizeof(title), "BRUSH DEFAULT (%d Surface%s shown)", n,
                 (n == 1) ? "" : "s");
    else
        snprintf(title, sizeof(title), "%d Surface%s : %s", n, (n == 1) ? "" : "s",
                 faceList.c_str());
    wchar_t wtitle[192];
    MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, 192);
    SetWindowTextW(hwnd, wtitle);

    // Tear down the previous tab/pages but keep the Apply/Reset/Close buttons,
    // which are the window's persistent furniture.
    HWND child = GetWindow(hwnd, GW_CHILD);
    while (child) {
        HWND next = GetWindow(child, GW_HWNDNEXT);
        const int cid = GetDlgCtrlID(child);
        if (cid != ID_SPF_APPLY && cid != ID_SPF_RESET && cid != ID_SPF_CLOSE)
            DestroyWindow(child);
        child = next;
    }
    for (int i = 0; i < 3; i++) g_spfPages[i] = nullptr;
    s_hSpfTab = nullptr;

    RECT rc; GetClientRect(hwnd, &rc);
    s_hSpfTab = CreateCtrl(hwnd, WC_TABCONTROLW, L"", 4, 4, rc.right - 8, 30,
                           ID_SPF_TAB, TCS_TABS | WS_TABSTOP);
    {
        static const wchar_t* tabs[3] = {L"Flags", L"Alignment", L"Stats"};
        for (int i = 0; i < 3; i++) {
            TCITEMW tc = {};
            tc.mask = TCIF_TEXT;
            tc.pszText = (LPWSTR)tabs[i];
            TabCtrl_InsertItem(s_hSpfTab, i, &tc);
        }
    }
    TabCtrl_SetCurSel(s_hSpfTab, s_spfTab);

    const int bw = 84;
    CreateButton(hwnd, L"Apply", 0, 0, bw, 26, ID_SPF_APPLY);
    CreateButton(hwnd, L"Reset", 0, 0, bw, 26, ID_SPF_RESET);
    CreateButton(hwnd, L"Close", 0, 0, bw, 26, ID_SPF_CLOSE);
    SpfFitWindow(hwnd);

    SpfBuildAll(hwnd);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
}

// Re-read from the renderable and rebuild, so the controls show what was
// actually stored (including any clamping) after an Apply.
void SurfacePropsRefresh(void* hwnd) {
    if (!hwnd) return;
    HWND h = (HWND)hwnd;
    if (!IsWindow(h)) return;
    SpfLoadFromTarget();
    SpfBuildAll(h);
}

