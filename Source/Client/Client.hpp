#ifndef OMEGA_CLIENT_HPP
#define OMEGA_CLIENT_HPP

#include "../Network/Network.hpp"
#include <string>
#include <vector>
#include <functional>
#include <mutex>

// Client-side NPC representation for rendering. `position`/`yaw` hold the
// interpolated render values; `*_target`/`*_prev` plus `snapshot_time` drive
// the lerp between the last two server snapshots (NPC updates ~2.5 Hz).
struct ClientNPC {
    int world_index;
    int npc_index;
    int partition_index;
    net::NetVec3 position{0,0,0};
    net::NetVec3 target{0,0,0};
    net::NetVec3 prev{0,0,0};
    float yaw = 0;
    float yaw_target = 0;
    float yaw_prev = 0;
    double snapshot_time = 0;
    bool has_snapshot = false;
    int state = 0;     // NpcState as int
    int health = 100;
    bool active = true;
    char npc_type[32] = {0}; // pawn def name (e.g. .Walker.), from server
};

// Client-side pickup representation.
//
// This is the CLIENT'S VIEW OF SERVER STATE, not the thing that gets drawn.
// The drawn node is `PickupNode` inside PawnSystem (fed from World.ozone), and
// the two are reconciled by netId — see PawnSystem::ApplyPickupNetState. Keeping
// two lists was the original defect: a collect cleared `active` here, on a list
// nothing renders, so the pickup stayed on the floor and every re-walk asked the
// server for something it had already consumed.
struct ClientPickup {
    int id;
    int world_index;
    net::NetVec3 position{0,0,0};
    int type = 0;
    int value = 0;
    bool active = true;
    char weapon_def_name[64] = {0};
    // Authored World.ozone name, replicated as an optional tail. Empty when the
    // server predates the field or the pickup came from the procedural fallback.
    char typeName[64] = {0};
};

// Server-owned COOP objective (OBJECTIVE_STATE). Mirrored per world.
struct ClientObjective {
    int world_index = -1;
    std::string id;
    int progress = 0;
    int target = 1;
};

// Remote player representation (other players connected)
struct RemotePlayer {
    uint32_t player_id;
    net::NetVec3 position{0,0,0};
    float yaw = 0;
    float pitch = 0;
    float health = 100;
    bool active = false;
    uint32_t color_packed = 0xFFFFFFFF; // RGBA
    int ammo = 0;           // current ammo in the remote player's selected weapon
    int magazine = 0;       // its magazine size
    uint8_t stance = 0;     // net::PlayerStance (crouch/sprint) for rendering
};

// Projectile from weapon fire for rendering
struct ClientProjectile {
    net::NetVec3 origin{0,0,0};
    net::NetVec3 direction{0,0,0};
    float spawn_time = 0;
    int weapon_type = 1;
    uint32_t owner_id = 0;
    uint32_t color_packed = 0xFFFFFFFF; // RGBA
};

// Game client networking module.
// Connects to AngelServ and syncs player position / receives updates.

class OmegaClient {
public:
    OmegaClient() = default;
    ~OmegaClient() {
        // Disconnect first (its handler touches members that are still alive),
        // then drop the callbacks. net::NetworkClient::~NetworkClient calls
        // disconnect() unconditionally, so without this it fires
        // on_disconnected() while THIS object is already being torn down - and
        // m_msg_mutex, m_pickups, m_scores and friends are all declared after
        // m_client, i.e. destroyed BEFORE it. Locking a destroyed mutex is
        // undefined; glibc happens to tolerate it, and MSYS2's std::mutex
        // reports it as std::system_error("Invalid argument") at process exit.
        disconnect();
        m_client.set_callbacks(net::ClientCallbacks{});
    }
    OmegaClient(const OmegaClient&) = delete;
    OmegaClient& operator=(const OmegaClient&) = delete;

    // Connect to a server. Returns true on success.
    bool connect(const char* ip, uint16_t port);

    // Disconnect from server
    void disconnect();

    // Call every frame: sends player position, processes incoming messages.
    // `stance` is a net::PlayerStance value replicated to other clients.
    void update(float cam_x, float cam_y, float cam_z,
                float cam_yaw, float cam_pitch, uint8_t stance = 0);

