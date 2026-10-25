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
    // The mission bound to a world (server uses this to seed COOP objectives).
    const MissionDef* FindMissionByWorld(const std::string& world) const;
    // The `next` mission's world, or "" (mission graph traversal).
    std::string NextMissionWorld(const std::string& mission) const;

    // The world to load for the campaign's starting mission ("" if none).
    std::string FirstMissionWorld() const;
    // The world for a named mission ("" if unknown).
    std::string MissionWorld(const std::string& mission) const;

    // Set the active mission: records it, resets + seeds ObjectiveSystem with
    // its objectives (each `count` from the `: objective` def). Returns the
    // mission's world ("" if unknown).
    std::string SetActiveMission(const std::string& mission);

    // The active mission's objectives as resolved from their defs, in authored
    // order. `target` is the NPC def / pickup name the objective counts (empty
    // = anything); `type` is kill / collect.
    struct LocalObjective {
        std::string id;
        std::string type;
        std::string target;
        int count = 1;
    };
    const std::vector<LocalObjective>& ActiveObjectives() const { return m_activeObjectives; }

    // Advance the ACTIVE mission's LOCAL objective store from a gameplay fact
    // (topic-level, so this is called with "kill"/"collect" and the NPC def or
    // pickup name). Single-player owns progression locally because the server's
    // store is unreachable offline; co-op keeps the server's copy and mirrors it
    // over OBJECTIVE_STATE.
    void AdvanceLocalObjectives(const std::string& type, const std::string& sourceName);

    // Drop the active mission and clear the objective/flag store. A NEW SESSION
    // starts with no campaign context; without this a practice match (or a
    // multiplayer join) inherited the previous session's campaign mission, and
    // SaveGame then wrote it into Campaign.sav as if it had been played.
    void ClearActiveMission();

    const std::string& ActiveMission() const { return m_activeMission; }
    std::string ActiveSector() const;

    // Persistence: the campaign save (GameData/Saves/Campaign.sav).
    // v1 format: "campaign=1\nmission=<name>". A file with no campaign= line
    // is the legacy single-line format and still loads; an unknown version is
    // rejected (returns false) so a future layout is never misread.
    std::string SerializeState() const;
    bool DeserializeState(const std::string& data);

private:
    GameManager() = default;

    GameDef     m_game;
    CampaignDef m_campaign;
    std::vector<SectorDef>  m_sectors;
    std::vector<MissionDef> m_missions;
    std::vector<LocalObjective> m_activeObjectives;
    std::string m_activeMission;
    bool m_loaded = false;
};

} // namespace game
} // namespace oz
