#include "GameState.hpp"
#include "../World/OzoneParser.hpp"
#include "../World/GameType.hpp"
#include "../World/LevelSettings.hpp"
#include "../Game/GameManager.hpp"
#include "../Script/LightningEntityRegistry.hpp"
#include "../Log.hpp"

// Defined in OzoneParser.cpp (linked into both client and server) and
// declared in OzoneParser.hpp. Forward-declared here so init_worlds can
// strip quotes from levelinfo paths the same way the loader does.
std::string StripQuotes(std::string s);
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <limits>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <sys/stat.h>

namespace fs = std::filesystem;

static std::unordered_map<std::string, ServerPawnDef> g_serverPawnDefs;
static bool g_serverPawnDefsLoaded = false;

static void seed_world_entities(WorldState& ws, const std::string& gamedata_dir);
static void load_player_record(ServerPlayer& player, const std::string& gamedata_dir);

static void load_server_pawn_defs(const std::string& gamedata_dir) {
    if (g_serverPawnDefsLoaded) return;
    std::string defsDir = (gamedata_dir.empty() ? std::string("GameData") : gamedata_dir) + "/Pawn/PawnDefs";
    if (!fs::exists(defsDir)) {
        // Fallback hardcoded defs (match .cfg values)
        auto add = [&](const char* name, float speed, float aggro, float attack, float dmg, int hp) {
            ServerPawnDef& d = g_serverPawnDefs[name];
            d.name = name; d.speed = speed; d.aggro_range = aggro;
            d.attack_range = attack; d.damage = dmg; d.max_health = hp;
        };
        add("Walker", 1.5f, 6.0f, 1.5f, 10.0f, 100);
        add("Skaarj", 2.5f, 10.0f, 2.0f, 20.0f, 150);
        add("Brute", 1.0f, 4.0f, 1.5f, 30.0f, 250);
        add("Floater", 1.2f, 8.0f, 3.0f, 15.0f, 80);
        g_serverPawnDefsLoaded = true;
        return;
    }
    int loaded = 0;
    for (auto& entry : fs::directory_iterator(defsDir)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".cfg") continue;
        std::ifstream f(entry.path());
        if (!f.is_open()) continue;
        std::string name; float speed=1.5f, aggro=6.0f, attack=1.5f, dmg=10.0f; int hp=100;
        std::string line;
        // std::stof / std::stoi THROW on a non-numeric value and nothing caught them,
        // so one typo in one GameData/Pawn/PawnDefs/*.cfg terminated the dedicated
        // server during world init — no log line naming the file, no partial load, and
        // no way to tell a bad def from a bad build. OzoneParser wraps every stof for
        // exactly this reason. A malformed value now keeps the default and the rest of
        // the file still loads; the file and key are named so the bad line is findable.
        // Both take the value as a parameter rather than capturing it, because `v` is
        // declared inside the loop below.
        auto readFloat = [&](const std::string& key, const std::string& val, float& out) {
            try { out = std::stof(val); }
            catch (const std::exception&) {
                OZ_WARN("Pawn def %s/%s: '%s = %s' is not a number — keeping %g",
                        entry.path().filename().string().c_str(), name.c_str(),
                        key.c_str(), val.c_str(), (double)out);
            }
        };
        auto readInt = [&](const std::string& key, const std::string& val, int& out) {
            try { out = std::stoi(val); }
            catch (const std::exception&) {
                OZ_WARN("Pawn def %s/%s: '%s = %s' is not an integer — keeping %d",
                        entry.path().filename().string().c_str(), name.c_str(),
                        key.c_str(), val.c_str(), out);
            }
        };
        while (std::getline(f, line)) {
            line.erase(0, line.find_first_not_of(" \t\r\n"));
            if (line.empty() || line[0]=='#' || line[0]==';') continue;
            size_t eq = line.find('='); if (eq==std::string::npos) continue;
            std::string k = line.substr(0,eq), v = line.substr(eq+1);
            k.erase(0,k.find_first_not_of(" \t")); k.erase(k.find_last_not_of(" \t")+1);
            v.erase(0,v.find_first_not_of(" \t")); v.erase(v.find_last_not_of(" \t\r")+1);
            if (k=="name") name=v;
            else if (k=="speed")        readFloat(k, v, speed);
            else if (k=="aggroRange")   readFloat(k, v, aggro);
            else if (k=="attackRange")  readFloat(k, v, attack);
            else if (k=="damage")       readFloat(k, v, dmg);
            else if (k=="maxHealth")    readInt(k, v, hp);
        }
        if (!name.empty()) {
            ServerPawnDef& d = g_serverPawnDefs[name];
            d.name=name; d.speed=speed; d.aggro_range=aggro;
            d.attack_range=attack; d.damage=dmg; d.max_health=hp;
            loaded++;
        }
    }
    OZ_INFO("Server: loaded %d pawn defs from %s", loaded, defsDir);
    g_serverPawnDefsLoaded = true;
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
GameState::GameState() {
    m_players.reserve(net::MAX_PLAYERS);
}

GameState::~GameState() {
}

// ---------------------------------------------------------------------------
// XP calculation
// ---------------------------------------------------------------------------
int GameState::xp_needed_for_level(int level) const {
    if (level <= 0) return 1;
    // XP needed doubles each level approximately
    float needed = XP_BASE_TO_NEXT * std::pow(XP_GROWTH_FACTOR, level - 1);
    return std::max(1, (int)needed);
}

int GameState::get_player_xp_level(int xp) {
    int level = 1;
    int needed = XP_BASE_TO_NEXT;
    while (xp >= needed) {
        xp -= needed;
        level++;
        needed = (int)(needed * XP_GROWTH_FACTOR);
        if (level > 100) break; // hard cap
    }
    return level;
}

