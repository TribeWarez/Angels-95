#ifndef OMEGA_NETWORK_HPP
#define OMEGA_NETWORK_HPP

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <ctime>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #ifdef _MSC_VER
        #pragma comment(lib, "ws2_32.lib")
    #endif
    typedef int socklen_t;
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <errno.h>
#endif

namespace net {

constexpr uint32_t MAGIC = 0x4F5A574F;

// Wire protocol version. Sent in every CLIENT_AUTH (see ClientAuthPayload).
//   1 = pre-profile handshake (challenge token only)
//   2 = carries a profile block (ClientAuthPayload::structSize >= kAuthStructSizeV2)
//
// Bump this whenever a change would make an older peer misparse a message —
// NOT for purely additive optional fields. The server rejects a version it does
// not recognise rather than guessing, because a misparse here silently assigns
// the wrong identity to a player.
constexpr uint16_t PROTOCOL_VERSION = 2;

// Size of the v1 CLIENT_AUTH payload: the bare challenge token.
constexpr uint32_t kAuthSizeV1 = sizeof(uint32_t);
constexpr size_t MAX_MESSAGE_SIZE = 1024;
constexpr int HEARTBEAT_INTERVAL = 5;
constexpr int TIMEOUT_INTERVAL = 30;
constexpr int DISCOVERY_PORT = 27100;
constexpr int MAX_PLAYERS = 32;
constexpr int PLAYER_NAME_MAX = 64;
constexpr int IP_STRING_MAX = 48;

// Profile field caps on the wire. The server truncates to these; anything longer
// is discarded rather than trusted.
constexpr int PROFILE_MODEL_MAX = 128;
constexpr int PROFILE_VOICE_MAX = 48;

// Teams the server will ever be able to represent. A client asking for anything
// outside 0..MAX_TEAMS is clamped, not honoured. 0 means "no preference".
constexpr int MAX_TEAMS = 8;

struct NetVec3 {
    float x, y, z;
};

enum class MessageType : uint32_t {
    PING = 1,
    PONG = 2,
    PLAYER_JOIN = 3,
    PLAYER_LEAVE = 4,
    PLAYER_UPDATE = 5,
    GAME_STATE = 6,
    CHAT = 7,
    COMMAND = 8,
    FILE_TRANSFER = 9,
    SCENE_UPDATE = 10,
    // Phase 2: extended game state types
    PICKUP_COLLECT = 11,
    PICKUP_RESPAWN = 12,
    NPC_STATE_UPDATE = 13,
    XP_UPDATE = 14,
    PLAYER_HURT = 15,
    PLAYER_KILL = 16,
    PLAYER_ACTION = 17,
    PICKUP_COLLECTED = 18,
    NPC_DAMAGE = 19,
    WEAPON_AMMO = 20,
    SERVER_CHALLENGE = 21,
    CLIENT_AUTH = 22,
    MELEE_HIT = 23,
    // Server -> client: the profile the server actually accepted for this
    // player, after sanitising and de-duplicating it. The client's requested
    // values are advisory, so it must echo this rather than assume its own.
    PROFILE_STATE = 24,
    // Server -> client: per-player score + team + match-over flag. Additive
    // optional field; old clients hit the default case and ignore it.
    // PROTOCOL_VERSION stays at 2 — a new MessageType is forward/backward
    // compatible by definition.
    SCORE_STATE = 25,
    // Client -> server: "I have no (or stale) pickup state; re-send this
    // player's pickup list for the world they are in." Answered with a run of
    // PICKUP_RESPAWN messages. Added because pickups were otherwise only sent on
    // join and on respawn, so one lost datagram — or a client-side filter that
    // suppressed the request — left a pickup drawn on the floor that could never
    // be collected again. Like any new MessageType this is forward/backward
    // compatible by definition: an older client never sends it, an older server
    // ignores it. PROTOCOL_VERSION stays at 2.
    PICKUP_RESYNC = 26,
    // Client -> server: "put me in world N". The server validates N against its own
    // loaded world list and answers with WORLD_CHANGE; it never trusts the client's
    // idea of what exists.
    //
    // This exists because ServerPlayer::world_index was written once at creation and
    // never updated, so it was permanently 0. That made two authoritative checks
    // compare against 0 forever — collect_pickup's WRONG_WORLD reject (GameState.cpp
    // collect_pickup) and MELEE_HIT's world match — and it pinned the 15 s per-player
    // pickup resync to world 0. Any server whose active world was not index 0 rejected
    // every collect and dropped every melee hit, silently except for one OZ_WARN on
    // the melee path.
    //
    // PROTOCOL_VERSION stays at 2: a new MessageType is forward/backward compatible
    // by definition (an older client never sends it; an older server ignores it),
    // which is the same reasoning as SCORE_STATE and PICKUP_RESYNC.
    REQUEST_WORLD = 27,
    // Server -> client: the world the server actually moved this player into, plus
    // that world's spawn point. Sent as the answer to REQUEST_WORLD and on join, so
    // the client learns the server's decision rather than assuming its request
    // succeeded. `accepted == 0` means the request was refused; `world_index` is
    // then the world the player remains in.
    WORLD_CHANGE = 28,
    // Server -> client (COOP): one world objective's progress. Sent periodically
    // for the player's own world (like SCORE_STATE) and on join, so a lost
    // datagram self-heals. Additive; no PROTOCOL_VERSION bump.
    OBJECTIVE_STATE = 29,
    // --- Party / lobby (Phase 4 scaffold) ---
    PARTY_HELLO = 30,   // C->S  request to join/create a party
    PARTY_STATE = 31,   // S->C  party membership snapshot
    // S->C: a player's cosmetic character + colour. Sent on join for every
    // player already present and broadcast when a new player joins. The id is
    // a strict roster token (no path separators); clients resolve it against
    // characters.cfg and fall back to the default model. A new MessageType is
    // forward/backward compatible by definition, so no PROTOCOL_VERSION bump.
    PLAYER_PROFILE = 32,
};

// Melee stamina: the server keeps its own pool per player so a swinging client
// cannot bypass stamina_cost. The regen rate lives in Server/GameState.hpp
// (which the client cannot include); keep the two in step.
constexpr float SERVER_MAX_STAMINA = 100.0f;

// The profile as the server holds it. `displayName` is always safe to print;
// `modelPath`/`voiceSet` are OPAQUE STRINGS THE SERVER NEVER OPENS OR RESOLVES
// — they exist so the client can be told what it asked for. Any future code that
// loads modelPath server-side must treat it as untrusted input.
struct ServerProfile {
    std::string displayName;
    std::string modelPath;
    std::string voiceSet;
    int         team = 0;          // server-assigned; 0 until teams exist
    int         requestedTeam = 0; // UNTRUSTED client hint, clamped on receipt
    int         party_id = 0;      // server-assigned party; 0 = ungrouped
};

struct NetworkPlayer {
    uint32_t id;
    char name[PLAYER_NAME_MAX];
    char ip_address[IP_STRING_MAX];
    uint16_t port;
    bool connected;
    double last_ping;
    double last_seen;
    // Sanitised client profile (protocol v2). `name` above is kept in sync with
    // profile.displayName so existing consumers stay correct.
    ServerProfile profile;
    // True once this player authenticated at v2 or later.
    bool profile_received = false;
};

#pragma pack(push, 1)
struct NetworkMessage {
    uint32_t magic;
    uint32_t type;
    uint32_t size;
    uint32_t sequence;
    uint32_t timestamp;
    uint8_t payload[MAX_MESSAGE_SIZE];
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
// Client profile sync (protocol v2)
// ---------------------------------------------------------------------------
// Everything in ClientAuthPayload is CLIENT-CLAIMED and therefore UNTRUSTED.
// The server treats it as a request and answers with the values it accepted
// (PROFILE_STATE), so a client can never assume its own copy was honoured.
//
// In particular `requestedTeam` is a *preference hint only*. The server owns
// team assignment: it has no team concept yet, and when one lands the
// server-side decision is authoritative. Nothing about scoring may ever read
// this field as if it were verified.
//
// `structSize` makes the tail forward-compatible: a peer that recognises only
// the first N bytes can read those and ignore the rest, so optional fields can
// be appended later without another version bump.
#pragma pack(push, 1)
struct ClientAuthPayload {
    uint32_t challengeToken;                       // also the v1 payload, alone
    uint16_t protocolVersion;
    uint16_t structSize;                           // == sizeof(ClientAuthPayload)
    char     displayName[PLAYER_NAME_MAX];
    char     modelPath[PROFILE_MODEL_MAX];         // opaque to the server
    char     voiceSet[PROFILE_VOICE_MAX];          // opaque to the server
    int32_t  requestedTeam;                        // UNTRUSTED hint, 0 = none
};

// Minimum bytes the server must see before it may read the version/profile.
// A shorter payload is a v1 client, which is still accepted (see below).
constexpr uint32_t kAuthSizeV2 = offsetof(ClientAuthPayload, displayName);

// Server -> client: the sanitised profile actually in force for a player.
struct ProfileStateData {
    uint32_t playerId;
    int32_t  team;                                  // server-assigned (always 0 today)
    int32_t  requestedTeam;                         // what the server stored from the client
    uint8_t  nameWasChanged;                        // 1 => server rewrote displayName
    char     displayName[PLAYER_NAME_MAX];
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
// Profile sanitising (server side; exposed for tests)
// ---------------------------------------------------------------------------
// Strip control characters, collapse nothing else, and cap the length.
// Returns an empty string when nothing printable survives, so callers can
// substitute a fallback rather than accepting an empty identity.
std::string SanitizeDisplayName(const std::string& raw, size_t max_len);

// Cap and scrub an asset reference (model path / voice set). Rejects any path
// containing a ".." segment and drops characters outside printable ASCII.
// Never resolves or opens the result.
std::string SanitizeAssetRef(const std::string& raw, size_t max_len);

// Character roster id: ASCII alphanumerics, '_' and '-', 1..31 chars. Unlike an
// asset ref this contains no path separators at all, because the id is
// rebroadcast to every client and must never become a path a peer can ask
// another peer to load.
std::string SanitizeCharacterId(const std::string& raw, size_t max_len = 31);

// Clamp a client-claimed team into 0..MAX_TEAMS. Out-of-range becomes 0
// ("no preference") rather than clamping to an arbitrary team, so a hostile
// value cannot steer team assignment by sitting near a boundary.
int ClampRequestedTeam(int32_t raw);

// Make `name` unique among `taken` by appending "(2)", "(3)", ... Truncates the
// base name as needed so the result always fits.
std::string MakeUniqueName(const std::string& name, const std::vector<std::string>& taken);

// Player stance, replicated so remote clients can render crouch/sprint.
enum PlayerStance : uint8_t {
    STANCE_STAND  = 0,
    STANCE_CROUCH = 1,
    STANCE_SPRINT = 2
};

struct PlayerUpdateData {
    uint32_t player_id;  // 0 for client→server; server fills when relaying
    NetVec3 position;
    float yaw;
    float pitch;
    float health;
    float mana;
    float psychic_energy;
    int level;
    int xp;
    int inventory[5]; // Objects 1-5 ownership flags
    uint8_t stance;   // net::PlayerStance of the sender (crouch/sprint)
};

struct PickupCollectData {
    uint32_t player_id;
    int pickup_id;
    int world_index;
    char weapon_def_name[64]; // for weapon pickups
};

// Server -> client: a pickup is present and active. Also used as the re-sync
// answer to a rejected collect and for the periodic full snapshot.
//
// FORWARD COMPATIBILITY: `typeName` is an optional trailing field, following the
// same pattern as ClientAuthPayload::structSize. A reader MUST accept a message
// whose `size` stops before it and treat the name as empty, and must never
// require it. That is why the client checks against kPickupRespawnSizeBase
// rather than sizeof(): bumping PROTOCOL_VERSION is for changes that would make
// an older peer MISPARSE a message, and an unread tail does not.
struct PickupRespawnData {
    int pickup_id;
    int world_index;
    NetVec3 position;
    int type;       // PickupType as int
    int value;
    char weapon_def_name[64]; // for weapon pickups
    // --- optional tail; may be absent on an older peer ---
    char typeName[64];        // authored World.ozone pickup name, "" if unknown
};

// Bytes a reader must see before it can use everything except `typeName`.
constexpr uint32_t kPickupRespawnSizeBase = offsetof(PickupRespawnData, typeName);

struct NpcStateUpdateData {
    int world_index;
    int npc_index;       // index in global_npcs or partition_npcs
    int partition_index; // -1 for global NPCs
    NetVec3 position;
    float yaw;
    int state;          // NpcState as int
    int health;
    bool active;
    char npc_type[32];  // pawn def name (e.g. "Walker") so clients spawn the right sprite/scream
};

// Client -> server world request. The client names a world by its INDEX into the
// server's loaded world list, because that is what every other message already
// carries (`world_index` on pickup, melee and NPC payloads) and what the client
// itself learned from SCENE_UPDATE's world list.
//
// UNTRUSTED: the client-claimed index is validated against the server's own world
// count before use. A client cannot create a world, move itself into a world that
// does not exist, or move a DIFFERENT player.
struct RequestWorldData {
    int world_index;
};

// Server -> client world assignment. See WORLD_CHANGE in MessageType.
struct WorldChangeData {
    int world_index;   // the world the player is now IN (never -1)
    int accepted;      // 1 = the request was honoured, 0 = refused, player unchanged
    float spawnX;      // optional tail: authoritative spawn for that world
    float spawnY;
    float spawnZ;
};

// Bytes a reader must see before it can trust the trailing spawn. The world
// assignment itself is what gates pickup and melee validation, so a peer predating
// the tail still gets the part that matters.
//
// Gate on THIS, never on sizeof(WorldChangeData) — requiring the full size made a
// client drop every message from a server predating the field, which is the same
// trap as PickupRespawnData::typeName.
constexpr uint32_t kWorldChangeSizeBase = offsetof(WorldChangeData, spawnX);

// Server -> client COOP objective progress (OBJECTIVE_STATE). One objective per
// message; the server sends the player's own world's objectives periodically, so
// a lost datagram self-heals. See OBJECTIVE_STATE in MessageType.
struct ObjectiveStateData {
    int  world_index;      // which world the objective belongs to
    int  progress;
    int  target;           // <= 0 = binary objective
    char id[32];           // objective id (null-terminated)
};
// The id field is a fixed 32 bytes and the send site uses strncpy: a longer id
// is silently truncated on the wire, so the client's copy stops matching the
// server's. Objective ids are def names (shipped max: 24 chars). GameState
// warns at seed time when this fails; this predicate is the one definition.
constexpr size_t kObjectiveIdMax = sizeof(ObjectiveStateData::id);
constexpr bool ObjectiveIdFits(const std::string& id) {
    return id.size() < kObjectiveIdMax;
}

// --- Party / lobby (Phase 4 scaffold) ---
constexpr size_t kMaxName = PLAYER_NAME_MAX;
struct PartyHelloData {
    char displayName[kMaxName];   // untrusted; server sanitises
    int  requested_team;          // untrusted hint
};

// S->C cosmetic profile. `character` is a roster id (not a path). `color` is
// the client's requested palette index, validated by the server; 0 = unassigned
// (clients fall back to their deterministic id colour).
struct PlayerProfileData {
    uint32_t player_id;
    char     character[32];
    uint8_t  color;
    uint8_t  reserved[3];
};
constexpr size_t kPlayerProfileSizeBase = sizeof(PlayerProfileData);

struct XpUpdateData {
    uint32_t player_id;
    int xp;
    int level;
    int xp_to_next;
};

struct PlayerHurtData {
    uint32_t player_id; // victim
    int damage;
    float remaining_health;
};

struct PlayerKillData {
    uint32_t killer_id;
    uint32_t victim_id;
};

struct PickupCollectedData {
    uint32_t player_id;
    int pickup_id;
    // Required: pickup ids restart at 0 per world, so (pickup_id) alone is not
    // unique across the partition set the client holds. Without this, collecting
    // pickup 4 in world 1 also hides pickup 4 in every other loaded world.
    int world_index;
    int item_id;
    int quantity;
    char weapon_def_name[64]; // for weapon pickups
};

struct WeaponAmmoData {
    uint32_t player_id;
    int slot;           // hotbar slot 0-7
    int ammo;           // current ammo count
    int magazine;       // max ammo
    int action;         // 0=fire, 1=reload, 2=sync
};

struct WeaponFireData {
    uint32_t player_id;
    float origin_x, origin_y, origin_z;
    float dir_x, dir_y, dir_z;
    int weapon_type; // 1 = wand/energy bolt
    int power;       // damage multiplier
};

struct NpcDamageData {
    uint32_t player_id;
    int world_index;
    int npc_index;
    int partition_index; // -1 for global NPCs
    int damage;
};

// Melee hit reporting to the server. The client resolves the target locally
// (nearest pawn in reach, unobstructed by geometry) and the server
// re-validates the distance against the NPC's own position before applying
// damage, so a client cannot claim a hit it did not make. reach/stamina_cost
// travel with the report so the server can enforce the same limits and keep
// its own stamina pool in step with the client.
struct MeleeHitData {
    uint32_t player_id;
    int world_index;
    int npc_index;
    int partition_index;   // -1 = global NPC
    int damage;
    float reach;
    float stamina_cost;
    float origin_x, origin_y, origin_z;
    float dir_x, dir_y, dir_z;
};

struct ChatData {
    char text[256];
};

struct ScoreStateData {
    uint32_t player_id;
    int32_t  score;
    int32_t  team;          // server-assigned, authoritative
    int32_t  winningTeam;   // -1 when match not over
    uint8_t  matchOver;
};

struct CommandData {
    char cmd[64];
    char args[256];
};

struct SceneUpdateData {
    uint32_t data_size;
    char data[MAX_MESSAGE_SIZE - sizeof(uint32_t)];
};

struct ServerCallbacks {
    std::function<void(NetworkPlayer& player)> on_player_join;
    std::function<void(NetworkPlayer& player)> on_player_leave;
    std::function<void(const NetworkMessage& msg, const NetworkPlayer& sender)> on_message_received;
};

struct ClientCallbacks {
    std::function<void()> on_connected;
    std::function<void()> on_disconnected;
    std::function<void(const NetworkMessage& msg)> on_message_received;
};

// ---------------------------------------------------------------------------
// Platform socket helpers (internal)
// ---------------------------------------------------------------------------
#ifdef _WIN32
    #define TO_SOCK(fd)  ((SOCKET)(intptr_t)(fd))
    #define sock_fd_good(fd) ((int)(fd) >= 0)
    #define sock_fd_bad(fd)  ((int)(fd) < 0)
    #define close_sock(fd)  closesocket(TO_SOCK(fd))
    #define sock_set_opt(opt,optlen) (const char*)(opt),(optlen)
    #define sock_sendto_buf(buf,len) (const char*)(buf),(int)(len)
    #define sock_recvfrom_buf(buf,len) (char*)(buf),(int)(len)
#else
    #define TO_SOCK(fd)  (fd)
    #define sock_fd_good(fd) ((fd) >= 0)
    #define sock_fd_bad(fd)  ((fd) < 0)
    #define close_sock(fd)  close(fd)
    #define sock_set_opt(opt,optlen) (opt),(optlen)
    #define sock_sendto_buf(buf,len) (buf),(len)
    #define sock_recvfrom_buf(buf,len) (buf),(len)
#endif

// ---------------------------------------------------------------------------
// NetworkServer
// ---------------------------------------------------------------------------
class NetworkServer {
public:
    NetworkServer();
    ~NetworkServer();
    NetworkServer(const NetworkServer&) = delete;
    NetworkServer& operator=(const NetworkServer&) = delete;
    NetworkServer(NetworkServer&&) = delete;
    NetworkServer& operator=(NetworkServer&&) = delete;

