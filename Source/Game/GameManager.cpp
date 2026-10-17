#include "GameManager.hpp"

#include "../Script/LightningEntityRegistry.hpp"
#include "../Events/ObjectiveSystem.hpp"

#include <sstream>

namespace oz {
namespace game {

namespace {

std::string StatStr(const EntityDef* d, const char* key) {
    if (!d) return {};
    auto it = d->stats.strings.find(key);
    return (it != d->stats.strings.end()) ? it->second : std::string();
}
float StatFloat(const EntityDef* d, const char* key, float def) {
    if (!d) return def;
    auto it = d->stats.floats.find(key);
    return (it != d->stats.floats.end()) ? it->second : def;
}

// Split a comma-separated stat list. Values are authored quote-free and
// space-free (the stats parser stops at the first space), but tolerate spaces
// anyway.
std::vector<std::string> SplitComma(const std::string& raw) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= raw.size()) {
        size_t comma = raw.find(',', start);
        std::string tok = (comma == std::string::npos)
            ? raw.substr(start) : raw.substr(start, comma - start);
        size_t a = tok.find_first_not_of(" \t");
        size_t b = tok.find_last_not_of(" \t");
        if (a != std::string::npos) out.push_back(tok.substr(a, b - a + 1));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

} // namespace

GameManager& GameManager::Instance() {
    static GameManager* instance = new GameManager();
    return *instance;
}

void GameManager::Init() {
    auto& reg = LightningEntityRegistry::Instance();
    std::vector<const EntityDef*> defs;

    m_game = GameDef{};
    m_campaign = CampaignDef{};
    m_sectors.clear();
    m_missions.clear();

    reg.FindByType(EntityType::GAME, defs);
    if (!defs.empty()) {
        const EntityDef* d = defs.back();
        m_game.name        = d->name;
        m_game.modes       = SplitComma(StatStr(d, "modes"));
        m_game.startSector = StatStr(d, "start_sector");
        m_game.saveEnabled = StatFloat(d, "save_enabled", 1.0f) != 0.0f;
        m_game.sourcePath  = d->sourcePath;
    }

    defs.clear();
    reg.FindByType(EntityType::CAMPAIGN, defs);
    if (!defs.empty()) {
        const EntityDef* d = defs.back();
        m_campaign.name    = d->name;
        m_campaign.sectors = SplitComma(StatStr(d, "sectors"));
        m_campaign.sourcePath = d->sourcePath;
    }

    defs.clear();
    reg.FindByType(EntityType::SECTOR, defs);
    for (const EntityDef* d : defs) {
        SectorDef s;
        s.name      = d->name;
        s.numerology = (int)StatFloat(d, "numerology", 0.0f);
        s.theme     = StatStr(d, "theme");
        s.role      = StatStr(d, "role");
        s.missions  = SplitComma(StatStr(d, "missions"));
        s.sourcePath = d->sourcePath;
        m_sectors.push_back(s);
    }

    defs.clear();
    reg.FindByType(EntityType::MISSION, defs);
    for (const EntityDef* d : defs) {
        MissionDef m;
        m.name       = d->name;
        m.sector     = StatStr(d, "sector");
        m.world      = StatStr(d, "world");
        m.mode       = StatStr(d, "mode");
        m.objectives = SplitComma(StatStr(d, "objectives"));
        m.next       = StatStr(d, "next");
        m.onStart    = StatStr(d, "on_start");
        m.onComplete = StatStr(d, "on_complete");
        m.sourcePath = d->sourcePath;
        m_missions.push_back(m);
    }

    m_loaded = !m_game.name.empty() || !m_campaign.sectors.empty() ||
               !m_sectors.empty() || !m_missions.empty();
}

void GameManager::Shutdown() {
    *this = GameManager{};
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

const MissionDef* GameManager::FindMissionByWorld(const std::string& world) const {
    for (const auto& m : m_missions)
        if (m.world == world) return &m;
    return nullptr;
}

std::string GameManager::NextMissionWorld(const std::string& mission) const {
    const MissionDef* m = FindMission(mission);
    return (m && !m->next.empty()) ? MissionWorld(m->next) : std::string();
}

std::string GameManager::MissionWorld(const std::string& mission) const {
    const MissionDef* m = FindMission(mission);
    return m ? m->world : std::string();
}

std::string GameManager::FirstMissionWorld() const {
    // Prefer the authored start sector; fall back to the first sector / mission.
    const SectorDef* sec = FindSector(m_game.startSector);
    if (!sec && !m_campaign.sectors.empty())
        sec = FindSector(m_campaign.sectors.front());
    if (sec && !sec->missions.empty())
        return MissionWorld(sec->missions.front());
    if (!m_missions.empty())
        return m_missions.front().world;
    return {};
}

std::string GameManager::SetActiveMission(const std::string& mission) {
    const MissionDef* m = FindMission(mission);
    if (!m) return {};
    m_activeMission = mission;

    // Seed the objective store from the mission's objective defs.
    auto& objs = events::ObjectiveSystem::Instance();
    objs.Reset();
    auto& reg = LightningEntityRegistry::Instance();
    for (const std::string& objName : m->objectives) {
        const EntityDef* od = reg.Find(objName);
        int count = 1;
        if (od) {
            auto it = od->stats.floats.find("count");
            if (it != od->stats.floats.end()) count = (int)it->second;
        }
        objs.Define(objName, count);
    }
    return m->world;
}

std::string GameManager::ActiveSector() const {
    const MissionDef* m = FindMission(m_activeMission);
    return m ? m->sector : std::string();
}

std::string GameManager::SerializeState() const {
    if (m_activeMission.empty()) return {};
    return "mission=" + m_activeMission;
}

bool GameManager::DeserializeState(const std::string& data) {
    const std::string key = "mission=";
    if (data.rfind(key, 0) != 0) return false;
    return !SetActiveMission(data.substr(key.size())).empty();
}

} // namespace game
} // namespace oz