// ---------------------------------------------------------------------------
// World initialization
// ---------------------------------------------------------------------------
void GameState::init_worlds(const std::string& gamedata_dir,
                             const std::vector<std::string>& world_list) {
    m_gamedata_dir = gamedata_dir;
    m_worlds.clear();
    for (size_t i = 0; i < world_list.size(); i++) {
        WorldState ws;
        ws.world_index = (int)i;
        ws.name = world_list[i];

        // Default world bounds; could be extended with a config file
        ws.world_min_x = -2000.0f;
        ws.world_max_x = 2000.0f;
        ws.world_min_z = -2000.0f;
        ws.world_max_z = 2000.0f;

        // Parse the world file's levelinfo so game rules come from the world
        // itself. ResolveGameTypeInfo layers: built-in table -> .ozls override
        // -> levelinfo numbers (maxPlayers/friendlyFire/scoreLimit/timeLimit).
        {
            std::string ozonePath = gamedata_dir + "/Worlds/" + world_list[i] + "/World.ozone";
            auto prims = OzoneParser::parse_file(ozonePath);
            for (auto& pr : prims) {
                if (pr.type != OzonePrimitiveType::ENTITY_LEVELINFO) continue;
                LevelSettings ls;
                ParseLevelInfo(ls, pr.args,
                               StripQuotes(pr.entityType),
                               StripQuotes(pr.entitySubType),
                               StripQuotes(pr.gametypeKey));
                ws.gameTypeInfo = oz::gametype::ResolveGameTypeInfo(
                    oz::gametype::GameTypeFromId(ls.gameType), ls, "");
                ws.max_players = (ls.maxPlayers > 0) ? (uint32_t)ls.maxPlayers : net::MAX_PLAYERS;
                ws.friendly_fire = ws.gameTypeInfo.friendlyFire || ls.friendlyFire;
                OZ_INFO("World %s rules: mode=%s maxPlayers=%u friendlyFire=%d scoreLimit=%d%s",
                        world_list[i].c_str(),
                        oz::gametype::GameTypeKey(ws.gameTypeInfo.id),
                        ws.max_players, (int)ws.friendly_fire,
                        ws.gameTypeInfo.scoreLimitEnabled ? ws.gameTypeInfo.scoreLimit : 0,
                        ws.gameTypeInfo.scoreLimitEnabled ? "" : " (off)");
                break; // first levelinfo wins
            }
        }

        // World 0 is the server's primary map (current_map_name()), so its rules
        // drive the single global match. Other worlds keep their own rules in ws;
        // they no longer overwrite the globals (the old last-world-wins bug).
        if (i == 0) {
            m_gameTypeInfo = ws.gameTypeInfo;
            m_max_players = ws.max_players;
            m_friendly_fire = ws.friendly_fire;
            m_match.type = ws.gameTypeInfo.id;
        }

        // Create partitions
        ws.partitions.resize(PARTITIONS_PER_WORLD);
        float dx = (ws.world_max_x - ws.world_min_x) / PARTITION_COLS;
        float dz = (ws.world_max_z - ws.world_min_z) / PARTITION_ROWS;
        for (int p = 0; p < PARTITION_ROWS; p++) {
            for (int q = 0; q < PARTITION_COLS; q++) {
                int idx = p * PARTITION_COLS + q;
                ws.partitions[idx].id = idx;
                ws.partitions[idx].min_x = ws.world_min_x + q * dx;
                ws.partitions[idx].max_x = ws.world_min_x + (q + 1) * dx;
                ws.partitions[idx].min_z = ws.world_min_z + p * dz;
                ws.partitions[idx].max_z = ws.world_min_z + (p + 1) * dz;
            }
        }

        // Populate from the world file's own entities; the procedural
        // generator is a fallback for worlds that define none.
        seed_world_entities(ws, gamedata_dir);
        if (ws.global_npcs.empty() && ws.global_pickups.empty())
            init_global_npcs_and_pickups(ws);

        // COOP / Campaign objectives — ONLY behind --campaign (default OFF).
        // The ClearArea fallback arrived in the same hierarchy commits as the
        // mission arc (e0d1519/7c27cdb), so both are campaign content: with
        // the flag OFF this world keeps zero objectives, nothing goes out as
        // OBJECTIVE_STATE, and current_mission_world_index() plus Server's
        // coordinated transition stay inert.
        if (m_campaignEnabled) {
            // Prefer the mission def from the game hierarchy (`: mission` ->
            // `: objective` counts); fall back to a default "clear the area"
            // when the world is not part of the hierarchy.
            const bool coop =
                ws.gameTypeInfo.id == oz::gametype::GameType::COOP ||
                ws.gameTypeInfo.id == oz::gametype::GameType::COOPERATIVE ||
                ws.gameTypeInfo.id == oz::gametype::GameType::CAMPAIGN;
            auto& gm = oz::game::GameManager::Instance();
            const oz::game::MissionDef* mission =
                gm.Loaded() ? gm.FindMissionByWorld(ws.name) : nullptr;
            if (mission) {
                ws.mission = mission->name;
                auto& reg = LightningEntityRegistry::Instance();
                for (const std::string& objName : mission->objectives) {
                    int target = 1;
                    std::string otype = "kill";
                    const EntityDef* od = reg.Find(objName);
                    if (od) {
                        auto it = od->stats.floats.find("count");
                        if (it != od->stats.floats.end()) target = (int)it->second;
                        auto tit = od->stats.strings.find("type");
                        if (tit != od->stats.strings.end() && !tit->second.empty())
                            otype = tit->second;
                    }
                    WorldState::Objective o;
                    o.id = objName;
                    o.type = otype;
                    o.target = target;
                    // OBJECTIVE_STATE carries the id in a fixed 32-byte field
                    // (strncpy at the send site). A longer def name truncates
                    // on the wire with no error anywhere - fail loudly here,
                    // once, at seed time.
                    if (!net::ObjectiveIdFits(o.id))
                        OZ_WARN("objective id '%s' is %zu chars - OBJECTIVE_STATE "
                                "carries at most %zu; clients will see it truncated. "
                                "Shorten the ': objective' def name.",
                                o.id.c_str(), o.id.size(), net::kObjectiveIdMax - 1);
                    ws.objectives.push_back(o);
                }
            } else if (coop) {
                WorldState::Objective obj;
                obj.id = "ClearArea";
                obj.type = "kill";
                obj.target = (int)ws.global_npcs.size();
                if (obj.target > 0) ws.objectives.push_back(obj);
            }
        }

        // Attempt to load saved state
        load_world_state(ws, gamedata_dir);

        m_worlds.push_back(ws);
        OZ_INFO("World %s initialized with %d partitions, %zu global NPCs, %zu pickups",
                ws.name.c_str(), PARTITIONS_PER_WORLD,
                 ws.global_npcs.size(), ws.global_pickups.size());
    }
}

void GameState::init_global_npcs_and_pickups(WorldState& ws) {
    // Load pawn defs from .cfg files (or fallback hardcoded)
    load_server_pawn_defs(m_gamedata_dir);
    // Collect available type names for random assignment
    std::vector<std::string> pawnTypes;
    for (auto& [name, def] : g_serverPawnDefs)
        pawnTypes.push_back(name);
    if (pawnTypes.empty()) pawnTypes.push_back("Walker");

    // Spawn NPCs for this world (10 NPCs, using .cfg stats)
    for (int i = 0; i < 10; i++) {
        ServerNPC npc;
        float angle = (2 * 3.14159265f / 10) * i;
        float radius = 10.0f + (i % 5) * 2.0f;
        npc.spawn_pos.x = radius * std::cos(angle);
        npc.spawn_pos.y = 0;
        npc.spawn_pos.z = radius * std::sin(angle);
        npc.position = npc.spawn_pos;
        // Assign random pawn type from loaded defs
        const std::string& typeName = pawnTypes[i % pawnTypes.size()];
        npc.typeName = typeName;
        auto it = g_serverPawnDefs.find(typeName);
        if (it != g_serverPawnDefs.end()) {
            auto& d = it->second;
            npc.speed = d.speed;
            npc.aggro_range = d.aggro_range;
            npc.attack_range = d.attack_range;
            npc.damage = d.damage;
            npc.max_health = d.max_health;
            npc.health = d.max_health;
            npc.give_up_range = d.give_up_range;
            npc.attack_cooldown_max = d.attack_cooldown_max;
        }
        npc.patrol_radius = 3.0f;
        npc.state = NpcState::PATROL;
        ws.global_npcs.push_back(npc);
    }

    // Spawn pickups: first ring near spawn for playability, rest across world
    srand(static_cast<unsigned>(ws.world_index + 1));
    static const PickupType kNearTypes[] = {
        PickupType::HEALTH, PickupType::MANA, PickupType::PSYCHIC,
        PickupType::KEY, PickupType::COIN, PickupType::POWERUP,
        PickupType::HEALTH, PickupType::MANA, PickupType::PSYCHIC,
        PickupType::COIN, PickupType::PSYCHIC, PickupType::POWERUP
    };
    for (int i = 0; i < 40; i++) {
        ServerPickup pickup;
        pickup.id = i;
        if (i < 12) {
            pickup.type = kNearTypes[i];
        } else {
            pickup.type = static_cast<PickupType>(rand() % PICKUP_TYPE_COUNT);
        }
        pickup.value = pickup_default_value(pickup.type);
        if (pickup.type == PickupType::PSYCHIC) {
            int idx = rand() % 9;
            pickup.value = (idx + 1) * 111;
        }
        pickup.respawn_time = pickup_default_respawn(pickup.type);
        pickup.respawnable = pickup_can_respawn(pickup.type);
        pickup.active = true;
        pickup.respawn_timer = 0.0f;
        // Set default weapon def name for weapon pickups
        if (pickup.type == PickupType::WEAPON) {
            strcpy(pickup.weapon_def_name, "automag");
        }

        float x, z, y;
        if (i < 12) {
            float angle = (2.0f * 3.14159265f / 12.0f) * (float)i;
            x = 8.0f * std::cos(angle);
            z = -10.0f + 8.0f * std::sin(angle);
            y = 18.0f;
        } else {
            x = ws.world_min_x + (rand() % 4000) - 2000.0f;
            z = ws.world_min_z + (rand() % 4000) - 2000.0f;
            x = std::max(ws.world_min_x, std::min(ws.world_max_x, x));
            z = std::max(ws.world_min_z, std::min(ws.world_max_z, z));
            y = 0.0f;
        }
        pickup.position = {x, y, z};
        pickup.rotation = rand() % 360;

        int part_idx = get_partition_index(ws, x, z);
        if (part_idx >= 0) {
            ws.partitions[part_idx].pickups.push_back(pickup);
        } else {
            ws.global_pickups.push_back(pickup);
        }
    }
}

