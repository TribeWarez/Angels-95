#include "Network.hpp"
#include "../Version.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <thread>
#include <chrono>
#ifndef _WIN32
#include <fcntl.h>
#endif

namespace net {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
static int winsock_init_counted = 0;

static bool winsock_init() {
#ifdef _WIN32
    if (winsock_init_counted++ > 0) return true;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return false;
    }
#endif
    return true;
}

static void winsock_cleanup() {
#ifdef _WIN32
    if (winsock_init_counted > 0 && --winsock_init_counted == 0)
        WSACleanup();
#endif
}

static double now_seconds() {
    return (double)time(nullptr);
}

// Monotonic high-resolution clock for RTT (now_seconds() has 1s granularity).
static double now_monotonic_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static void fd_set_nonblocking(int fd) {
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(TO_SOCK(fd), FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------
const char* message_type_string(MessageType type) {
    switch (type) {
        case MessageType::PING:  return "PING";
        case MessageType::PONG:  return "PONG";
        case MessageType::PLAYER_JOIN:    return "PLAYER_JOIN";
        case MessageType::PLAYER_LEAVE:   return "PLAYER_LEAVE";
        case MessageType::PLAYER_UPDATE:  return "PLAYER_UPDATE";
        case MessageType::GAME_STATE:     return "GAME_STATE";
        case MessageType::CHAT:           return "CHAT";
        case MessageType::COMMAND:        return "COMMAND";
        case MessageType::FILE_TRANSFER:  return "FILE_TRANSFER";
        case MessageType::SCENE_UPDATE:   return "SCENE_UPDATE";
        case MessageType::PICKUP_COLLECT: return "PICKUP_COLLECT";
        case MessageType::PICKUP_RESPAWN: return "PICKUP_RESPAWN";
        case MessageType::NPC_STATE_UPDATE: return "NPC_STATE_UPDATE";
        case MessageType::XP_UPDATE:      return "XP_UPDATE";
        case MessageType::PLAYER_HURT:    return "PLAYER_HURT";
        case MessageType::PLAYER_KILL:    return "PLAYER_KILL";
        case MessageType::PLAYER_ACTION:  return "PLAYER_ACTION";
        case MessageType::PICKUP_COLLECTED: return "PICKUP_COLLECTED";
        case MessageType::NPC_DAMAGE:       return "NPC_DAMAGE";
    case MessageType::MELEE_HIT:         return "MELEE_HIT";
        case MessageType::PROFILE_STATE:     return "PROFILE_STATE";
        case MessageType::SCORE_STATE:      return "SCORE_STATE";
        case MessageType::PICKUP_RESYNC:   return "PICKUP_RESYNC";
        case MessageType::REQUEST_WORLD:    return "REQUEST_WORLD";
        case MessageType::WORLD_CHANGE:     return "WORLD_CHANGE";
        case MessageType::PLAYER_PROFILE:   return "PLAYER_PROFILE";
        case MessageType::PLAYER_STATE:     return "PLAYER_STATE";
        // These were missing, so OBJECTIVE_STATE (the co-op objective stream)
        // logged as UNKNOWN on every packet.
        case MessageType::OBJECTIVE_STATE:  return "OBJECTIVE_STATE";
        case MessageType::PARTY_HELLO:      return "PARTY_HELLO";
        case MessageType::PARTY_STATE:      return "PARTY_STATE";
case MessageType::REQUEST_CHUNK:    return "REQUEST_CHUNK";
        case MessageType::WEAPON_AMMO:      return "WEAPON_AMMO";
        case MessageType::SERVER_CHALLENGE: return "SERVER_CHALLENGE";
        case MessageType::CLIENT_AUTH:      return "CLIENT_AUTH";
        default: return "UNKNOWN";
    }
}

bool is_valid_ip(const char* ip) {
    if (!ip) return false;
    struct sockaddr_in sa;
    return inet_pton(AF_INET, ip, &sa.sin_addr) == 1;
}

// ---------------------------------------------------------------------------
// Profile sanitising (protocol v2)
//
// These run on data a client controls, before it reaches a log line, the
// scoreboard or GameState. The guiding rule is that the server must never be
// talked into printing escape sequences, storing an unbounded string, or
// holding a path it could be tricked into resolving later.
// ---------------------------------------------------------------------------

// Trim ASCII space/tab from both ends, in place on a std::string.
static void TrimAsciiSpace(std::string& s) {
    const char* ws = " \t";
    size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) { s.clear(); return; }
    size_t e = s.find_last_not_of(ws);
    s = s.substr(b, e - b + 1);
}

std::string SanitizeDisplayName(const std::string& raw, size_t max_len) {
    std::string out;
    out.reserve(std::min(raw.size(), max_len));
    for (unsigned char c : raw) {
        // Drop C0 controls and DEL: they are how a name smuggles terminal
        // escape sequences into server logs and the console. Bytes >= 0x80 are
        // kept so UTF-8 display names survive intact.
        if (c < 0x20 || c == 0x7F) continue;
        out.push_back(static_cast<char>(c));
    }
    TrimAsciiSpace(out);
    if (out.size() > max_len) out.resize(max_len);
    // A name of only stripped control characters yields "", and the caller then
    // substitutes a generated identity rather than accepting a nameless player.
    return out;
}

std::string SanitizeAssetRef(const std::string& raw, size_t max_len) {
    if (raw.empty()) return "";
    std::string out;
    out.reserve(std::min(raw.size(), max_len));
    for (unsigned char c : raw) {
        if (c < 0x20 || c == 0x7F) continue;
        // Asset references are ASCII paths in this engine; rejecting other
        // bytes avoids encoding tricks in a value the server may one day hand
        // to a loader.
        if (c > 0x7E) continue;
        out.push_back(static_cast<char>(c));
    }
    if (out.size() > max_len) out.resize(max_len);

    // Reject traversal outright rather than trying to normalise it. The server
    // never opens these, but a value that has already been normalised-by-force
    // is a value a future loader is more likely to trust.
    size_t seg = 0;
    for (size_t i = 0; i <= out.size(); ++i) {
        if (i == out.size() || out[i] == '/' || out[i] == '\\') {
            if (i - seg == 2 && out[seg] == '.' && out[seg + 1] == '.') return "";
            seg = i + 1;
        }
    }
    TrimAsciiSpace(out);
    return out;
}

std::string SanitizeCharacterId(const std::string& raw, size_t max_len) {
    if (raw.empty() || max_len == 0) return "";
    if (raw.size() > max_len) return "";
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        if (!(std::isalnum(c) || c == '_' || c == '-')) return "";
        out.push_back(static_cast<char>(c));
    }
    return out;
}

