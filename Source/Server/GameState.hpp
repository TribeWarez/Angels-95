#ifndef OZGAME_STATE_HPP
#define OZGAME_STATE_HPP

#include "../Network/Network.hpp"
#include "../Pawn/CombatMath.hpp"
#include "../Pawn/PickupItems.hpp"
#include "../World/GameType.hpp"
#include "../World/LevelSettings.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <ctime>

// Server-side weapon slots per player. Matches the client's hotbar width
// (LightningEntityManager::HOTBAR_SIZE); both sides iterate this many.
static constexpr int SERVER_WEAPON_SLOTS = 8;

// Melee stamina regeneration, shared with the client so both pools refill at
// the same rate (the client's UpdateStamina reads this).
static constexpr float STAMINA_REGEN_PER_SECOND = 15.0f;  // ~6.7s empty -> full

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------
using NetVec3 = net::NetVec3;

// ---------------------------------------------------------------------------
// Player gameplay state (server side)
// ---------------------------------------------------------------------------
struct ServerPlayer {
    uint32_t id;
    char name[64];
    uint16_t port;
    NetVec3 position{0, 0, 0};
    float yaw = 0, pitch = 0;
    float health = 100.0f;
    float max_health = 100.0f;
    float mana = 0.0f;
    float max_mana = 100.0f;
    float psychic_energy = 0.0f;
    float max_psychic_energy = 100.0f;
    int level = 1;
    int xp = 0;
    int xp_to_next = 100;
    int inventory[5] = {0, 0, 0, 0, 0};  // indices 0-4 match Objects 1-5
    int ammo = 0;                        // shared ammo pool (refilled by AMMO pickups)
    int score = 0;                       // match score (server-owned, from register_kill)
    // Defense score fed to oz::MitigateDamage. Mirrors the client's
    // LightningEntityManager::GetPlayerDefense() sum of equipped `defense`.
    float armor = 0.0f;
    int world_index = 0;
    double last_seen;
    bool has_position = false;           // first validated PLAYER_UPDATE received

    bool connected = false;
    int health_ticks = 0; // for regen
    int mana_ticks = 0;
    int penergy_ticks = 0;
    int exploration_tick = 0; // for exploration XP
    uint32_t last_damage_tick = 0; // throttle NPC_DAMAGE
    uint32_t last_melee_tick = 0;  // throttle MELEE_HIT (separate: melee is faster)
    float stamina = net::SERVER_MAX_STAMINA;  // server-authoritative swing budget

    // COOP respawn. When health drops to 0 the player is flagged dead and a
    // countdown begins; when it expires they are reset to their world's spawn
    // (the first playerstart) with full health/mana. `respawn_at` is the
    // server time (seconds since epoch) at which the respawn fires.
    bool  dead = false;
    double respawn_at = 0.0;

    // Team (protocol v2). `team` is the server's own assignment and is the only
    // value gameplay may read. `requested_team` is the UNTRUSTED team the client
    // asked for; it is recorded so a future team system can honour preferences,
    // but nothing may score or match on it. Both are 0 until teams exist.
    int team = 0;
    int requested_team = 0;

    // Cosmetic character id + colour from the auth profile. The id is sanitised
    // to [A-Za-z0-9_-] by Server.cpp before it is stored or rebroadcast; it is
    // never opened/resolved on the server.
    std::string character;
    int color = 0;

    // Party/lobby (Phase 4 scaffold). 0 = no party. The server assigns a party
    // when a player joins a coop world; solo players stay ungrouped.
    int party_id = 0;

// Weapon registry: one entry per hotbar slot, holding the weapon def name.
    // SERVER_WEAPON_SLOTS matches the client's HOTBAR_SIZE; both sides iterate
    // this many, so the literal 8s in Server.cpp's weapon-registry loop and
    // GameState.cpp's save loop are replaced with it instead of drifting apart.
    char weapon_def[SERVER_WEAPON_SLOTS][64] = {{0}};
    int weapon_ammo[SERVER_WEAPON_SLOTS] = {0};
    int weapon_magazine[SERVER_WEAPON_SLOTS] = {0};
};

// ---------------------------------------------------------------------------
// NpcEntity (from Angels95, C++20)
// ---------------------------------------------------------------------------
enum class NpcState : uint8_t { IDLE, PATROL, CHASE, RETURN, DEAD };