// ---------------------------------------------------------------------------
// seed_world_entities — populate the world from its World.ozone entities
// (npc/pickup primitives) instead of the procedural generator. Coordinates
// are OZONE Z-up and converted to engine Y-up here. Falls back to the
// procedural ring generator only when the world file defines nothing.
// ---------------------------------------------------------------------------
static PickupType pickup_type_from_name(const std::string& name, bool& isWeapon) {
    isWeapon = false;
    static const std::pair<const char*, PickupType> kMap[] = {
        {"HealthVial", PickupType::HEALTH},   {"Medkit", PickupType::HEALTH},
        {"ManaVial", PickupType::MANA},       {"ManaTonic", PickupType::MANA},
        {"EnergyCrystal", PickupType::PSYCHIC},
        {"Key", PickupType::KEY},             {"Coin", PickupType::COIN},
        {"Powerup", PickupType::POWERUP},
        {"Ammo", PickupType::AMMO},
    };
    for (auto& [n, t] : kMap)
        if (name == n) return t;
    // Weapons are .ozls weapon defs (automag, flux_carbine, selenite_blade, ...)
    isWeapon = true;
    return PickupType::WEAPON;
}

static void seed_world_entities(WorldState& ws, const std::string& gamedata_dir) {
    load_server_pawn_defs(gamedata_dir);

    std::string ozonePath = gamedata_dir + "/Worlds/" + ws.name + "/World.ozone";
    auto prims = OzoneParser::parse_file(ozonePath);
    int nextPickupId = 0;

    for (auto& pr : prims) {
        if (pr.type == OzonePrimitiveType::ENTITY_PLAYERSTART) {
            // First playerstart is the authoritative spawn (same rule as the
            // client's GetFirstPlayerStart). Z-up -> Y-up.
            if (!ws.has_spawn && pr.args.size() >= 3) {
                ws.spawn_x = pr.args[0];
                ws.spawn_y = pr.args[2];
                ws.spawn_z = pr.args[1];
                ws.has_spawn = true;
            }
            continue;
        }
        if (pr.type == OzonePrimitiveType::ENTITY_NPC) {
            if (pr.args.size() < 3) continue;
            ServerNPC npc;
            // Z-up → Y-up (same conversion as the client loader)
            npc.spawn_pos = {pr.args[0], pr.args[2], pr.args[1]};
            npc.position = npc.spawn_pos;
            npc.typeName = pr.entityType;
            auto it = g_serverPawnDefs.find(pr.entityType);
            if (it != g_serverPawnDefs.end()) {
                auto& d = it->second;
                npc.speed = d.speed;
                npc.aggro_range = d.aggro_range;
                npc.attack_range = d.attack_range;
                npc.damage = d.damage;
                npc.max_health = d.max_health;
                npc.health = d.max_health;
                npc.give_up_range = d.give_up_range;
                npc.attack_cooldown_max = d.attack_cooldown_max;
            } else {
                // Unknown type — use Walker stats but keep the real type name
                // (the client spawns by npc_type with its own Walker fallback).
                OZ_WARN("World %s: unknown NPC type '%s' — using Walker stats",
                        ws.name.c_str(), pr.entityType.c_str());
                auto wit = g_serverPawnDefs.find("Walker");
                if (wit != g_serverPawnDefs.end()) {
                    auto& d = wit->second;
                    npc.speed = d.speed;
                    npc.aggro_range = d.aggro_range;
                    npc.attack_range = d.attack_range;
                    npc.damage = d.damage;
                    npc.max_health = d.max_health;
                    npc.health = d.max_health;
                    npc.give_up_range = d.give_up_range;
                    npc.attack_cooldown_max = d.attack_cooldown_max;
                }
            }
            npc.patrol_radius = 3.0f;
            npc.state = NpcState::PATROL;
            ws.global_npcs.push_back(npc);
        } else if (pr.type == OzonePrimitiveType::ENTITY_PATH_NODE) {
            if (pr.args.size() < 3) continue;
            ServerPathNode pn;
            pn.name = pr.entityType;
            pn.position = {pr.args[0], pr.args[2], pr.args[1]}; // Z-up -> Y-up
            if (pr.args.size() >= 4) pn.radius = pr.args[3];
            pn.loop = pr.pathLoop;
            std::stringstream ss(pr.entitySubType);
            std::string part;
            while (std::getline(ss, part, ','))
                if (!part.empty()) pn.next.push_back(part);
            ws.path_nodes.push_back(pn);
        } else if (pr.type == OzonePrimitiveType::ENTITY_PICKUP) {
            if (pr.args.size() < 3) continue;
            bool isWeapon = false;
            PickupType type = pickup_type_from_name(pr.entityType, isWeapon);
            ServerPickup pickup;
            // LOCKSTEP: this id is the file-order index among ENTITY_PICKUP
            // primitives and the client stamps the same number onto its drawn
            // PickupNode::netId (Source/World/OzOzoneLoader.cpp). Both sides must
            // keep walking the parsed primitives in file order, or a client's
            // "collect pickup N" targets a different physical pickup.
            pickup.id = nextPickupId++;
            pickup.type = type;
            strncpy(pickup.typeName, pr.entityType.c_str(), sizeof(pickup.typeName) - 1);
            pickup.typeName[sizeof(pickup.typeName) - 1] = '\0';
            // Z-up → Y-up
            pickup.position = {pr.args[0], pr.args[2], pr.args[1]};
            pickup.value = pickup_default_value(type);
            if (type == PickupType::PSYCHIC) {
                // PSYCHIC collect math expects multiples of 111 (items 3-11)
                int v = pickup.value;
                if (v <= 0 || v % 111 != 0) v = 111;
                pickup.value = v;
            }
            float respawn = (pr.args.size() >= 4) ? pr.args[3] : pickup_default_respawn(type);
            pickup.respawn_time = respawn;
            pickup.respawnable = respawn > 0.0f;
            pickup.active = true;
            pickup.respawn_timer = 0.0f;
            if (type == PickupType::WEAPON) {
                strncpy(pickup.weapon_def_name, pr.entityType.c_str(), sizeof(pickup.weapon_def_name) - 1);
                pickup.weapon_def_name[sizeof(pickup.weapon_def_name) - 1] = '\0';
            }
            // Globals broadcast to everyone (partitions are only an AOI
            // optimization; the entity counts here are small).
            ws.global_pickups.push_back(pickup);
        }
    }

    if (!ws.global_npcs.empty() || !ws.global_pickups.empty()) {
        OZ_INFO("World %s seeded from file: %zu NPCs, %zu global pickups (+ partitions)",
                ws.name.c_str(), ws.global_npcs.size(), ws.global_pickups.size());
    }
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Player management
// ---------------------------------------------------------------------------
uint32_t GameState::add_player(uint32_t id, const char* name, int team, int requested_team)
{
    if (m_player_count >= (int)m_max_players) {
        OZ_WARN("Cannot add player, server full (%u/%u)",
                (unsigned)m_player_count, (unsigned)m_max_players);
        return 0;
    }
    // Idempotent: re-join / re-auth for an already-connected id just refreshes
    // the name instead of creating a duplicate.
    if (ServerPlayer* existing = get_player(id)) {
        strncpy(existing->name, name, sizeof(existing->name) - 1);
        existing->name[sizeof(existing->name) - 1] = '\0';
        existing->requested_team = requested_team;
        return existing->id;
    }
    ServerPlayer player;
    player.id = id;
    strncpy(player.name, name, sizeof(player.name) - 1);
    player.name[sizeof(player.name) - 1] = '\0';
    player.health = player.max_health;
    player.mana = 0.0f;
    player.psychic_energy = 0.0f;
    player.level = 1;
    player.xp = 0;
    player.xp_to_next = xp_needed_for_level(1);
    player.connected = true;
    player.last_seen = time(nullptr);
    player.world_index = current_mission_world_index();
    memset(player.inventory, 0, sizeof(player.inventory));

    // Server-owned team assignment. `requested_team` is a HINT ONLY (clamped
    // on receipt by net::ClampRequestedTeam); AssignTeam decides the actual
    // team from the mode's teamCount and current balance. The client's
    // `team` parameter is ignored for new players — it was the untrusted
    // profile value, and the server is the only authority on team.
    {
        std::vector<int> teamSizes(m_gameTypeInfo.teamCount, 0);
        for (auto& p : m_players) {
            if (p.connected && p.team >= 0 && p.team < (int)teamSizes.size())
                teamSizes[p.team]++;
        }
        int safeRequested = net::ClampRequestedTeam((int32_t)requested_team);
        player.team = oz::gametype::AssignTeam(m_gameTypeInfo, teamSizes, safeRequested);
        player.requested_team = requested_team;  // record-only, never scored on
    }

    // Restore persisted state (by name) — only on first creation, not on
    // auth replays (the existing-player early-out above handles those).
    if (!m_gamedata_dir.empty()) load_player_record(player, m_gamedata_dir);

    // Find first free slot (check bounds - vector may be empty in tests)
    for (int i = 0; i < net::MAX_PLAYERS && i < (int)m_players.size(); i++) {
        if (!m_players[i].connected) {
            m_players[i] = player;
            m_player_count++;
            OZ_INFO("Player %s (id=%u) added at slot %d", player.name, player.id, i);
            return player.id;
        }
    }

    // Append (shouldn't reach here if MAX_PLAYERS is accurate)
    m_players.push_back(player);
    m_player_count++;
    return player.id;
}

void GameState::remove_player(uint32_t id) {
    for (auto &p : m_players) {
        if (p.id == id && p.connected) {
            p.connected = false;
            m_player_count--;
            OZ_INFO("Player %s (id=%u) removed", p.name, p.id);
            save_player_data(); // persist on clean disconnect
            return;
        }
    }
}

ServerPlayer* GameState::get_player(uint32_t id) {
    for (auto &p : m_players) {
        if (p.id == id && p.connected) return &p;
    }
    return nullptr;
}

// Choke point for ALL position updates (network handler, bots, cheats).
// Returns false and does not apply the update if the player is unknown,
// the inputs are malformed (NaN/Inf), or the move exceeds the per-update
// teleport limit. First update from a player always establishes a baseline.
bool GameState::update_player_position(uint32_t id, float x, float y, float z,
                                       float yaw, float pitch) {
    ServerPlayer* p = get_player(id);
    if (!p) return false;

    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
        !std::isfinite(yaw) || !std::isfinite(pitch)) {
        OZ_WARN("update_player_position(id=%u): non-finite position dropped", id);
        return false;
    }

    // Teleport clamp: sizeable per-update moves are rejected.
    constexpr float kMaxMovePerUpdate = 25.0f;
    if (p->has_position) {
        float dx = fabsf(x - p->position.x);
        float dy = fabsf(y - p->position.y);
        float dz = fabsf(z - p->position.z);
        if (dx > kMaxMovePerUpdate || dy > kMaxMovePerUpdate ||
            dz > kMaxMovePerUpdate) {
            OZ_WARN("update_player_position(id=%u): teleport delta (%.1f,%.1f,%.1f) dropped",
                    id, dx, dy, dz);
            return false;
        }
    }

    p->position = {x, y, z};
    p->yaw = yaw;
    p->pitch = pitch;
    p->has_position = true;
    p->last_seen = time(nullptr);
    return true;
}

