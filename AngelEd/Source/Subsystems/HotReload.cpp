// =============================================================================
// Subsystems/HotReload.cpp
//
// FRAGMENT - not a standalone translation unit. Included by Main.cpp after
// Subsystems/WorldGraphBridge.cpp (it uses Editor_GetCurrentWorldDir) and after
// Core/EditorLog.cpp (it logs). See Main.cpp for why the core layer is a unity
// TU.
// =============================================================================
//
// HOT RELOAD - live .ozls defs and PawnDefs/*.cfg, with no Refresh click.
//
// WHAT IT DOES
//   * watches GameData recursively for *.ozls and Pawn/PawnDefs/*.cfg writes
//   * debounces each path, then reloads ONLY that file
//   * refreshes the panels that were showing the old values
//
// WHAT IT DELIBERATELY DOES NOT DO
//   * It does not touch the WORLD. A def is not a world entity, so this never
//     calls MarkWorldGraphDirty(), pushes History, or reloads the document. The
//     WorldGraph and the viewport keep their selection, camera and undo stack.
//   * It does not rebuild the Actor Hierarchy for a stats-only edit. That tree
//     lists def NAMES, so only an added/removed name changes it - and when it
//     does, PopulateTreeView preserves expansion and selection.
//   * It never discards typed-but-unapplied stat edits in the Properties panel
//     (RefreshOpenDefProperties refuses while propDefPendingEdits is non-empty).
//
// WHY A WATCHER AND NOT A POLL
//   GameData holds ~2000+ files, so polling mtimes means either stat-ing all of
//   them or a recursive walk every tick. ReadDirectoryChangesW reports exactly
//   the paths that changed, including creates and deletes, for free.
//
// WHY ReloadFile AND NOT Init()
//   Init() rebuilds the registry and can relocate EntityDef objects. Find()
//   returns a pointer into the registry and LightningEntityManager instances /
//   SkyZoneNode::def store it, so a full rebuild can silently repoint them at a
//   different def. ReloadFile updates the by-name entry in place.
// =============================================================================

#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <thread>

// ---------------------------------------------------------------------------
// PawnDefs/*.cfg loading
//
// ONE parser, used by the startup load in main() and by the hot reload below.
// The editor's copy of the client's loader in Core.hpp has to stay in step with
// it (every field the client reads must be read here too, or the editor shows a
// different enemy than the game does). Keeping it in a second place inside
// main() is how those two drifted before.
// ---------------------------------------------------------------------------

static const char* kPawnDefsDir = "GameData/Pawn/PawnDefs";

// GameData/Pawn/PawnDefs/<file>.cfg -> the `name =` it defines, so a deleted or
// renamed file can remove the def it used to own. Loose files only: a packaged
// def has no file to delete.
static std::map<std::string, std::string> s_cfgDefName;

// Absolute, forward-slashed, lower-cased: the form two producers of the same
// path (the watcher's GameData-relative events and the scanner's absolute
// sourcePaths) can be compared in. The filesystem is case-insensitive on the
// only platform AngelEd builds for.
static std::string NormalPath(const std::string& p) {
    std::error_code ec;
    const fs::path abs = fs::absolute(fs::path(p), ec);
    std::string s = (ec ? fs::path(p) : abs).lexically_normal().string();
    for (auto& c : s) {
        if (c == '\\') c = '/';
        c = (char)::tolower((unsigned char)c);
    }
    return s;
}

// .cfg scalars used to be read with std::stof, which THROWS on malformed input -
// in a loop that a file watcher now runs on every save. A bad value keeps the
// field's default and says so, exactly like the OZONE parser's TryFloat.
static bool CfgFloat(const std::string& value, float& out) {
    if (value.empty()) return false;
    char* end = nullptr;
    const float v = std::strtof(value.c_str(), &end);
    if (!end || *end != '\0') return false;
    out = v;
    return true;
}

static bool CfgInt(const std::string& value, int& out) {
    if (value.empty()) return false;
    char* end = nullptr;
    const long v = std::strtol(value.c_str(), &end, 10);
    if (!end || *end != '\0') return false;
    out = (int)v;
    return true;
}