constexpr const char* npc_state_string(NpcState s) {
    switch (s) {
        case NpcState::IDLE:   return "idle";
        case NpcState::PATROL: return "patrol";
        case NpcState::CHASE:  return "chase";
        case NpcState::RETURN: return "return";
        case NpcState::DEAD:   return "dead";
        default:               return "?";
    }
}

struct ServerPawnDef {
    std::string name;
    float speed = 1.5f;
    float aggro_range = 6.0f;
    float attack_range = 1.5f;
    float damage = 10.0f;
    int max_health = 100;
    float give_up_range = 20.0f;
    int attack_cooldown_max = 30;
};

struct ServerNPC {
    bool active = true;
    NpcState state = NpcState::PATROL;
    NetVec3 position{0, 0, 0};
    float yaw = 0;
    float speed = 1.5f;
    float patrol_radius = 3.0f;
    NetVec3 spawn_pos{0, 0, 0};
    float state_timer = 0;
    int health = 100;
    int max_health = 100;
    float aggro_range = 6.0f;
    float give_up_range = 20.0f;
    float damage = 10.0f;
    float attack_range = 1.5f;
    int attack_cooldown = 0;
    int attack_cooldown_max = 30;
    float death_timer = 0.0f;    // respawn countdown when in DEAD state
    std::string typeName;        // e.g. "Walker", "Skaarj" — from .cfg or hardcoded
    int path_target = -1;        // index into WorldState::path_nodes (-1 = use spawn patrol)
};

// GameEngine.PathNode — waypoint for server-authoritative NPC patrol routes.
struct ServerPathNode {
    std::string name;
    NetVec3 position{0, 0, 0};
    std::vector<std::string> next; // successor node names
    bool loop = false;
    float radius = 1.0f;
};

// ---------------------------------------------------------------------------
// Pickup system (from Angels95 pickup_system.h)
// ---------------------------------------------------------------------------
// PickupType + PICKUP_TYPE_COUNT now live in PickupItems.hpp so the client can
// share the pickup->item mapping (see ResolvePickupItemId).

constexpr const char* pickup_type_str(PickupType t) {
    switch (t) {
        case PickupType::HEALTH:   return "heal";
        case PickupType::MANA:     return "mana";
        case PickupType::PSYCHIC:  return "psychic";
        case PickupType::ARMOR:    return "armor";
        case PickupType::WEAPON:   return "weapon";
        case PickupType::AMMO:     return "ammo";
        case PickupType::KEY:      return "key";
        case PickupType::COIN:     return "coin";
        case PickupType::POWERUP:  return "powerup";
        default:                   return "?";
    }
}

// Why GameState::collect_pickup refused a request.
//
// Every failure used to be a bare `return false` with no log and no reply, which
// turned "pickups do not work in multiplayer" into a four-way guess between an
// unknown id, a stale world index, an already-consumed pickup and a distance
// check — with nothing on either side of the wire to tell them apart. The enum
// exists so the caller can name the reason, and so a refusal can be answered
// with a re-sync of the player's own world.
enum class PickupReject : uint8_t {
    NONE = 0,
    UNKNOWN_PLAYER,
    WRONG_WORLD,     // client asked for a world the player is not in
    UNKNOWN_WORLD,   // that world is not loaded on this server
    NOT_FOUND,       // no such pickup, or it is already consumed
    OUT_OF_RANGE,    // further than MAX_COLLECT_RANGE from the player
};

constexpr const char* pickup_reject_str(PickupReject r) {
    switch (r) {
        case PickupReject::NONE:           return "none";
        case PickupReject::UNKNOWN_PLAYER: return "unknown player";
        case PickupReject::WRONG_WORLD:    return "world index is not the player's world";
        case PickupReject::UNKNOWN_WORLD:  return "world not loaded on this server";
        case PickupReject::NOT_FOUND:      return "no such pickup, or already collected";
        case PickupReject::OUT_OF_RANGE:   return "out of collect range";
    }
    return "?";
}