    // bind_ip: literal IPv4 to bind the GAME socket to, or nullptr/"" for all
    // interfaces. AngelServ's --bind used to reach only the HTTP listener,
    // leaving the UDP game port open on every interface.
    bool init(uint16_t port, uint32_t max_players = MAX_PLAYERS,
              const char* bind_ip = nullptr);
    bool start();
    void stop();
    void update();

    void set_callbacks(ServerCallbacks cb) { m_callbacks = std::move(cb); }

    bool send_message(const NetworkPlayer& player, const NetworkMessage& msg);
    bool broadcast_message(const NetworkMessage& msg);

    // Force-disconnect a player by id (admin kick). Fires on_player_leave.
    // Returns false if the id is unknown.
    bool kick_player(uint32_t id);

    uint32_t player_count() const { return m_player_count; }
    const std::vector<NetworkPlayer>& players() const { return m_players; }
    bool is_running() const { return m_running; }

private:
    bool        m_winsock_initialized = false;
    int         m_socket_fd = -1;
    struct sockaddr_in m_address;
    uint16_t    m_port = 0;
    bool        m_running = false;
    std::vector<NetworkPlayer> m_players;
    uint32_t    m_player_count = 0;
    uint32_t    m_message_sequence = 0;
    double      m_last_heartbeat = 0;
    ServerCallbacks m_callbacks;

