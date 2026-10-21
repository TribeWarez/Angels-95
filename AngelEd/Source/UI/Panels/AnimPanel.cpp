// ============================================================================
// AnimPanel.cpp
//
// FRAGMENT - not a standalone translation unit. Included by ../UiShell.cpp,
// which is the single TU for the whole UI layer. That is deliberate: see
// UiShell.cpp for why the layer is built as a unity build rather than as 16
// separate objects.
// ============================================================================

// =====================================================================
// Animation / vertex-keyframe tool (Phase B: clips + playback + scrub)
// =====================================================================
static const int ID_AN_CLOSE  = 100;
static const int ID_AN_LIST   = 101;
static const int ID_AN_NEW    = 102;
static const int ID_AN_DELETE = 103;
static const int ID_AN_SAVE   = 104;
static const int ID_AN_PLAY   = 105;
static const int ID_AN_PAUSE  = 106;
static const int ID_AN_STOP   = 107;
static const int ID_AN_TIME   = 108;
static const int ID_AN_FPS    = 109;
static const int ID_AN_LOOP   = 110;
static const int ID_AN_STATUS = 111;
static const int ID_AN_ADDKEY = 112;
static const int ID_AN_DELKEY = 113;
static const int ID_AN_EDITVERTS = 114;
static const int ID_AN_UNDO = 115;
static const int ID_AN_REDO = 116;
static const int ID_AN_SELALL = 117;
static const int ID_AN_CLRSEL = 118;

// Returns the AnimatedMesh for the currently targeted mesh node, or nullptr.
static oz::AnimatedMesh* AnimTargetMesh() {
    MeshObjectNode* n = PawnSystem::Instance().GetMeshObject(g_editorPanels.animTargetMesh);
    if (!n || !n->mesh) return nullptr;
    return dynamic_cast<oz::AnimatedMesh*>(n->mesh.get());
}

static void PopulateAnimPanel(HWND hwnd, HWND hList, HWND hTime, HWND hFps, HWND hStatus, HWND hLoop) {
    SendMessage(hList, LB_RESETCONTENT, 0, 0);
    MeshObjectNode* n = PawnSystem::Instance().GetMeshObject(g_editorPanels.animTargetMesh);
    oz::AnimatedMesh* am = AnimTargetMesh();

    if (!n) {
        SetWindowTextA(hStatus, "No mesh selected. Select a GameEngine.Mesh object.");
        return;
    }
    if (!am) {
        SetWindowTextA(hStatus, n->animFile.empty()
            ? "Mesh has no anim file. Use \"Convert to Animated\" in Entity Properties."
            : "Anim file not loaded (check the path).");
        return;
    }

    // Clip list
    for (int i = 0; i < am->ClipCount(); i++)
        SendMessageA(hList, LB_ADDSTRING, 0, (LPARAM)am->ClipName(i));
    int cur = am->FindClip(g_editorPanels.animClipName);
    if (cur < 0 && am->ClipCount() > 0) cur = 0;
    if (cur >= 0) {
        SendMessage(hList, LB_SETCURSEL, cur, 0);
        g_editorPanels.animClipName = am->ClipName(cur);
    }

    const ozanim::Clip* clip = am->GetAnimation().FindClip(g_editorPanels.animClipName);
    float dur = clip ? clip->Duration() : 0.0f;
    wchar_t buf[128];
    if (clip) swprintf(buf, 128, L"%d clip(s) | %d verts | dur %.2fs", am->ClipCount(),
                       am->TotalVertexCount(), dur);
    else      swprintf(buf, 128, L"%d clip(s) | %d verts", am->ClipCount(), am->TotalVertexCount());
    SetWindowTextW(hStatus, buf);

    if (clip) {
        char fb[32]; snprintf(fb, sizeof(fb), "%g", clip->fps);
        SetWindowTextA(hFps, fb);
        SendMessage(hLoop, BM_SETCHECK, clip->loop ? BST_CHECKED : BST_UNCHECKED, 0);
        g_editorPanels.animFps = clip->fps;
        g_editorPanels.animLoop = clip->loop;
        int pos = (dur > 0.0f) ? (int)(g_editorPanels.animTime / dur * 1000.0f) : 0;
        if (pos < 0) pos = 0;
        if (pos > 1000) pos = 1000;
        SendMessage(hTime, SBM_SETPOS, pos, TRUE);
    }
}

