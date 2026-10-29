// ============================================================================
// WorldGraphPanel.cpp
//
// FRAGMENT - not a standalone translation unit. Included by ../UiShell.cpp,
// which is the single TU for the whole UI layer. That is deliberate: see
// UiShell.cpp for why the layer is built as a unity build rather than as 16
// separate objects.
// ============================================================================

// =====================================================================
// World Hierarchy - grouped, searchable tree of every world object.
//
// This started as a flat ListView ("World Graph Explorer"). A flat list cannot
// answer the question the panel exists for - "what is in this level?" - and it
// silently omitted every Mesh.* prop, particle emitter, path node and wind zone
// because each new entity type needed a hand-written block. The hierarchy below
// is built from the same live containers the viewport uses, with one group per
// type, so a new type is one block here and one group in kGroupOrder.
//
// Selection payloads are still {SelType, index} captured at click time and posted
// on the event bus, so the panel never mutates the world and cannot act on a
// stale index if a delete shifts the container between click and drain.
// =====================================================================
static const int ID_WG_TREE    = 301;
static const int ID_WG_REFRESH = 302;
static const int ID_WG_CLOSE   = 303;
static const int ID_WG_SEARCH  = 304;
static const int ID_WG_REVEAL  = 305;

struct WorldGraphEntry {
    std::string typeLabel;
    std::string name;
    float posX, posY, posZ;
    float rotation;
    int selType;   // SelType encoded as int (see SelType.hpp)
    int selIndex;
    // The Map row is not an entity - it is the level itself (gameType rules,
    // weather particles, level skybox). It has no transform and must never be
    // offered Delete/Duplicate.
    bool isLevelHeader = false;
    // Sound/music emitters have no SelType. They are real scene objects, so they
    // are listed - but labelled "(info)" and not selectable rather than being a
    // row that silently does nothing.
    bool selectable = true;
    std::string group;
};

static std::vector<WorldGraphEntry> g_worldGraphEntries;

// Fixed group order. Entries are appended by BuildWorldGraphEntries in any order;
// the tree uses this table so the hierarchy never reshuffles between refreshes.
static const char* const kGroupOrder[] = {
    "Map",
    "Brushes",
    "Models",
    "Meshes",
    "NPCs",
    "Pickups",
    "Lights",
    "Zones",
    "Portals",
    "Player Starts",
    "Emitters",
    "Particles",
    "Path Nodes",
    "Wind Zones",
};
static const int kGroupCount = (int)(sizeof(kGroupOrder) / sizeof(kGroupOrder[0]));

static HWND g_wgTree = nullptr;
static HWND g_wgSearch = nullptr;
static HWND g_wgReveal = nullptr;
static HWND g_wgRefresh = nullptr;
static HWND g_wgClose = nullptr;

// Group expansion survives a rebuild. Default true so the first open shows the
// whole level rather than a column of closed folders.
static std::unordered_map<std::string, bool> g_wgGroupExpanded;
static bool g_wgRebuilding = false;
static std::string g_wgFilter;
// Last selection, kept so a rebuild can restore the same row after delete/undo.
static int g_wgSelType = -1;
static int g_wgSelIndex = -1;

// Phase 5.2 (lazy WorldGraph): entry indices grouped by group name, rebuilt with
// the entries. A group's leaf rows are inserted only when it is expanded, so a
// refresh costs O(groups) tree inserts rather than O(objects). The entries
// themselves are still materialised once per rebuild (cheap; GetMeshBoundingBox
// per brush is the only real work), because selection/reveal need the index
// space to be stable.
static std::unordered_map<std::string, std::vector<int>> g_wgGroupMembers;