// ---------------------------------------------------------------------------
// Partition helpers
// ---------------------------------------------------------------------------
int GameState::get_partition_index(const WorldState& ws, float x, float z) const {
    float fx = (x - ws.world_min_x) / (ws.world_max_x - ws.world_min_x);
    float fz = (z - ws.world_min_z) / (ws.world_max_z - ws.world_min_z);
    if (fx < 0 || fx >= 1 || fz < 0 || fz >= 1) return -1;
    int col = (int)(fx * PARTITION_COLS);
    int row = (int)(fz * PARTITION_ROWS);
    col = std::max(0, std::min(col, PARTITION_COLS - 1));
    row = std::max(0, std::min(row, PARTITION_ROWS - 1));
    return row * PARTITION_COLS + col;
}

WorldPartition* GameState::get_partition(WorldState& ws, int idx) {
    if (idx < 0 || idx >= (int)ws.partitions.size()) return nullptr;
    return &ws.partitions[idx];
}

void GameState::get_partitions_in_range(const WorldState& ws, int center_idx,
                                         std::vector<int>& out_indices, int range) const {
    out_indices.clear();
    if (center_idx < 0 || center_idx >= (int)ws.partitions.size()) return;
    int center_row = center_idx / PARTITION_COLS;
    int center_col = center_idx % PARTITION_COLS;
    for (int r = center_row - range; r <= center_row + range; r++) {
        for (int c = center_col - range; c <= center_col + range; c++) {
            if (r >= 0 && r < PARTITION_ROWS && c >= 0 && c < PARTITION_COLS) {
                out_indices.push_back(r * PARTITION_COLS + c);
            }
        }
    }
}

WorldState* GameState::get_world(int idx) {
    if (idx < 0 || idx >= (int)m_worlds.size()) return nullptr;
    return &m_worlds[idx];
}