    // Pending connection tracking (3-way handshake)
    static constexpr int MAX_PENDING_PER_IP = 3;
    static constexpr double PENDING_TIMEOUT = 5.0;
    struct PendingConnection {
        char ip_address[IP_STRING_MAX];
        uint16_t port;
        uint32_t challenge_token;
        double start_time;
        int retries;
    };
    std::vector<PendingConnection> m_pending;
    uint32_t m_next_challenge = 1;
};

// ---------------------------------------------------------------------------
// NetworkClient
// ---------------------------------------------------------------------------
class NetworkClient {
public:
    NetworkClient();
    ~NetworkClient();
    NetworkClient(const NetworkClient&) = delete;
    NetworkClient& operator=(const NetworkClient&) = delete;

    bool connect(const char* server_ip, uint16_t port);
    void disconnect();
    void update();
    bool send_message(const NetworkMessage& msg);
    void set_callbacks(ClientCallbacks cb) { m_callbacks = std::move(cb); }

    // Profile to present at authentication (protocol v2). Called by the client
    // layer from the active PlayerProfile; the transport itself just ships the
    // strings, so it stays independent of where they came from.
    void set_profile(const std::string& displayName,
                     const std::string& modelPath,
                     const std::string& voiceSet,
                     int requestedTeam) {
        m_profile_display = displayName;
        m_profile_model   = modelPath;
        m_profile_voice   = voiceSet;
        m_profile_team    = requestedTeam;
    }