static std::wstring WgWiden(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w((size_t)n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

static std::string WgNarrow(const wchar_t* w) {
    if (!w || !*w) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

static std::string WgLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static bool WgMatchesFilter(const WorldGraphEntry& e) {
    if (g_wgFilter.empty()) return true;
    const std::string needle = WgLower(g_wgFilter);
    return WgLower(e.name).find(needle) != std::string::npos ||
           WgLower(e.typeLabel).find(needle) != std::string::npos;
}

static void BuildWorldGraphEntries() {
    g_worldGraphEntries.clear();

    // The level itself, first.
    {
        WorldGraphEntry e;
        e.typeLabel = "Map";
        e.group     = "Map";
        e.name = "Map (level)";
        const std::string dir = Editor_GetCurrentWorldDir();
        if (!dir.empty()) {
            fs::path stem = fs::path(dir).stem();
            if (!stem.empty()) e.name += " - " + stem.string();
        }
        e.posX = e.posY = e.posZ = 0.0f;
        e.rotation = 0.0f;
        e.selType = sel::MAP;
        e.selIndex = 0;
        e.isLevelHeader = true;
        g_worldGraphEntries.push_back(e);
    }

    // Brushes from OzoneLoader RENDERABLES - the same source and the same index
    // space RaycastTestOzPrimitives picks from. GetCollisionVolumes() is the CSG
    // OUTPUT and is the wrong index space (see the R8 notes).
    {
        const int count = OzoneLoader::Instance().Count();
        for (int i = 0; i < count; i++) {
            const OzoneRenderable* r = OzoneLoader::Instance().Get(i);
            if (!r || !r->loaded || r->model.meshCount == 0) continue;
            if (!IsEditorPickableSurface(*r)) continue;
            const BoundingBox mb = GetMeshBoundingBox(r->model.meshes[0]);
            WorldGraphEntry e;
            e.typeLabel = "Brush";
            e.group     = "Brushes";
            e.name = TextFormat("OzPrimitive %d", i);
            e.posX = r->position.x + (mb.min.x + mb.max.x) * 0.5f * r->scale;
            e.posY = r->position.y + (mb.min.y + mb.max.y) * 0.5f * r->scale;
            e.posZ = r->position.z + (mb.min.z + mb.max.z) * 0.5f * r->scale;
            e.rotation = r->rotation * RAD2DEG;
            e.selType = sel::BRUSH;
            e.selIndex = i;
            g_worldGraphEntries.push_back(e);
        }
    }

    // Legacy WDL models.
    {
        int count = WorldGraph_GetModelCount();
        for (int i = 0; i < count; i++) {
            const char* name = WorldGraph_GetModelName(i);
            float x, y, z, r, s;
            WorldGraph_GetModelData(i, x, y, z, r, s);
            WorldGraphEntry e;
            e.typeLabel = "Model";
            e.group     = "Models";
            e.name = name ? name : "Model";
            e.posX = x; e.posY = y; e.posZ = z;
            e.rotation = r;
            e.selType = sel::MODEL;
            e.selIndex = i;
            g_worldGraphEntries.push_back(e);
        }
    }

    // Placed Mesh.* props. These were missing from the old list entirely, which
    // is why a placed prop could only be reached by clicking it in the viewport.
    {
        for (auto& m : PawnSystem::Instance().GetMeshObjects()) {
            WorldGraphEntry e;
            e.typeLabel = m.skeletal ? "Mesh.Skeletal" : "Mesh.Static";
            e.group     = "Meshes";
            e.name = m.meshPath.empty() ? "(unnamed mesh)" : m.meshPath;
            e.posX = m.position.x; e.posY = m.position.y; e.posZ = m.position.z;
            e.rotation = m.yaw;
            e.selType = sel::MESH;
            e.selIndex = (int)m.id;
            g_worldGraphEntries.push_back(e);
        }
    }

    // NPCs.
    {
        for (auto& p : PawnSystem::Instance().GetPawns()) {
            if (!p.active) continue;
            WorldGraphEntry e;
            e.typeLabel = "NPC";
            e.group     = "NPCs";
            e.name = p.defName.empty() ? "NPC" : p.defName;
            e.posX = p.position.x; e.posY = p.position.y; e.posZ = p.position.z;
            e.rotation = p.yaw;
            e.selType = sel::NPC;
            e.selIndex = (int)p.id;
            g_worldGraphEntries.push_back(e);
        }
    }

    // Pickups.
    {
        for (auto& pk : PawnSystem::Instance().GetPickups()) {
            if (!pk.active) continue;
            WorldGraphEntry e;
            e.typeLabel = "Pickup";
            e.group     = "Pickups";
            e.name = pk.typeName;
            e.posX = pk.position.x; e.posY = pk.position.y; e.posZ = pk.position.z;
            e.rotation = 0;
            e.selType = sel::PICKUP;
            e.selIndex = (int)pk.id;
            g_worldGraphEntries.push_back(e);
        }
    }

    // Lights.
    {
        for (auto& l : PawnSystem::Instance().GetLights()) {
            if (!l.active) continue;
            WorldGraphEntry e;
            e.typeLabel = "Light";
            e.group     = "Lights";
            e.name = l.name.empty() ? "Light" : l.name;
            e.posX = l.position.x; e.posY = l.position.y; e.posZ = l.position.z;
            e.rotation = 0;
            e.selType = sel::LIGHT;
            e.selIndex = (int)l.id;
            g_worldGraphEntries.push_back(e);
        }
    }

    // Zones.
    {
        for (auto& z : ZoneManager::Instance().GetZones()) {
            WorldGraphEntry e;
            e.typeLabel = "Zone";
            e.group     = "Zones";
            e.name = z.name.empty() ? "ZoneVolume" : z.name;
            e.posX = (z.bounds.min.x + z.bounds.max.x) * 0.5f;
            e.posY = (z.bounds.min.y + z.bounds.max.y) * 0.5f;
            e.posZ = (z.bounds.min.z + z.bounds.max.z) * 0.5f;
            e.rotation = 0;
            e.selType = sel::ZONE;
            e.selIndex = (int)z.id;
            g_worldGraphEntries.push_back(e);
        }
    }

    // Portals.
    {
        auto& portals = ZoneManager::Instance().GetPortals();
        for (size_t p = 0; p < portals.size(); p++) {
            auto& portal = portals[p];
            WorldGraphEntry e;
            e.typeLabel = "Portal";
            e.group     = "Portals";
            e.name = portal.targetWorld.empty() ? "<unassigned>" : portal.targetWorld;
            e.posX = (portal.bounds.min.x + portal.bounds.max.x) * 0.5f;
            e.posY = (portal.bounds.min.y + portal.bounds.max.y) * 0.5f;
            e.posZ = (portal.bounds.min.z + portal.bounds.max.z) * 0.5f;
            e.rotation = 0;
            e.selType = sel::PORTAL;
            e.selIndex = (int)p;
            g_worldGraphEntries.push_back(e);
        }
    }

    // Player starts.
    {
        for (auto& s : PawnSystem::Instance().GetPlayerStarts()) {
            WorldGraphEntry e;
            e.typeLabel = "Spawn";
            e.group     = "Player Starts";
            e.name = "PlayerStart";
            e.posX = s.position.x; e.posY = s.position.y; e.posZ = s.position.z;
            e.rotation = s.yaw;
            e.selType = sel::SPAWN;
            e.selIndex = (int)s.id;
            g_worldGraphEntries.push_back(e);
        }
    }

    // Sound/music markers. Informational: they have no SelType, so they are
    // labelled and cannot be selected instead of pretending to be a dead row.
    {
        for (auto& em : PawnSystem::Instance().GetEmitters()) {
            WorldGraphEntry e;
            e.typeLabel = (em.type == EmitterType::SOUND) ? "SoundEmitter" : "MusicEmitter";
            e.group     = "Emitters";
            e.name = "(info) Emitter";
            e.posX = em.position.x; e.posY = em.position.y; e.posZ = em.position.z;
            e.rotation = 0;
            e.selType = sel::NONE;
            e.selIndex = (int)em.id;
            e.selectable = false;
            g_worldGraphEntries.push_back(e);
        }
    }

    // Particle emitters.
    {
        for (auto& pe : PawnSystem::Instance().GetParticleEmitters()) {
            WorldGraphEntry e;
            e.typeLabel = "Particle";
            e.group     = "Particles";
            e.name = pe.type.empty() ? "Particle" : pe.type;
            e.posX = pe.position.x; e.posY = pe.position.y; e.posZ = pe.position.z;
            e.rotation = pe.yaw;
            e.selType = sel::PARTICLE;
            e.selIndex = (int)pe.id;
            g_worldGraphEntries.push_back(e);
        }
    }

    // Path nodes.
    {
        for (auto& pn : PawnSystem::Instance().GetPathNodes()) {
            WorldGraphEntry e;
            e.typeLabel = "PathNode";
            e.group     = "Path Nodes";
            e.name = pn.name.empty() ? "PathNode" : pn.name;
            e.posX = pn.position.x; e.posY = pn.position.y; e.posZ = pn.position.z;
            e.rotation = 0;
            e.selType = sel::PATHNODE;
            e.selIndex = (int)pn.id;
            g_worldGraphEntries.push_back(e);
        }
    }

    // Wind zones (bounds-centred).
    {
        for (auto& wz : PawnSystem::Instance().GetWindZones()) {
            WorldGraphEntry e;
            e.typeLabel = "WindZone";
            e.group     = "Wind Zones";
            e.name = "WindZone";
            e.posX = (wz.bounds.min.x + wz.bounds.max.x) * 0.5f;
            e.posY = (wz.bounds.min.y + wz.bounds.max.y) * 0.5f;
            e.posZ = (wz.bounds.min.z + wz.bounds.max.z) * 0.5f;
            e.rotation = 0;
            e.selType = sel::WINDZONE;
            e.selIndex = (int)wz.id;
            g_worldGraphEntries.push_back(e);
        }
    }
}

static bool WgFindEntry(int selType, int selIndex, int& outIndex) {
    for (int i = 0; i < (int)g_worldGraphEntries.size(); i++) {
        const auto& e = g_worldGraphEntries[i];
        if (e.selType == selType && e.selIndex == selIndex) {
            outIndex = i;
            return true;
        }
    }
    return false;
}

static void WgCaptureExpansion(HWND tree) {
    HTREEITEM root = TreeView_GetRoot(tree);
    if (!root) return;
    for (HTREEITEM g = TreeView_GetChild(tree, root); g;
         g = TreeView_GetNextSibling(tree, g)) {
        wchar_t label[128] = {};
        TVITEMW it = {};
        it.mask = TVIF_TEXT;
        it.hItem = g;
        it.pszText = label;
        it.cchTextMax = 128;
        TreeView_GetItem(tree, &it);
        std::string key = WgNarrow(label);
        // Group rows are labelled "<name>  (<count>)"; the count changes when an
        // entity is added or deleted, so the expansion key must be the bare name.
        const size_t paren = key.find("  (");
        if (paren != std::string::npos) key = key.substr(0, paren);
        g_wgGroupExpanded[key] =
            (TreeView_GetItemState(tree, g, TVIS_EXPANDED) & TVIS_EXPANDED) != 0;
    }
}

static void WgInsertEntry(HWND tree, HTREEITEM group, int entryIndex) {
    const auto& e = g_worldGraphEntries[entryIndex];
    std::wstring label = WgWiden(e.name);
    TVINSERTSTRUCTW ins = {};
    ins.hParent = group;
    ins.hInsertAfter = TVI_LAST;
    ins.item.mask = TVIF_TEXT | TVIF_PARAM;
    ins.item.pszText = label.data();
    ins.item.lParam = (LPARAM)(entryIndex + 1);   // 0 means "group"
    TreeView_InsertItem(tree, &ins);
}

// Populate a group's leaf rows on demand. A no-op when the group already has
// children, so the TVN_ITEMEXPANDED handler and the selection restore can both
// call it without knowing which one ran first.
static void WgPopulateGroup(HWND tree, HTREEITEM group) {
    if (!group) return;
    if (TreeView_GetChild(tree, group)) return;   // already populated
    wchar_t label[128] = {};
    TVITEMW it = {};
    it.mask = TVIF_TEXT;
    it.hItem = group;
    it.pszText = label;
    it.cchTextMax = 128;
    if (!TreeView_GetItem(tree, &it)) return;
    std::string key = WgNarrow(label);
    const size_t paren = key.find("  (");
    if (paren != std::string::npos) key = key.substr(0, paren);
    auto mit = g_wgGroupMembers.find(key);
    if (mit == g_wgGroupMembers.end()) return;
    for (int idx : mit->second) WgInsertEntry(tree, group, idx);
}

static void WgBuildTree(HWND tree) {
    const bool searching = !g_wgFilter.empty();
    g_wgRebuilding = true;

    if (!searching) WgCaptureExpansion(tree);
    TreeView_DeleteAllItems(tree);
    g_wgGroupMembers.clear();

    // Root: the world name.
    std::string worldName = "World";
    {
        const std::string dir = Editor_GetCurrentWorldDir();
        if (!dir.empty()) {
            fs::path stem = fs::path(dir).stem();
            if (!stem.empty()) worldName = stem.string();
        }
    }
    std::string rootText = worldName + "  (" +
                           std::to_string(g_worldGraphEntries.size()) + " objects)";
    std::wstring rootLabel = WgWiden(rootText);
    TVINSERTSTRUCTW rootIns = {};
    rootIns.hParent = TVI_ROOT;
    rootIns.hInsertAfter = TVI_LAST;
    rootIns.item.mask = TVIF_TEXT | TVIF_PARAM;
    rootIns.item.pszText = rootLabel.data();
    rootIns.item.lParam = 0;
    HTREEITEM root = TreeView_InsertItem(tree, &rootIns);
    if (!root) { g_wgRebuilding = false; return; }
    // NOTE: do NOT expand the root here. A tree item with no children yet cannot
    // be expanded, and inserting children afterwards does not retroactively open
    // it - the hierarchy then looks completely empty. Expand after the groups are
    // inserted (below).

    // Build the per-group member lists once (used by lazy population). Filter
    // here because BuildWorldGraphEntries materialises every entry regardless of
    // the search box; the tree shows only matches.
    for (int i = 0; i < (int)g_worldGraphEntries.size(); i++) {
        if (!WgMatchesFilter(g_worldGraphEntries[i])) continue;
        g_wgGroupMembers[g_worldGraphEntries[i].group].push_back(i);
    }

    std::vector<std::pair<std::string, HTREEITEM>> groupItems;
    for (int gi = 0; gi < kGroupCount; gi++) {
        const std::string groupName = kGroupOrder[gi];

        auto membersIt = g_wgGroupMembers.find(groupName);
        if (membersIt == g_wgGroupMembers.end() || membersIt->second.empty()) continue;

        std::string groupText = groupName + "  (" +
                                std::to_string(membersIt->second.size()) + ")";
        std::wstring groupLabel = WgWiden(groupText);
        TVINSERTSTRUCTW gins = {};
        gins.hParent = root;
        gins.hInsertAfter = TVI_LAST;
        gins.item.mask = TVIF_TEXT | TVIF_PARAM;
        gins.item.pszText = groupLabel.data();
        gins.item.lParam = 0;
        HTREEITEM group = TreeView_InsertItem(tree, &gins);
        if (!group) continue;
        groupItems.emplace_back(groupName, group);

        bool expand = searching || groupName == "Map";
        if (!searching) {
            auto it = g_wgGroupExpanded.find(groupName);
            // First ever build: open everything except the two potentially huge
            // geometry groups, which are one click away.
            if (it != g_wgGroupExpanded.end()) expand = it->second;
            else if (groupName != "Brushes" && groupName != "Meshes") expand = true;
        }
        // Populate BEFORE expanding: an item with no children cannot be opened.
        if (expand) {
            WgPopulateGroup(tree, group);
            TreeView_Expand(tree, group, TVE_EXPAND);
        }
    }

    // The root has children now, so this is the point at which expanding it
    // actually shows the hierarchy.
    TreeView_Expand(tree, root, TVE_EXPAND);

    // Restore the row that was selected before the rebuild. Its group may not be
    // populated yet (lazy), so populate + expand it first.
    if (g_wgSelType >= 0) {
        int entryIndex = -1;
        if (WgFindEntry(g_wgSelType, g_wgSelIndex, entryIndex)) {
            const std::string& grp = g_worldGraphEntries[entryIndex].group;
            for (auto& [name, group] : groupItems) {
                if (name != grp) continue;
                WgPopulateGroup(tree, group);
                TreeView_Expand(tree, group, TVE_EXPAND);
                for (HTREEITEM c = TreeView_GetChild(tree, group); c;
                     c = TreeView_GetNextSibling(tree, c)) {
                    TVITEMW it = {};
                    it.mask = TVIF_PARAM;
                    it.hItem = c;
                    TreeView_GetItem(tree, &it);
                    if ((int)it.lParam == entryIndex + 1) {
                        TreeView_SelectItem(tree, c);
                        TreeView_EnsureVisible(tree, c);
                        g_wgRebuilding = false;
                        return;
                    }
                }
            }
        }
    }
    g_wgRebuilding = false;
}

static void WgSelectFromTree(HWND tree, bool openProperties) {
    HTREEITEM item = TreeView_GetSelection(tree);
    if (!item) return;
    TVITEMW it = {};
    it.mask = TVIF_PARAM;
    it.hItem = item;
    if (!TreeView_GetItem(tree, &it)) return;
    const LPARAM lp = it.lParam;
    if (lp <= 0) return;                      // group or root
    const int idx = (int)lp - 1;
    if (idx < 0 || idx >= (int)g_worldGraphEntries.size()) return;

    auto& e = g_worldGraphEntries[idx];
    if (!e.selectable) {
        // Informational row (sound/music emitter). The label and the menu already
        // say "(info)"; selecting it must not pretend to select something.
        return;
    }

    g_wgSelType = e.selType;
    g_wgSelIndex = e.selIndex;

    ed::Selection pick;
    pick.ref = ed::SelRef{e.selType < 0 ? ed::SelKind::None
                                        : ToBusKind((SelType)e.selType),
                          e.selIndex};
    pick.name = e.name;
    pick.x = e.posX; pick.y = e.posY; pick.z = e.posZ;
    ed::EventBus::instance().post(ed::Ev::SelectEntity, pick);
    if (openProperties)
        ed::EventBus::instance().post(ed::Ev::OpenProperties, pick);
}

static void WgShowContextMenu(HWND hwnd, HWND tree) {
    POINT pt;
    GetCursorPos(&pt);

    // Right-click selects the row under the cursor first, so the menu acts on
    // what the user pointed at rather than on the previous selection.
    TVHITTESTINFO ht = {};
    ht.pt = pt;
    ScreenToClient(tree, &ht.pt);
    HTREEITEM hit = TreeView_HitTest(tree, &ht);
    if (hit) TreeView_SelectItem(tree, hit);

    HTREEITEM item = TreeView_GetSelection(tree);
    if (!item) return;
    TVITEMW it = {};
    it.mask = TVIF_PARAM;
    it.hItem = item;
    if (!TreeView_GetItem(tree, &it)) return;
    if (it.lParam <= 0) return;
    const int idx = (int)it.lParam - 1;
    if (idx < 0 || idx >= (int)g_worldGraphEntries.size()) return;
    auto& e = g_worldGraphEntries[idx];

    HMENU menu = CreatePopupMenu();
    AppendMenuA(menu, MF_STRING, 2001, "Properties");
    if (e.isLevelHeader) {
        // The level has no transform and cannot be deleted/duplicated.
        int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                                 pt.x, pt.y, 0, hwnd, NULL);
        DestroyMenu(menu);
        if (cmd == 2001) WgSelectFromTree(tree, true);
        return;
    }
    if (!e.selectable) {
        AppendMenuA(menu, MF_STRING | MF_GRAYED, 2001, "Properties (not selectable)");
        TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
        DestroyMenu(menu);
        return;
    }

    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(menu, MF_STRING, 2002, "Frame Selection");
    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(menu, MF_STRING, 2003, "Delete");
    AppendMenuA(menu, MF_STRING, 2004, "Duplicate");

    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                             pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(menu);

    if (cmd == 2001) {
        WgSelectFromTree(tree, true);
    } else if (cmd == 2002) {
        WgSelectFromTree(tree, false);
        g_editorPanels.focusSelection = true;
    } else if (cmd == 2003 || cmd == 2004) {
        ed::Selection pick;
        pick.ref = ed::SelRef{ToBusKind((SelType)e.selType), e.selIndex};
        pick.name = e.name;
        pick.x = e.posX; pick.y = e.posY; pick.z = e.posZ;
        ed::EventBus::instance().post(cmd == 2003 ? ed::Ev::DeleteEntity
                                                  : ed::Ev::DuplicateEntity, pick);
    }
}

// Select and reveal whatever the viewport currently has selected. This is the
// other half of the selection sync: click something in 3D, press Reveal, and the
// tree opens its group and scrolls to the row.
static void WgRevealCurrentSelection(HWND tree) {
    const int selType = EdSel_CurrentType();
    const int selIndex = EdSel_CurrentIndex();
    if (selType == sel::NONE || selType < 0) return;

    int entryIndex = -1;
    if (!WgFindEntry(selType, selIndex, entryIndex)) {
        // The data may have changed since the last build (delete, undo, spawn).
        BuildWorldGraphEntries();
        WgBuildTree(tree);
        if (!WgFindEntry(selType, selIndex, entryIndex)) {
            MessageBoxA(GetAncestor(tree, GA_ROOT),
                        "The current selection is not in the world hierarchy "
                        "(it may have just been deleted).",
                        "World Hierarchy", MB_OK | MB_ICONINFORMATION);
            return;
        }
    }

    HTREEITEM root = TreeView_GetRoot(tree);
    for (HTREEITEM g = root ? TreeView_GetChild(tree, root) : nullptr; g;
         g = TreeView_GetNextSibling(tree, g)) {
        // Lazy groups have no children until expanded; populate first.
        WgPopulateGroup(tree, g);
        for (HTREEITEM c = TreeView_GetChild(tree, g); c;
             c = TreeView_GetNextSibling(tree, c)) {
            TVITEMW it = {};
            it.mask = TVIF_PARAM;
            it.hItem = c;
            TreeView_GetItem(tree, &it);
            if ((int)it.lParam == entryIndex + 1) {
                TreeView_Expand(tree, g, TVE_EXPAND);
                TreeView_SelectItem(tree, c);
                TreeView_EnsureVisible(tree, c);
                // Deliberately do NOT SetFocus(tree): opening the panel from F3
                // must leave keyboard focus on the viewport, or the next F3 would
                // be swallowed by the panel's own input handling.
                return;
            }
        }
    }
}

static void WgLayout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int m = 6, btnH = 26, rowH = 24;
    const int btnW = 84;

    if (g_wgSearch)
        MoveWindow(g_wgSearch, m, m, rc.right - 2 * m, rowH, TRUE);
    if (g_wgTree) {
        const int top = m + rowH + m;
        const int h = rc.bottom - top - btnH - 2 * m;
        MoveWindow(g_wgTree, m, top, rc.right - 2 * m, (h > 40 ? h : 40), TRUE);
    }
    const int by = rc.bottom - m - btnH;
    int bx = m;
    if (g_wgReveal)  { MoveWindow(g_wgReveal,  bx, by, btnW, btnH, TRUE); bx += btnW + m; }
    if (g_wgRefresh) { MoveWindow(g_wgRefresh, bx, by, btnW, btnH, TRUE); bx += btnW + m; }
    if (g_wgClose)   { MoveWindow(g_wgClose, rc.right - m - btnW, by, btnW, btnH, TRUE); }
}

static LRESULT CALLBACK WorldGraphProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        // Search first so the tab ring visits it before the tree (creation order is
        // the ring order; the tree is the bigger control and comes second).
        g_wgSearch = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            6, 6, 100, 24, hwnd, (HMENU)ID_WG_SEARCH, g_hInst, nullptr);
        SendMessageW(g_wgSearch, EM_SETCUEBANNER, TRUE, (LPARAM)L"Filter by name or type");

        g_wgTree = CreateWindowEx(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP |
            TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT |
            TVS_SHOWSELALWAYS | TVS_INFOTIP,
            6, 36, 100, 100, hwnd, (HMENU)ID_WG_TREE, g_hInst, nullptr);

        g_wgReveal  = CreateButton(hwnd, L"Reveal",  6, 200, 84, 26, ID_WG_REVEAL);
        g_wgRefresh = CreateButton(hwnd, L"Refresh", 96, 200, 84, 26, ID_WG_REFRESH);
        g_wgClose   = CreateButton(hwnd, L"Close",   186, 200, 84, 26, ID_WG_CLOSE);

        g_wgGroupExpanded.clear();
        BuildWorldGraphEntries();
        WgBuildTree(g_wgTree);
        WgLayout(hwnd);
        break;
    }
    case WM_GETMINMAXINFO:
        EnforceMinTrackSize(l, 320, 240);
        break;
    case WM_SIZE:
        WgLayout(hwnd);
        break;
    case WM_USER + 50:
        BuildWorldGraphEntries();
        WgBuildTree(g_wgTree);
        break;
    case WM_NOTIFY: {
        NMHDR* nm = (NMHDR*)l;
        if (nm->idFrom == ID_WG_TREE) {
            switch (nm->code) {
            case TVN_SELCHANGEDW:
                if (!g_wgRebuilding) WgSelectFromTree(g_wgTree, false);
                break;
            case TVN_ITEMEXPANDEDW:
                // Lazy population: insert a group's leaves the first time it
                // opens, so a refresh never creates O(objects) tree items.
                if (!g_wgRebuilding) {
                    NMTREEVIEWW* ntv = (NMTREEVIEWW*)l;
                    if (ntv && ntv->itemNew.hItem && ntv->action == TVE_EXPAND)
                        WgPopulateGroup(g_wgTree, ntv->itemNew.hItem);
                }
                break;
            case NM_DBLCLK:
                if (!g_wgRebuilding) WgSelectFromTree(g_wgTree, true);
                break;
            case NM_RCLICK:
                if (!g_wgRebuilding) WgShowContextMenu(hwnd, g_wgTree);
                break;
            case NM_RETURN:
                if (!g_wgRebuilding) WgSelectFromTree(g_wgTree, true);
                break;
            default: break;
            }
        }
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == ID_WG_CLOSE) {
            ShowWorldGraph(false);
        } else if (id == ID_WG_REFRESH) {
            BuildWorldGraphEntries();
            WgBuildTree(g_wgTree);
        } else if (id == ID_WG_REVEAL) {
            WgRevealCurrentSelection(g_wgTree);
        } else if (id == ID_WG_SEARCH && HIWORD(w) == EN_CHANGE) {
            wchar_t buf[128] = {};
            GetWindowTextW(g_wgSearch, buf, 128);
            g_wgFilter = WgNarrow(buf);
            WgBuildTree(g_wgTree);
        }
        break;
    }
    case WM_CLOSE:
        ShowWorldGraph(false);
        break;
    case WM_DESTROY:
        g_wgTree = nullptr;
        g_wgSearch = nullptr;
        g_wgReveal = nullptr;
        g_wgRefresh = nullptr;
        g_wgClose = nullptr;
        g_editorPanels.hWorldGraph = nullptr;
        break;
    default:
        return DefWindowProc(hwnd, msg, w, l);
    }
    return 0;
}

void ShowWorldGraph(bool show) {
    g_editorPanels.showWorldGraph = show;
    if (g_editorPanels.hWorldGraph) {
        ShowWindow((HWND)g_editorPanels.hWorldGraph, show ? SW_SHOW : SW_HIDE);
        // Opening is the one moment that MUST show current data. The tree is built
        // in WM_CREATE, which runs before the world is loaded.
        if (show) {
            g_editorPanels.worldGraphDirty = false;
            RefreshWorldGraph();
            WgRevealCurrentSelection(g_wgTree);
        }
    }
}

void RefreshWorldGraph() {
    if (g_editorPanels.hWorldGraph)
        SendMessage((HWND)g_editorPanels.hWorldGraph, WM_USER + 50, 0, 0);
}
