// ============================================================================
// ScriptPanel.cpp
//
// FRAGMENT - not a standalone translation unit. Included by ../UiShell.cpp,
// which is the single TU for the whole UI layer. That is deliberate: see
// UiShell.cpp for why the layer is built as a unity build rather than as 16
// separate objects.
// ============================================================================

// =====================================================================
// Script Manager â€” LightningScript (.ozls) browser + editor launcher
// =====================================================================
static const int ID_SCRIPT_CLOSE  = 100;
static const int ID_SCRIPT_LIST   = 101;
static const int ID_SCRIPT_REFRESH = 102;
static const int ID_SCRIPT_EDIT   = 103;
static const int ID_SCRIPT_NEW    = 104;
static const int ID_SCRIPT_DELETE = 105;
static const int ID_SCRIPT_RELOAD = 106;
static const int ID_SCRIPT_DETAIL = 107;
static const int ID_SCRIPT_NEWNAME = 108;
static const int ID_SCRIPT_NEWTYPE = 109;
static const int ID_SCRIPT_NEWCREATE = 110;
// Opens the Entity Properties panel on the selected def. This is the only route
// to a def with no world instance - Player.ozls is exactly that, and its
// jump_sound / hurt_sound stats have to be editable somewhere.
static const int ID_SCRIPT_PROPS  = 111;
// Was 111 too - the same value as ID_SCRIPT_PROPS. The Properties button hit the
// NEWCANCEL branch below first and merely closed the (inactive) new-script form,
// so Properties never worked. Keep every control ID in this panel distinct.
static const int ID_SCRIPT_NEWCANCEL = 112;

struct ScriptListEntry {
    std::string name;
    std::string typeName;   // EntityTypeName() or "?" for unparsed files
    int actionCount = 0;
    std::string path;
    int defIndex = -1;      // index into LightningEntityRegistry::GetAllDefs(), -1 = parse error
};

static std::vector<ScriptListEntry> g_scripts;
static bool s_scriptNewMode = false;

// Folder for newly created defs, by entity type. Paths are relative to GameData.
// Skyzones are handled at the call site: they belong to the world being edited.
static const char* ScriptFolderForType(const std::string& type) {
    if (type == "weapon") return "Tex/Global/Gun";
    if (type == "pawn") return "Pawn/Pawns";
    if (type == "game" || type == "campaign") return "Game/Angels95";
    if (type == "sector")    return "Game/Angels95/Sectors";
    if (type == "mission")   return "Game/Angels95/Missions";
    if (type == "objective") return "Game/Angels95/Objectives";
    if (type == "trigger")   return "Game/Angels95/Triggers";
    if (type == "event")     return "Game/Angels95/Events";
    return "Tex/Global/Items"; // pickup / consumable / upgrade / armor
}

