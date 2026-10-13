// ============================================================================
// SoundPanel.cpp
//
// FRAGMENT - not a standalone translation unit. Included by ../UiShell.cpp,
// which is the single TU for the whole UI layer. That is deliberate: see
// UiShell.cpp for why the layer is built as a unity build rather than as 16
// separate objects.
// ============================================================================

// =====================================================================
// Sound Manager v2 ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â Category tabs, volume, loop, source info
// =====================================================================
static const int ID_SOUND_LIST     = 101;
static const int ID_SOUND_REFRESH  = 102;
static const int ID_SOUND_CLOSE    = 103;
static const int ID_SOUND_PLAY     = 104;
static const int ID_SOUND_STOP     = 105;
static const int ID_SOUND_CAT_SFX  = 106;
static const int ID_SOUND_CAT_MUS  = 107;
static const int ID_SOUND_CAT_AMB  = 108;
static const int ID_SOUND_VOLUME   = 109;
static const int ID_SOUND_LOOP     = 110;
static const int ID_SOUND_SRC_LABEL= 111;

static int g_soundCategory = 0; // 0=SFX, 1=Music, 2=Ambience
static std::vector<ResourceEntry> g_sfxFiles;
static std::vector<ResourceEntry> g_musicFiles;
static std::vector<ResourceEntry> g_ambFiles;

void ShowSoundManager(bool show) {
    g_editorPanels.showSoundMgr = show;
    if (g_editorPanels.hSoundMgr)
        ShowWindow((HWND)g_editorPanels.hSoundMgr, show ? SW_SHOW : SW_HIDE);
}

void ScanSoundBrowserFiles() {
    ScanFilesAndPackages("Audio/Sounds", { ".wav", ".ogg", ".mp3" }, g_sfxFiles);
    ScanFilesAndPackages("Audio/Ambience", { ".wav", ".ogg", ".mp3" }, g_ambFiles);
    ScanFilesAndPackages("Audio/Music", { ".wav", ".ogg", ".mp3" }, g_musicFiles);
    if (g_editorPanels.hSoundMgr)
        SendMessage((HWND)g_editorPanels.hSoundMgr, WM_USER + 50, 0, 0);
}

static void SoundMgrPopulateList(HWND hList) {
    SendMessage(hList, LB_RESETCONTENT, 0, 0);
    const auto& files = (g_soundCategory == 0) ? g_sfxFiles :
                        (g_soundCategory == 1) ? g_musicFiles : g_ambFiles;
    for (const auto& snd : files)
        SendMessageA(hList, LB_ADDSTRING, 0, (LPARAM)snd.name.c_str());
}