constexpr int pickup_default_value(PickupType t) {
    switch (t) {
        case PickupType::HEALTH:   return 25;
        case PickupType::MANA:     return 20;
        case PickupType::PSYCHIC:  return 15;
        case PickupType::ARMOR:    return 50;
        case PickupType::WEAPON:   return 1;
        case PickupType::AMMO:     return 30;
        case PickupType::KEY:      return 1;
        case PickupType::COIN:     return 1;
        case PickupType::POWERUP:  return 1;
        default:                   return 0;
    }
}

constexpr float pickup_default_respawn(PickupType t) {
    switch (t) {
        case PickupType::HEALTH:   return 30.0f;
        case PickupType::MANA:     return 30.0f;
        case PickupType::PSYCHIC:  return 25.0f;
        case PickupType::ARMOR:    return 45.0f;
        case PickupType::WEAPON:   return 30.0f; // respawn after 30s
        case PickupType::AMMO:     return 20.0f;
        case PickupType::KEY:      return 0.0f; // never respawn
        case PickupType::COIN:     return 0.0f; // never respawn
        case PickupType::POWERUP:  return 60.0f;
        default:                   return 30.0f;
    }
}

constexpr bool pickup_can_respawn(PickupType t) {
    return pickup_default_respawn(t) > 0.0f;
}

struct ServerPickup {
    int id;
    PickupType type;
    NetVec3 position{0, 0, 0};
    float rotation = 0;
    int value = 25;
    float respawn_time = 30.0f;
    float respawn_timer = 0.0f;  // counts up when inactive; respawns when >= respawn_time
    bool active = true;          // visible and collectable
    bool respawnable = true;
    char weapon_def_name[64] = {0}; // for WEAPON pickups
    // Authored pickup name from the World.ozone line ("HealthVial", "rifle_01",
    // ...). Replicated as the optional tail of PickupRespawnData so a client can
    // tell "the server has this pickup" from "I am drawing a node the server
    // never issued", which is otherwise indistinguishable. Empty for the
    // procedural fallback worlds.
    char typeName[64] = {0};
};

// ---------------------------------------------------------------------------
// ServerProjectile — projectile simulated server-side
// ---------------------------------------------------------------------------
struct ServerProjectile {
    uint32_t id = 0;
    NetVec3 position{0,0,0};
    NetVec3 velocity{0,0,0};
    float lifetime = 2.0f;
    float age = 0.0f;
    float damage = 10.0f;
    uint32_t owner_id = UINT32_MAX;
    bool active = true;
};

// ---------------------------------------------------------------------------
// World partition — coordinate-based area of interest management
// ---------------------------------------------------------------------------
constexpr int PARTITIONS_PER_WORLD = 64; // 8x8 grid
constexpr int PARTITION_ROWS = 8;
constexpr int PARTITION_COLS = 8;

struct WorldPartition {
    int id = -1;
    float min_x, max_x, min_z, max_z;
    std::vector<ServerNPC> npcs;
    std::vector<ServerPickup> pickups;
};

// ---------------------------------------------------------------------------
// World state — one per loaded world
// ---------------------------------------------------------------------------
struct WorldState {
    int world_index = 0;
    std::string name;
    float world_min_x = -2000.0f, world_max_x = 2000.0f;
    float world_min_z = -2000.0f, world_max_z = 2000.0f;
    size_t partition_rows = PARTITION_ROWS;
    size_t partition_cols = PARTITION_COLS;
    std::vector<WorldPartition> partitions;
    std::vector<ServerNPC> global_npcs;   // not partition-locked
    std::vector<ServerPickup> global_pickups; // not partition-locked
    std::vector<ServerPathNode> path_nodes;   // GameEngine.PathNode waypoint graph
    std::vector<ServerProjectile> projectiles;
    uint32_t next_projectile_id = 1;

    // Per-world rules resolved from this world's levelinfo. Previously a single
    // GameState member was overwritten on every world in init_worlds, so the
    // server's mode/maxPlayers/friendlyFire were whichever world loaded LAST -
    // while current_map_name() reports world 0. Rules now live with the world;
    // world 0 additionally mirrors into the global match.
    oz::gametype::GameTypeInfo gameTypeInfo{};
    uint32_t max_players = 8;
    bool friendly_fire = false;