int ClampRequestedTeam(int32_t raw) {
    // Out of range collapses to "no preference" instead of clamping to a
    // boundary team, so a hostile value cannot nudge team assignment.
    if (raw < 0 || raw > MAX_TEAMS) return 0;
    return static_cast<int>(raw);
}

std::string MakeUniqueName(const std::string& name, const std::vector<std::string>& taken) {
    const size_t cap = PLAYER_NAME_MAX - 1;
    auto is_taken = [&](const std::string& n) {
        for (const auto& t : taken) if (t == n) return true;
        return false;
    };
    if (!is_taken(name)) return name;

    for (int n = 2; n < 10000; ++n) {
        std::string suffix = "(" + std::to_string(n) + ")";
        std::string base = name;
        if (base.size() + suffix.size() > cap) {
            base.resize(cap > suffix.size() ? cap - suffix.size() : 0);
        }
        std::string candidate = base + suffix;
        if (!is_taken(candidate)) return candidate;
    }
    return name;
}

uint16_t find_free_port() {
    winsock_init();
    int fd = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0;

    if (bind(TO_SOCK(fd), (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close_sock(fd);
        return 0;
    }

    socklen_t len = sizeof(addr);
    if (getsockname(TO_SOCK(fd), (struct sockaddr*)&addr, &len) < 0) {
        close_sock(fd);
        return 0;
    }

    uint16_t port = ntohs(addr.sin_port);
    close_sock(fd);
    return port;
}

// ---------------------------------------------------------------------------
// NetworkServer
// ---------------------------------------------------------------------------
NetworkServer::NetworkServer() {}
NetworkServer::~NetworkServer() { stop(); }

bool NetworkServer::init(uint16_t port, uint32_t max_players, const char* bind_ip) {
    if (!winsock_init()) return false;

    static_assert(sizeof(NetworkPlayer) <= MAX_MESSAGE_SIZE);

    m_port = port;
    m_players.resize(max_players);
    m_player_count = 0;
    m_message_sequence = 0;
    m_running = false;

    m_socket_fd = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd_bad(m_socket_fd)) {
        fprintf(stderr, "Server: socket creation failed\n");
        return false;
    }

    memset(&m_address, 0, sizeof(m_address));
    m_address.sin_family = AF_INET;
    m_address.sin_addr.s_addr = INADDR_ANY;
    if (bind_ip && *bind_ip) {
        if (inet_pton(AF_INET, bind_ip, &m_address.sin_addr) != 1) {
            // Server.cpp resolves --bind before this call; a value that still
            // fails to parse is treated as "all interfaces" with a warning
            // rather than leaving the socket unbound.
            fprintf(stderr, "Server: invalid bind address '%s' - using 0.0.0.0\n",
                    bind_ip);
            m_address.sin_addr.s_addr = INADDR_ANY;
        }
    }
    m_address.sin_port = htons(port);

    int opt = 1;
    setsockopt(TO_SOCK(m_socket_fd), SOL_SOCKET, SO_REUSEADDR,
               sock_set_opt(&opt, sizeof(opt)));

    return true;
}

bool NetworkServer::start() {
    if (m_running || sock_fd_bad(m_socket_fd)) return false;

    if (bind(TO_SOCK(m_socket_fd), (struct sockaddr*)&m_address,
             sizeof(m_address)) < 0) {
        fprintf(stderr, "Server: bind failed on port %u\n", m_port);
        return false;
    }

    fd_set_nonblocking(m_socket_fd);
    m_running = true;
    m_last_heartbeat = now_seconds();

    printf("Server listening on UDP %u\n", m_port);
    return true;
}

void NetworkServer::stop() {
    if (!m_running) return;
    m_running = false;
    if (sock_fd_good(m_socket_fd)) {
        close_sock(m_socket_fd);
        m_socket_fd = -1;
    }
    for (auto& p : m_players) {
        if (p.connected) {
            p.connected = false;
            if (m_callbacks.on_player_leave) m_callbacks.on_player_leave(p);
        }
    }
    m_player_count = 0;
    winsock_cleanup();
}

void NetworkServer::update() {
    if (!m_running) return;

    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    NetworkMessage msg;

    while (true) {
        ssize_t bytes = recvfrom(TO_SOCK(m_socket_fd),
                                 sock_recvfrom_buf(&msg, sizeof(msg)),
                                 0,
                                 (struct sockaddr*)&client_addr, &addr_len);
        if (bytes <= 0) break;

        if (static_cast<size_t>(bytes) < sizeof(NetworkMessage)) continue;
        if (msg.magic != MAGIC) continue;

        char client_ip[IP_STRING_MAX];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        uint16_t client_port = ntohs(client_addr.sin_port);

        NetworkPlayer* player = nullptr;
        for (auto& p : m_players) {
            if (p.connected &&
                std::strcmp(p.ip_address, client_ip) == 0 &&
                p.port == client_port) {
                player = &p;
                break;
            }
        }

        if (player) {
            player->last_seen = now_seconds();
        }

        // ---- 3-way handshake: unknown client -> challenge -> auth ----
        auto msgType = static_cast<MessageType>(msg.type);

        if (!player && (msgType == MessageType::PLAYER_JOIN || msgType == MessageType::CLIENT_AUTH)) {
            // Check for existing pending connection from this IP:port
            PendingConnection* pending = nullptr;
            for (auto& p : m_pending) {
                if (std::strcmp(p.ip_address, client_ip) == 0 && p.port == client_port) {
                    pending = &p;
                    break;
                }
            }

            if (msgType == MessageType::PLAYER_JOIN) {
                // Rate-limit: count pending from this IP
                int ipCount = 0;
                double now = now_seconds();
                for (auto& p : m_pending) {
                    if (std::strcmp(p.ip_address, client_ip) == 0) ipCount++;
                }
                if (ipCount >= MAX_PENDING_PER_IP) {
                    printf("Handshake rejected: too many pending from %s\n", client_ip);
                    continue; // drop silently
                }

                if (!pending) {
                    // Create new pending connection
                    PendingConnection pc;
                    std::strncpy(pc.ip_address, client_ip, sizeof(pc.ip_address) - 1);
                    pc.ip_address[sizeof(pc.ip_address) - 1] = '\0';
                    pc.port = client_port;
                    pc.challenge_token = m_next_challenge++;
                    pc.start_time = now_seconds();
                    pc.retries = 0;
                    m_pending.push_back(pc);
                    pending = &m_pending.back();
                } else {
                    // Re-send challenge (client may have lost it)
                    if (now - pending->start_time > PENDING_TIMEOUT) {
                        // Expired — remove and drop
                        m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
                            [&](const PendingConnection& p) { return &p == pending; }), m_pending.end());
                        continue;
                    }
                }

                // Send challenge to client
                NetworkMessage challenge;
                challenge.magic = MAGIC;
                challenge.type = static_cast<uint32_t>(MessageType::SERVER_CHALLENGE);
                challenge.size = sizeof(pending->challenge_token);
                challenge.sequence = m_message_sequence++;
                challenge.timestamp = static_cast<uint32_t>(now_seconds());
                memcpy(challenge.payload, &pending->challenge_token, sizeof(pending->challenge_token));

    struct sockaddr_in addr;
                memset(&addr, 0, sizeof(addr));
                addr.sin_family = AF_INET;
                addr.sin_port = htons(client_port);
                inet_pton(AF_INET, client_ip, &addr.sin_addr);
                sendto(TO_SOCK(m_socket_fd),
                       sock_sendto_buf(&challenge, sizeof(challenge)), 0,
                       (struct sockaddr*)&addr, sizeof(addr));
                printf("Challenge sent to %s:%u (token=%u)\n", client_ip, client_port, pending->challenge_token);
                continue;
            }

            if (msgType == MessageType::CLIENT_AUTH) {
                if (!pending) {
                    printf("CLIENT_AUTH from %s:%u without pending challenge\n", client_ip, client_port);
                    continue;
                }

                // Verify token
                uint32_t received_token = 0;
                if (msg.size >= sizeof(received_token))
                    memcpy(&received_token, msg.payload, sizeof(received_token));

                if (received_token != pending->challenge_token) {
                    printf("CLIENT_AUTH from %s:%u: invalid token (got %u, expected %u)\n",
                           client_ip, client_port, received_token, pending->challenge_token);
                    // Allow retries
                    pending->retries++;
                    if (pending->retries >= 3) {
                        m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
                            [&](const PendingConnection& p) { return &p == pending; }), m_pending.end());
                    }
                    continue;
                }

                // Token valid — parse the optional v2 profile block, then create
                // the player. A payload too short to hold a version is a v1
                // client: accepted, but it gets generated defaults.
                ServerProfile parsed;
                bool have_profile = false;
                if (msg.size >= kAuthSizeV2) {
                    ClientAuthPayload auth{};
                    // Copy only the bytes actually sent; the struct is packed but
                    // a truncated send must not read past msg.size.
                    const uint32_t copy = std::min<uint32_t>(static_cast<uint32_t>(msg.size),
                                                             static_cast<uint32_t>(sizeof(auth)));
                    memcpy(&auth, msg.payload, copy);

                    if (auth.protocolVersion != PROTOCOL_VERSION) {
                        printf("CLIENT_AUTH from %s:%u: protocol version %u (server %u) — rejected\n",
                               client_ip, client_port, (unsigned)auth.protocolVersion,
                               (unsigned)PROTOCOL_VERSION);
                        // Drop the pending entry so the client stops retrying and
                        // falls back to its disconnect/retry path.
                        m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
                            [&](const PendingConnection& pc) { return &pc == pending; }), m_pending.end());
                        continue;
                    }

                    // structSize is the client's own idea of the layout; it must
                    // not claim to be larger than what it actually sent, or a
                    // truncated packet would be read past its end.
                    const uint32_t have = std::min<uint32_t>(copy, static_cast<uint32_t>(auth.structSize));
                    // A field is only read if its last byte actually arrived. The
                    // struct is packed, so end offsets are byte-exact.
                    auto field_arrived = [&](size_t offset, size_t width) {
                        return have >= offset + width;
                    };
                    // Copy a fixed char[] field with an explicit cap. A client
                    // may fill all N bytes with no terminator, so treating the
                    // field as a C string would read into the neighbouring field.
                    auto field_to_string = [](const char* field, size_t cap) {
                        size_t len = 0;
                        while (len < cap && field[len] != '\0') ++len;
                        return std::string(field, len);
                    };

                    const size_t nameOff = offsetof(ClientAuthPayload, displayName);
                    if (field_arrived(nameOff, 1))
                        parsed.displayName = SanitizeDisplayName(
                            field_to_string(auth.displayName, PLAYER_NAME_MAX), PLAYER_NAME_MAX - 1);

                    const size_t modelOff = offsetof(ClientAuthPayload, modelPath);
                    if (field_arrived(modelOff, 1))
                        parsed.modelPath = SanitizeAssetRef(
                            field_to_string(auth.modelPath, PROFILE_MODEL_MAX), PROFILE_MODEL_MAX - 1);

                    const size_t voiceOff = offsetof(ClientAuthPayload, voiceSet);
                    if (field_arrived(voiceOff, 1))
                        parsed.voiceSet = SanitizeAssetRef(
                            field_to_string(auth.voiceSet, PROFILE_VOICE_MAX), PROFILE_VOICE_MAX - 1);

                    const size_t teamOff = offsetof(ClientAuthPayload, requestedTeam);
                    if (field_arrived(teamOff, sizeof(int32_t)))
                        parsed.requestedTeam = ClampRequestedTeam(auth.requestedTeam);
                    // Server assigns the team; it has no team concept yet, so
                    // this stays 0 and requestedTeam is recorded separately as
                    // an untrusted preference for future use.
                    parsed.team = 0;
                    have_profile = true;
                }

                // Token valid — create player
                for (auto& p : m_players) {
                    if (!p.connected) {
                        player = &p;
                        player->id = static_cast<uint32_t>(&p - m_players.data());
                        player->profile_received = have_profile;

                        // Prefer the profile's name, but never accept an empty or
                        // duplicate one: names are used as player identities in
                        // GameState (which persists save data keyed by name), so
                        // two players sharing one would collide.
                        std::vector<std::string> taken;
                        for (const auto& other : m_players)
                            if (other.connected) taken.push_back(other.profile.displayName);

                        std::string chosen = parsed.displayName;
                        const std::string generated = "Player_" + std::to_string(player->id + 1);
                        bool name_changed = false;
                        if (chosen.empty()) {
                            chosen = MakeUniqueName(generated, taken);
                            name_changed = true;
                        } else {
                            const std::string unique = MakeUniqueName(chosen, taken);
                            name_changed = (unique != chosen);
                            chosen = unique;
                        }
                        parsed.displayName = chosen;
                        player->profile = parsed;
                        // Keep the legacy `name` field in sync so every existing
                        // consumer (logs, GameState, chat) sees the new identity.
                        std::snprintf(player->name, sizeof(player->name), "%s", chosen.c_str());

                        std::snprintf(player->ip_address, sizeof(player->ip_address), "%s", client_ip);
                        player->port = client_port;
                        player->connected = true;
                        player->last_ping = now_seconds();
                        player->last_seen = now_seconds();
                        m_player_count++;
                        // Remove pending entry
                        m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
                            [&](const PendingConnection& pc) {
                                return std::strcmp(pc.ip_address, client_ip) == 0 && pc.port == client_port;
                            }), m_pending.end());
                        printf("Player %s (id=%u) authenticated from %s:%u (proto %u%s)\n",
                               player->name, player->id, client_ip, client_port,
                               have_profile ? (unsigned)PROTOCOL_VERSION : 1u,
                               name_changed ? ", name adjusted" : "");
                        if (m_callbacks.on_player_join)
                            m_callbacks.on_player_join(*player);

                        // Tell the client what the server actually accepted, so it
                        // never assumes its own values were honoured.
                        if (have_profile) {
                            ProfileStateData ps{};
                            ps.playerId = player->id;
                            ps.team = player->profile.team;
                            ps.requestedTeam = player->profile.requestedTeam;
                            ps.nameWasChanged = name_changed ? 1 : 0;
                            std::snprintf(ps.displayName, sizeof(ps.displayName), "%s",
                                          player->profile.displayName.c_str());
                            NetworkMessage reply;
                            reply.magic = MAGIC;
                            reply.type = static_cast<uint32_t>(MessageType::PROFILE_STATE);
                            reply.size = static_cast<uint32_t>(sizeof(ps));
                            reply.sequence = m_message_sequence++;
                            reply.timestamp = static_cast<uint32_t>(now_seconds());
                            memcpy(reply.payload, &ps, sizeof(ps));
                            send_message(*player, reply);
                        }
                        break;
                    }
                }
                if (!player) {
                    printf("Server full, rejecting %s:%u\n", client_ip, client_port);
                }
                continue; // CLIENT_AUTH handled, don't fall through to message routing
            }
        }

        if (player) {
            switch (msgType) {
                case MessageType::PING: {
                    NetworkMessage pong;
                    pong.magic = MAGIC;
                    pong.type = static_cast<uint32_t>(MessageType::PONG);
                    pong.size = 0;
                    pong.sequence = msg.sequence;
                    pong.timestamp = static_cast<uint32_t>(now_seconds());
                    send_message(*player, pong);
                    break;
                }
                case MessageType::PONG:
                    // A client answers the server's heartbeat PING with PONG.
                    // The server does not measure per-client RTT, so this is an
                    // expected protocol message - not an unhandled one. Without
                    // this case it fell through to on_message_received and
                    // Server.cpp warned "Unhandled message type PONG" every
                    // heartbeat, which looked like a broken handshake.
                    break;
                case MessageType::PLAYER_JOIN:
                    // Already connected — ignore rebroadcasts.
                    break;
                case MessageType::CLIENT_AUTH:
                    // Re-auth from a connected player: replay the join payload
                    // so a client whose original ack was lost can finish its
                    // handshake. on_player_join must be idempotent.
                    if (m_callbacks.on_player_join)
                        m_callbacks.on_player_join(*player);
                    break;
                case MessageType::PLAYER_LEAVE:
                    printf("Player %s left\n", player->name);
                    player->connected = false;
                    m_player_count--;
                    if (m_callbacks.on_player_leave)
                        m_callbacks.on_player_leave(*player);
                    break;
                default:
                    if (m_callbacks.on_message_received)
                        m_callbacks.on_message_received(msg, *player);
                    break;
            }
        }
    }

    double now = now_seconds();

    // Pending connection timeouts
    double now_pending = now_seconds();
    m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
        [&](const PendingConnection& p) { return now_pending - p.start_time > PENDING_TIMEOUT; }),
        m_pending.end());

    // Player timeouts
    for (auto& p : m_players) {
        if (p.connected && now_pending - p.last_seen > TIMEOUT_INTERVAL) {
            printf("Player %s timed out\n", p.name);
            p.connected = false;
            m_player_count--;
            if (m_callbacks.on_player_leave)
                m_callbacks.on_player_leave(p);
        }
    }

    // Heartbeat
    if (now - m_last_heartbeat > HEARTBEAT_INTERVAL) {
        NetworkMessage hb;
        hb.magic = MAGIC;
        hb.type = static_cast<uint32_t>(MessageType::PING);
        hb.size = 0;
        hb.sequence = m_message_sequence++;
        hb.timestamp = static_cast<uint32_t>(now);
        broadcast_message(hb);
        m_last_heartbeat = now;
    }
}