    // The profile the server accepted for us (PROFILE_STATE). Empty until the
    // server replies; `has_server_profile` tells them apart.
    bool has_server_profile() const { return m_server_profile_valid; }
    const ServerProfile& server_profile() const { return m_server_profile; }
    void clear_server_profile() { m_server_profile_valid = false; m_server_profile = ServerProfile(); }

    bool is_connected() const { return m_connected; }
    bool is_connecting() const { return m_connecting; }
    const std::string& get_server_ip() const { return m_server_ip; }
    uint16_t server_port() const { return m_server_port; }
    double get_last_ping_time() const { return m_last_ping_time; }
    double get_last_pong_time() const { return m_last_pong_time; }
    double get_rtt_s() const { return m_rtt; }
    int get_ping_ms() const { return static_cast<int>(std::round(m_rtt * 1000.0)); }
    void set_ping_interval(double seconds) { m_ping_interval = seconds; }

private:
    bool        m_winsock_initialized = false;
    int         m_socket_fd = -1;
    struct sockaddr_in m_server_address;
    bool        m_connected = false;
    bool        m_connecting = false;
    std::string m_server_ip;
    uint16_t    m_server_port = 0;
    uint32_t    m_message_sequence = 0;
    double      m_last_ping_time = 0;
    double      m_last_pong_time = 0;
    ClientCallbacks m_callbacks;