    // Send a chat message
    void send_chat(const char* text);

    // Send pickup collect request
    void send_pickup_collect(int pickup_id, int world_index, const char* weapon_def_name = nullptr);

    // Send NPC damage
    void send_npc_damage(int world_index, int npc_index, int partition_index, int damage);

    // Report a resolved melee hit. The server re-validates reach against the
    // NPC's own position and applies the damage, so melee is server-authoritative
    // in MP rather than a local-only ApplyPawnDamage that is skipped for
    // networkControlled pawns.
    void send_melee_hit(int world_index, int npc_index, int partition_index,
                        int damage, float reach, float stamina_cost,
                        float ox, float oy, float oz,
                        float dx, float dy, float dz);

    // Send weapon fire action
    void send_weapon_fire(float ox, float oy, float oz,
                          float dx, float dy, float dz,
                          int weapon_type, int power = 10);

    // Send weapon ammo update (fire/reload/sync)
    void send_weapon_ammo(int slot, int ammo, int magazine, int action);

    // Status
    bool is_connected() const { return m_client.is_connected(); }
    int get_ping_ms() const { return m_client.get_ping_ms(); }
    const std::string& get_server_ip() const { return m_client.get_server_ip(); }

    // Identity the server accepted for us (protocol v2). Valid only after the
    // server has replied; differs from the local profile whenever the server
    // sanitised or de-duplicated the display name.
    const net::ServerProfile& server_profile() const;
    bool has_server_profile() const;

    // Incoming scene data — call from game loop to apply dynamic WDL
    std::string consume_pending_scene_data();

    // Incoming chat messages
    std::string consume_pending_chat_message();

    // Access received NPC / pickup state.
    // These return snapshots by value: the vectors are mutated by the network
    // thread under m_msg_mutex, and handing out a reference let the game thread
    // iterate while push_back reallocated (iterator-invalidation data race).
    std::vector<ClientNPC> npcs() const { std::lock_guard<std::mutex> l(m_msg_mutex); return m_npcs; }
    std::vector<ClientPickup> pickups() const { std::lock_guard<std::mutex> l(m_msg_mutex); return m_pickups; }

    // COOP objectives for a world (copy under the lock).
    std::vector<ClientObjective> objectives(int world_index) const {
        std::lock_guard<std::mutex> l(m_msg_mutex);
        std::vector<ClientObjective> out;
        for (const auto& o : m_objectives)
            if (o.world_index == world_index) out.push_back(o);
        return out;
    }
    const std::vector<RemotePlayer>& remote_players() const { return m_remote_players; }
    const std::vector<ClientProjectile>& projectiles() const { return m_projectiles; }
    int get_xp() const { return m_xp; }
    int get_level() const { return m_level; }
    int get_xp_to_next() const { return m_xp_to_next; }

    // Score state (from SCORE_STATE packets)
    const std::unordered_map<uint32_t, net::ScoreStateData>& scores() const { return m_scores; }
    bool match_over() const { return m_matchOver; }
    double match_over_at() const { return m_matchOverAt; }
    int winning_team() const {
        for (const auto& [pid, ss] : m_scores) {
            if (ss.matchOver) return ss.winningTeam;
        }
        return -1;
    }

    // Callbacks for game integration
    void set_on_scene_received(std::function<void(const std::string&)> cb) {
        m_on_scene_received = std::move(cb);
    }
    void set_on_chat_received(std::function<void(const std::string&)> cb) {
        m_on_chat_received = std::move(cb);
    }
    void set_on_item_collected(std::function<void(int item_id, int quantity)> cb) {
        m_on_item_collected = std::move(cb);
    }
    void set_on_weapon_collected(std::function<void(const char* weapon_def_name)> cb) {
        m_on_weapon_collected = std::move(cb);
    }
    void set_on_player_hurt(std::function<void(int damage, float remaining_health)> cb) {
        m_on_player_hurt = std::move(cb);
    }

    // Fired whenever the server's pickup view changed (a respawn, a collect, a
    // re-sync snapshot). The argument is a SNAPSHOT taken under m_msg_mutex and
    // released before the callback runs — calling a consumer that touched the
    // client again from inside the lock would deadlock. Consumers reconcile their
    // drawn nodes from this; see PawnSystem::ApplyPickupNetState.
    void set_on_pickups_changed(std::function<void(const std::vector<ClientPickup>&)> cb) {
        m_on_pickups_changed = std::move(cb);
    }