bool NetworkServer::kick_player(uint32_t id) {
    for (auto& p : m_players) {
        if (p.connected && p.id == id) {
            printf("Player %s kicked\n", p.name);
            p.connected = false;
            m_player_count--;
            if (m_callbacks.on_player_leave)
                m_callbacks.on_player_leave(p);
            return true;
        }
    }
    return false;
}

bool NetworkServer::send_message(const NetworkPlayer& player,
                                 const NetworkMessage& msg) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(player.port);
    inet_pton(AF_INET, player.ip_address, &addr.sin_addr);

    // Zeroed record: see MakeSendRecord. Callers leave the tail uninitialised.
    NetworkMessage out = MakeSendRecord(msg);

    ssize_t bytes = sendto(TO_SOCK(m_socket_fd),
                           sock_sendto_buf(&out, sizeof(out)), 0,
                           (struct sockaddr*)&addr, sizeof(addr));
    return bytes == static_cast<ssize_t>(sizeof(out));
}

bool NetworkServer::broadcast_message(const NetworkMessage& msg) {
    bool ok = true;
    for (const auto& p : m_players) {
        if (p.connected && !send_message(p, msg)) ok = false;
    }
    return ok;
}

// ---------------------------------------------------------------------------
// NetworkClient
// ---------------------------------------------------------------------------
NetworkClient::NetworkClient() {}
NetworkClient::~NetworkClient() { disconnect(); }

