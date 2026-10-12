// ============================================================================
// WorldGraphPanel.cpp
//
// FRAGMENT - not a standalone translation unit. Included by ../UiShell.cpp,
// which is the single TU for the whole UI layer. That is deliberate: see
// UiShell.cpp for why the layer is built as a unity build rather than as 16
// separate objects.
// ============================================================================

// =====================================================================
// WorldGraph Explorer - ListView of all world entities
// =====================================================================
static const int ID_WG_LIST    = 301;
static const int ID_WG_REFRESH = 302;
static const int ID_WG_CLOSE   = 303;

struct WorldGraphEntry {
    std::string typeLabel;
    std::string name;
    float posX, posY, posZ;
    float rotation;
    int selType;   // SelType encoded as int (see SelType.hpp)
    int selIndex;
    // The Map row is not an entity — it is the level itself (gameType rules,
    // weather particles, level skybox). It is emitted first and marked so the
    // list can set it apart from the instance rows: a flat ListView has no
    // hierarchy, and without this the level looks like another brush.
    bool isLevelHeader = false;
};

static std::vector<WorldGraphEntry> g_worldGraphEntries;

static void BuildWorldGraphEntries() {
    g_worldGraphEntries.clear();

    // The level itself, first. Right-clicking it opens the Entity Properties
    // panel on SelType::MAP, which is how the level-only state that used to live
    // in the Zone Properties dialog (gameType rules, weather particles, skybox)
    // is now reached. See Wiki/Editor-PropertyPanel-Refactor.md.
    {
        WorldGraphEntry e;
        e.typeLabel = "Map (level)";
        // Name the world from its loaded directory (…/Worlds/<name>), which is
        // what this panel is listing instances of anyway.
        e.name = "World";
        {
            const std::string dir = Editor_GetCurrentWorldDir();
            if (!dir.empty()) {
                fs::path stem = fs::path(dir).stem();
                if (!stem.empty()) e.name = stem.string();
            }
        }
        // No position: a level has none. Every consumer that reads these for an
        // instance type is guarded on SelType::MAP in Main.cpp (the gizmo snap,
        // the Delete/Duplicate menu items and the keyboard gate).
        e.posX = e.posY = e.posZ = 0.0f;
        e.rotation = 0.0f;
        e.selType = sel::MAP;
        e.selIndex = 0;
        e.isLevelHeader = true;
        g_worldGraphEntries.push_back(e);
    }

    // Brushes from OzoneLoader RENDERABLES, the same source and the same index space
    // RaycastTestOzPrimitives picks from.
    //
    // This used to iterate GetCollisionVolumes() - the CSG output, whose entries are
    // merged AABBs - and store that index as SelType::BRUSH. R8 established that
    // SelType::BRUSH means a RENDERABLE index and removed the collision-volume pick
    // path, but only on the mouse route; this list kept the old index space. Every
    // consumer of the row (Properties, Delete, Duplicate) then read a collision-volume
    // index as a renderable index, which is the same defect R8 fixed for clicks, in a
    // second location.
    //
    // It was also self-inconsistent: the CSG output includes AutoConvex
    // SURF_COLLISION_PROXY boxes and drops `sub`-carved geometry, so the Explorer listed
    // rows that a click cannot select, and omitted rows it can.
    //
    // The skip filter is now the shared IsEditorPickableSurface predicate, which
    // Selection.cpp's raycast and the surface picker also call. This used to be a
    // hand-copied third version of the same test, and two copies of "what counts
    // as a visible brush" is exactly how the Explorer and the raycast drifted
    // apart in the first place.
    {
        const int count = OzoneLoader::Instance().Count();
        for (int i = 0; i < count; i++) {
            const OzoneRenderable* r = OzoneLoader::Instance().Get(i);
            if (!r || !r->loaded || r->model.meshCount == 0) continue;
            if (!IsEditorPickableSurface(*r))
                continue;
            const BoundingBox mb = GetMeshBoundingBox(r->model.meshes[0]);
            const float cx = r->position.x + (mb.min.x + mb.max.x) * 0.5f * r->scale;
            const float cy = r->position.y + (mb.min.y + mb.max.y) * 0.5f * r->scale;
            const float cz = r->position.z + (mb.min.z + mb.max.z) * 0.5f * r->scale;

            WorldGraphEntry e;
            e.typeLabel = "Brush";
            // Same label the picker produces, so one brush reads identically in the
            // Explorer, the Properties panel and AngelEd.log.
            e.name = TextFormat("OzPrimitive %d", i);
            e.posX = cx;
            e.posY = cy;
            e.posZ = cz;
            e.rotation = r->rotation * RAD2DEG;   // the list column is degrees
            e.selType = sel::BRUSH;
            e.selIndex = i;
            g_worldGraphEntries.push_back(e);
        }
    }

    // WDL Models
    {
        int count = WorldGraph_GetModelCount();
        for (int i = 0; i < count; i++) {
            const char* name = WorldGraph_GetModelName(i);
            float x, y, z, r, s;
            WorldGraph_GetModelData(i, x, y, z, r, s);
            WorldGraphEntry e;
            e.typeLabel = "Model";
            e.name = name ? name : "Model";
            e.posX = x; e.posY = y; e.posZ = z;
            e.rotation = r;
            e.selType = 2; // SelType::MODEL
            e.selIndex = i;
            g_worldGraphEntries.push_back(e);
        }
    }

    // Pawns (NPCs)
    {
        auto& pawns = PawnSystem::Instance().GetPawns();
        for (auto& p : pawns) {
            if (!p.active) continue;
            WorldGraphEntry e;
            e.typeLabel = "NPC";
            e.name = p.defName;
            e.posX = p.position.x; e.posY = p.position.y; e.posZ = p.position.z;
            e.rotation = p.yaw;
            e.selType = 3; // SelType::NPC
            e.selIndex = (int)p.id;
            g_worldGraphEntries.push_back(e);
        }
    }

    // Pickups
    {
        auto& pickups = PawnSystem::Instance().GetPickups();
        for (auto& pk : pickups) {
            if (!pk.active) continue;
            WorldGraphEntry e;
            e.typeLabel = "Pickup";
            e.name = pk.typeName;
            e.posX = pk.position.x; e.posY = pk.position.y; e.posZ = pk.position.z;
            e.rotation = 0;
            e.selType = 4; // SelType::PICKUP
            e.selIndex = (int)pk.id;
            g_worldGraphEntries.push_back(e);
        }
    }

    // Lights
    {
        auto& lights = PawnSystem::Instance().GetLights();
        for (auto& l : lights) {
            if (!l.active) continue;
            WorldGraphEntry e;
            e.typeLabel = "Light";
            e.name = l.name.empty() ? "Light" : l.name;
            e.posX = l.position.x; e.posY = l.position.y; e.posZ = l.position.z;
            e.rotation = 0;
            e.selType = 5; // SelType::LIGHT
            e.selIndex = (int)l.id;
            g_worldGraphEntries.push_back(e);
        }
    }

    // Zones
    {
        auto& zones = ZoneManager::Instance().GetZones();
        for (auto& z : zones) {
            WorldGraphEntry e;
            e.typeLabel = "Zone";
            e.name = z.name.empty() ? "ZoneVolume" : z.name;
            e.posX = (z.bounds.min.x + z.bounds.max.x) * 0.5f;
            e.posY = (z.bounds.min.y + z.bounds.max.y) * 0.5f;
            e.posZ = (z.bounds.min.z + z.bounds.max.z) * 0.5f;
            e.rotation = 0;
            e.selType = 6; // SelType::ZONE
            e.selIndex = (int)z.id;
            g_worldGraphEntries.push_back(e);
        }
    }

    // Portals (level connections)
    {
        auto& portals = ZoneManager::Instance().GetPortals();
        for (size_t p = 0; p < portals.size(); p++) {
            auto& portal = portals[p];
            WorldGraphEntry e;
            e.typeLabel = "Portal";
            e.name = portal.targetWorld.empty() ? "<unassigned>" : portal.targetWorld;
            e.posX = (portal.bounds.min.x + portal.bounds.max.x) * 0.5f;
            e.posY = (portal.bounds.min.y + portal.bounds.max.y) * 0.5f;
            e.posZ = (portal.bounds.min.z + portal.bounds.max.z) * 0.5f;
            e.rotation = 0;
            e.selType = 8; // SelType::PORTAL
            e.selIndex = (int)p;
            g_worldGraphEntries.push_back(e);
        }
    }

    // Player Starts
    {
        auto& starts = PawnSystem::Instance().GetPlayerStarts();
        for (auto& s : starts) {
            WorldGraphEntry e;
            e.typeLabel = "Spawn";
            e.name = "PlayerStart";
            e.posX = s.position.x; e.posY = s.position.y; e.posZ = s.position.z;
            e.rotation = s.yaw;
            e.selType = 7; // SelType::SPAWN
            e.selIndex = (int)s.id;
            g_worldGraphEntries.push_back(e);
        }
    }

    // Emitters
    {
        auto& emitters = PawnSystem::Instance().GetEmitters();
        for (auto& em : emitters) {
            WorldGraphEntry e;
            e.typeLabel = (em.type == EmitterType::SOUND) ? "SoundEmitter" : "MusicEmitter";
            e.name = "Emitter";
            e.posX = em.position.x; e.posY = em.position.y; e.posZ = em.position.z;
            e.rotation = 0;
            e.selType = 0; // SelType::NONE - emitters not directly selectable
            e.selIndex = (int)em.id;
            g_worldGraphEntries.push_back(e);
        }
    }
}

