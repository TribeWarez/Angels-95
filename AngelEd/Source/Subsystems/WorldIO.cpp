// =============================================================================
// Subsystems/WorldIO.cpp
//
// Reading a world document, and the legacy type/primitive name helpers.
//
// FRAGMENT - not a standalone translation unit. Included by Main.cpp, which is the
// single TU for AngelEd's core layer. See Wiki/Editor-Architecture-Refactor.md.
// =============================================================================

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
        // The OZONE loader does not populate the editor's level metadata, so a
        // saved levelinfo skybox/particles would be invisible in the viewport.
        // Parse them here into GetLevelMetadata()/SetLevelMetadata().
        auto prims = OzoneParser::parse_file(path.string().c_str());
        for (auto& pr : prims) {
            if (pr.type == OzonePrimitiveType::ENTITY_LEVELINFO) {
                LevelMetadata meta = GetLevelMetadata();
                auto arg = [&](int i) -> float {
                    return (i >= 0 && i < (int)pr.args.size()) ? pr.args[i] : 0.0f;
                };
                meta.gameType = (GameType)(int)arg(0);
                meta.maxPlayers = (int)arg(1);
                meta.respawnTime = arg(2);
                meta.timeLimitEnabled = arg(3) != 0.0f;
                meta.timeLimitMinutes = arg(4);
                meta.scoreLimit = (int)arg(5);
                meta.friendlyFire = arg(6) != 0.0f;

                // Prefer the human-readable `gametype=<key>` tail when present.
                // OzoneParser classifies it separately (otherwise it would be
                // mistaken for the skybox path), and the runtime's ParseLevelInfo
                // gives the key priority over the numeric id with a warned
                // fallback. Reading only arg(0) here meant the editor and the
                // client could resolve a hand-edited file to DIFFERENT modes, and
                // then disagree on score limit and team count.
                if (!pr.gametypeKey.empty()) {
                    // Same rule as ParseLevelInfo (LevelSettings.hpp:59-67):
                    // GameTypeFromName returns SINGLEPLAYER for an unknown key
                    // rather than signalling failure, so validity is checked first.
                    if (oz::gametype::IsGameTypeKey(pr.gametypeKey)) {
                        meta.gameType = oz::gametype::GameTypeFromName(pr.gametypeKey);
                    } else {
                        OZ_WARN("levelinfo: unknown gametype='%s', using positional id %d",
                                pr.gametypeKey.c_str(), (int)meta.gameType);
                    }
                }

                meta.skyboxTexturePath = pr.entityType;
                // Second path token = side/cap skybox. Previously unread, so it
                // never reached the model and was then lost on the next save.
                meta.skyboxSidePath = pr.entitySubType;
                SetLevelMetadata(meta);
                EditorLog("OZONE levelinfo: mode=%d skybox='%s'%s%s",
                          (int)meta.gameType, meta.skyboxTexturePath.c_str(),
                          meta.skyboxSidePath.empty() ? "" : " side=",
                          meta.skyboxSidePath.c_str());
            } else if (pr.type == OzonePrimitiveType::ENTITY_PARTICLES) {
                LevelMetadata meta = GetLevelMetadata();
                auto arg = [&](int i) -> float {
                    return (i >= 0 && i < (int)pr.args.size()) ? pr.args[i] : 0.0f;
                };
                meta.particleType = (ParticleType)(int)arg(0);
                meta.particleDensity = arg(1);
                meta.particleSpeed = arg(2);
                meta.particleColorR = (int)arg(3);
                meta.particleColorG = (int)arg(4);
                meta.particleColorB = (int)arg(5);
                meta.particleWindX = arg(6);
                meta.particleWindZ = arg(7);
                SetLevelMetadata(meta);
            }
        }
        // The scene was just replaced wholesale. The WorldGraph list is derived from
        // PawnSystem/ZoneManager/OzoneLoader, so every row is now stale.
        MarkWorldGraphDirty();
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
        default: return "water";
    }
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
static std::string MakeWorldRelativePath(const std::string& p) {
    std::string s = p;
    for (auto& c : s) if (c == '\\') c = '/';
    // Repo-relative is the most portable form (loader accepts GameData/ as-is)
    size_t gd = s.find("GameData/");
    if (gd != std::string::npos) return s.substr(gd);
    // World-relative subdirs
    size_t oz = s.find("oztex/");
    if (oz != std::string::npos) return s.substr(oz);
    size_t wt = s.find("Worlds/");
    if (wt != std::string::npos) {
        size_t w1 = s.find('/', wt + 7); // end of the world folder
        if (w1 != std::string::npos && w1 + 1 < s.size()) return s.substr(w1 + 1);
    }
    return s;
}
