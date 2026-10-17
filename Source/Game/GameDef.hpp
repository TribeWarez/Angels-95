#pragma once
// ---------------------------------------------------------------------------
// GameDef — the Angels95 game-layer hierarchy data model.
//
// SCAFFOLD (Phase 0). Data structs only; the loader is a no-op stub. Phase 2
// parses `: game` / `: campaign` / `: sector` / `: mission` / `: objective`
// defs out of GameData/Game/Angels95/ via LightningScriptParser and fills these.
//
// The hierarchy (see Wiki/Game-Hierarchy.md):
//   GameDef  (modes, starting sector, save policy)
//     -> CampaignDef  (ordered sectors)
//          -> SectorDef  (a Sefirah node: theme, numerology, role, missions)
//               -> MissionDef  (world= + mode= + objectives)
//                    -> ObjectiveDef (referenced by name)
//
// Raylib-free on purpose: links into headless test targets and the server.
// ---------------------------------------------------------------------------

#include <string>
#include <vector>

namespace oz {
namespace game {

struct ObjectiveDef {
    std::string name;
    std::string type;            // kill/collect/reach/survive/escort/defend/custom
    std::string target;          // def name / marker / counter key
    int         count = 1;
    std::string onComplete;      // referenced event/action name (Phase 2)
    std::string onFail;
    std::string sourcePath;
};

struct MissionDef {
    std::string name;
    std::string sector;          // owning sector name
    std::string world;           // GameData/Worlds/<world> to load
    std::string mode;            // gametype key: campaign / cooperative / coop
    std::vector<std::string> objectives;   // ObjectiveDef names, in order
    std::string onStart;         // event/action name
    std::string onComplete;
    std::string sourcePath;
};

struct SectorDef {
    std::string name;
    int         numerology = 0;  // Sefirah node number 1..5
    std::string theme;           // "terran_prime", "hydrosis_prime", ...
    std::string role;            // "hub", "combat", "raid", ...
    std::vector<std::string> missions;
    std::string sourcePath;
};

struct CampaignDef {
    std::string name;
    std::vector<std::string> sectors;   // ordered
    std::string sourcePath;
};

struct GameDef {
    std::string name;                   // "Angels95"
    std::vector<std::string> modes;     // "campaign", "cooperative"
    std::string startSector;
    bool        saveEnabled = true;
    std::string sourcePath;
};

} // namespace game
} // namespace oz