static void PopulateWorldGraphList(HWND hList) {
    ListView_DeleteAllItems(hList);
    for (size_t i = 0; i < g_worldGraphEntries.size(); i++) {
        auto& e = g_worldGraphEntries[i];

std::wstring wtype(e.typeLabel.begin(), e.typeLabel.end());
        LVITEMW lvi = {};
        lvi.mask = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem = (int)i;
        lvi.lParam = i;
        lvi.pszText = const_cast<wchar_t*>(wtype.c_str());
        ListView_InsertItem(hList, &lvi);

        std::wstring wname(e.name.begin(), e.name.end());
        ListView_SetItemText(hList, (int)i, 1, const_cast<wchar_t*>(wname.c_str()));

        // A level has no transform, so leave the position columns blank instead
        // of printing a misleading "0.0 0.0 0.0" that reads like a real location.
        // The "(level)" label plus the blank columns are what distinguishes it
        // (see the NM_NOTE in WorldGraphProc about why not a bold font).
        if (!e.isLevelHeader) {
            wchar_t wbuf[32];
            swprintf(wbuf, 32, L"%.1f", e.posX);
            ListView_SetItemText(hList, (int)i, 2, wbuf);
            swprintf(wbuf, 32, L"%.1f", e.posY);
            ListView_SetItemText(hList, (int)i, 3, wbuf);
            swprintf(wbuf, 32, L"%.1f", e.posZ);
            ListView_SetItemText(hList, (int)i, 4, wbuf);
            swprintf(wbuf, 32, L"%.1f", e.rotation);
            ListView_SetItemText(hList, (int)i, 5, wbuf);
        } else {
            for (int col = 2; col <= 5; col++)
                ListView_SetItemText(hList, (int)i, col, const_cast<wchar_t*>(L""));
        }
    }
}