int GameState::current_mission_world_index() const {
    // --campaign OFF: joiners land in world 0. The seed gate already leaves
    // every objective list empty, so the loop below would fall through to 0
    // anyway — the explicit early-out states the gate and defends against any
    // future objective source.
    if (!m_campaignEnabled) return 0;
    for (const auto& ws : m_worlds) {
        if (!ws.objectives.empty() && !ws.mission_transitioned)
            return ws.world_index;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// NPC AI tick
// ---------------------------------------------------------------------------
void GameState::tick_npcs(WorldState& ws, float dt) {
    auto process_npc = [&](ServerNPC& npc, int part_idx) {
        // Dead NPCs still tick (respawn countdown) — everything else skips.
        if (!npc.active && npc.state != NpcState::DEAD) return;

        // Find nearest player in same or adjacent partitions
        ServerPlayer* nearest_player = nullptr;
        float nearest_dist = std::numeric_limits<float>::max();
        std::vector<int> nearby_parts;
        if (part_idx >= 0) {
            get_partitions_in_range(ws, part_idx, nearby_parts, 1);
            nearby_parts.push_back(part_idx);
        }

        for (auto& pp : m_players) {
            if (!pp.connected) continue;
            // COOP: a global NPC (part_idx < 0) must not aggro a player in a
            // different world — all worlds are authored near the origin, so a
            // naive distance check would pull NPCs across world boundaries.
            if (pp.world_index != ws.world_index) continue;
            if (part_idx >= 0) {
                // Check if the player is in one of the nearby partitions
                int pp_part = get_partition_index(ws, pp.position.x, pp.position.z);
                if (pp_part < 0) continue;
                bool found = false;
                for (int np : nearby_parts) {
                    if (np == pp_part) { found = true; break; }
                }
                if (!found) continue;
            }
            float dx = pp.position.x - npc.position.x;
            float dy = pp.position.y - npc.position.y;
            float dz = pp.position.z - npc.position.z;
            float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist < nearest_dist) {
                nearest_dist = dist;
                nearest_player = &pp;
            }
        }

        switch (npc.state) {
            case NpcState::IDLE:
                npc.state_timer += dt;
                if (npc.state_timer >= 3.0f) {
                    npc.state = NpcState::PATROL;
                    npc.state_timer = 0;
                }
                break;

            case NpcState::PATROL:
                if (nearest_player && nearest_dist < npc.aggro_range) {
                    npc.state = NpcState::CHASE;
                    npc.state_timer = 0;
                    break;
                }
                // GameEngine.PathNode: follow the waypoint graph when the world
                // defines one, otherwise fall back to the legacy circular patrol.
                if (!ws.path_nodes.empty()) {
                    int n = (int)ws.path_nodes.size();
                    if (npc.path_target < 0 || npc.path_target >= n) {
                        // Pick the nearest node as the starting waypoint
                        float best = std::numeric_limits<float>::max();
                        int best_i = 0;
                        for (int i = 0; i < n; i++) {
                            float ddx = ws.path_nodes[i].position.x - npc.position.x;
                            float ddz = ws.path_nodes[i].position.z - npc.position.z;
                            float d = ddx * ddx + ddz * ddz;
                            if (d < best) { best = d; best_i = i; }
                        }
                        npc.path_target = best_i;
                    }
                    ServerPathNode& target = ws.path_nodes[npc.path_target];
                    float ddx = target.position.x - npc.position.x;
                    float ddz = target.position.z - npc.position.z;
                    float dist = std::sqrt(ddx * ddx + ddz * ddz);
                    if (dist <= target.radius) {
                        // Advance to a linked successor (by name), else loop/hold
                        int next_i = -1;
                        for (const auto& linkName : target.next) {
                            for (int i = 0; i < n; i++) {
                                if (ws.path_nodes[i].name == linkName) { next_i = i; break; }
                            }
                            if (next_i >= 0) break;
                        }
                        if (next_i < 0 && target.loop && n > 0) next_i = 0;
                        if (next_i >= 0) npc.path_target = next_i; // else hold at node
                    } else if (dist > 0.0001f) {
                        float speed_factor = npc.speed * dt;
                        npc.position.x += ddx / dist * speed_factor;
                        npc.position.z += ddz / dist * speed_factor;
                        npc.yaw = std::atan2(ddz, ddx) * 180.0f / 3.14159265f;
                    }
                } else {
                    // Circular patrol around spawn
                    float dx = npc.position.x - npc.spawn_pos.x;
                    float dz = npc.position.z - npc.spawn_pos.z;
                    float dist_to_spawn = std::sqrt(dx * dx + dz * dz);
                    if (dist_to_spawn > npc.patrol_radius) {
                        // Move back toward spawn point
                        float speed_factor = npc.speed * dt;
                        npc.position.x -= dx / dist_to_spawn * speed_factor;
                        npc.position.z -= dz / dist_to_spawn * speed_factor;
                    } else {
                        // Wander randomly (simulate patrol by moving in yaw direction)
                        npc.yaw += 10.0f * dt; // slowly turn
                        float rad = npc.yaw * 3.14159265f / 180.0f;
                        npc.position.x += npc.speed * dt * std::cos(rad);
                        npc.position.z += npc.speed * dt * std::sin(rad);
                    }
                }
                break;

            case NpcState::CHASE:
                if (!nearest_player) {
                    npc.state = NpcState::RETURN;
                    break;
                }
                // Check if player is too far from spawn
                {
                    float dx = npc.spawn_pos.x - nearest_player->position.x;
                    float dz = npc.spawn_pos.z - nearest_player->position.z;
                    float dist_player_spawn = std::sqrt(dx * dx + dz * dz);
                    if (dist_player_spawn > npc.give_up_range) {
                        npc.state = NpcState::RETURN;
                        break;
                    }
                }
                // Move toward player. HORIZONTAL ONLY: the server has no collision
                // world, so the old 3D approach made an NPC fly up/down to the
                // player's eye height and hover there. Keeping y fixed leaves the
                // NPC on the ground plane it was seeded at.
                {
                    float dx = nearest_player->position.x - npc.position.x;
                    float dz = nearest_player->position.z - npc.position.z;
                    float dist_to_player = std::sqrt(dx * dx + dz * dz);
                    if (dist_to_player <= npc.attack_range) {
                        // Attack the player
                        if (npc.attack_cooldown <= 0) {
                            damage_player(*nearest_player, (int)npc.damage);
                            npc.attack_cooldown = npc.attack_cooldown_max;
                        }
                    } else {
                        float speed_factor = npc.speed * 1.5f * dt;
                        npc.position.x += (dx / dist_to_player) * speed_factor;
                        npc.position.z += (dz / dist_to_player) * speed_factor;
                        // Face player
                        npc.yaw = std::atan2(dz, dx) * 180.0f / 3.14159265f;
                    }
                }
                break;

            case NpcState::RETURN:
                {
                    float dx = npc.spawn_pos.x - npc.position.x;
                    float dz = npc.spawn_pos.z - npc.position.z;
                    float dist_to_spawn = std::sqrt(dx * dx + dz * dz);
                    if (dist_to_spawn <= 1.0f) {
                        npc.state = NpcState::IDLE;
                        npc.state_timer = 0;
                        npc.health = npc.max_health; // heal on return
                    } else {
                        float speed_factor = npc.speed * dt;
                        npc.position.x += (dx / dist_to_spawn) * speed_factor;
                        npc.position.z += (dz / dist_to_spawn) * speed_factor;
                    }
                }
                break;

            case NpcState::DEAD:
                npc.death_timer += dt;
                if (npc.death_timer >= 10.0f) {
                    npc.state = NpcState::IDLE;
                    npc.state_timer = 0;
                    npc.health = npc.max_health;
                    npc.position = npc.spawn_pos;
                    npc.death_timer = 0.0f;
                    npc.path_target = -1; // re-acquire the nearest waypoint
                    npc.active = true; // back in the world; broadcast loop picks it up again
                }
                break;
        }

        // Decrement attack cooldown
        if (npc.attack_cooldown > 0) npc.attack_cooldown--;
    };

    // Process global NPCs
    for (auto &npc : ws.global_npcs) {
        process_npc(npc, -1);
    }

    // Process partition NPCs
    for (auto &part : ws.partitions) {
        for (auto &npc : part.npcs) {
            process_npc(npc, part.id);
        }
    }
}

// ---------------------------------------------------------------------------
// Pickup tick
// ---------------------------------------------------------------------------
void GameState::tick_pickups(WorldState& ws, float dt) {
    auto process_pickup = [&](ServerPickup& p) {
        if (!p.active && p.respawnable) {
            p.respawn_timer += dt;
            if (p.respawn_timer >= p.respawn_time) {
                respawn_pickup(ws, p);
            }
        }
    };

    for (auto &part : ws.partitions) {
        for (auto &p : part.pickups) {
            process_pickup(p);
        }
    }

    for (auto &p : ws.global_pickups) {
        process_pickup(p);
    }
}

bool GameState::collect_pickup(uint32_t player_id, int pickup_id, int world_index,
                               PickupType* out_type, int* out_value,
                               char* out_weapon_def_name, size_t weapon_def_name_len,
                               PickupReject* out_reject) {
    // Every early return below names its reason. A silent `return false` here is
    // what made this bug undiagnosable from the outside.
    auto reject = [&](PickupReject why) {
        if (out_reject) *out_reject = why;
        return false;
    };
    if (out_reject) *out_reject = PickupReject::NONE;

    ServerPlayer* player = get_player(player_id);
    if (!player) return reject(PickupReject::UNKNOWN_PLAYER);

    // The client's world index is not trusted: it must be the world the player
    // is actually standing in. Without this check a client can reach into any
    // loaded world (all worlds are authored near the origin, so several pickup
    // clusters sit within collect range of one another).
    if (world_index != player->world_index) return reject(PickupReject::WRONG_WORLD);

    WorldState* ws = get_world(world_index);
    if (!ws) return reject(PickupReject::UNKNOWN_WORLD);

    // Find the pickup in partitions or global
    ServerPickup* pickup = nullptr;
    for (auto &part : ws->partitions) {
        for (auto &p : part.pickups) {
            if (p.id == pickup_id && p.active) {
                pickup = &p;
                break;
            }
        }
        if (pickup) break;
    }
    if (!pickup) {
        for (auto &p : ws->global_pickups) {
            if (p.id == pickup_id && p.active) {
                pickup = &p;
                break;
            }
        }
    }
    if (!pickup) return reject(PickupReject::NOT_FOUND);

    // Distance validation — reject if player is too far from pickup
    float dx = player->position.x - pickup->position.x;
    float dy = player->position.y - pickup->position.y;
    float dz = player->position.z - pickup->position.z;
    float dist = sqrtf(dx*dx + dy*dy + dz*dz);
    if (dist > MAX_COLLECT_RANGE) return reject(PickupReject::OUT_OF_RANGE);

    // Write out-params before marking inactive
    if (out_type)  *out_type  = pickup->type;
    if (out_value) *out_value = pickup->value;
    if (out_weapon_def_name && weapon_def_name_len > 0 && pickup->type == PickupType::WEAPON) {
        strncpy(out_weapon_def_name, pickup->weapon_def_name, weapon_def_name_len - 1);
        out_weapon_def_name[weapon_def_name_len - 1] = '\0';
    }

    // Apply pickup effect
    switch (pickup->type) {
        case PickupType::HEALTH:
            player->health = std::min(player->max_health, player->health + pickup->value);
            break;
        case PickupType::MANA:
            player->mana = std::min(player->max_mana, player->mana + pickup->value);
            break;
        case PickupType::PSYCHIC:
            player->psychic_energy = std::min(player->max_psychic_energy, player->psychic_energy + pickup->value);
            break;
        case PickupType::ARMOR:
            // Raises the mitigation score consumed by damage_player(). Also
            // tops health so the pickup is not a pure downgrade.
            player->armor += (float)pickup->value;
            player->health = std::min(player->max_health + 50, player->health + pickup->value);
            break;
        case PickupType::WEAPON: // Unlocks Object1-5
            for (int i = 0; i < 5; i++) {
                if (player->inventory[i] == 0) {
                    player->inventory[i] = 1;
                    break;
                }
            }
            break;
        case PickupType::AMMO:
            player->ammo = std::min(player->ammo + pickup->value, 999);
            break;
        case PickupType::POWERUP:
        case PickupType::COIN:
        case PickupType::KEY:
            // Generic effect: add XP
            player->xp = std::min(player->xp + pickup->value, 999999);
            break;
    }

    // Mark pickup inactive
    pickup->active = false;
    pickup->respawn_timer = 0.0f;
    // COOP: advance every "collect" objective in the player's world.
    if (WorldState* cws = get_world(world_index))
        AdvanceObjectivesByType(*cws, "collect", 1);
    OZ_INFO("Player %s collected pickup %d (type %s) worth %d",
            player->name, pickup_id, pickup_type_str(pickup->type), pickup->value);
    return true;
}

void GameState::respawn_pickup(WorldState& ws, ServerPickup& pickup) {
    pickup.active = true;
    pickup.respawn_timer = 0.0f;
    m_respawned_this_tick.push_back({ws.world_index, pickup.id});
    OZ_INFO("Pickup %d (type %s) respawned", pickup.id, pickup_type_str(pickup.type));
}

// ---------------------------------------------------------------------------
// XP system
// ---------------------------------------------------------------------------
void GameState::add_xp(uint32_t player_id, int amount) {
    ServerPlayer* p = get_player(player_id);
    if (!p) return;
    p->xp += amount;
    int new_level = get_player_xp_level(p->xp);
    while (new_level > p->level) {
        p->level++;
        p->max_health += 10;  // Gain 10 HP per level
        p->max_mana += 5;     // Gain 5 MP per level
        p->health = p->max_health; // Full heal on level up
        p->mana = p->max_mana;
        p->xp_to_next = xp_needed_for_level(p->level);
        OZ_INFO("Player %s leveled up to %d!", p->name, p->level);
    }
    p->xp_to_next = xp_needed_for_level(p->level);
}

// ---------------------------------------------------------------------------
// Damage system
// ---------------------------------------------------------------------------
void GameState::damage_npc(ServerNPC& npc, int amount, uint32_t killer_id,
                           oz::gametype::DamageSource source, int world_index) {
    if (npc.state == NpcState::DEAD) return; // dead NPCs can't be re-killed
    npc.health -= amount;
    if (npc.health <= 0) {
        npc.active = false;
        npc.health = 0;
        npc.state = NpcState::DEAD;
        npc.death_timer = 0.0f;
        OZ_INFO("NPC killed at (%.2f, %.2f, %.2f)", npc.position.x, npc.position.y, npc.position.z);
        // COOP: advance every "kill" objective in the world.
        if (world_index >= 0 && world_index < (int)m_worlds.size())
            AdvanceObjectivesByType(m_worlds[(size_t)world_index], "kill", 1);
        if (killer_id != UINT32_MAX) {
            add_xp(killer_id, XP_PER_KILL);
            OZ_INFO("Killer %u awarded %d XP for NPC kill", killer_id, XP_PER_KILL);
            register_kill(killer_id, UINT32_MAX, source);
        }
    }
}

bool GameState::respawn_dead_player(ServerPlayer& player) {
    WorldState* ws = get_world(player.world_index);
    if (!ws) return false;
    player.health = (int)player.max_health;
    player.mana = player.max_mana;
    player.psychic_energy = player.max_psychic_energy;
    player.stamina = net::SERVER_MAX_STAMINA;
    if (ws->has_spawn) {
        player.position = {ws->spawn_x, ws->spawn_y, ws->spawn_z};
        player.yaw = 0.0f;
        player.pitch = 0.0f;
        player.has_position = true;
    }
    player.dead = false;
    player.respawn_at = 0.0;
    OZ_INFO("Player %s respawned in world %d", player.name, player.world_index);
    return true;
}

// Advance a world objective (COOP). Returns true if progress changed.
bool GameState::AdvanceObjective(WorldState& ws, const std::string& id, int delta) {
    for (auto& o : ws.objectives) {
        if (o.id != id) continue;
        if (o.target > 0 && o.progress >= o.target) return false; // already complete
        o.progress += delta;
        if (o.target > 0 && o.progress > o.target) o.progress = o.target;
        return true;
    }
    return false;
}

int GameState::AdvanceObjectivesByType(WorldState& ws, const std::string& type, int delta) {
    int n = 0;
    for (auto& o : ws.objectives) {
        if (o.type != type) continue;
        if (o.target > 0 && o.progress >= o.target) continue;
        o.progress += delta;
        if (o.target > 0 && o.progress > o.target) o.progress = o.target;
        n++;
    }
    return n;
}

void GameState::damage_player(ServerPlayer& player, int amount,
                               uint32_t attacker_id,
                               oz::gametype::DamageSource source) {
    // Armor mitigation is shared with the client via oz::MitigateDamage so both
    // ends resolve damage identically. This used to be a flat 20% cut that
    // ignored the defense stat entirely.
    if (player.dead) return;   // already dead; ignore further damage
    float mitigated = oz::MitigateDamage((float)amount, (float)player.armor);
    player.health -= (int)mitigated;
    if (player.health < 0) player.health = 0;
    if (player.health <= 0) {
        player.health = 0;
        player.dead = true;
        player.respawn_at = time(nullptr) + 3.0;   // 3 s respawn (configurable later)
        if (attacker_id != UINT32_MAX && attacker_id != player.id)
            register_kill(attacker_id, player.id, source);
    }
}

void GameState::register_kill(uint32_t killer_id, uint32_t victim_id,
                              oz::gametype::DamageSource source) {
    if (m_gameTypeInfo.id == oz::gametype::GameType::SINGLEPLAYER ||
        m_gameTypeInfo.id == oz::gametype::GameType::COOP ||
        m_gameTypeInfo.id == oz::gametype::GameType::CAMPAIGN ||
        m_gameTypeInfo.id == oz::gametype::GameType::COOPERATIVE) {
        return; // non-scoring modes
    }
    if (killer_id == UINT32_MAX) return; // no killer to credit
    if (killer_id == victim_id) {
        // Suicide: apply penalty to the killer's own team
        ServerPlayer* p = get_player(killer_id);
        if (p) {
            p->score += m_gameTypeInfo.suicidePenalty;
            oz::gametype::MatchState::WinReason r;
            oz::gametype::ApplyScoreEvent(m_gameTypeInfo, m_match, p->team,
                                          m_gameTypeInfo.suicidePenalty, &r);
        }
        return;
    }
    ServerPlayer* killer = get_player(killer_id);
    if (!killer) return;
    killer->score += m_gameTypeInfo.killScore;
    oz::gametype::MatchState::WinReason r;
    oz::gametype::ApplyScoreEvent(m_gameTypeInfo, m_match, killer->team,
                                  m_gameTypeInfo.killScore, &r);
}

// ---------------------------------------------------------------------------
// Projectile system
// ---------------------------------------------------------------------------
void GameState::spawn_projectile(WorldState& ws, uint32_t owner_id,
                                 const NetVec3& origin, const NetVec3& direction,
                                 float speed, float damage, float lifetime) {
    ServerProjectile p;
    p.id = ws.next_projectile_id++;
    p.position = origin;
    p.velocity = {direction.x * speed, direction.y * speed, direction.z * speed};
    p.damage = damage;
    p.lifetime = lifetime;
    p.owner_id = owner_id;
    p.active = true;
    p.age = 0.0f;
    ws.projectiles.push_back(p);
}

void GameState::tick_projectiles(WorldState& ws, float dt) {
    const float HIT_RADIUS = 2.0f;
    for (auto& p : ws.projectiles) {
        if (!p.active) continue;
        p.age += dt;
        if (p.age >= p.lifetime) { p.active = false; continue; }
        p.position.x += p.velocity.x * dt;
        p.position.y += p.velocity.y * dt;
        p.position.z += p.velocity.z * dt;

        // Check collision with NPCs
        // Global NPCs
        for (auto& npc : ws.global_npcs) {
            if (!npc.active) continue;
            float dx = p.position.x - npc.position.x;
            float dy = p.position.y - npc.position.y;
            float dz = p.position.z - npc.position.z;
            float dist = sqrtf(dx*dx + dy*dy + dz*dz);
            if (dist < HIT_RADIUS) {
                p.active = false;
                damage_npc(npc, (int)p.damage, p.owner_id, oz::gametype::DamageSource::WORLD, ws.world_index);
                break;
            }
        }
        if (!p.active) continue;
        // Partition NPCs
        for (auto& part : ws.partitions) {
            for (auto& npc : part.npcs) {
                if (!npc.active) continue;
                float dx = p.position.x - npc.position.x;
                float dy = p.position.y - npc.position.y;
                float dz = p.position.z - npc.position.z;
                float dist = sqrtf(dx*dx + dy*dy + dz*dz);
                if (dist < HIT_RADIUS) {
                    p.active = false;
                    damage_npc(npc, (int)p.damage, p.owner_id, oz::gametype::DamageSource::WORLD, ws.world_index);
                    break;
                }
            }
            if (!p.active) break;
        }
        if (!p.active) continue;

        // Check collision with players (skip owner). Projectiles are the only
        // PvP path — gated by the world's friendlyFire levelinfo rule.
        for (auto& player : m_players) {
            if (!player.connected) continue;
            if (player.id == p.owner_id) continue;
            if (!m_friendly_fire) continue;
            float dx = p.position.x - player.position.x;
            float dy = p.position.y - player.position.y;
            float dz = p.position.z - player.position.z;
            float dist = sqrtf(dx*dx + dy*dy + dz*dz);
            if (dist < HIT_RADIUS) {
                p.active = false;
                damage_player(player, (int)p.damage, p.owner_id,
                              oz::gametype::DamageSource::PLAYER);
                break;
            }
        }
    }
    // Remove inactive
    ws.projectiles.erase(
        std::remove_if(ws.projectiles.begin(), ws.projectiles.end(),
                       [](const ServerProjectile& p) { return !p.active; }),
        ws.projectiles.end());
}

// ---------------------------------------------------------------------------
// Tick all worlds
// ---------------------------------------------------------------------------
void GameState::tick(float dt) {
    m_tick_count++;
    for (auto &world : m_worlds) {
        tick_npcs(world, dt);
        tick_pickups(world, dt);
        tick_projectiles(world, dt);
    }

    // COOP respawn: dead players wait out their timer, then respawn at their
    // world's spawn (first playerstart) with full health/mana.
    double now = time(nullptr);
    for (auto& p : m_players) {
        if (!p.connected || !p.dead) continue;
        if (now < p.respawn_at) continue;
        respawn_dead_player(p);
    }

    // Advance the match clock and evaluate win conditions. CheckWinConditions
    // is a pure read (no score mutation), so calling it every tick is cheap.
    if (!match_over()) {
        m_match.elapsedSeconds += dt;
        oz::gametype::WinCheck wc = oz::gametype::CheckWinConditions(m_gameTypeInfo, m_match);
        if (wc.reason != oz::gametype::MatchState::WinReason::NONE) {
            m_match.winner = wc.reason;
            m_match.winningTeam = wc.winningTeam;
            OZ_INFO("Match ended: reason=%d team=%d", (int)wc.reason, wc.winningTeam);
        }
    }

    // Player health/magic/penergy regeneration every tick
    for (auto &p : m_players) {
        if (!p.connected) continue;
        p.health_ticks++;
        p.mana_ticks++;
        p.penergy_ticks++;
        // Stamina regen, so a player who spends it on melee swings recovers.
        // Continuous (not tick-counted like the others) so both ends regenerate
        // at the same rate and the client's gate matches the server's.
        if (p.stamina < net::SERVER_MAX_STAMINA) {
            p.stamina += STAMINA_REGEN_PER_SECOND * dt;
            if (p.stamina > net::SERVER_MAX_STAMINA) p.stamina = net::SERVER_MAX_STAMINA;
        }
        // Health regen: 1 point per 10 ticks (1 second at 10 ticks/s)
        if (p.health > 0 && p.health < p.max_health && p.health_ticks >= 10) {
            p.health++;
            p.health_ticks = 0;
        }
        // Mana regen
        if (p.mana < p.max_mana && p.mana_ticks >= 8) {
            p.mana++;
            p.mana_ticks = 0;
        }
        // Psychic energy regen
        if (p.psychic_energy < p.max_psychic_energy && p.penergy_ticks >= 12) {
            p.psychic_energy++;
            p.penergy_ticks = 0;
        }

        // Exploration XP — 1 XP per 10 seconds alive
        p.exploration_tick++;
        if (p.exploration_tick >= 100) { // 100 ticks = 10 seconds at 10 ticks/s
            p.exploration_tick = 0;
            add_xp(p.id, XP_EXPLORE_PER_SEC * 10);
        }
    }

    // Periodic autosave every 60s (600 ticks at 10 Hz) — bounds the loss on
    // a crash to one minute of state.
    if (m_tick_count % 600 == 0) save_all_worlds();
}

// ---------------------------------------------------------------------------
// Save / Load world state
// ---------------------------------------------------------------------------
void GameState::save_world_state(const WorldState& ws, const std::string& gamedata_dir) {
    std::string dir = gamedata_dir + "/Saves/World" + ws.name;
    #ifdef _WIN32
    mkdir(dir.c_str());
#else
    mkdir(dir.c_str(), 0755);
#endif

    // Save partition data
    for (size_t i = 0; i < ws.partitions.size(); i++) {
        // Build path
        char path[512];
        snprintf(path, sizeof(path), "%s/Saves/World%s/Partition%zu.dat",
                 gamedata_dir.c_str(), ws.name.c_str(), i);
        std::ofstream out(path);
        if (!out) continue;
        // V2 header: version + entity counts (loader skips stale/mismatched files)
        out << "V2," << ws.partitions[i].pickups.size() << "," << ws.partitions[i].npcs.size() << "\n";
        // Serialize partition pickups and npcs in a simple CSV-like format
        for (auto &p : ws.partitions[i].pickups) {
            out << "PICKUP," << p.id << "," << (int)p.type << "," << p.position.x << "," << p.position.y << "," << p.position.z << ","
                << p.respawn_time << "," << p.respawn_timer << "," << p.value << ","
                << p.active << "," << p.respawnable << "\n";
        }
        for (auto &n : ws.partitions[i].npcs) {
            out << "NPC," << n.active << "," << (int)n.state << "," << n.position.x << "," << n.position.y << ","
                << n.position.z << "," << n.health << "," << n.state_timer << "\n";
        }
    }

    // Save global NPCs
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/Saves/World%s/GlobalNPCs.dat",
                 gamedata_dir.c_str(), ws.name.c_str());
        std::ofstream out(path);
        if (!out) return;
        out << "V2," << ws.global_npcs.size() << "\n";
        for (auto &n : ws.global_npcs) {
            out << n.active << "," << (int)n.state << "," << n.position.x << "," << n.position.y << ","
                << n.position.z << "," << n.health << "," << n.state_timer << "," << n.speed << ","
                << n.patrol_radius << "," << n.spawn_pos.x << "," << n.spawn_pos.y << "," << n.spawn_pos.z << "\n";
        }
    }
}