void NetworkClient::BuildAuthPayload(uint32_t token, ClientAuthPayload& out) const {
    out = ClientAuthPayload{};
    out.challengeToken  = token;
    out.protocolVersion = PROTOCOL_VERSION;
    out.structSize      = static_cast<uint16_t>(sizeof(ClientAuthPayload));

    // Truncate rather than let a long profile overflow a fixed wire field. The
    // server re-sanitises everything, but never send an unterminated field.
    auto copy_field = [](char* dst, size_t cap, const std::string& src) {
        const size_t n = src.size() < (cap - 1) ? src.size() : (cap - 1);
        if (n) memcpy(dst, src.data(), n);
        dst[n] = '\0';
    };
    copy_field(out.displayName, PLAYER_NAME_MAX,    m_profile_display);
    copy_field(out.modelPath,   PROFILE_MODEL_MAX, m_profile_model);
    copy_field(out.voiceSet,    PROFILE_VOICE_MAX, m_profile_voice);
    out.requestedTeam = static_cast<int32_t>(ClampRequestedTeam(m_profile_team));
}

bool NetworkClient::connect(const char* server_ip, uint16_t port) {
    if (m_connected) return true;
    if (m_connecting) return false; // already in handshake
    if (!winsock_init()) return false;

    m_socket_fd = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd_bad(m_socket_fd)) {
        fprintf(stderr, "Client: socket creation failed\n");
        return false;
    }

    fd_set_nonblocking(m_socket_fd);

    memset(&m_server_address, 0, sizeof(m_server_address));
    m_server_address.sin_family = AF_INET;
    m_server_address.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip, &m_server_address.sin_addr) <= 0) {
        fprintf(stderr, "Client: invalid IP %s\n", server_ip);
        close_sock(m_socket_fd);
        m_socket_fd = -1;
        return false;
    }

    m_server_ip = server_ip;
    m_server_port = port;
    m_connecting = true;
    m_connected = false;
    m_challenge_token = 0;
    m_handshake_retries = 0;
    m_handshake_start = now_seconds();
    m_last_ping_time = now_seconds();
    m_last_pong_time = now_seconds();
    m_last_ping_sent = now_monotonic_s();
    m_rtt = 0;
    m_ping_sent_at = 0;
    m_pending_ping_seq = 0xFFFFFFFF;

    // Send initial hello
    NetworkMessage join;
    join.magic = MAGIC;
    join.type = static_cast<uint32_t>(MessageType::PLAYER_JOIN);
    join.size = 0;
    join.sequence = m_message_sequence++;
    join.timestamp = static_cast<uint32_t>(now_seconds());

    ssize_t bytes = sendto(TO_SOCK(m_socket_fd),
                           sock_sendto_buf(&join, sizeof(join)), 0,
                           (struct sockaddr*)&m_server_address,
                           sizeof(m_server_address));
    if (bytes == sizeof(join)) {
        printf("Client: handshake started with %s:%u\n", server_ip, port);
        return true;
    }

    fprintf(stderr, "Client: connect failed to %s:%u\n", server_ip, port);
    close_sock(m_socket_fd);
    m_socket_fd = -1;
    m_connecting = false;
    return false;
}

