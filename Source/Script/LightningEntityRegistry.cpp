#include "LightningEntityRegistry.hpp"
#include "LightningScriptParser.hpp"
#include "../Log.hpp"
#include "../Package/PackageAssetLoader.hpp"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <unordered_set>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Init — scan packages + GameData/ for *.ozls files
// ---------------------------------------------------------------------------
void LightningEntityRegistry::Init() {
    m_defs.clear();
    m_allDefs.clear();
    std::vector<std::string> contents;
    std::vector<std::string> paths;

    // Basenames already loaded from the filesystem; package copies of the same
    // file are skipped so defs are not duplicated (filesystem is authoritative).
    std::unordered_set<std::string> seenBasenames;

    // Scan GameData/ recursively for .ozls
    fs::path gd = fs::current_path() / "GameData";
    if (fs::exists(gd)) {
        for (auto& entry : fs::recursive_directory_iterator(gd)) {
            if (entry.is_regular_file()) {
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".ozls") {
                    std::ifstream file(entry.path());
                    if (file.is_open()) {
                        std::string content((std::istreambuf_iterator<char>(file)),
                                             std::istreambuf_iterator<char>());
                        contents.push_back(content);
                        paths.push_back(entry.path().string());
                        seenBasenames.insert(entry.path().filename().string());
                    }
                }
            }
        }
    }

    // Scan packages for .ozls entries (skip filesystem duplicates/dedup packages)
    std::vector<std::string> pkgFiles;
    PackageAssetLoader::Instance().ListAllFiles(pkgFiles);
    for (const auto& pkgPath : pkgFiles) {
        std::string ext = pkgPath.substr(pkgPath.rfind('.'));
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".ozls") continue;
        std::string base = pkgPath.substr(pkgPath.rfind('/') + 1);
        if (seenBasenames.count(base)) continue;
        seenBasenames.insert(base);
        size_t sz;
        const uint8_t* data = PackageAssetLoader::Instance().Find(pkgPath.c_str(), sz);
        if (data) {
            contents.push_back(std::string((const char*)data, sz));
            paths.push_back(pkgPath);
        }
    }

    // Parse all found .ozls files
    auto defs = LightningScriptParser::ParseAll(contents, paths);
    // Keep every parsed def so world-local defs with colliding names
    // (e.g. "zone_sky_0" in each world) stay resolvable by world dir.
    // Copy into m_allDefs BEFORE moving into the map (moved-from defs are empty).
    for (auto& def : defs) {
        m_allDefs.push_back(def);
    }
    for (auto& def : defs) {
        m_defs[def.name] = std::move(def);
    }

    OZ_INFO("LightningRegistry: loaded %zu entity definitions%s",
            m_defs.size(),
            m_defs.size() > 0 ? "" : " (no .ozls files found)");
}

// ---------------------------------------------------------------------------
// Find — look up by exact name
// ---------------------------------------------------------------------------
const EntityDef* LightningEntityRegistry::Find(const std::string& name) const {
    auto it = m_defs.find(name);
    if (it != m_defs.end()) return &it->second;
    return nullptr;
}

// ---------------------------------------------------------------------------
// FindByType — collect all defs of a given type (includes every world's defs)
// ---------------------------------------------------------------------------
void LightningEntityRegistry::FindByType(EntityType type, std::vector<const EntityDef*>& out) const {
    out.clear();
    for (auto& def : m_allDefs) {
        if (def.type == type) out.push_back(&def);
    }
}

// ---------------------------------------------------------------------------
// Register — add or update a single definition (used by editor live reload)
// ---------------------------------------------------------------------------
bool LightningEntityRegistry::Register(const EntityDef& def) {
    if (def.name.empty() || def.type == EntityType::UNKNOWN) return false;
    m_defs[def.name] = def;
    for (auto& d : m_allDefs) {
        if (d.name == def.name) { d = def; return true; }
    }
    m_allDefs.push_back(def);
    return true;
}

