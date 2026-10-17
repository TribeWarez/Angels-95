#pragma once
// ---------------------------------------------------------------------------
// GameManager — the Angels95 game-layer entry point.
//
// SCAFFOLD (Phase 0). Singleton with an inert Init/Shutdown. Phase 2 loads the
// GameData/Game/Angels95/ hierarchy into GameDef/CampaignDef/SectorDef/MissionDef,
// owns the active mode (CAMPAIGN / COOPERATIVE) and the active mission, drives
// ObjectiveSystem, and exposes progression to the client menu and the server.
//
// This is the ONE place the engine base is allowed to reach "up" into game
// policy, and it does so through a narrow interface (mode + mission + objectives)
// so the engine never names a Sefirah, a soul, or a guild.
// ---------------------------------------------------------------------------

#include "GameDef.hpp"

namespace oz {
namespace game {

class GameManager {
public:
    static GameManager& Instance();

    // Phase 2: scan GameData/Game/* for `: game`/`: campaign`/`: sector`/
    // `: mission`/`: objective` defs and resolve the hierarchy. Phase 0: no-op.
    void Init();
    void Shutdown();

    bool Loaded() const { return m_loaded; }

    const GameDef&     Game() const     { return m_game; }
    const CampaignDef& Campaign() const { return m_campaign; }

    // Phase 2 helpers (declared so callers can be written against the shape):
    const SectorDef*  FindSector(const std::string& name) const;
    const MissionDef* FindMission(const std::string& name) const;

private:
    GameManager() = default;

    GameDef     m_game;
    CampaignDef m_campaign;
    std::vector<SectorDef>  m_sectors;
    std::vector<MissionDef> m_missions;
    bool m_loaded = false;
};

} // namespace game
} // namespace oz
