#pragma once
// ---------------------------------------------------------------------------
// GameManager — the Angels95 game-layer entry point.
//
// Phase 2: loads the GameData/Game/Angels95/ hierarchy (`: game` / `: campaign`
// / `: sector` / `: mission` / `: objective` defs) out of the LightningEntity
// registry into GameDef/CampaignDef/SectorDef/MissionDef, owns the active mode
// and mission, and seeds ObjectiveSystem for the active mission.
//
// This is the ONE place the engine base reaches "up" into game policy, and it
// does so through a narrow interface (mode + mission + objectives) so the engine
// never names a Sefirah, a soul, or a guild.
// ---------------------------------------------------------------------------

#include "GameDef.hpp"
#include <string>
#include <vector>

namespace oz {
namespace game {

class GameManager {
public:
    static GameManager& Instance();

    // Scan the registry for the hierarchy defs and resolve them. Idempotent.
    void Init();
    void Shutdown();

    bool Loaded() const { return m_loaded; }

    const GameDef&     Game() const     { return m_game; }
    const CampaignDef& Campaign() const { return m_campaign; }
    const std::vector<SectorDef>&  Sectors() const  { return m_sectors; }
    const std::vector<MissionDef>& Missions() const { return m_missions; }

    const SectorDef*  FindSector(const std::string& name) const;
    const MissionDef* FindMission(const std::string& name) const;

    // The world to load for the campaign's starting mission ("" if none).
    std::string FirstMissionWorld() const;
    // The world for a named mission ("" if unknown).
    std::string MissionWorld(const std::string& mission) const;

    // Set the active mission: records it, resets + seeds ObjectiveSystem with
    // its objectives (each `count` from the `: objective` def). Returns the
    // mission's world ("" if unknown).
    std::string SetActiveMission(const std::string& mission);

    const std::string& ActiveMission() const { return m_activeMission; }
    std::string ActiveSector() const;

    // Persistence: "mission=<name>" (the campaign save).
    std::string SerializeState() const;
    bool DeserializeState(const std::string& data);

private:
    GameManager() = default;

    GameDef     m_game;
    CampaignDef m_campaign;
    std::vector<SectorDef>  m_sectors;
    std::vector<MissionDef> m_missions;
    std::string m_activeMission;
    bool m_loaded = false;
};

} // namespace game
} // namespace oz