void NetworkClient::disconnect() {
    bool wasConnected = m_connected;
    m_connected = false;
    m_connecting = false;

    if (wasConnected && sock_fd_good(m_socket_fd)) {
        NetworkMessage leave;
        leave.magic = MAGIC;
        leave.type = static_cast<uint32_t>(MessageType::PLAYER_LEAVE);
        leave.size = 0;
        leave.sequence = m_message_sequence++;
        leave.timestamp = static_cast<uint32_t>(now_seconds());

        sendto(TO_SOCK(m_socket_fd),
               sock_sendto_buf(&leave, sizeof(leave)), 0,
               (struct sockaddr*)&m_server_address, sizeof(m_server_address));
    }

    if (m_callbacks.on_disconnected) m_callbacks.on_disconnected();

    if (sock_fd_good(m_socket_fd)) {
        close_sock(m_socket_fd);
        m_socket_fd = -1;
    }
    winsock_cleanup();
}

void NetworkClient::update() {
    if (!m_connected && !m_connecting) return;

    double now = now_seconds();

    // Handshake timeout/retry
    if (m_connecting && !m_connected) {
        if (now - m_handshake_start > HANDSHAKE_TIMEOUT) {
            if (m_handshake_retries >= MAX_HANDSHAKE_RETRIES) {
                fprintf(stderr, "Client: handshake failed after %d retries\n", m_handshake_retries);
                disconnect();
                return;
            }
            m_handshake_retries++;
            m_handshake_start = now;
            NetworkMessage retry;
            retry.magic = MAGIC;
            retry.sequence = m_message_sequence++;
            retry.timestamp = static_cast<uint32_t>(now);
            if (m_challenge_token != 0) {
                // Already challenged: resend our auth token WITH the profile so
                // a lost ack cannot downgrade us to a v1 (nameless) join.
                retry.type = static_cast<uint32_t>(MessageType::CLIENT_AUTH);
                ClientAuthPayload retry_auth{};
                BuildAuthPayload(m_challenge_token, retry_auth);
                retry.size = static_cast<uint32_t>(sizeof(retry_auth));
                memcpy(retry.payload, &retry_auth, sizeof(retry_auth));
            } else {
                retry.type = static_cast<uint32_t>(MessageType::PLAYER_JOIN);
                retry.size = 0;
            }
            sendto(TO_SOCK(m_socket_fd),
                   sock_sendto_buf(&retry, sizeof(retry)), 0,
                   (struct sockaddr*)&m_server_address, sizeof(m_server_address));
        }
    }

    // Heartbeat ping: measure real RTT. The server echoes our sequence on
    // PONG, pairing this outgoing ping with its response. Uses the monotonic
    // clock so sub-second round-trips resolve cleanly.
    if (m_connected && now_monotonic_s() - m_last_ping_sent > m_ping_interval) {
        m_last_ping_sent = now_monotonic_s();
        m_ping_sent_at = m_last_ping_sent;
        m_pending_ping_seq = m_message_sequence++;
        NetworkMessage ping;
        ping.magic = MAGIC;
        ping.type = static_cast<uint32_t>(MessageType::PING);
        ping.size = 0;
        ping.sequence = m_pending_ping_seq;
        ping.timestamp = static_cast<uint32_t>(now * 1000.0);
        send_message(ping);
    }

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    NetworkMessage msg;

    while (true) {
        ssize_t bytes = recvfrom(TO_SOCK(m_socket_fd),
                                 sock_recvfrom_buf(&msg, sizeof(msg)),
                                 0,
                                 (struct sockaddr*)&addr, &addr_len);
        if (bytes <= 0) break;
        if (static_cast<size_t>(bytes) < sizeof(NetworkMessage)) continue;
        if (msg.magic != MAGIC) continue;

        auto rcvType = static_cast<MessageType>(msg.type);

        // Handle handshake challenge when connecting
        if (m_connecting && rcvType == MessageType::SERVER_CHALLENGE) {
            uint32_t token = 0;
            if (msg.size >= sizeof(token))
                memcpy(&token, msg.payload, sizeof(token));
            m_challenge_token = token;

            // Respond with CLIENT_AUTH, carrying the profile (protocol v2).
            ClientAuthPayload auth_payload{};
            BuildAuthPayload(token, auth_payload);

            NetworkMessage auth;
            auth.magic = MAGIC;
            auth.type = static_cast<uint32_t>(MessageType::CLIENT_AUTH);
            auth.size = static_cast<uint32_t>(sizeof(auth_payload));
            auth.sequence = m_message_sequence++;
            auth.timestamp = static_cast<uint32_t>(now);
            memcpy(auth.payload, &auth_payload, sizeof(auth_payload));
            sendto(TO_SOCK(m_socket_fd),
                   sock_sendto_buf(&auth, sizeof(auth)), 0,
                   (struct sockaddr*)&m_server_address, sizeof(m_server_address));

            // Stay in the connecting state: `m_connected` must only flip once
            // the server confirms (first non-challenge message). If this AUTH
            // is lost, the retry path below resends it and the server re-acks.
            printf("Client: challenge accepted (token=%u), awaiting server confirm\n", token);
            continue;
        }

        // First non-challenge message from the server confirms authentication.
        if (m_connecting) {
            m_connected = true;
            m_connecting = false;
            printf("Client: authenticated with server\n");
            if (m_callbacks.on_connected) m_callbacks.on_connected();
        }

        switch (rcvType) {
            case MessageType::PROFILE_STATE: {
                // The server tells us which identity it accepted. Store it so
                // the UI can show the final name rather than assuming our own
                // request was honoured.
                if (msg.size >= sizeof(ProfileStateData)) {
                    ProfileStateData ps{};
                    memcpy(&ps, msg.payload, sizeof(ps));
                    const size_t cap = sizeof(ps.displayName);
                    size_t len = 0;
                    while (len < cap && ps.displayName[len] != '\0') ++len;
                    m_server_profile.displayName.assign(ps.displayName, len);
                    m_server_profile.team          = ps.team;
                    m_server_profile.requestedTeam = ClampRequestedTeam(ps.requestedTeam);
                    m_server_profile_valid = true;
                    // Our own id. Directed server messages (PLAYER_STATE, and the
                    // PLAYER_HURT relay) are attributed with it; it used to be
                    // parsed and thrown away, which is why no inbound message
                    // could ever ask "is this about me?".
                    m_profile_player_id = ps.playerId;
                    m_profile_player_id_valid = true;
                    if (ps.nameWasChanged)
                        printf("Client: server adjusted display name to '%s'\n",
                               m_server_profile.displayName.c_str());
                }
                break;
            }
            case MessageType::PONG:
                m_last_pong_time = now;
                // Pair with the most recent client-sent PING via sequence.
                if (m_pending_ping_seq != 0xFFFFFFFF && msg.sequence == m_pending_ping_seq) {
                    double rtt = now_monotonic_s() - m_ping_sent_at;
                    m_pending_ping_seq = 0xFFFFFFFF;
                    if (rtt >= 0 && rtt < 10.0) m_rtt = rtt;
                }
                break;
            case MessageType::PING: {
                NetworkMessage pong;
                pong.magic = MAGIC;
                pong.type = static_cast<uint32_t>(MessageType::PONG);
                pong.size = 0;
                pong.sequence = msg.sequence;
                pong.timestamp = msg.timestamp;
                send_message(pong);
                break;
            }
            default:
                if (m_callbacks.on_message_received)
                    m_callbacks.on_message_received(msg);
                break;
        }
    }

    if (now - m_last_pong_time > TIMEOUT_INTERVAL) {
        fprintf(stderr, "Server timeout, disconnecting\n");
        disconnect();
    }
}