// Parse one PawnDefs .cfg into `out`. Returns false when the file has no name=,
// which is the one field every other field is keyed on. Unknown keys are
// ignored, matching the client's loader.
static bool ParsePawnCfgText(const std::string& text, const std::string& baseDir,
                             PawnDef& out) {
    std::istringstream f(text);
    std::string name, sp, sc;
    std::string modelPath, modelTex, meshType;
    std::string animIdle, animPatrol, animChase, animReturn, animDeath;
    std::string animFile;
    float speed = 1.5f, aggroRange = 6.0f, attackRange = 1.5f, damage = 10.0f;
    float animSpeed = 1.0f, modelScale = 1.0f, modelYOffset = 0.0f;
    float hoverAmplitude = 0.35f, hoverSpeed = 1.6f;
    bool hover = false;
    int maxHealth = 100;

    std::string line;
    while (std::getline(f, line)) {
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        val.erase(0, val.find_first_not_of(" \t"));
        val.erase(val.find_last_not_of(" \t\r") + 1);

        if (key == "name") name = val;
        else if (key == "sprite_path") sp = val;
        else if (key == "scream_path") sc = val;
        // Model fields - the client's loader (Core.hpp) reads these.
        else if (key == "model_path") modelPath = val;
        else if (key == "model_texture") modelTex = val;
        else if (key == "mesh_type") meshType = val;
        else if (key == "anim_idle") animIdle = val;
        else if (key == "anim_patrol") animPatrol = val;
        else if (key == "anim_chase") animChase = val;
        else if (key == "anim_return") animReturn = val;
        else if (key == "anim_death") animDeath = val;
        else if (key == "animfile" || key == "anim_file") animFile = val;
        else if (key == "hover")
            hover = (val == "1" || val == "true" || val == "True" || val == "yes");
        else if (key == "speed")          { if (!CfgFloat(val, speed))          OZ_WARN("PawnDef: bad speed '%s'", val.c_str()); }
        else if (key == "aggroRange")     { if (!CfgFloat(val, aggroRange))     OZ_WARN("PawnDef: bad aggroRange '%s'", val.c_str()); }
        else if (key == "attackRange")    { if (!CfgFloat(val, attackRange))    OZ_WARN("PawnDef: bad attackRange '%s'", val.c_str()); }
        else if (key == "damage")         { if (!CfgFloat(val, damage))         OZ_WARN("PawnDef: bad damage '%s'", val.c_str()); }
        else if (key == "anim_speed")     { if (!CfgFloat(val, animSpeed))      OZ_WARN("PawnDef: bad anim_speed '%s'", val.c_str()); }
        else if (key == "model_scale")    { if (!CfgFloat(val, modelScale))     OZ_WARN("PawnDef: bad model_scale '%s'", val.c_str()); }
        else if (key == "model_y_offset") { if (!CfgFloat(val, modelYOffset))   OZ_WARN("PawnDef: bad model_y_offset '%s'", val.c_str()); }
        else if (key == "hover_amplitude"){ if (!CfgFloat(val, hoverAmplitude)) OZ_WARN("PawnDef: bad hover_amplitude '%s'", val.c_str()); }
        else if (key == "hover_speed")    { if (!CfgFloat(val, hoverSpeed))     OZ_WARN("PawnDef: bad hover_speed '%s'", val.c_str()); }
        else if (key == "maxHealth")      { if (!CfgInt(val, maxHealth))        OZ_WARN("PawnDef: bad maxHealth '%s'", val.c_str()); }
    }

    if (name.empty()) return false;

    out = PawnDef{};
    out.name = name;
    out.speed = speed;
    out.aggroRange = aggroRange;
    out.attackRange = attackRange;
    out.damage = damage;
    out.maxHealth = maxHealth;
    out.sprite_path = sp;
    out.scream_path = sc;
    out.model_path = modelPath;
    out.model_texture = modelTex;
    out.model_scale = modelScale;
    out.mesh_type = meshType;
    out.anim_idle = animIdle;
    out.anim_patrol = animPatrol;
    out.anim_chase = animChase;
    out.anim_return = animReturn;
    out.anim_death = animDeath;
    out.anim_speed = animSpeed;
    out.anim_file = animFile;
    out.model_y_offset = modelYOffset;
    out.hover = hover;
    out.hover_amplitude = hoverAmplitude;
    out.hover_speed = hoverSpeed;
    out.baseDir = baseDir;
    return true;
}