    // COOP objective state (Phase 4). Advanced by gameplay (NPC kills, pickups)
    // and broadcast to clients as OBJECTIVE_STATE.
    struct Objective {
        std::string id;
        std::string type;   // kill / collect / reach / survive / ... (from the def)
        // Optional filter: the NPC def (kill) or pickup name (collect) that
        // counts. Authored as `target` on the `: objective` def and read by
        // GameState::AdvanceObjectivesByType. Empty = any kill / any pickup,
        // which is what the fallback ClearArea objective uses.
        std::string targetName;
        int progress = 0;
        int target = 1;
    };
    std::vector<Objective> objectives;

    // Authoritative spawn (first `playerstart`), used by WORLD_CHANGE. Was never
    // read, so every world change sent the client to (0,0,0).
    float spawn_x = 0.0f, spawn_y = 0.0f, spawn_z = 0.0f;
    bool  has_spawn = false;
    // The mission this world implements (GameManager), for coordinated
    // transitions. Empty when the world is not part of the hierarchy.
    std::string mission;
    // Set once all objectives complete and the mission has advanced, so the
    // coordinated transition fires exactly once.
    bool mission_transitioned = false;
    // Set when `--mission <name>` pinned a LATER starting mission: this world
    // lies BEHIND the operator's chosen frontier, so it is never entered,
    // never offered to joiners and never gates the arc. Deliberately distinct
    // from mission_transitioned ("this world was played and finished") so the
    // two facts do not have to be re-derived - a world skipped this way was
    // never played at all.
    bool mission_skipped = false;
};

// Pickup collect range (world units)
constexpr float MAX_COLLECT_RANGE = 5.0f;

// XP constants
constexpr int XP_PER_KILL = 20;
constexpr int XP_EXPLORE_PER_SEC = 1;
constexpr int XP_BASE_TO_NEXT = 100;
constexpr float XP_GROWTH_FACTOR = 1.3f;

// ---------------------------------------------------------------------------
// Server Game State class
// ---------------------------------------------------------------------------
class GameState {
public:
    GameState();
    ~GameState();

    // Prevent copy
    GameState(const GameState&) = delete;
    GameState& operator=(const GameState&) = delete;

    // Initialization
    void init_worlds(const std::string& gamedata_dir, const std::vector<std::string>& world_list);
    void init_global_npcs_and_pickups(WorldState& ws);

    // Player management
    uint32_t add_player(uint32_t id, const char* name,
                     int team = 0, int requested_team = 0); // returns player id
    void remove_player(uint32_t id);
    ServerPlayer* get_player(uint32_t id);
    // Validated position update (choke point). Returns false if the player is
    // unknown, inputs are non-finite, or the per-update move exceeds the
    // teleport clamp; the position is only applied on success.
    bool update_player_position(uint32_t id, float x, float y, float z, float yaw, float pitch);
    int player_count() const { return m_player_count; }
    const std::vector<ServerPlayer>& players() const { return m_players; }

    // NPC AI tick
    void tick_npcs(WorldState& ws, float dt);

    // Projectile management
    void spawn_projectile(WorldState& ws, uint32_t owner_id,
                          const NetVec3& origin, const NetVec3& direction,
                          float speed, float damage, float lifetime);
    void tick_projectiles(WorldState& ws, float dt);

    // Pickup tick
    void tick_pickups(WorldState& ws, float dt);

    // Collect a pickup. `out_reject` (optional) receives the reason on failure;
    // it is set to NONE on success. Never dereference it on a `true` return.
    bool collect_pickup(uint32_t player_id, int pickup_id, int world_index,
                        PickupType* out_type = nullptr, int* out_value = nullptr,
                        char* out_weapon_def_name = nullptr, size_t weapon_def_name_len = 0,
                        PickupReject* out_reject = nullptr);
    void respawn_pickup(WorldState& ws, ServerPickup& pickup);

    // Enumerate active pickups (for join sync)
    template<typename Fn>
    void for_each_active_pickup(int world_index, Fn&& fn) {
        WorldState* ws = get_world(world_index);
        if (!ws) return;
        for (auto& part : ws->partitions)
            for (auto& p : part.pickups)
                if (p.active) fn(*ws, p);
        for (auto& p : ws->global_pickups)
            if (p.active) fn(*ws, p);
    }