// Build an AnimIntent from the panel's CURRENT state and post it.
//
// `hwnd` is unused today but keeps the signature honest: reading fps/loop straight
// off the controls rather than off the mirrored g_editorPanels fields removes the
// possibility of posting an intent built from values the panel has already moved
// past. Pass nullptr where there is no control to read (keyboard shortcuts).
static void PostAnimIntent(ed::Ev kind, HWND hwnd) {
    ed::AnimIntent ai;
    ai.meshId   = g_editorPanels.animTargetMesh;
    ai.clipName = g_editorPanels.animClipName;
    ai.time     = g_editorPanels.animTime;
    ai.fps      = g_editorPanels.animFps;
    ai.loop     = g_editorPanels.animLoop;
    if (hwnd) {
        HWND hFps = GetDlgItem(hwnd, ID_AN_FPS);
        if (hFps) {
            char b[32] = {0};
            GetWindowTextA(hFps, b, 32);
            const float v = (float)atof(b);
            if (v > 0.0f) ai.fps = v;
        }
        HWND hLoop = GetDlgItem(hwnd, ID_AN_LOOP);
        if (hLoop)
            ai.loop = SendMessage(hLoop, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    ed::EventBus::instance().post(kind, ai);
}

static LRESULT CALLBACK AnimPanelProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    static HWND hList, hTime, hFps, hStatus, hLoop, hEditBtn;
    switch (msg) {
    case WM_CREATE: {
        hList = CreateListBox(hwnd, 8, 8, 360, 130, ID_AN_LIST);
        CreateButton(hwnd, L"New Clip", 8, 144, 72, 26, ID_AN_NEW);
        CreateButton(hwnd, L"Delete",   84, 144, 60, 26, ID_AN_DELETE);
        CreateButton(hwnd, L"Save",    148, 144, 56, 26, ID_AN_SAVE);
        CreateButton(hwnd, L"Add Key", 208, 144, 68, 26, ID_AN_ADDKEY);
        CreateButton(hwnd, L"Del Key", 280, 144, 64, 26, ID_AN_DELKEY);

        CreateLabel(hwnd, L"FPS:", 8, 180, 32, 20, 0);
        hFps = CreateCtrl(hwnd, L"EDIT", L"30", 42, 178, 46, 22, ID_AN_FPS, WS_BORDER);
        hLoop = CreateWindowEx(0, L"BUTTON", L"Loop",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            96, 178, 64, 22, hwnd, (HMENU)(INT_PTR)ID_AN_LOOP, g_hInst, nullptr);
        SendMessage(hLoop, BM_SETCHECK, BST_CHECKED, 0);
        CreateButton(hwnd, L"Play",  166, 176, 62, 26, ID_AN_PLAY);
        CreateButton(hwnd, L"Pause", 232, 176, 62, 26, ID_AN_PAUSE);
        CreateButton(hwnd, L"Stop",  298, 176, 70, 26, ID_AN_STOP);

        hTime = CreateWindowEx(0, L"SCROLLBAR", L"",
            WS_CHILD | WS_VISIBLE | SBS_HORZ, 8, 210, 360, 18,
            hwnd, (HMENU)(INT_PTR)ID_AN_TIME, g_hInst, nullptr);
        SetScrollRange(hTime, SB_CTL, 0, 1000, TRUE);

        hStatus = CreateLabel(hwnd, L"", 8, 234, 360, 40, 0);

        hEditBtn = CreateWindowEx(0, L"BUTTON", L"Edit Verts",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            8, 276, 90, 24, hwnd, (HMENU)(INT_PTR)ID_AN_EDITVERTS, g_hInst, nullptr);
        CreateButton(hwnd, L"Undo", 104, 276, 56, 24, ID_AN_UNDO);
        CreateButton(hwnd, L"Redo", 164, 276, 56, 24, ID_AN_REDO);
        CreateButton(hwnd, L"Sel All", 224, 276, 66, 24, ID_AN_SELALL);
        CreateButton(hwnd, L"Clr Sel", 294, 276, 74, 24, ID_AN_CLRSEL);
        CreateButton(hwnd, L"Close", 8, 306, 80, 26, ID_AN_CLOSE);
        break;
    }
    case WM_USER + 50:
        PopulateAnimPanel(hwnd, hList, hTime, hFps, hStatus, hLoop);
        if (hEditBtn)
            SendMessage(hEditBtn, BM_SETCHECK, g_editorPanels.animEditVerts ? BST_CHECKED : BST_UNCHECKED, 0);
        break;
    case WM_HSCROLL: {
        if ((HWND)l == hTime) {
            int pos = (int)SendMessage(hTime, SBM_GETPOS, 0, 0);
            g_editorPanels.animTimeSlider = pos;
            // Scrub is a continuous signal, but posting it is still better than a
            // sticky flag: the value travels with the event, so a drag that ends
            // and a second drag that starts cannot collapse into one write of the
            // LAST position seen rather than each position passed through.
            ed::AnimIntent ai;
            ai.meshId   = g_editorPanels.animTargetMesh;
            ai.clipName = g_editorPanels.animClipName;
            ai.time     = pos / 1000.0f;
            ed::EventBus::instance().post(ed::Ev::AnimScrub, ai);
        }
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == ID_AN_CLOSE) { ShowAnimPanel(false); break; }
        if (id == ID_AN_LIST && HIWORD(w) == LBN_SELCHANGE) {
            int s = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
            if (s >= 0 && s < (int)SendMessage(hList, LB_GETCOUNT, 0, 0)) {
                char buf[128] = {0};
                SendMessageA(hList, LB_GETTEXT, s, (LPARAM)buf);
                g_editorPanels.animClipName = buf;
                g_editorPanels.animTime = 0.0f;
                g_editorPanels.actionAnimRefresh = true;
            }
            break;
        }
        if (id == ID_AN_NEW)    { PostAnimIntent(ed::Ev::AnimNewClip, hwnd); break; }
        if (id == ID_AN_DELETE) { PostAnimIntent(ed::Ev::AnimDeleteClip, hwnd); break; }
        // Save stays a FIELD on purpose. The anim handlers set actionAnimSave
        // themselves (NewClip, DeleteClip, ApplyClipMeta all do) and the consumer
        // runs later in the SAME frame, so the file write is part of the command
        // chain rather than an independent user action. Queueing it would push the
        // write a frame later for no benefit.
        if (id == ID_AN_SAVE)   { g_editorPanels.actionAnimSave = true; break; }
        if (id == ID_AN_ADDKEY) { PostAnimIntent(ed::Ev::AnimAddKey, hwnd); break; }
        if (id == ID_AN_DELKEY) { PostAnimIntent(ed::Ev::AnimDeleteKey, hwnd); break; }
        if (id == ID_AN_EDITVERTS) { PostAnimIntent(ed::Ev::AnimToggleEdit, hwnd); break; }
        if (id == ID_AN_UNDO)   { PostAnimIntent(ed::Ev::AnimUndo, hwnd); break; }
        if (id == ID_AN_REDO)   { PostAnimIntent(ed::Ev::AnimRedo, hwnd); break; }
        if (id == ID_AN_SELALL) { PostAnimIntent(ed::Ev::AnimSelectAll, hwnd); break; }
        if (id == ID_AN_CLRSEL) { PostAnimIntent(ed::Ev::AnimClearSelection, hwnd); break; }
        if (id == ID_AN_PLAY)   { g_editorPanels.animPlaying = true;  g_editorPanels.actionAnimRefresh = true; break; }
        if (id == ID_AN_PAUSE)  { g_editorPanels.animPlaying = false; break; }
        if (id == ID_AN_STOP)   { g_editorPanels.animPlaying = false; g_editorPanels.animTime = 0.0f; g_editorPanels.actionAnimRefresh = true; break; }
        if (id == ID_AN_FPS && HIWORD(w) == EN_CHANGE) {
            char b[32] = {0};
            GetWindowTextA(hFps, b, 32);
            float v = (float)atof(b);
            if (v > 0.0f) g_editorPanels.animFps = v;
            PostAnimIntent(ed::Ev::AnimApplyClipMeta, hwnd);
            break;
        }
        if (id == ID_AN_LOOP && HIWORD(w) == BN_CLICKED) {
            g_editorPanels.animLoop =
                SendMessage(GetDlgItem(hwnd, ID_AN_LOOP), BM_GETCHECK, 0, 0) == BST_CHECKED;
            PostAnimIntent(ed::Ev::AnimApplyClipMeta, hwnd);
            break;
        }
        break;
    }
    case WM_GETMINMAXINFO:
        EnforceMinTrackSize(l, 390, 350);
        break;
    case WM_CLOSE: ShowAnimPanel(false); break;
    case WM_DESTROY: g_editorPanels.hAnimPanel = nullptr; break;
    default: return DefWindowProc(hwnd, msg, w, l);
    }
    return 0;
}

void ShowAnimPanel(bool show) {
    g_editorPanels.showAnimPanel = show;
    if (g_editorPanels.hAnimPanel) {
        ShowWindow((HWND)g_editorPanels.hAnimPanel, show ? SW_SHOW : SW_HIDE);
        if (show) SendMessage((HWND)g_editorPanels.hAnimPanel, WM_USER + 50, 0, 0);
    }
}

void RefreshAnimPanel() {
    if (g_editorPanels.hAnimPanel)
        SendMessage((HWND)g_editorPanels.hAnimPanel, WM_USER + 50, 0, 0);
}