// Register-or-update one parsed def. UpdateDef keeps the slot (and pushes the
// movement knobs onto live pawns); RegisterDef is only for a genuinely new name.
// UpdateDef returning false IS the "new name" test - FindDef is private on
// purpose, and having one place answer the question keeps the two in step.
static void ApplyPawnDef(const PawnDef& def, bool& wasNew) {
    auto& ps = PawnSystem::Instance();
    wasNew = !ps.UpdateDef(def);
    if (wasNew) ps.RegisterDef(def);
}

static void RegisterHardcodedPawnDefs() {
    auto& ps = PawnSystem::Instance();
    struct Fb { const char* name; float speed, aggro, attack, damage; int hp; };
    static const Fb kFallback[] = {
        {"Walker",  1.5f, 6.0f,  1.5f, 10.0f, 100},
        {"Skaarj",  2.5f, 10.0f, 2.0f, 20.0f, 150},
        {"Brute",   1.0f, 4.0f,  1.5f, 30.0f, 250},
        {"Floater", 1.2f, 8.0f,  3.0f, 15.0f, 80},
    };
    for (const auto& f : kFallback) {
        PawnDef d;
        d.name = f.name;
        d.speed = f.speed;
        d.aggroRange = f.aggro;
        d.attackRange = f.attack;
        d.damage = f.damage;
        d.maxHealth = f.hp;
        ps.RegisterDef(d);
        PawnManagerAddPawn(f.name, (std::string("GameData/Pawn/Pawns/") + f.name + ".png").c_str());
    }
}

// Load every PawnDefs/*.cfg (loose first, then packaged; loose wins). Returns
// the number of defs applied. Safe to call again: existing names are UPDATED,
// never duplicated (RegisterDef refuses duplicates, which would otherwise leave
// an edited def frozen at its first-loaded values).
static int LoadAllPawnDefs() {
    std::vector<std::pair<std::string, std::string>> cfgFiles; // basename, text
    std::vector<std::string> cfgPaths;                          // parallel; "" = packaged

    if (fs::exists(kPawnDefsDir)) {
        for (auto& entry : fs::directory_iterator(kPawnDefsDir)) {
            if (!entry.is_regular_file()) continue;
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext != ".cfg") continue;
            std::ifstream f(entry.path());
            if (!f.is_open()) continue;
            std::stringstream ss;
            ss << f.rdbuf();
            cfgFiles.emplace_back(entry.path().filename().string(), ss.str());
            cfgPaths.push_back(entry.path().string());
        }
    }
    for (const std::string& cfg : PackageAssetLoader::Instance().ListAssetBasenames(".cfg")) {
        bool seen = false;
        for (auto& cf : cfgFiles) if (cf.first == cfg) { seen = true; break; }
        if (seen) continue;
        std::string text = LoadFileTextWithFallback((std::string(kPawnDefsDir) + "/" + cfg).c_str());
        if (!text.empty()) {
            cfgFiles.emplace_back(cfg, text);
            cfgPaths.push_back("");   // packaged: no editable source file
        }
    }

    if (cfgFiles.empty()) {
        EditorLog("WARN: no pawn defs loose or packaged (%s), using hardcoded defaults", kPawnDefsDir);
        RegisterHardcodedPawnDefs();
        return 4;
    }

    int loaded = 0;
    for (size_t i = 0; i < cfgFiles.size(); i++) {
        PawnDef def;
        if (!ParsePawnCfgText(cfgFiles[i].second, std::string(kPawnDefsDir) + "/", def)) {
            EditorLog("WARN: %s defines no name= - skipped", cfgFiles[i].first.c_str());
            continue;
        }
        bool wasNew = false;
        ApplyPawnDef(def, wasNew);
        if (!cfgPaths[i].empty()) s_cfgDefName[NormalPath(cfgPaths[i])] = def.name;
        PawnManagerAddPawn(def.name.c_str(),
            (!def.sprite_path.empty() ? def.sprite_path
                                      : std::string("GameData/Pawn/Pawns/") + def.name + ".png").c_str());
        loaded++;
        EditorLog("Loaded pawn def: %s", def.name.c_str());
    }
    EditorLog("Loaded %d pawn definitions from %s (loose + packaged)", loaded, kPawnDefsDir);
    return loaded;
}