void GameState::load_world_state(WorldState& ws, const std::string& gamedata_dir) {
    // Attempt to load global NPCs
    char path[512];
    snprintf(path, sizeof(path), "%s/Saves/World%s/GlobalNPCs.dat",
             gamedata_dir.c_str(), ws.name.c_str());
    std::ifstream infile(path);
    if (!infile.good()) {
        // No saved data, that's fine
        return;
    }

    // V2 header: version + count. Skip stale saves whose NPC count no longer
    // matches the freshly seeded world (e.g. the world file was edited).
    std::string header;
    if (!std::getline(infile, header) || header.rfind("V2,", 0) != 0) {
        OZ_WARN("World %s: ignoring stale (pre-V2) save file", ws.name.c_str());
        return;
    }
    {
        unsigned savedCount = 0;
        if (sscanf(header.c_str(), "V2,%u", &savedCount) == 1 &&
            savedCount != ws.global_npcs.size()) {
            OZ_WARN("World %s: save has %u NPCs but world seeds %zu — ignoring",
                    ws.name.c_str(), savedCount, ws.global_npcs.size());
            return;
        }
    }
    OZ_INFO("Loading saved world state for %s", ws.name.c_str());

    // Read global NPCs
    std::string line;
    int npc_i = 0;
    while (npc_i < (int)ws.global_npcs.size() && std::getline(infile, line)) {
        // Parse: active,state,px,py,pz,health,state_timer,speed,patrol_radius,spx,spy,spz
        int active, state;
        sscanf(line.c_str(), "%d,%d,%f,%f,%f,%d,%f,%f,%f,%f,%f,%f",
               &active, &state,
               &ws.global_npcs[npc_i].position.x,
               &ws.global_npcs[npc_i].position.y,
               &ws.global_npcs[npc_i].position.z,
               &ws.global_npcs[npc_i].health,
               &ws.global_npcs[npc_i].state_timer,
               &ws.global_npcs[npc_i].speed,
               &ws.global_npcs[npc_i].patrol_radius,
               &ws.global_npcs[npc_i].spawn_pos.x,
               &ws.global_npcs[npc_i].spawn_pos.y,
               &ws.global_npcs[npc_i].spawn_pos.z);
        ws.global_npcs[npc_i].active = (active != 0);
        ws.global_npcs[npc_i].state = static_cast<NpcState>(state);
        ws.global_npcs[npc_i].max_health = ws.global_npcs[npc_i].health;
        npc_i++;
    }

    // Read partition pickups (written by save_world_state; NPCs in partitions
    // only exist for procedural fallback worlds and are skipped here).
    for (size_t i = 0; i < ws.partitions.size(); i++) {
        char ppath[512];
        snprintf(ppath, sizeof(ppath), "%s/Saves/World%s/Partition%zu.dat",
                 gamedata_dir.c_str(), ws.name.c_str(), i);
        std::ifstream pin(ppath);
        if (!pin.good()) continue;
        std::string pheader;
        if (!std::getline(pin, pheader) || pheader.rfind("V2,", 0) != 0) continue;
        while (std::getline(pin, line)) {
            if (line.rfind("PICKUP,", 0) != 0) continue;
            int id = -1, type = 0, active = 0, respawnable = 0, value = 0;
            float px, py, pz, rtime, rtimer;
            if (sscanf(line.c_str(), "PICKUP,%d,%d,%f,%f,%f,%f,%f,%d,%d,%d",
                       &id, &type, &px, &py, &pz, &rtime, &rtimer, &value, &active, &respawnable) != 10)
                continue;
            // Match by pickup id across this partition + globals
            ServerPickup* target = nullptr;
            for (auto& p : ws.partitions[i].pickups)
                if (p.id == id) { target = &p; break; }
            if (!target)
                for (auto& p : ws.global_pickups)
                    if (p.id == id) { target = &p; break; }
            if (!target) continue;
            target->position = {px, py, pz};
            target->respawn_time = rtime;
            target->respawn_timer = rtimer;
            target->value = value;
            target->active = (active != 0);
            target->respawnable = (respawnable != 0);
        }
    }
}