    // Real RTT measurement: the client sends PING with an incremented
    // sequence and records the send time; the server echoes the sequence on
    // PONG, which pairs the round trip.
    double      m_rtt = 0;
    double      m_ping_sent_at = 0;
    double      m_last_ping_sent = 0;
    double      m_ping_interval = 5.0;
    uint32_t    m_pending_ping_seq = 0xFFFFFFFF;

    // Handshake state
    int         m_handshake_retries = 0;
    uint32_t    m_challenge_token = 0;
    double      m_handshake_start = 0;
    static constexpr int MAX_HANDSHAKE_RETRIES = 5;
    static constexpr double HANDSHAKE_TIMEOUT = 2.0;

    // Outgoing profile (see set_profile).
    std::string m_profile_display;
    std::string m_profile_model;
    std::string m_profile_voice;
    int         m_profile_team = 0;

    // Profile the server confirmed via PROFILE_STATE.
    ServerProfile m_server_profile;
    bool          m_server_profile_valid = false;

    // Fill `out` with the auth payload for `token`, truncating every string to
    // its wire cap so the copy into NetworkMessage::payload is always safe.
    void BuildAuthPayload(uint32_t token, ClientAuthPayload& out) const;
};

// ---------------------------------------------------------------------------
// NetworkDiscovery
// ---------------------------------------------------------------------------
struct DiscoveredServer {
    std::string name;
    std::string version;
    std::string ip;
    uint16_t port = 0;
    uint32_t cur_players = 0;
    uint32_t max_players = 0;
    // Optional tail fields (new servers). Old servers omit them; old clients
    // ignore them because they stop parsing after the players token.
    std::string map;
    uint16_t http_port = 0;
    bool password = false;
    double last_seen = 0.0;   // monotonic seconds, for staleness pruning
};

class NetworkDiscovery {
public:
    NetworkDiscovery();
    ~NetworkDiscovery();
    NetworkDiscovery(const NetworkDiscovery&) = delete;
    NetworkDiscovery& operator=(const NetworkDiscovery&) = delete;