bool NetworkClient::send_message(const NetworkMessage& msg) {
    if (!m_connected || sock_fd_bad(m_socket_fd)) return false;
    // Same zeroed-record rule as the server side (see MakeSendRecord).
    NetworkMessage out = MakeSendRecord(msg);

    ssize_t bytes = sendto(TO_SOCK(m_socket_fd),
                           sock_sendto_buf(&out, sizeof(out)), 0,
                           (struct sockaddr*)&m_server_address,
                           sizeof(m_server_address));
    return bytes == static_cast<ssize_t>(sizeof(out));
}

// ---------------------------------------------------------------------------
// NetworkDiscovery
// ---------------------------------------------------------------------------
NetworkDiscovery::NetworkDiscovery() {}
NetworkDiscovery::~NetworkDiscovery() { stop(); }

bool NetworkDiscovery::init(const char* game_name, const char* game_version,
                            uint16_t port) {
    if (!winsock_init()) return false;

    m_game_name = game_name ? game_name : "Angels95";
    m_game_version = game_version ? game_version : oz::kBuildVersion;
    m_game_port = port;
    return true;
}

bool NetworkDiscovery::start() {
    if (m_running) return true;

    m_socket_fd = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd_bad(m_socket_fd)) {
        fprintf(stderr, "Discovery: socket creation failed\n");
        return false;
    }

    int broadcast = 1;
    setsockopt(TO_SOCK(m_socket_fd), SOL_SOCKET, SO_BROADCAST,
               sock_set_opt(&broadcast, sizeof(broadcast)));

    int reuse = 1;
    setsockopt(TO_SOCK(m_socket_fd), SOL_SOCKET, SO_REUSEADDR,
               sock_set_opt(&reuse, sizeof(reuse)));

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    // Server mode owns DISCOVERY_PORT; client mode binds an ephemeral port so
    // unicast replies never contend with a server socket on the same host.
    bind_addr.sin_port = htons(m_game_port != 0 ? DISCOVERY_PORT : 0);
    bind(TO_SOCK(m_socket_fd), (struct sockaddr*)&bind_addr, sizeof(bind_addr));

    memset(&m_broadcast_address, 0, sizeof(m_broadcast_address));
    m_broadcast_address.sin_family = AF_INET;
    m_broadcast_address.sin_addr.s_addr = INADDR_BROADCAST;
    m_broadcast_address.sin_port = htons(DISCOVERY_PORT);

    fd_set_nonblocking(m_socket_fd);
    m_running = true;
    return true;
}