// ---------------------------------------------------------------------------
// Applying one changed file
// ---------------------------------------------------------------------------

// Rebuild the Properties panel's def section when the file it is showing is the
// one that just changed. The panel decides whether it may rebuild (it refuses
// while there are unapplied row edits); this only routes the match.
static void RefreshOpenPropsIf(const std::string& path) {
    if (g_editorPanels.propDefPath.empty()) return;
    if (NormalPath(g_editorPanels.propDefPath) != NormalPath(path)) return;
    RefreshOpenDefProperties();
}

static void ApplyOzlsChange(const std::string& path) {
    auto res = LightningEntityRegistry::Instance().ReloadFile(path);
    if (!res.ok) {
        EditorLog("Hot reload: %s no longer parses - keeping the previous definition", path.c_str());
        return;
    }
    if (res.added.empty() && res.changed.empty() && res.removed.empty()) return;

    EditorLog("Hot reload: %s (%zu changed, %zu added, %zu removed)",
              path.c_str(), res.changed.size(), res.added.size(), res.removed.size());

    // Icons are memoised from the def's `icon` stat; a reloaded def may point at
    // a different one.
    EngineBillboard::InvalidateIconCache();

    // The list is rebuilt in place and keeps its selected row. Cheap enough to
    // do unconditionally: it also keeps the backing list correct for a later
    // open of the panel.
    RefreshScriptManager();

    // Only a NAME appearing or disappearing changes what the Actor Hierarchy /
    // Pickup lists contain. A stats edit (the common save) must not rebuild
    // either tree - that is the whole point of not touching the hierarchy.
    if (!res.added.empty() || !res.removed.empty()) {
        RefreshPawnManager();
        if (g_editorPanels.hPickupPanel)
            SendMessage((HWND)g_editorPanels.hPickupPanel, WM_USER + 50, 0, 0);
    }

    // NOTE: deliberately no MarkWorldGraphDirty() here. A def is not a world
    // entity: the world document, its selection, camera and undo history are
    // untouched by a .ozls edit, so rebuilding the WorldGraph would only throw
    // away the user's place in it.
    RefreshOpenPropsIf(path);
}

static void ApplyPawnCfgChange(const std::string& path) {
    auto& ps = PawnSystem::Instance();
    const std::string key = NormalPath(path);

    auto rec = s_cfgDefName.find(key);
    const std::string oldName = (rec != s_cfgDefName.end()) ? rec->second : std::string();

    std::error_code ec;
    if (!fs::exists(path, ec) || ec) {
        if (!oldName.empty() && ps.RemoveDef(oldName.c_str())) {
            EditorLog("Hot reload: removed pawn def '%s' (%s deleted)", oldName.c_str(), path.c_str());
            s_cfgDefName.erase(rec);
            RefreshPawnManager();
            RefreshOpenPropsIf(path);
        }
        return;
    }

    std::ifstream f(path);
    if (!f.is_open()) return;
    std::stringstream ss;
    ss << f.rdbuf();
    PawnDef def;
    if (!ParsePawnCfgText(ss.str(), std::string(kPawnDefsDir) + "/", def)) {
        // A half-typed file has no name= yet. Keep the last good def rather than
        // making the enemy disappear while the author is still typing.
        EditorLog("Hot reload: %s has no name= - keeping the previous def", path.c_str());
        return;
    }

    // A renamed def must not leave the old name spawnable.
    const bool renamed = !oldName.empty() && oldName != def.name;
    if (renamed) ps.RemoveDef(oldName.c_str());

    bool wasNew = false;
    ApplyPawnDef(def, wasNew);
    s_cfgDefName[key] = def.name;

    EditorLog("Hot reload: pawn def '%s' %s%s", def.name.c_str(),
              wasNew ? "added" : "reloaded", renamed ? " (renamed)" : "");

    // Same rule as .ozls: the tree lists names, so a value change needs no
    // rebuild. Only an add/rename/remove changes its rows.
    if (wasNew || renamed) RefreshPawnManager();
    RefreshOpenPropsIf(path);
}