    // Server mode: pass the game port (announces presence + answers probes).
    // Client mode: pass 0 (sends probes, collects OZRESPONSE replies).
    bool init(const char* game_name, const char* game_version, uint16_t port);
    bool start();
    void stop();
    void update();
    bool send_request();
    bool parse_response(const char* response,
                        std::string& out_name,
                        std::string& out_version,
                        uint32_t& out_cur_players,
                        uint32_t* out_max_players,
                        uint16_t* out_port = nullptr,
                        std::string* out_map = nullptr,
                        uint16_t* out_http_port = nullptr,
                        bool* out_password = nullptr);

    // Server mode: keep the announced player counts current.
    void set_player_count(uint32_t cur, uint32_t max) {
        m_current_players = cur;
        m_max_players = max;
    }

    // Server mode: publish the optional tail (map/HTTP port/password) in the
    // discovery reply. Call on startup and whenever the active map changes.
    void set_server_info(uint16_t http_port, const char* map, bool password) {
        m_http_port = http_port;
        m_server_map = map ? map : "";
        m_has_password = password;
    }

    // Client mode: copy currently-known servers (entries unseen for >9s are dropped).
    void poll_discovered(std::vector<DiscoveredServer>& out);

    bool is_running() const { return m_running; }

private:
    void handle_datagram(char* buf, size_t len, uint32_t sender_addr, uint16_t sender_port);