    // Enumerate EVERY pickup, active or not. The join snapshot and the re-sync
    // answer use this: `for_each_active_pickup` made a pickup another player had
    // already taken vanish from the snapshot entirely, so a joining client kept
    // DRAWING its own copy from World.ozone (the file lists it). Walking into it
    // then sent a collect the server answered NOT_FOUND, forever, because the
    // client was never told it was consumed.
    template<typename Fn>
    void for_each_pickup(int world_index, Fn&& fn) {
        WorldState* ws = get_world(world_index);
        if (!ws) return;
        for (auto& part : ws->partitions)
            for (auto& p : part.pickups) fn(*ws, p);
        for (auto& p : ws->global_pickups) fn(*ws, p);
    }
    int world_count() const { return (int)m_worlds.size(); }

    // Effective slot cap / friendly-fire rule, resolved from world 0's
    // levelinfo. The heartbeat and /status MUST report this rather than
    // net::MAX_PLAYERS: the shipped levels author maxPlayers=8 while the
    // network layer accepts 32, and add_player enforces the former.
    uint32_t max_players() const { return m_max_players; }
    bool friendly_fire() const { return m_friendly_fire; }
    // Team score (TDM/CTF); -1/out-of-range reads as 0.
    int team_score(int team) const {
        return (team >= 0 && team < 8) ? m_match.teamScores[team] : 0;
    }

    // XP
    void add_xp(uint32_t player_id, int amount);
    int xp_needed_for_level(int level) const;

    // Damage / killing
    // DamageSource defaults to WORLD so existing call sites are unchanged;
    // future LavaZone/AcidZone/fire call sites pass their own source so a
    // kill can be attributed (or not) by the mode's rules.
    void damage_npc(ServerNPC& npc, int amount, uint32_t killer_id = UINT32_MAX,
                    oz::gametype::DamageSource source = oz::gametype::DamageSource::WORLD,
                    int world_index = -1);
    void damage_player(ServerPlayer& player, int amount,
                       uint32_t attacker_id = UINT32_MAX,
                       oz::gametype::DamageSource source = oz::gametype::DamageSource::WORLD);

    // Advance a world objective (COOP). Returns true if progress changed.
    bool AdvanceObjective(WorldState& ws, const std::string& id, int delta);
    // Advance every objective of a given type (kill / collect). A non-empty
    // targetName restricts it to objectives whose `target` matches that NPC def
    // or pickup name, so `kill Walker` no longer counts a Brute. Returns the
    // number advanced.
    int AdvanceObjectivesByType(WorldState& ws, const std::string& type, int delta,
                                const std::string& targetName = std::string());

    // Award a kill to the scoring system. killer_id == victim_id is a
    // suicide (applies suicidePenalty). killer_id == UINT32_MAX with
    // source == WORLD is an environmental kill: no team credit by default.
    void register_kill(uint32_t killer_id, uint32_t victim_id,
                       oz::gametype::DamageSource source = oz::gametype::DamageSource::PLAYER);

    // COOP: respawn a dead player in their own world. Returns true when it fired.
    bool respawn_dead_player(ServerPlayer& player);

    // Match state (owned by GameState; advanced once per tick)
    oz::gametype::MatchState m_match;
    bool match_over() const { return m_match.winner != oz::gametype::MatchState::WinReason::NONE; }
    int winning_team() const { return m_match.winningTeam; }
    const oz::gametype::GameTypeInfo& gametype_info() const { return m_gameTypeInfo; }
    oz::gametype::GameType gametype_id() const { return m_gameTypeInfo.id; }
    int score_limit() const { return m_gameTypeInfo.scoreLimit; }

    // Partition helpers
    int get_partition_index(const WorldState& ws, float x, float z) const;
    WorldPartition* get_partition(WorldState& ws, int idx);
    void get_partitions_in_range(const WorldState& ws, int center_idx, std::vector<int>& out_indices, int range) const;

    // Tick all worlds
    void tick(float dt);
    uint32_t tick_count() const { return m_tick_count; }

    // Access to worlds
    std::vector<WorldState>& worlds() { return m_worlds; }
    WorldState* get_world(int idx);

    // The world a NEW player should spawn in: the first world that is still the
    // arc's frontier (see IsFrontier). Falls back to world 0 when no world
    // qualifies (no objectives seeded, or the mission is complete).
    int current_mission_world_index() const;