static void ApplyFileChange(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == ".ozls")      ApplyOzlsChange(path);
    else if (ext == ".cfg")  ApplyPawnCfgChange(path);
}

// ---------------------------------------------------------------------------
// Watcher
// ---------------------------------------------------------------------------
#ifdef _WIN32
namespace hotreload {

// How long a path must be quiet before it is re-read. Editors save with a
// temp-file + rename and emit several records per save; reading on the first
// record parses a half-written file.
constexpr int kSettleMs = 180;

struct PendingEdit {
    std::chrono::steady_clock::time_point due;
    uint64_t stamp = 0;      // last observed write time; apply once it repeats
};

static HANDLE s_dir = INVALID_HANDLE_VALUE;
static HANDLE s_stop = nullptr;
static std::thread s_thread;
static std::mutex s_mutex;
static std::vector<std::string> s_events;     // absolute paths, worker -> main
static bool s_overflow = false;               // event buffer overflowed
static std::string s_root;                    // absolute GameData path
static std::map<std::string, PendingEdit> s_pending;   // main thread only
// Write time of a file the EDITOR itself already reloaded (Properties panel
// Apply). The watcher sees that write too; without this the panel would be
// rebuilt a second time ~200 ms after the user pressed Apply.
static std::map<std::string, uint64_t> s_appliedStamp; // main thread only

static void Queue(const std::string& path) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_events.push_back(path);
}

// True for the two kinds of file this feature owns. Everything else in GameData
// (textures, models, worlds) is ignored - a .ozone save must not trigger a
// registry reload.
static bool Interesting(const fs::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == ".ozls") return true;
    if (ext != ".cfg") return false;
    // A .cfg anywhere else (characters.cfg, a game config) is not a pawn def.
    return NormalPath(p.string()).find("/gamedata/pawn/pawndefs/") != std::string::npos;
}

static void WatchThread() {
    // DWORD-aligned storage: FILE_NOTIFY_INFORMATION records are 4-byte aligned.
    std::vector<DWORD> buffer(16 * 1024);   // 64 KiB of events
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    while (true) {
        DWORD bytes = 0;
        const BOOL started = ReadDirectoryChangesW(
            s_dir, buffer.data(), (DWORD)(buffer.size() * sizeof(DWORD)),
            TRUE,   // recursive: defs live anywhere under GameData
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE |
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME,
            &bytes, &ov, nullptr);
        if (!started) break;

        HANDLE waits[2] = { s_stop, ov.hEvent };
        const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0) {         // Stop() signalled
            CancelIo(s_dir);
            break;
        }

        DWORD got = 0;
        if (!GetOverlappedResult(s_dir, &ov, &got, FALSE) || got == 0) {
            // got == 0 means the buffer overflowed and events were LOST. Ask the
            // main thread for a full rescan instead of silently missing an edit.
            { std::lock_guard<std::mutex> lock(s_mutex); s_overflow = true; }
            ResetEvent(ov.hEvent);
            continue;
        }

        const BYTE* base = reinterpret_cast<const BYTE*>(buffer.data());
        DWORD offset = 0;
        while (true) {
            const FILE_NOTIFY_INFORMATION* info =
                reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(base + offset);
            std::wstring wname(info->FileName, info->FileNameLength / sizeof(wchar_t));
            const fs::path full = fs::path(s_root) / fs::path(wname);
            if (Interesting(full))
                Queue(full.lexically_normal().string());
            if (info->NextEntryOffset == 0) break;
            offset += info->NextEntryOffset;
        }
        ResetEvent(ov.hEvent);
    }

    if (ov.hEvent) CloseHandle(ov.hEvent);
}

} // namespace hotreload
#endif // _WIN32