// Minimal .ozls skeleton per type
static std::string ScriptTemplate(const std::string& name, const std::string& type) {
    if (type == "weapon")
        return "entity \"" + name + "\" : weapon {\n"
               "    stats {\n        damage = 10\n        fire_rate = 0.4\n"
               "        projectile_speed = 20\n        projectile_lifetime = 2.0\n"
               "        magazine = 12\n        reload_time = 2.0\n    }\n"
               "    actions {\n        on_fire {\n            say \"Bang!\"\n        }\n"
               "        on_reload {\n            say \"Reloading...\"\n        }\n    }\n}\n";
    if (type == "pickup")
        return "entity \"" + name + "\" : pickup {\n"
               "    stats {\n        item_id = 0\n        respawn_time = 30\n    }\n"
               "    actions {\n        on_collect {\n            msg \"Picked up\"\n        }\n    }\n}\n";
    if (type == "pawn")
        return "entity \"" + name + "\" : pawn {\n"
               "    actions {\n        on_death {\n            msg \"Enemy down.\"\n        }\n    }\n}\n";
    if (type == "skyzone")
        return "entity \"" + name + "\" : skyzone {\n"
               "    actions {\n        on_enter {\n            msg \"Entered zone.\"\n        }\n"
               "        on_exit {\n            msg \"Left zone.\"\n        }\n    }\n}\n";
    if (type == "consumable")
        return "entity \"" + name + "\" : consumable {\n"
               "    stats {\n        value = 25\n    }\n"
               "    actions {\n        on_use {\n            playerstat health += 25\n            consume\n        }\n    }\n}\n";
    // --- Game-hierarchy types (see Wiki/Game-Hierarchy.md) ---
    if (type == "game")
        return "entity \"" + name + "\" : game {\n"
               "    stats {\n        modes = campaign,cooperative\n"
               "        start_sector = Sefirah_1_TerranPrime\n        save_enabled = 1\n    }\n}\n";
    if (type == "campaign")
        return "entity \"" + name + "\" : campaign {\n"
               "    stats {\n        game = Angels95\n        sectors = \n    }\n}\n";
    if (type == "sector")
        return "entity \"" + name + "\" : sector {\n"
               "    stats {\n        numerology = 1\n        theme = \n        role = \n"
               "        skybox = GameData/Tex/Global/Sky/sky_day.png\n        missions = \n    }\n}\n";
    if (type == "mission")
        return "entity \"" + name + "\" : mission {\n"
               "    stats {\n        sector = \n        world = \n        mode = campaign\n"
               "        objectives = \n    }\n"
               "    actions {\n        on_start { msg \"Mission start\" }\n"
               "        on_complete { msg \"Mission complete\" }\n    }\n}\n";
    if (type == "objective")
        return "entity \"" + name + "\" : objective {\n"
               "    stats {\n        type = kill\n        target = Walker\n        count = 5\n    }\n"
               "    actions {\n        on_complete { msg \"Objective complete\" }\n    }\n}\n";
    if (type == "trigger")
        return "entity \"" + name + "\" : trigger {\n"
               "    stats {\n        topic = trigger.enter\n        once = 1\n        team = 0\n    }\n"
               "    actions {\n        on_enter { msg \"Triggered\" }\n    }\n}\n";
    if (type == "event")
        return "entity \"" + name + "\" : event {\n"
               "    actions {\n        on_emit { msg \"Event\" }\n    }\n}\n";
    return "entity \"" + name + "\" : upgrade {\n    stats {\n    }\n    actions {\n    }\n}\n";
}

// Registry-driven .ozls list: parsed defs first, then files that failed to parse
static void ScanScriptFiles() {
    g_scripts.clear();
    auto& registry = LightningEntityRegistry::Instance();
    const auto& defs = registry.GetAllDefs();
    for (size_t i = 0; i < defs.size(); i++) {
        const EntityDef& def = defs[i];
        ScriptListEntry e;
        e.name = def.name;
        e.typeName = EntityTypeName(def.type);
        e.actionCount = (int)def.actions.size();
        e.path = def.sourcePath;
        e.defIndex = (int)i;
        g_scripts.push_back(e);
    }
    // Filesystem fallback: .ozls files that produced no def (parse errors)
    fs::path gd = fs::current_path() / "GameData";
    if (fs::exists(gd)) {
        for (auto& entry : fs::recursive_directory_iterator(gd)) {
            if (!entry.is_regular_file()) continue;
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext != ".ozls") continue;
            std::string p = entry.path().string();
            bool known = false;
            for (auto& s : g_scripts) if (s.path == p) { known = true; break; }
            if (!known) {
                ScriptListEntry e;
                e.name = entry.path().stem().string();
                e.typeName = "?";
                e.path = p;
                g_scripts.push_back(e);
            }
        }
    }
    std::sort(g_scripts.begin(), g_scripts.end(),
              [](const ScriptListEntry& a, const ScriptListEntry& b) { return a.name < b.name; });
    if (g_editorPanels.hScriptMgr)
        SendMessage((HWND)g_editorPanels.hScriptMgr, WM_USER + 50, 0, 0);
}

// Read-only detail text for the selected script (def stats + actions)
static void BuildDefSummary(const EntityDef& def, const std::string& sourcePath,
                            int actionCountHint, std::string& out);
static void BuildScriptDetail(int sel, std::string& out) {
    out.clear();
    if (sel < 0 || sel >= (int)g_scripts.size()) return;
    const ScriptListEntry& e = g_scripts[sel];
    if (e.defIndex < 0) {
        out += e.name + "  [" + e.typeName + "]\n";
        out += "source: " + e.path + "\n";
        out += "\n(parse error â€” file did not produce an entity definition)\n";
        return;
    }
    auto& defs = LightningEntityRegistry::Instance().GetAllDefs();
    if (e.defIndex >= (int)defs.size()) return;
    BuildDefSummary(defs[e.defIndex], e.path, -1, out);
}