void NetworkDiscovery::stop() {
    if (!m_running) return;
    m_running = false;
    if (sock_fd_good(m_socket_fd)) {
        close_sock(m_socket_fd);
        m_socket_fd = -1;
    }
    winsock_cleanup();
}

bool NetworkDiscovery::send_request() {
    if (sock_fd_bad(m_socket_fd)) {
        start();
    }

    const char* magic = "OZDISCOVER";
    ssize_t sent = sendto(TO_SOCK(m_socket_fd),
                          sock_sendto_buf(magic, strlen(magic)), 0,
                          (struct sockaddr*)&m_broadcast_address,
                          sizeof(m_broadcast_address));
    return sent == static_cast<ssize_t>(strlen(magic));
}

bool NetworkDiscovery::parse_response(const char* response,
                                      std::string& out_name,
                                      std::string& out_version,
                                      uint32_t& out_cur_players,
                                      uint32_t* out_max_players,
                                      uint16_t* out_port,
                                      std::string* out_map,
                                      uint16_t* out_http_port,
                                      bool* out_password) {
    if (!response) return false;

    const char* prefix = "OZRESPONSE";
    size_t plen = strlen(prefix);
    if (std::strncmp(response, prefix, plen) != 0) return false;

    const char* p = response + plen + 1;
    const char* end = std::strchr(p, ':');
    if (!end) return false;
    out_name.assign(p, end - p);
    p = end + 1;

    end = std::strchr(p, ':');
    if (!end) return false;
    out_version.assign(p, end - p);
    p = end + 1;

    end = std::strchr(p, ':');
    if (!end) return false;
    std::string sport(p, end - p);
    uint16_t port = 0;
    try {
        unsigned long v = std::stoul(sport);
        if (v > 65535) return false;
        port = static_cast<uint16_t>(v);
    } catch (...) {
        return false;
    }
    if (out_port) *out_port = port;
    p = end + 1;

    uint32_t cur = 0, max = 0;
    int consumed = 0;
    if (std::sscanf(p, "%u/%u%n", &cur, &max, &consumed) < 2) return false;
    out_cur_players = cur;
    if (out_max_players) *out_max_players = max;

    // Optional forward-compatible tail. Old servers stop after players/max, so
    // every field here is optional; an unknown or malformed key is ignored.
    if (out_map) out_map->clear();
    if (out_http_port) *out_http_port = 0;
    if (out_password) *out_password = false;
    const char* tail = p + consumed;
    while (tail && *tail == ';') {
        ++tail;
        const char* eq = std::strchr(tail, '=');
        if (!eq) break;
        const std::string key(tail, eq - tail);
        const char* valueStart = eq + 1;
        const char* valueEnd = std::strchr(valueStart, ';');
        const std::string value = valueEnd ? std::string(valueStart, valueEnd - valueStart)
                                           : std::string(valueStart);
        if (key == "http" && out_http_port) {
            try {
                unsigned long v = std::stoul(value);
                if (v <= 65535) *out_http_port = static_cast<uint16_t>(v);
            } catch (...) {}
        } else if (key == "map" && out_map) {
            *out_map = value;
        } else if (key == "pw" && out_password) {
            *out_password = (value == "1" || value == "true");
        }
        tail = valueEnd;
    }

    return true;
}