    bool        m_winsock_initialized = false;
    int         m_socket_fd = -1;
    struct sockaddr_in m_broadcast_address;
    bool        m_running = false;
    std::string m_game_name;
    std::string m_game_version;
    uint16_t    m_game_port = 0;
    uint32_t    m_max_players = 32;
    uint32_t    m_current_players = 0;
    uint16_t    m_http_port = 0;
    std::string m_server_map;
    bool        m_has_password = false;
    std::vector<DiscoveredServer> m_discovered;
};

// ---------------------------------------------------------------------------
// Melee limits enforced by the server.
//
// A melee report is client-originated, so each of these is a ceiling on what a
// client may claim. Reach is capped rather than trusted outright; damage is
// capped so a spoofed swing cannot one-shot; the tick gap bounds swing rate.
// ---------------------------------------------------------------------------
constexpr float MELEE_MAX_REACH = 5.0f;   // longest shipped reach is 3.5
constexpr int   MELEE_MAX_DAMAGE = 500;  // above the toughest NPC's health
constexpr uint32_t MELEE_MIN_TICKS = 1;  // 1 server tick between swings (10/s cap)

// NPC_DAMAGE is also client-originated (the shipped client no longer sends it
// for weapons -- MELEE_HIT / server projectiles own those -- but any client may).
// Its damage ceiling is separate from the melee one because the two paths have
// different legit maxima: melee is a single swing, ranged is a projectile that
// may be tuned higher. Both are ceilings on a client claim, not gameplay tuning.
constexpr int   NPC_MAX_DAMAGE = 500;    // above the toughest NPC's health
constexpr float NPC_MAX_RANGE  = 20.0f;  // max weapon range the server accepts

// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------
const char* message_type_string(MessageType type);
bool is_valid_ip(const char* ip);
uint16_t find_free_port();

} // namespace net

#endif // OMEGA_NETWORK_HPP