// ---------------------------------------------------------------------------
// Save / Load player state — Saves/PlayerData.dat (one record per player,
// keyed by NAME; network player ids are slot positions and change across
// restarts). Format:
//   V2
//   PLAYER,<id>,<name>,<px>,<py>,<pz>,<yaw>,<pitch>,<health>,<max_health>,
//          <mana>,<psychic>,<level>,<xp>,<xp_to_next>,<inv0..4>,<ammo>,<world_index>
//   WEAP,<slot>,<weapon_def_name>,<ammo>,<magazine>   (up to 8 per player)
// ---------------------------------------------------------------------------
void GameState::save_player_data() {
    if (m_gamedata_dir.empty()) return;
    std::string dir = m_gamedata_dir + "/Saves";
    std::filesystem::create_directories(dir); // recursive — handles missing parents
    std::string tmpPath = dir + "/PlayerData.dat.tmp";
    std::string finalPath = dir + "/PlayerData.dat";
    {
        std::ofstream out(tmpPath);
        if (!out) { OZ_WARN("save_player_data: cannot write %s", tmpPath.c_str()); return; }
        out << "V2\n";
        for (auto& p : m_players) {
            if (!p.connected) continue;
            out << "PLAYER," << p.id << "," << p.name << ","
                << p.position.x << "," << p.position.y << "," << p.position.z << ","
                << p.yaw << "," << p.pitch << ","
                << p.health << "," << p.max_health << ","
                << p.mana << "," << p.psychic_energy << ","
                << p.level << "," << p.xp << "," << p.xp_to_next << ",";
            for (int i = 0; i < 5; i++) out << p.inventory[i] << ",";
            out << p.ammo << "," << p.world_index << "\n";
            for (int s = 0; s < SERVER_WEAPON_SLOTS; s++) {
                if (p.weapon_def[s][0] == '\0') continue;
                out << "WEAP," << s << "," << p.weapon_def[s] << ","
                    << p.weapon_ammo[s] << "," << p.weapon_magazine[s] << "\n";
            }
        }
    }
    // Atomic-ish replace: tmp → final
    std::remove(finalPath.c_str());
    std::rename(tmpPath.c_str(), finalPath.c_str());
}