void NetworkDiscovery::handle_datagram(char* buf, size_t len, uint32_t sender_addr, uint16_t sender_port) {
    buf[len] = '\0';

    if (std::strncmp(buf, "OZDISCOVER", 10) == 0) {
        // Server mode: answer probes (and other servers' announces) with our details,
        // sent back to the probe's source port.
        if (m_game_port == 0) return;
        auto clean = [](const std::string& in) {
            std::string out;
            for (unsigned char c : in) {
                if (c >= 0x20 && c != 0x7F && c != ':' && c != ';') out.push_back((char)c);
            }
            return out;
        };
        const std::string safeName = clean(m_game_name);
        const std::string safeVer = clean(m_game_version);
        const std::string safeMap = clean(m_server_map);
        char reply[384];
        int rlen = snprintf(reply, sizeof(reply),
                            "OZRESPONSE:%s:%s:%u:%u/%u;http=%u;map=%s;pw=%d",
                            safeName.c_str(), safeVer.c_str(),
                            (unsigned)m_game_port,
                            m_current_players, m_max_players,
                            (unsigned)m_http_port, safeMap.c_str(),
                            m_has_password ? 1 : 0);
        if (rlen > 0) {
            struct sockaddr_in to;
            memset(&to, 0, sizeof(to));
            to.sin_family = AF_INET;
            to.sin_addr.s_addr = sender_addr;
            to.sin_port = sender_port != 0 ? sender_port : htons(DISCOVERY_PORT);
            sendto(TO_SOCK(m_socket_fd), sock_sendto_buf(reply, (size_t)rlen), 0,
                   (struct sockaddr*)&to, sizeof(to));
        }
        return;
    }

    if (std::strncmp(buf, "OZRESPONSE", 10) == 0) {
        // Client mode: record the responder.
        DiscoveredServer entry;
        uint32_t cur = 0, max = 0;
        uint16_t port = 0;
        if (parse_response(buf, entry.name, entry.version, cur, &max, &port,
                           &entry.map, &entry.http_port, &entry.password)) {
            entry.cur_players = cur;
            entry.max_players = max;
            entry.port = port;
            char ipstr[64];
            struct sockaddr_in tmp;
            memset(&tmp, 0, sizeof(tmp));
            tmp.sin_family = AF_INET;
            tmp.sin_addr.s_addr = sender_addr;
            inet_ntop(AF_INET, &tmp.sin_addr, ipstr, sizeof(ipstr));
            entry.ip = ipstr;
            entry.last_seen = now_seconds();

            for (auto& d : m_discovered) {
                if (d.ip == entry.ip && d.port == entry.port) {
                    d.last_seen = entry.last_seen;
                    d.name = entry.name;
                    d.version = entry.version;
                    d.cur_players = entry.cur_players;
                    d.max_players = entry.max_players;
                    d.map = entry.map;
                    d.http_port = entry.http_port;
                    d.password = entry.password;
                    return;
                }
            }
            m_discovered.push_back(std::move(entry));
        }
    }
}

void NetworkDiscovery::poll_discovered(std::vector<DiscoveredServer>& out) {
    out.clear();
    double now = now_seconds();
    for (auto it = m_discovered.begin(); it != m_discovered.end();) {
        if (now - it->last_seen > 9.0)
            it = m_discovered.erase(it);
        else
            ++it;
    }
    out = m_discovered;
}

void NetworkDiscovery::update() {
    if (!m_running && sock_fd_bad(m_socket_fd)) return;

    if (!m_running) {
        start();
        if (!m_running) return;
    }

    struct sockaddr_in sender;
    socklen_t sender_len = sizeof(sender);
    char buf[256];

    ssize_t received;
    while ((received = recvfrom(TO_SOCK(m_socket_fd),
                                sock_recvfrom_buf(buf, sizeof(buf)-1),
                                0,
                                (struct sockaddr*)&sender, &sender_len)) > 0) {
        if (received < 1 || (size_t)received >= sizeof(buf)) continue;
        handle_datagram(buf, (size_t)received, sender.sin_addr.s_addr, sender.sin_port);
    }

    // Server mode only: periodic announce (also acts as a probe for peers).
    if (m_game_port == 0) return;

    static double last_announce = 0;
    double now = now_seconds();
    if (now - last_announce > 3.0) {
        char announce[160];
        int len = snprintf(announce, sizeof(announce),
                           "OZDISCOVER:%s:%s:%u:%u/%u",
                           m_game_name.c_str(), m_game_version.c_str(),
                            (unsigned)m_game_port,
                           m_current_players, m_max_players);
        sendto(TO_SOCK(m_socket_fd),
               sock_sendto_buf(announce, (size_t)len), 0,
               (struct sockaddr*)&m_broadcast_address,
               sizeof(m_broadcast_address));
        last_announce = now;
    }
}

} // namespace net