static LRESULT CALLBACK SoundMgrProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    static HWND hList, hLoopBtn, hVolTrack, hSrcLabel;
    switch (msg) {
    case WM_CREATE: {
        int x = 10, y = 10, bw = 80;

        // Category tabs
        CreateButton(hwnd, L"SFX",      x, y, bw, 24, ID_SOUND_CAT_SFX);
        CreateButton(hwnd, L"Music",    x + bw + 4, y, bw, 24, ID_SOUND_CAT_MUS);
        CreateButton(hwnd, L"Ambience", x + (bw + 4) * 2, y, bw + 10, 24, ID_SOUND_CAT_AMB);
        y += 30;

        // Source label
        hSrcLabel = CreateLabel(hwnd, L"Source: scanning...", x, y, 300, 16, ID_SOUND_SRC_LABEL);
        y += 20;

        // Sound list
        hList = CreateListBox(hwnd, x, y, 380, 160, ID_SOUND_LIST);
        y += 166;

        // Volume slider
        CreateLabel(hwnd, L"Volume:", x, y, 50, 20, 20);
        hVolTrack = CreateWindowEx(0, TRACKBAR_CLASS, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ,
                                   x + 55, y, 180, 24, hwnd, (HMENU)ID_SOUND_VOLUME, g_hInst, nullptr);
        SendMessage(hVolTrack, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
        SendMessage(hVolTrack, TBM_SETPOS, TRUE, 80);
        y += 30;

        // Loop checkbox
        hLoopBtn = CreateCtrl(hwnd, L"BUTTON", L"Loop", x, y, 100, 22, ID_SOUND_LOOP, BS_AUTOCHECKBOX);
        y += 28;

        // Action buttons
        CreateButton(hwnd, L"Play",    x, y, 70, 26, ID_SOUND_PLAY);
        CreateButton(hwnd, L"Stop",    x + 76, y, 70, 26, ID_SOUND_STOP);
        CreateButton(hwnd, L"Refresh", x + 152, y, 70, 26, ID_SOUND_REFRESH);
        CreateButton(hwnd, L"Close",   x + 300, y, 90, 26, ID_SOUND_CLOSE);

        ScanSoundBrowserFiles();
        break;
    }
    case WM_USER + 50: {
        SoundMgrPopulateList(hList);
        // Update source label
        const char* cats[] = {"Audio/Sounds/ (SFX)", "Audio/Music/", "Audio/Ambience/"};
        SetWindowTextA(hSrcLabel, TextFormat("Source: GameData/%s", cats[g_soundCategory]));
        break;
    }
    case WM_HSCROLL: {
        if ((HWND)l == hVolTrack) {
            int vol = (int)SendMessage(hVolTrack, TBM_GETPOS, 0, 0);
            // Live slider state, not a message: the main loop re-applies the
            // volume every frame while the preview plays. Renamed off the
            // action* prefix so it does not read as a request.
            g_editorPanels.previewSoundVolume = vol;
        }
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == ID_SOUND_CLOSE) {
            ShowSoundManager(false);
        } else if (id == ID_SOUND_REFRESH) {
            ScanSoundBrowserFiles();
        } else if (id == ID_SOUND_STOP) {
            ed::EventBus::instance().post(ed::Ev::StopSoundPreview);
        } else if (id == ID_SOUND_PLAY || (id == ID_SOUND_LIST && HIWORD(w) == LBN_DBLCLK)) {
            int sel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
            const auto& files = (g_soundCategory == 0) ? g_sfxFiles :
                                (g_soundCategory == 1) ? g_musicFiles : g_ambFiles;
            if (sel >= 0 && sel < (int)files.size()) {
                // Playback settings stay FIELDS, not part of the payload: they
                // are live slider state the main loop re-reads every frame, not part
                // of the request. The PATH travels, because a category switch
                // between the click and the frame would otherwise preview from
                // the wrong list.
                g_editorPanels.previewSoundCategory = g_soundCategory;
                g_editorPanels.previewSoundLoop = (int)SendMessage(hLoopBtn, BM_GETCHECK, 0, 0);
                ed::EventBus::instance().post(ed::Ev::PlaySound, files[sel].path);
            }
        } else if (id == ID_SOUND_CAT_SFX || id == ID_SOUND_CAT_MUS || id == ID_SOUND_CAT_AMB) {
            g_soundCategory = (id == ID_SOUND_CAT_SFX) ? 0 : (id == ID_SOUND_CAT_MUS) ? 1 : 2;
            SoundMgrPopulateList(hList);
            SetWindowTextA(hSrcLabel, TextFormat("Source: %s",
                (g_soundCategory == 0) ? "GameData/Audio/Sounds/" :
                (g_soundCategory == 1) ? "GameData/Audio/Music/" : "GameData/Audio/Ambience/"));
        }
        break;
    }
    case WM_CLOSE:
        ed::EventBus::instance().post(ed::Ev::StopSoundPreview);
        ShowSoundManager(false);
        break;
    case WM_DESTROY:
        g_editorPanels.hSoundMgr = nullptr;
        break;
    default:
        return DefWindowProc(hwnd, msg, w, l);
    }
    return 0;
}

