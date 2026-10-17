#include "GameManager.hpp"

namespace oz {
namespace game {

GameManager& GameManager::Instance() {
    static GameManager* instance = new GameManager();
    return *instance;
}

// SCAFFOLD (Phase 0): inert. Phase 2 scans GameData/Game/*, parses the game
// hierarchy defs, and populates m_game/m_campaign/m_sectors/m_missions. Kept as
// real (empty) bodies so the object links and callers can already reference it.
void GameManager::Init() {
    // Phase 2: load the Angels95 game hierarchy.
    m_loaded = false;
}

void GameManager::Shutdown() {
    m_game = GameDef{};
    m_campaign = CampaignDef{};
    m_sectors.clear();
    m_missions.clear();
    m_loaded = false;
}

const SectorDef* GameManager::FindSector(const std::string& name) const {
    for (const auto& s : m_sectors)
        if (s.name == name) return &s;
    return nullptr;
}

const MissionDef* GameManager::FindMission(const std::string& name) const {
    for (const auto& m : m_missions)
        if (m.name == name) return &m;
    return nullptr;
}

} // namespace game
} // namespace oz