void ShowScriptManager(bool show) {
    g_editorPanels.showScriptMgr = show;
    if (g_editorPanels.hScriptMgr) {
        if (show) SendMessage((HWND)g_editorPanels.hScriptMgr, WM_USER + 50, 0, 0);
        ShowWindow((HWND)g_editorPanels.hScriptMgr, show ? SW_SHOW : SW_HIDE);
    }
}

static LRESULT CALLBACK ScriptMgrProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    static HWND hList = nullptr;
    static HWND hDetail = nullptr;

    // Reflow for the normal (list + detail) mode. The new-script form is a small
    // fixed dialog and is left alone. Called after every rebuild and on WM_SIZE.
    auto applyLayout = [&]() {
        RECT rc;
        GetClientRect(hwnd, &rc);
        HWND list = GetDlgItem(hwnd, ID_SCRIPT_LIST);
        HWND detail = GetDlgItem(hwnd, ID_SCRIPT_DETAIL);
        if (!list || !detail) return;

        const int m = 10, btnH = 28;
        const int top = 32;
        const int avail = rc.bottom - top - btnH - 3 * m;
        int listH = avail * 45 / 100;
        if (listH < 60) listH = 60;
        MoveWindow(list, m, top, rc.right - 2 * m, listH, TRUE);
        int detailH = avail - listH - 8;
        if (detailH < 40) detailH = 40;
        MoveWindow(detail, m, top + listH + 8, rc.right - 2 * m, detailH, TRUE);

        const int by = rc.bottom - m - btnH;
        struct Btn { int id; int w; };
        const Btn btns[] = {
            {ID_SCRIPT_EDIT, 70}, {ID_SCRIPT_PROPS, 80}, {ID_SCRIPT_NEW, 90},
            {ID_SCRIPT_DELETE, 70}, {ID_SCRIPT_RELOAD, 70}, {ID_SCRIPT_CLOSE, 70},
        };
        int x = m;
        for (const auto& b : btns) {
            if (HWND h = GetDlgItem(hwnd, b.id))
                MoveWindow(h, x, by, b.w, btnH, TRUE);
            x += b.w + 6;
        }
    };

    // Builds the panel contents for the current mode (normal / new-script form).
    // Destroy-and-rebuild follows the house PopulatePropertiesPanel pattern.
    auto buildPanel = [&]() {
        HWND child = GetWindow(hwnd, GW_CHILD);
        while (child) {
            HWND next = GetWindow(child, GW_HWNDNEXT);
            DestroyWindow(child);
            child = next;
        }
        hList = nullptr;
        hDetail = nullptr;
        SendMessage(hwnd, WM_SETREDRAW, FALSE, 0);
        if (s_scriptNewMode) {
            CreateLabel(hwnd, L"New Script Name:", 10, 14, 120, 20, 0);
            CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"my_script",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                130, 10, 180, 22, hwnd, (HMENU)(INT_PTR)ID_SCRIPT_NEWNAME, g_hInst, nullptr);
            CreateLabel(hwnd, L"Type:", 10, 42, 120, 20, 0);
            HWND hCombo = CreateWindowEx(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                130, 38, 180, 200, hwnd, (HMENU)(INT_PTR)ID_SCRIPT_NEWTYPE, g_hInst, nullptr);
            const wchar_t* types[] = { L"weapon", L"pickup", L"pawn", L"skyzone", L"consumable", L"upgrade",
                                       L"game", L"campaign", L"sector", L"mission", L"objective", L"trigger", L"event" };
            for (auto* t : types) SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)t);
            SendMessage(hCombo, CB_SETCURSEL, 0, 0);
            CreateButton(hwnd, L"Create", 320, 10, 90, 26, ID_SCRIPT_NEWCREATE);
            CreateButton(hwnd, L"Cancel", 320, 40, 90, 26, ID_SCRIPT_NEWCANCEL);
            CreateLabel(hwnd, L"Folder: the type's home under GameData (skyzones: the open world)  â€”  opened in your editor after creation.",
                        10, 72, 500, 20, 0);
        } else {
            CreateLabel(hwnd, L"LightningScript files (.ozls â€” GameData/ + packages):", 10, 10, 520, 20, 1);
            hList = CreateListBox(hwnd, 10, 32, 520, 170, ID_SCRIPT_LIST);
            hDetail = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                10, 210, 520, 160, hwnd, (HMENU)(INT_PTR)ID_SCRIPT_DETAIL, g_hInst, nullptr);
            CreateButton(hwnd, L"Edit", 10, 378, 90, 28, ID_SCRIPT_EDIT);
            CreateButton(hwnd, L"Properties", 106, 378, 90, 28, ID_SCRIPT_PROPS);
            CreateButton(hwnd, L"New Script...", 202, 378, 110, 28, ID_SCRIPT_NEW);
            CreateButton(hwnd, L"Delete", 318, 378, 90, 28, ID_SCRIPT_DELETE);
            CreateButton(hwnd, L"Reload", 414, 378, 90, 28, ID_SCRIPT_RELOAD);
            CreateButton(hwnd, L"Close", 510, 378, 90, 28, ID_SCRIPT_CLOSE);
        }
        SendMessage(hwnd, WM_SETREDRAW, TRUE, 0);
        applyLayout();
        InvalidateRect(hwnd, nullptr, TRUE);
    };

    switch (msg) {
    case WM_CREATE:
        buildPanel();
        if (!s_scriptNewMode) {
            ScanScriptFiles();
            SendMessage(hwnd, WM_USER + 50, 0, 0); // fill list (hScriptMgr not set yet during WM_CREATE)
        }
        break;
    case WM_USER + 50: {
        if (s_scriptNewMode || !hList) break;
        SendMessage(hList, LB_RESETCONTENT, 0, 0);
        for (const auto& scr : g_scripts) {
            char line[320];
            snprintf(line, sizeof(line), "%s  [%s]%s", scr.name.c_str(), scr.typeName.c_str(),
                     scr.defIndex < 0 ? "  (parse error)" :
                     (scr.actionCount > 0 ? "" : "  (no actions)"));
            SendMessageA(hList, LB_ADDSTRING, 0, (LPARAM)line);
        }
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == ID_SCRIPT_CLOSE) {
            ShowScriptManager(false);
        } else if (id == ID_SCRIPT_NEW) {
            s_scriptNewMode = true;
            buildPanel();
        } else if (id == ID_SCRIPT_NEWCANCEL) {
            s_scriptNewMode = false;
            buildPanel();
            ScanScriptFiles();
        } else if (id == ID_SCRIPT_NEWCREATE) {
            wchar_t nameBuf[128] = {0};
            GetWindowTextW(GetDlgItem(hwnd, ID_SCRIPT_NEWNAME), nameBuf, 128);
            char nameA[128] = {0};
            WideCharToMultiByte(CP_UTF8, 0, nameBuf, -1, nameA, 128, nullptr, nullptr);
            int typeSel = (int)SendMessage(GetDlgItem(hwnd, ID_SCRIPT_NEWTYPE), CB_GETCURSEL, 0, 0);
            static const char* types[] = { "weapon", "pickup", "pawn", "skyzone", "consumable", "upgrade",
                                           "game", "campaign", "sector", "mission", "objective", "trigger", "event" };
            const int typeCount = (int)(sizeof(types) / sizeof(types[0]));
            std::string type = (typeSel >= 0 && typeSel < typeCount) ? types[typeSel] : "pickup";
            std::string name = nameA;
            while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.erase(0, 1);
            while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
            for (auto& c : name) if (c == ' ' || c == '\\' || c == '/' || c == ':') c = '_';
            if (name.empty()) {
                MessageBoxA(hwnd, "Script name must not be empty.", "New Script", MB_OK);
                break;
            }
            // A skyzone def describes the open world's sky, so it is written next to
            // World.ozone - LoadWorldOverrides resolves world defs by their path.
            fs::path dir;
            if (type == "skyzone") {
                std::string worldDir = Editor_GetCurrentWorldDir();
                dir = worldDir.empty() ? (fs::current_path() / "GameData" / "Worlds")
                                       : fs::path(worldDir);
            } else {
                dir = fs::current_path() / "GameData" / ScriptFolderForType(type);
            }
            std::error_code ec;
            fs::create_directories(dir, ec);
            fs::path out = dir / (name + ".ozls");
            if (fs::exists(out)) {
                MessageBoxA(hwnd, ("Already exists: " + out.string()).c_str(), "New Script", MB_OK);
                break;
            }
            std::ofstream ofs(out, std::ios::binary);
            if (!ofs) {
                MessageBoxA(hwnd, ("Could not write: " + out.string()).c_str(), "New Script", MB_OK);
                break;
            }
            ofs << ScriptTemplate(name, type);
            ofs.close();
            LightningEntityRegistry::Instance().Init();
            if (g_editorPanels.hPawnMgr)
                SendMessage((HWND)g_editorPanels.hPawnMgr, WM_USER + 50, 0, 0);
            ShellExecuteA(hwnd, "open", out.string().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            s_scriptNewMode = false;
            buildPanel();
            ScanScriptFiles();
        } else if (id == ID_SCRIPT_REFRESH) {
            ScanScriptFiles();
        } else if (id == ID_SCRIPT_RELOAD) {
            LightningEntityRegistry::Instance().Init();
            ScanScriptFiles();
            if (g_editorPanels.hPawnMgr)
                SendMessage((HWND)g_editorPanels.hPawnMgr, WM_USER + 50, 0, 0);
            OZ_INFO("Script registry reloaded (%zu defs)", g_scripts.size());
        } else if (id == ID_SCRIPT_DELETE) {
            int sel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < (int)g_scripts.size()) {
                const ScriptListEntry& e = g_scripts[sel];
                if (!fs::exists(e.path)) {
                    MessageBoxA(hwnd,
                        "This script lives inside a package.\nEdit the source .ozls and repack to change it.",
                        "Delete Script", MB_OK | MB_ICONINFORMATION);
                    break;
                }
                std::string q = "Delete '" + e.name + "'?\n" + e.path;
                if (MessageBoxA(hwnd, q.c_str(), "Delete Script", MB_YESNO | MB_ICONWARNING) == IDYES) {
                    std::error_code ec;
                    fs::remove(e.path, ec);
                    LightningEntityRegistry::Instance().Init();
                    ScanScriptFiles();
                    if (g_editorPanels.hPawnMgr)
                        SendMessage((HWND)g_editorPanels.hPawnMgr, WM_USER + 50, 0, 0);
                }
            }
        } else if (id == ID_SCRIPT_EDIT ||
                   (id == ID_SCRIPT_LIST && HIWORD(w) == LBN_DBLCLK)) {
            int sel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < (int)g_scripts.size()) {
                const std::string& p = g_scripts[sel].path;
                if (!fs::exists(p)) {
                    std::string msg = "Script is packaged (no editable source file):\n" + p +
                                      "\n\nEdit the GameData source .ozls and repack.";
                    MessageBoxA(hwnd, msg.c_str(), "Edit Script", MB_OK | MB_ICONINFORMATION);
                } else {
                    ShellExecuteA(hwnd, "open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
        } else if (id == ID_SCRIPT_PROPS ||
                   (id == ID_SCRIPT_LIST && HIWORD(w) == LBN_DBLCLK &&
                    SendMessage(hList, LB_GETCURSEL, 0, 0) < 0)) {
            // Open the Entity Properties panel on the selected def. Uses a
            // def-name target rather than a world-instance selection, so this
            // works for defs with no instance in the open world (Player.ozls).
            int sel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < (int)g_scripts.size()) {
                const std::string& defName = g_scripts[sel].name;
                ShowDefPropertiesFor(defName);
            }
        } else if (id == ID_SCRIPT_LIST && HIWORD(w) == LBN_SELCHANGE) {
            int sel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
            std::string detail;
            BuildScriptDetail(sel, detail);
            SetWindowTextA(hDetail, detail.c_str());
        }
        break;
    }
    case WM_SIZE:
        applyLayout();
        break;
    case WM_GETMINMAXINFO:
        EnforceMinTrackSize(l, 620, 440);
        break;
    case WM_CLOSE:
        s_scriptNewMode = false;
        ShowScriptManager(false);
        break;
    case WM_DESTROY:
        g_editorPanels.hScriptMgr = nullptr;
        break;
    default:
        return DefWindowProc(hwnd, msg, w, l);
    }
    return 0;
}