void GameState::load_player_data() {
    // Bulk loader not used — players are restored individually by name on
    // join (load_player_record). Kept for API compatibility.
}

// Restore one player's record by name (called from add_player on first join)
static void load_player_record(ServerPlayer& player, const std::string& gamedata_dir) {
    std::string path = gamedata_dir + "/Saves/PlayerData.dat";
    std::ifstream in(path);
    if (!in.good()) return;
    std::string line;
    if (!std::getline(in, line) || line.rfind("V2", 0) != 0) return; // stale/empty
    std::string name(player.name);
    bool inRecord = false;
    bool found = false;
    while (std::getline(in, line)) {
        if (line.rfind("PLAYER,", 0) == 0) {
            // PLAYER,<id>,<name>,...
            size_t c1 = line.find(',', 7);
            if (c1 == std::string::npos) { inRecord = false; continue; }
            size_t c2 = line.find(',', c1 + 1);
            std::string savedName = (c2 == std::string::npos)
                ? line.substr(c1 + 1) : line.substr(c1 + 1, c2 - c1 - 1);
            inRecord = (savedName == name);
            if (!inRecord) continue;
            found = true;
            // Parse the remainder after the name field
            std::string rest = (c2 == std::string::npos) ? "" : line.substr(c2 + 1);
            float px=0, py=0, pz=0, yaw=0, pitch=0;
            float health=100, max_health=100, mana=0, psychic=0;
            int level=1, xp=0, xpn=100, ammo=0, worldIdx=0;
            int inv[5] = {0,0,0,0,0};
            sscanf(rest.c_str(), "%f,%f,%f,%f,%f,%f,%f,%f,%f,%d,%d,%d,%d,%d,%d,%d,%d,%d",
                   &px, &py, &pz, &yaw, &pitch, &health, &max_health, &mana, &psychic,
                   &level, &xp, &xpn, &inv[0], &inv[1], &inv[2], &inv[3], &inv[4], &ammo);
            // worldIdx parsed? format has ammo then world_index — %d count above covers ammo; parse world_index separately
            (void)worldIdx;
            player.position = {px, py, pz};
            player.yaw = yaw;
            player.pitch = pitch;
            player.health = health;
            player.max_health = max_health;
            player.mana = mana;
            player.psychic_energy = psychic;
            player.level = level;
            player.xp = xp;
            player.xp_to_next = xpn;
            memcpy(player.inventory, inv, sizeof(inv));
            player.ammo = (float)ammo;
        } else if (line.rfind("WEAP,", 0) == 0 && inRecord && found) {
            int slot = -1, wammo = 0, wmag = 0;
            char def[64] = {0};
            if (sscanf(line.c_str(), "WEAP,%d,%63[^,],%d,%d", &slot, def, &wammo, &wmag) == 4 &&
                slot >= 0 && slot < 8) {
                strncpy(player.weapon_def[slot], def, sizeof(player.weapon_def[slot]) - 1);
                player.weapon_def[slot][sizeof(player.weapon_def[slot]) - 1] = '\0';
                player.weapon_ammo[slot] = wammo;
                player.weapon_magazine[slot] = wmag;
            }
        } else if (line.rfind("PLAYER,", 0) == 0) {
            inRecord = false; // moved past our record
            if (found) break;
        }
    }
    if (found) OZ_INFO("Restored saved player data for '%s'", name.c_str());
}