    // A world is the arc's FRONTIER when it has objectives, has not already
    // advanced, and was not skipped by --mission. The single definition of
    // "the mission runs here", shared by the join placement above and Server's
    // coordinated transition loop - two hand-copied predicates is exactly how
    // a pinned frontier and a joiner's spawn drift apart.
    static bool IsFrontier(const WorldState& ws) {
        return !ws.objectives.empty() && !ws.mission_transitioned && !ws.mission_skipped;
    }

    // --campaign opt-in (Server.cpp sets this BEFORE init_worlds). OFF =
    // classic co-op: init_worlds seeds no objectives (neither a `: mission`
    // def nor the ClearArea fallback — both arrived with the campaign arc),
    // joiners land in world 0, and Server.cpp skips the coordinated mission
    // transition. The flag lives here rather than in argv so tests can pin
    // both sides.
    void set_campaign_enabled(bool on) { m_campaignEnabled = on; }
    bool campaign_enabled() const { return m_campaignEnabled; }

    // Campaign completion IS the authored exit: mark `from` complete when `to`
    // is the world its mission's `next` points at. Objectives do NOT gate this
    // (they are the route's progress read-out), and `mission_skipped` worlds
    // (behind a --mission pin) never complete. Returns true when it just
    // completed; Server's REQUEST_WORLD handler then moves the party.
    bool complete_mission_at_exit(WorldState& from, const WorldState& to);

    // `--mission <name>` (implies --campaign): pin the arc's starting frontier
    // to a named mission instead of the campaign's first one. Set BEFORE
    // init_worlds; the named mission must resolve to a loaded world - Server.cpp
    // validates that and refuses to start on an unknown name rather than
    // silently falling back to world 0. Empty = default (first mission).
    void set_starting_mission(const std::string& name) { m_startingMission = name; }
    const std::string& starting_mission() const { return m_startingMission; }

    // `--world <name>`: the plain-co-op spawn world. Validated by Server.cpp
    // after init_worlds; the name is stored as an index so a directory-order
    // change cannot silently retarget a running server. Ignored when the
    // campaign arc is enabled (the mission frontier owns join placement).
    bool set_default_world(const std::string& name);
    int default_world_index() const { return m_defaultWorld; }

    // Respawned pickups notifier (consumed by server for broadcast)
    struct RespawnedPickupInfo { int world_index; int pickup_id; };
    std::vector<RespawnedPickupInfo> consume_respawned_pickups() {
        std::vector<RespawnedPickupInfo> out;
        std::swap(out, m_respawned_this_tick);
        return out;
    }

    // Save/load
    void save_world_state(const WorldState& ws, const std::string& gamedata_dir);
    void load_world_state(WorldState& ws, const std::string& gamedata_dir);
    // Player persistence (Saves/PlayerData.dat, keyed by player name).
    // Uses the gamedata_dir captured in init_worlds.
    void save_player_data();
    void load_player_data();
    void save_all_worlds() {
        for (auto& w : m_worlds) save_world_state(w, m_gamedata_dir);
        save_player_data();
    }

private:
    std::vector<ServerPlayer> m_players;
    int m_player_count = 0;
    uint32_t m_max_players = net::MAX_PLAYERS; // from world levelinfo (maxPlayers)
    bool m_friendly_fire = false;              // from world levelinfo (friendlyFire)
    oz::gametype::GameTypeInfo m_gameTypeInfo; // resolved per-world from levelinfo + .ozls override
    std::string m_gamedata_dir;                // captured by init_worlds for save/load
    bool m_campaignEnabled = false;            // --campaign (default OFF = classic co-op)
    std::string m_startingMission;             // --mission <name> (empty = campaign's first)
    int m_defaultWorld = 0;                    // --world <name> for plain co-op
    uint32_t m_next_player_id = 1;
    uint32_t m_tick_count = 0;
    std::vector<WorldState> m_worlds;
    std::vector<RespawnedPickupInfo> m_respawned_this_tick;

    // `--mission <name>`: mark every world BEFORE the pinned starting mission
    // in the mission graph as mission_skipped. Runs at the end of init_worlds
    // (it needs ws.mission seeded and m_worlds populated).
    void apply_starting_mission();

    static int get_player_xp_level(int xp);
};

#endif // OZGAME_STATE_HPP