// Re-read everything. Only used when the OS event buffer overflowed: normal
// operation reloads just the path that changed.
static void HotReload_FullRescan() {
    EditorLog("Hot reload: watcher buffer overflowed - rescanning every definition");
    LightningEntityRegistry::Instance().Init();
    LightningEntityRegistry::Instance().LoadWorldOverrides(Editor_GetCurrentWorldDir());
    LoadAllPawnDefs();
    RefreshScriptManager();
    RefreshPawnManager();
}

// Record that the editor itself just wrote + reloaded `path`. The watcher will
// still see the write; this lets Poll() swallow exactly that one echo so the
// Properties panel is not rebuilt a second time after the user pressed Apply.
void HotReload_NoteApplied(const std::string& path) {
#ifdef _WIN32
    std::error_code ec;
    const auto stamp = fs::last_write_time(path, ec);
    if (ec) return;
    hotreload::s_appliedStamp[NormalPath(path)] =
        (uint64_t)stamp.time_since_epoch().count();
#else
    (void)path;
#endif
}

void HotReload_Start() {
#ifdef _WIN32
    if (hotreload::s_thread.joinable()) return;

    std::error_code ec;
    hotreload::s_root = fs::absolute(fs::path("GameData"), ec).string();
    if (ec || !fs::exists(hotreload::s_root)) {
        EditorLog("Hot reload: no GameData directory to watch");
        return;
    }

    hotreload::s_dir = CreateFileW(fs::path(hotreload::s_root).wstring().c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (hotreload::s_dir == INVALID_HANDLE_VALUE) {
        EditorLog("Hot reload: cannot watch GameData (error %lu)", GetLastError());
        return;
    }

    hotreload::s_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    hotreload::s_thread = std::thread(hotreload::WatchThread);
    EditorLog("Hot reload: watching GameData for *.ozls and PawnDefs/*.cfg");
#endif
}

void HotReload_Poll() {
#ifdef _WIN32
    using namespace hotreload;
    if (!s_thread.joinable()) return;

    std::vector<std::string> events;
    bool overflow = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        events.swap(s_events);
        overflow = s_overflow;
        s_overflow = false;
    }
    if (overflow) HotReload_FullRescan();

    const auto now = std::chrono::steady_clock::now();
    const auto settle = std::chrono::milliseconds(kSettleMs);

    for (const auto& e : events)
        s_pending[e] = PendingEdit{ now + settle, 0 };

    for (auto it = s_pending.begin(); it != s_pending.end(); ) {
        if (now < it->second.due) { ++it; continue; }

        const std::string path = it->first;
        std::error_code ec;
        const auto stamp = fs::last_write_time(path, ec);
        const uint64_t s = ec ? 0 : (uint64_t)stamp.time_since_epoch().count();
        if (s != it->second.stamp) {
            // Still being written (or first look): observe once more, then apply
            // only when a whole settle window passes with no further change.
            it->second.stamp = s;
            it->second.due = now + settle;
            ++it;
            continue;
        }
        it = s_pending.erase(it);

        // Swallow the echo of a write the editor already applied itself.
        auto applied = s_appliedStamp.find(NormalPath(path));
        if (applied != s_appliedStamp.end()) {
            const bool same = (applied->second == s);
            s_appliedStamp.erase(applied);
            if (same) continue;
        }

        ApplyFileChange(path);
    }
#endif
}

void HotReload_Stop() {
#ifdef _WIN32
    using namespace hotreload;
    if (s_stop) SetEvent(s_stop);
    if (s_thread.joinable()) s_thread.join();
    if (s_dir != INVALID_HANDLE_VALUE) {
        CloseHandle(s_dir);
        s_dir = INVALID_HANDLE_VALUE;
    }
    if (s_stop) {
        CloseHandle(s_stop);
        s_stop = nullptr;
    }
#endif
}