// ---------------------------------------------------------------------------
// LoadWorldOverrides — make the by-name map world-correct
// ---------------------------------------------------------------------------
namespace {

std::string Slashes(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

// Strip trailing separators so "…/Dust_Ravine/" and "…/Dust_Ravine" agree.
std::string StripTrailingSlash(std::string s) {
    while (!s.empty() && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    return s;
}

// The world NAME a def was parsed from, or "" when the def is global
// (a category folder such as Pawn/Pawns or Tex/Global/Gun, or a package)
// Comparing names rather than full paths keeps this working for the absolute
// paths the .ozls scanner records and for packaged worlds.
std::string WorldNameOfDef(const std::string& sourcePath) {
    std::string sp = Slashes(sourcePath);

    // Packaged worlds: System/Data/Zones/world_<Name>.ozone
    const size_t zones = sp.find("System/Data/Zones/world_");
    if (zones != std::string::npos) {
        sp = sp.substr(zones + std::strlen("System/Data/Zones/world_"));
        const size_t dot = sp.find_first_of(".\\");
        return (dot == std::string::npos) ? sp : sp.substr(0, dot);
    }

    static const char* kRoot = "GameData/Worlds/";
    const size_t at = sp.find(kRoot);
    if (at == std::string::npos)
        return "";
    sp = sp.substr(at + std::strlen(kRoot));
    const size_t slash = sp.find('/');
    return (slash == std::string::npos) ? sp : sp.substr(0, slash);
}

// TRUE when defs of this type are GLOBAL regardless of source path.
//
// The game hierarchy (`: game/: campaign/: sector/: mission/: objective`) and
// the named volume/event templates (`: trigger/: event`) are referenced by
// name from ANY world, so a source path inside GameData/Worlds/ (or an
// unexpected package layout) must not make them world-scoped — path-based
// classification (WorldNameOfDef) would otherwise drop them for every world
// except the one folder they happen to live in. Type-based classification is
// the authority here; path-based stays in effect for everything else.
bool IsGlobalDefType(EntityType t) {
    switch (t) {
        case EntityType::GAME:
        case EntityType::CAMPAIGN:
        case EntityType::SECTOR:
        case EntityType::MISSION:
        case EntityType::OBJECTIVE:
        case EntityType::TRIGGER:
        case EntityType::EVENT:
            return true;
        default:
            return false;
    }
}

// The world NAME a LoadWorld asset prefix refers to. Handles both
// "GameData/Worlds/<Name>/" and a direct .ozone path (editor playtest), whose
// world name is its containing folder.
std::string WorldNameFromPrefix(const std::string& assetPrefix) {
    const std::string s = StripTrailingSlash(Slashes(assetPrefix));
    static const char* kRoot = "GameData/Worlds/";
    const size_t at = s.find(kRoot);
    if (at != std::string::npos) {
        const std::string rest = s.substr(at + std::strlen(kRoot));
        const size_t slash = rest.find('/');
        return (slash == std::string::npos) ? rest : rest.substr(0, slash);
    }
    const size_t slash = s.find_last_of('/');
    if (slash == std::string::npos) return "";
    const std::string last = s.substr(slash + 1);
    if (last.find('.') != std::string::npos)
        return "";   // a bare filename, not a world directory
    return last;
}

} // namespace

void LightningEntityRegistry::LoadWorldOverrides(const std::string& worldDir) {
    const std::string want = WorldNameFromPrefix(worldDir);
    if (want.empty())
        return;

    int promoted = 0, dropped = 0;
    for (auto it = m_defs.begin(); it != m_defs.end(); ) {
        // Type-based classification first: game-hierarchy and trigger/event
        // defs are global no matter where they were parsed from (see
        // IsGlobalDefType). Path-based classification (WorldNameOfDef) applies
        // to the remaining, world-scoped types.
        if (IsGlobalDefType(it->second.type)) {
            ++it;
            continue;
        }
        const std::string owner = WorldNameOfDef(it->second.sourcePath);
        if (owner.empty() || owner == want) {   // global def, or already ours
            ++it;
            continue;
        }
        // This name is currently held by another world's def. Promote this
        // world's version if it defines the same name, otherwise remove it so
        // the other world cannot leak into this one.
        const EntityDef* mine = nullptr;
        for (const auto& d : m_allDefs) {
            if (d.name == it->first && WorldNameOfDef(d.sourcePath) == want) {
                mine = &d;
                break;
            }
        }
        if (mine) {
            it->second = *mine;
            ++promoted;
            ++it;
        } else {
            OZ_DEBUG("Registry: world '%s' does not define '%s'; dropping %s's copy",
                     want.c_str(), it->first.c_str(), owner.c_str());
            it = m_defs.erase(it);
            ++dropped;
        }
    }
    if (promoted || dropped)
        OZ_INFO("Registry: world overrides for '%s' (%d promoted, %d dropped)",
                want.c_str(), promoted, dropped);
}