static LRESULT CALLBACK WorldGraphProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    static HWND hList;
    switch (msg) {
    case WM_CREATE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int bw = 80, margin = 6;
        int listH = rc.bottom - bw - margin * 3;

        hList = CreateWindowEx(0, WC_LISTVIEW, L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL,
            margin, margin, rc.right - margin * 2, listH,
            hwnd, (HMENU)ID_WG_LIST, g_hInst, nullptr);
        ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        // Columns: Type, Name, PosX, PosY, PosZ, Rot
        LVCOLUMNW lvc = {};
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        lvc.fmt = LVCFMT_LEFT;
        const wchar_t* headers[] = {L"Type", L"Name", L"PosX", L"PosY", L"PosZ", L"Rot"};
        int widths[] = {80, 140, 70, 70, 70, 60};
        for (int i = 0; i < 6; i++) {
            lvc.cx = widths[i];
            lvc.pszText = const_cast<wchar_t*>(headers[i]);
            ListView_InsertColumn(hList, i, &lvc);
        }

        CreateButton(hwnd, L"Refresh", margin, listH + margin * 2, bw, 26, ID_WG_REFRESH);
        CreateButton(hwnd, L"Close", rc.right - bw - margin, listH + margin * 2, bw, 26, ID_WG_CLOSE);

        BuildWorldGraphEntries();
        PopulateWorldGraphList(hList);
        break;
    }
    case WM_USER + 50: {
        BuildWorldGraphEntries();
        PopulateWorldGraphList(hList);
        break;
    }
    case WM_NOTIFY: {
        NMHDR* nm = (NMHDR*)l;
        // NB: the level row is NOT emphasised with a bold font. A ListView has no
        // per-item bold state (LVIS_BOLD does not exist) and NM_CUSTOMDRAW cannot
        // supply one here either: MinGW's NMLVCUSTOMDRAW exposes only iSubItem
        // (the column), not iItem, so there is no portable row index to test.
        // BuildWorldGraphEntries therefore distinguishes it by label and by
        // leaving its position columns blank.
        if (nm->idFrom == ID_WG_LIST &&
            (nm->code == NM_DBLCLK || nm->code == NM_RCLICK)) {
            int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
            if (sel >= 0 && sel < (int)g_worldGraphEntries.size()) {
                auto& e = g_worldGraphEntries[sel];
                // ONE event carries the whole selection. This used to be five
                // fields (actionSelectFromGraph plus Type/Name/Pos[3]) written
                // together and read together with nothing coupling them, so a
                // handler could read a new index against the previous frame's
                // type and position without noticing.
                ed::Selection pick;
                // WorldGraphEntry::selType is an int mirror of SelType (see SelType.hpp), and
                // sel::DEF_ONLY (-1) means "a def with no world instance", which has
                // no live target. Both of those collapse to None, which
                // AdoptSelection() rejects — so a Properties/Delete on such a row
                // does nothing instead of acting on the previous selection.
                pick.ref = ed::SelRef{e.selType < 0 ? ed::SelKind::None
                                                    : ToBusKind((SelType)e.selType),
                                      e.selIndex};
                pick.name = e.name;
                pick.x = e.posX; pick.y = e.posY; pick.z = e.posZ;
                ed::EventBus::instance().post(ed::Ev::SelectEntity, pick);

                if (nm->code == NM_RCLICK) {
                    HMENU hMenu = CreatePopupMenu();
                    AppendMenuA(hMenu, MF_STRING, 1001, "Properties");
                    // Delete/Duplicate are for placed objects only. Both handlers are
                    // `else if` chains with no `else`, so offering them on the Map row
                    // (the level) would silently deselect it and leave a no-op undo
                    // snapshot instead of failing visibly.
                    if (e.selType != sel::MAP) {
                        AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
                        AppendMenuA(hMenu, MF_STRING, 1002, "Delete");
                        AppendMenuA(hMenu, MF_STRING, 1003, "Duplicate");
                    }
                    POINT pt;
                    GetCursorPos(&pt);
                    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
                    DestroyMenu(hMenu);
                    // Each of these posts the SAME captured selection, so the
                    // target is fixed at click time. Previously Delete/Duplicate
                    // stored e.selIndex and then the handler ignored it, acting on
                    // whatever g_sel happened to be — correct only because the
                    // select field was set in the same notification.
                    if (cmd == 1001) {
                        // OpenProperties, not ApplyProperties: this only OPENS the
                        // panel. ApplyProperties writes the panel's edited values
                        // (posted by the panel's own Apply button) and is a separate
                        // kind for exactly that reason.
                        ed::EventBus::instance().post(ed::Ev::OpenProperties, pick);
                    } else if (cmd == 1002) {
                        ed::EventBus::instance().post(ed::Ev::DeleteEntity, pick);
                    } else if (cmd == 1003) {
                        ed::EventBus::instance().post(ed::Ev::DuplicateEntity, pick);
                    }
                }
            }
        }
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == ID_WG_CLOSE) ShowWorldGraph(false);
        if (id == ID_WG_REFRESH) {
            BuildWorldGraphEntries();
            PopulateWorldGraphList(hList);
        }
        break;
    }
    case WM_CLOSE: ShowWorldGraph(false); break;
    case WM_DESTROY: g_editorPanels.hWorldGraph = nullptr; break;
    default: return DefWindowProc(hwnd, msg, w, l);
    }
    return 0;
}

void ShowWorldGraph(bool show) {
    g_editorPanels.showWorldGraph = show;
    if (g_editorPanels.hWorldGraph) {
        ShowWindow((HWND)g_editorPanels.hWorldGraph, show ? SW_SHOW : SW_HIDE);
        // Opening is the one moment that MUST show current data. The list is built in
        // WM_CREATE from CreateAllEditorWindows, which runs before the world is loaded,
        // so without this the panel presents an empty scene and keeps it.
        if (show) {
            g_editorPanels.worldGraphDirty = false;
            RefreshWorldGraph();
        }
    }
}

void RefreshWorldGraph() {
    if (g_editorPanels.hWorldGraph)
        SendMessage((HWND)g_editorPanels.hWorldGraph, WM_USER + 50, 0, 0);
}