    // Fired when the server assigns this client to a world (on join, and as the
    // answer to request_world). `accepted` is false when the server refused the
    // request — the world index is still where we actually are, so a consumer that
    // wants to reconcile reads the index and only uses `accepted` for feedback.
    //
    // Called with m_msg_mutex HELD, unlike set_on_pickups_changed which snapshots
    // and releases first. Do not call back into the client from here.
    void set_on_world_changed(std::function<void(int world_index, bool accepted)> cb) {
        m_on_world_changed = std::move(cb);
    }

    // Worlds the server has actually issued pickups for. Preferred over parsing
    // the world list out of the SCENE_UPDATE JSON: this is stated by the server
    // in the same packet that carries the pickups, so it cannot disagree with
    // them. Empty means "no pickups known yet".
    bool has_pickup_world(int world_index) const;

    // Ask the server to re-send this player's pickup list for their own world.
    // Cheap; the server answers with a full PICKUP_RESPAWN snapshot.
    void request_pickup_resync();

    // Ask the server to move this client into `world_index` (an index into the
    // world's list, as learned from SCENE_UPDATE). The request is ADVISORY: the
    // server bounds-checks it against its own loaded worlds and answers with
    // WORLD_CHANGE either way. Never assume this took — read server_world_index().
    void request_world(int world_index);

    // The world the SERVER last told us we are in. Distinct from the client's own
    // view (which drives what it renders): the server's word is the one that gates
    // pickup collection and melee validation, so a client that switches worlds
    // locally without asking the server will have its collects rejected.
    int server_world_index() const;
    // Whether the most recent WORLD_CHANGE granted the request.
    bool world_change_accepted() const;

    // Worlds the server has issued pickups for (world indices only).
    std::vector<int> pickup_worlds() const;

private:
    net::NetworkClient m_client;
    std::string m_pending_scene;
    std::string m_chat_msg;
    // mutable so const accessors can take the lock (see npcs()/pickups()).
    mutable std::mutex m_msg_mutex;

    std::vector<ClientNPC> m_npcs;
    std::vector<ClientPickup> m_pickups;
    std::vector<ClientObjective> m_objectives;
    std::vector<RemotePlayer> m_remote_players;
    std::vector<ClientProjectile> m_projectiles;
    int m_xp = 0;
    int m_level = 1;
    int m_xp_to_next = 100;

    // Score state from SCORE_STATE packets (server-owned). Keyed by player_id.
    std::unordered_map<uint32_t, net::ScoreStateData> m_scores;
    bool m_matchOver = false;
    double m_matchOverAt = 0.0;

    std::function<void(const std::string&)> m_on_scene_received;
    std::function<void(const std::string&)> m_on_chat_received;
    std::function<void(int item_id, int quantity)> m_on_item_collected;
    std::function<void(const char* weapon_def_name)> m_on_weapon_collected;
    std::function<void(int damage, float remaining_health)> m_on_player_hurt;
    std::function<void(const std::vector<ClientPickup>&)> m_on_pickups_changed;
    std::function<void(int world_index, bool accepted)> m_on_world_changed;

    // The server's authoritative world assignment for this client, from
    // WORLD_CHANGE. -1 means "the server has not said yet", which is different from
    // 0 and must not be treated as world 0 — that conflation is what made the pickup
    // path silently filter everything out.
    int m_serverWorldIndex = -1;
    bool m_worldChangeAccepted = false;
    double m_lastWorldChangeAt = 0.0;
    net::NetVec3 m_worldSpawn{0, 0, 0};
    bool m_hasWorldSpawn = false;
    // Pickup collects we have requested but not yet seen acknowledged, keyed by
    // (pickup_id, world_index). A set rather than a single slot: overwriting one
    // entry with another used to drop the earlier grant on the floor even though
    // the server had already applied the effect.
    std::vector<std::pair<int, int>> m_pending_collects;

    void handle_message(const net::NetworkMessage& msg);
    void on_connected();
    void on_disconnected();
};

#endif // OMEGA_CLIENT_HPP