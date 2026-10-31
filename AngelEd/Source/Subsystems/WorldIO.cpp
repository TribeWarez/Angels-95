// =============================================================================
// Subsystems/WorldIO.cpp
//
// Reading a world document, and the legacy type/primitive name helpers.
//
// FRAGMENT - not a standalone translation unit. Included by Main.cpp, which is the
// single TU for AngelEd's core layer. See Wiki/Editor-Architecture-Refactor.md.
// =============================================================================

#include "../Core/AssetPath.hpp"

static const char* LegacyPickupType(int idx) {
    static const char* map[] = {"HealthVial","ManaVial","EnergyCrystal","Key","Coin","Powerup"};
    return (idx >= 0 && idx < 6) ? map[idx] : nullptr;
}

static bool LoadWorldDocument(const fs::path& path) {
    EditorLog("Loading world: %s", path.string().c_str());
    ClearScene();
    HistoryClear();
    SetWorldDirectory(path.parent_path());
    g_documentPath = path;
    // The warning is a property of the world just CLOSED, not this one. It was
    // only ever cleared by the next successful brush commit, so a red
    // "NO SOLID" collision count could persist into an unrelated level.
    g_editorPanels.collisionOpWarning = false;

    // Auto-load .oztex packages from the world directory (for textures used by this level)
    {
        fs::path worldDir = path.parent_path();
        int loadedPkg = 0;
        if (fs::exists(worldDir)) {
            for (auto& entry : fs::recursive_directory_iterator(worldDir)) {
                if (entry.is_regular_file()) {
                    std::string ext = entry.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext == ".oztex" || ext == ".ozpak") {
                        auto& loader = PackageAssetLoader::Instance();
                        if (loader.LoadPackageFile(entry.path().string().c_str())) {
                            loadedPkg++;
                            EditorLog("Loaded package: %s", entry.path().filename().string().c_str());
                        }
                    }
                }
            }
        }
        if (loadedPkg > 0) {
            EditorLog("Auto-loaded %d texture package(s) from world directory", loadedPkg);
        }
    }

    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (extension == ".ozone") {
        EditorLog("OZONE format: %s", path.string().c_str());
        bool ok = OzoneLoader::Instance().LoadFile(path.string().c_str());

        // A world whose file ALREADY overflows the CSG per-brush carve budget
        // used to open quietly: the only signal was an OZ_WARN in the log, and
        // the loud "this carved nothing" dialog fired only on a FRESH brush
        // commit. So a level with broken geometry looked fine until the author
        // happened to touch a brush. Report it on open, where the author is.
        {
            const int overflow = OzoneLoader::Instance().LastCsgOverflowCount();
            if (overflow > 0) {
                g_editorPanels.collisionOpWarning = true;
                EditorLog("*** CSG: %d brush(es) hit the per-brush carve budget on load — "
                          "those cuts are MISSING, so geometry the author intended to "
                          "remove is still solid ***", overflow);
                MessageBoxA(nullptr,
                    "This world has CSG sub/intersect brushes that hit the per-brush "
                    "carve budget and stopped early.\n\n"
                    "Geometry those brushes were meant to cut away is STILL SOLID, so "
                    "the player will be blocked by walls that look open.\n\n"
                    "Split the offending brushes into smaller operations.",
                    "CSG carve budget exceeded", MB_OK | MB_ICONWARNING);
            }
        }

        // The loader only parses; applying the parsed entities to the editor's
        // scene is an explicit step.
        InjectOzoneEntities(OzoneLoader::Instance().GetEntities(),
                            PawnSystem::Instance());
        // Level metadata (`levelinfo` / `particles`). The parse lives in
        // Subsystems/LevelState.cpp so that EVERY path which opens a world runs it -
        // it used to be inline here, and the no-argument startup load and
        // HistoryRestore both bypassed it, which meant a plain open-save round trip
        // wrote a level with NO levelinfo (skybox and game rules gone).
        //
        // The OZONE loader itself does not populate the editor's metadata (it
        // parses entities only), so the document is re-parsed here for it.
        LoadLevelMetadataFromPrims(OzoneParser::parse_file(path.string().c_str()));
        // The scene was just replaced wholesale. The WorldGraph list is derived from
        // PawnSystem/ZoneManager/OzoneLoader, so every row is now stale.
        MarkWorldGraphDirty();
        g_documentDirty = false;   // a freshly opened document matches disk
        return ok;
    }
    // OZONE-only editor: any non-.ozone world format is unsupported.
    EditorLog("Unsupported world format '%s' (OZONE-only editor)", extension.c_str());
    return false;
}

static const char* WDLZoneTypeName(ZoneType t) {
    switch (t) {
        case ZoneType::ZONE_LADDER: return "ladder";
        case ZoneType::ZONE_SKY: return "sky";
        case ZoneType::ZONE_REVERB: return "reverb";
        case ZoneType::ZONE_GAMEPLAY_SOUND: return "sound";
        // ZONE_GRAVITY and ZONE_PORTAL used to fall through to "water" here, so
        // ExportToOzone rewrote a gravity zone as a water zone on the first
        // save. ZoneTypeFromString accepts both tokens, so the round-trip is
        // lossless now. Keep this switch exhaustive: a new ZoneType with no
        // case is a silent type change on save, not a compile error.
        case ZoneType::ZONE_GRAVITY: return "gravity";
        case ZoneType::ZONE_CRT: return "crt";
        case ZoneType::ZONE_PORTAL: return "portal";
        case ZoneType::ZONE_WATER: break;
    }
    return "water";
}

static const char* OzonePrimName(int typeId) {
    switch ((OzonePrimitiveType)typeId) {
        case OzonePrimitiveType::BOX:      return "box";
        case OzonePrimitiveType::CYLINDER: return "cyl";
        case OzonePrimitiveType::SPHERE:   return "sph";
        case OzonePrimitiveType::PYRAMID:  return "pyr";
        case OzonePrimitiveType::PLANE:    return "pln";
        default: return nullptr; // entity types / heightmap
    }
}

static const char* CsgPrefixName(int op) {
    if (op == (int)CsgOp::SUB || op == (int)CsgOp::DE_RESC) return "sub";
    if (op == (int)CsgOp::INTERSECT) return "intersect";
    return "add";
}

// Renderable texture paths are stored resolved (absolute) after loading; convert
// back to a portable, world- or repo-relative path.
//
// The implementation is Core/AssetPath.hpp so the exporter, this file and
// tests/AssetPath.test.cpp share ONE definition. Keep the local name: five call
// sites in OzoneExport.cpp use it.
static std::string MakeWorldRelativePath(const std::string& p) {
    return ozasset::ToPortablePath(p);
}
