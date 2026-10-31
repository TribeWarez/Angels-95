#ifndef OMEGA_MASTER_PROTOCOL_HPP
#define OMEGA_MASTER_PROTOCOL_HPP

// Master-server protocol shared by AngelMaster (daemon), the AngelServ uplink
// (MasterClient.hpp), the client browser (InternetBrowser.hpp) and the unit
// tests. Pure logic only: no sockets, raylib or engine dependencies.
//
// Wire formats
//   UDP heartbeat :  OZHEARTBEAT:gamename=angels95;gamever=..;host=..;map=..;
//                    port=27015;httpport=8080;players=3;maxplayers=16;password=0
//   HTTP list     :  GET /api/servers?gamename=angels95 -> JSON (see below)
//   HTTP heartbeat:  POST /api/heartbeat  body = the UDP heartbeat line

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
#endif

namespace master {

constexpr const char* GAMENAME          = "angels95";
constexpr const char* GAMEVER_DEFAULT   = "0.2.1";
constexpr int DEFAULT_UDP_PORT          = 27900;
constexpr int DEFAULT_HTTP_PORT         = 27950;
constexpr int HEARTBEAT_INTERVAL_S      = 30;   // server -> master cadence
constexpr int EXPIRE_SECONDS            = 90;   // 3x heartbeat interval
constexpr int MAX_SERVERS               = 4096;
constexpr const char* HEARTBEAT_PREFIX  = "OZHEARTBEAT:";
// Mirror of net::MAX_PLAYERS (the wire limit). Duplicated because this header is
// deliberately dependency-free, so it cannot include Network.hpp; a heartbeat
// advertising more than this is not a real server.
constexpr int MAX_ADVERTISED_PLAYERS    = 32;
constexpr size_t MAX_NAME_LEN           = 64;
constexpr size_t MAX_MAP_LEN            = 64;
constexpr size_t MAX_VER_LEN            = 16;

// ---------------------------------------------------------------------------
// Small string helpers
// ---------------------------------------------------------------------------
inline std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

inline bool StartsWith(const std::string& s, const char* prefix) {
    size_t n = strlen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

inline bool EndsWith(const std::string& s, const char* suffix) {
    size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Values must not contain the ';' separator or newlines. Anything else is
// replaced with a space so a hostile name can't inject extra fields.
inline std::string SanitizeValue(const std::string& v, size_t max_len) {
    std::string out;
    out.reserve(std::min(v.size(), max_len));
    for (char c : v) {
        if (c == ';' || c == '\r' || c == '\n' || c == ':') c = ' ';
        out.push_back(c);
        if (out.size() >= max_len) break;
    }
    return Trim(out);
}

// ---------------------------------------------------------------------------
// Heartbeat
// ---------------------------------------------------------------------------
struct Heartbeat {
    std::string gamename = GAMENAME;
    std::string gamever  = GAMEVER_DEFAULT;
    std::string host;                 // display name
    std::string map;
    uint16_t    port = 0;             // game UDP port
    uint16_t    http_port = 0;        // HTTP status port (0 = unknown)
    uint32_t    players = 0;
    uint32_t    maxplayers = 0;
    bool        password = false;
    uint32_t    protocol = 0;         // net::PROTOCOL_VERSION; 0 = legacy/unknown
    std::string public_ip;            // optional, for NAT
};

// A live entry as tracked by the master / handed to the client.
struct ServerEntry {
    std::string ip;                   // master-observed or heartbeat public_ip
    std::string name;
    std::string map;
    std::string gamever;
    std::string gamename = GAMENAME;
    uint16_t    port = 0;
    uint16_t    http_port = 0;
    uint32_t    players = 0;
    uint32_t    maxplayers = 0;
    bool        password = false;
    uint32_t    protocol = 0;         // 0 = a master predating the proto field
    double      last_seen = 0.0;      // monotonic seconds (master) or epoch (json)
};

inline std::string EncodeHeartbeat(const Heartbeat& hb) {
    Heartbeat h = hb;
    h.host = SanitizeValue(h.host, MAX_NAME_LEN);
    h.map  = SanitizeValue(h.map, MAX_MAP_LEN);
    h.gamever = SanitizeValue(h.gamever, MAX_VER_LEN);
    char buf[512];
    snprintf(buf, sizeof(buf),
             "%sgamename=%s;gamever=%s;host=%s;map=%s;port=%u;httpport=%u;"
             "players=%u;maxplayers=%u;password=%d",
             HEARTBEAT_PREFIX,
             h.gamename.c_str(), h.gamever.c_str(), h.host.c_str(), h.map.c_str(),
             (unsigned)h.port, (unsigned)h.http_port,
             (unsigned)h.players, (unsigned)h.maxplayers, h.password ? 1 : 0);
    std::string line(buf);
    // Optional tail: an old master's ParseKV ignores unknown keys, so adding
    // this does not need a protocol bump. `proto` is the wire-compatibility
    // id (net::PROTOCOL_VERSION), distinct from the human build id in gamever.
    if (h.protocol > 0) line += ";proto=" + std::to_string((unsigned)h.protocol);
    if (!h.public_ip.empty()) {
        std::string ip;
        for (char c : h.public_ip) {
            if (isalnum((unsigned char)c) || c == '.' || c == ':') ip.push_back(c);
        }
        if (!ip.empty()) line += ";publicip=" + ip;
    }
    return line;
}

inline bool ParseKV(const std::string& data, const char* key, std::string& out) {
    size_t klen = strlen(key);
    size_t pos = 0;
    while (pos < data.size()) {
        size_t end = data.find_first_of(";\n", pos);
        if (end == std::string::npos) end = data.size();
        size_t eq = data.find('=', pos);
        if (eq != std::string::npos && eq < end) {
            std::string k = Trim(data.substr(pos, eq - pos));
            if (k == key) { out = Trim(data.substr(eq + 1, end - eq - 1)); return true; }
        }
        pos = end + 1;
    }
    return false;
}

// Accepts "OZHEARTBEAT:key=val;..." and bare "key=val;..." bodies.
inline bool DecodeHeartbeat(const std::string& data, Heartbeat& out) {
    std::string body = Trim(data);
    if (StartsWith(body, HEARTBEAT_PREFIX)) body = body.substr(strlen(HEARTBEAT_PREFIX));

    std::string v;
    if (!ParseKV(body, "gamename", v) || v != GAMENAME) return false;

    out.gamename = GAMENAME;
    if (ParseKV(body, "gamever", v)) out.gamever = SanitizeValue(v, MAX_VER_LEN);
    if (ParseKV(body, "host", v))    out.host = SanitizeValue(v, MAX_NAME_LEN);
    if (ParseKV(body, "map", v))     out.map = SanitizeValue(v, MAX_MAP_LEN);

    unsigned long n = 0;
    if (ParseKV(body, "port", v)) {
        n = strtoul(v.c_str(), nullptr, 10);
        if (n == 0 || n > 65535) return false;
        out.port = (uint16_t)n;
    } else {
        return false; // game port is mandatory
    }
    if (ParseKV(body, "httpport", v)) {
        n = strtoul(v.c_str(), nullptr, 10);
        out.http_port = (n > 65535) ? 0 : (uint16_t)n;
    }
    // Clamped: a heartbeat is unauthenticated text from any host that can reach
    // the master. `players=4294967295` used to be stored verbatim, shown as
    // nonsense in the browser and summed into /api/stats (a uint32 total that
    // wraps). A real server never advertises more than the wire limit.
    if (ParseKV(body, "players", v)) {
        n = strtoul(v.c_str(), nullptr, 10);
        out.players = (n > (unsigned long)MAX_ADVERTISED_PLAYERS)
                          ? (uint32_t)MAX_ADVERTISED_PLAYERS : (uint32_t)n;
    }
    if (ParseKV(body, "maxplayers", v)) {
        n = strtoul(v.c_str(), nullptr, 10);
        out.maxplayers = (n > (unsigned long)MAX_ADVERTISED_PLAYERS)
                             ? (uint32_t)MAX_ADVERTISED_PLAYERS : (uint32_t)n;
    }
    if (ParseKV(body, "password", v))   out.password = (v == "1" || v == "true");
    if (ParseKV(body, "proto", v)) {
        unsigned long p = strtoul(v.c_str(), nullptr, 10);
        if (p <= 0xFFFFFFFFul) out.protocol = (uint32_t)p;
    }
    if (ParseKV(body, "publicip", v)) {
        if (v.find_first_not_of("0123456789.abcdefABCDEF:") == std::string::npos)
            out.public_ip = v;
    }
    if (out.maxplayers == 0) out.maxplayers = 32;
    // A full server cannot have more players than slots; without this the
    // browser can show "40 / 8" and the aggregate counters disagree.
    if (out.players > out.maxplayers) out.players = out.maxplayers;
    return true;
}

// ---------------------------------------------------------------------------
// JSON helpers (tolerant scanner; master output is ours, but we never trust
// inputs blindly)
// ---------------------------------------------------------------------------
inline std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out.push_back(c);
        }
    }
    return out;
}

// Returns pointer just past the ':' for `"key"`, or nullptr.
inline const char* JsonFindKey(const std::string& obj, const char* key) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = obj.find(k);
    if (p == std::string::npos) return nullptr;
    p += k.size();
    while (p < obj.size() && (obj[p] == ' ' || obj[p] == '\t' || obj[p] == ':')) ++p;
    return obj.c_str() + p;
}

inline bool JsonGetString(const std::string& obj, const char* key, std::string& out) {
    const char* p = JsonFindKey(obj, key);
    if (!p || *p != '"') return false;
    ++p;
    std::string s;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            ++p;
            switch (*p) {
                case 'n': s.push_back('\n'); break;
                case 'r': s.push_back('\r'); break;
                case 't': s.push_back('\t'); break;
                default:  s.push_back(*p);
            }
        } else {
            s.push_back(*p);
        }
        ++p;
    }
    out = s;
    return true;
}

inline bool JsonGetUInt(const std::string& obj, const char* key, uint32_t& out) {
    const char* p = JsonFindKey(obj, key);
    if (!p) return false;
    if (*p == '"') ++p;
    if (*p < '0' || *p > '9') return false;
    uint32_t v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (uint32_t)(*p - '0'); ++p; }
    out = v;
    return true;
}

inline bool JsonGetBool(const std::string& obj, const char* key, bool& out) {
    const char* p = JsonFindKey(obj, key);
    if (!p) return false;
    if (strncmp(p, "true", 4) == 0 || *p == '1') { out = true; return true; }
    if (strncmp(p, "false", 5) == 0 || *p == '0') { out = false; return true; }
    return false;
}

// Split out the objects inside the "servers": [ ... ] array. Brace/string aware.
inline std::vector<std::string> JsonExtractObjects(const std::string& json) {
    std::vector<std::string> out;
    size_t arr = json.find("\"servers\"");
    if (arr == std::string::npos) return out;
    size_t lb = json.find('[', arr);
    if (lb == std::string::npos) return out;

    int depth = 0;
    bool in_str = false;
    size_t obj_start = std::string::npos;
    for (size_t i = lb + 1; i < json.size(); ++i) {
        char c = json[i];
        if (in_str) {
            if (c == '\\') { ++i; continue; }
            if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') { in_str = true; continue; }
        if (c == '{') {
            if (depth == 0) obj_start = i;
            ++depth;
        } else if (c == '}') {
            if (depth > 0) {
                --depth;
                if (depth == 0 && obj_start != std::string::npos) {
                    out.push_back(json.substr(obj_start, i - obj_start + 1));
                    obj_start = std::string::npos;
                }
            }
        } else if (c == ']' && depth == 0) {
            break;
        }
    }
    return out;
}

inline std::string BuildServerListJson(const std::vector<ServerEntry>& servers,
                                       const std::string& gamename) {
    std::string json = "{\"ok\":true,\"gamename\":\"";
    json += JsonEscape(gamename) + "\",\"count\":" + std::to_string(servers.size()) + ",\"servers\":[";
    bool first = true;
    for (const auto& s : servers) {
        if (!first) json += ',';
        first = false;
        json += "{\"ip\":\"" + JsonEscape(s.ip) + "\"";
        json += ",\"port\":" + std::to_string(s.port);
        json += ",\"httpport\":" + std::to_string(s.http_port);
        json += ",\"name\":\"" + JsonEscape(s.name) + "\"";
        json += ",\"map\":\"" + JsonEscape(s.map) + "\"";
        json += ",\"gamever\":\"" + JsonEscape(s.gamever) + "\"";
        json += ",\"proto\":" + std::to_string(s.protocol);
        json += ",\"players\":" + std::to_string(s.players);
        json += ",\"maxplayers\":" + std::to_string(s.maxplayers);
        json += std::string(",\"password\":") + (s.password ? "true" : "false");
        json += ",\"lastseen\":" + std::to_string((long long)s.last_seen);
        json += "}";
    }
    json += "]}";
    return json;
}

// True when a tracked server has not heartbeated within EXPIRE_SECONDS.
inline bool IsExpired(const ServerEntry& e, long long now_epoch) {
    return (now_epoch - (long long)e.last_seen) > EXPIRE_SECONDS;
}

inline bool ParseServerListJson(const std::string& json,
                                std::vector<ServerEntry>& out) {
    out.clear();
    for (const auto& obj : JsonExtractObjects(json)) {
        ServerEntry e;
        if (!JsonGetString(obj, "ip", e.ip) || e.ip.empty()) continue;
        uint32_t n = 0;
        if (!JsonGetUInt(obj, "port", n) || n == 0 || n > 65535) continue;
        e.port = (uint16_t)n;
        if (JsonGetUInt(obj, "httpport", n)) e.http_port = (n > 65535) ? 0 : (uint16_t)n;
        if (JsonGetUInt(obj, "players", n)) e.players = n;
        if (JsonGetUInt(obj, "maxplayers", n)) e.maxplayers = n;
        JsonGetString(obj, "name", e.name);
        JsonGetString(obj, "map", e.map);
        JsonGetString(obj, "gamever", e.gamever);
        if (JsonGetUInt(obj, "proto", n)) e.protocol = n;
        JsonGetBool(obj, "password", e.password);
        out.push_back(std::move(e));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Announced-address policy and heartbeat rate limiting.
//
// These live here (rather than in Master.cpp) so tests/Master.test.cpp can pin
// them without opening a socket. Pure data in, pure data out.
// ---------------------------------------------------------------------------
inline bool IsValidIpv4(const std::string& ip) {
    // Deliberately NOT inet_pton: MasterProtocol.hpp is the one header the
    // socket-free test_master target includes, and it does not link ws2_32.
    unsigned a = 0, b = 0, c = 0, d = 0;
    char extra = 0;
    if (sscanf(ip.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4)
        return false;
    return a <= 255 && b <= 255 && c <= 255 && d <= 255;
}

inline bool IsPrivateIpv4(const std::string& ip) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    if (a == 10) return true;                        // 10.0.0.0/8
    if (a == 172 && b >= 16 && b <= 31) return true; // 172.16.0.0/12
    if (a == 192 && b == 168) return true;           // 192.168.0.0/16
    if (a == 127) return true;                       // loopback
    if (a == 169 && b == 254) return true;           // link-local
    return false;
}

// Decide which address the master lists for a heartbeat.
//
// A self-reported `public_ip` is trusted ONLY from a private source (a server
// on the master's own LAN, which is the one case the master cannot observe).
// A public source IS already the address that must be listed, so trusting a
// claimed value there lets any internet host announce an arbitrary ip:port and
// poison the list. The old rule trusted exactly the public case and ignored
// the private one - backwards for both NAT and spoofing.
inline std::string ResolveAnnouncedIp(const std::string& source_ip,
                                      const std::string& claimed_public_ip) {
    if (claimed_public_ip.empty() || !IsValidIpv4(claimed_public_ip)) return source_ip;
    if (IsPrivateIpv4(source_ip)) return claimed_public_ip;
    return source_ip;
}

// Heartbeat admission control. The key is the announced server identity
// (`ip:port`), NOT the source address: several dedicated servers legitimately
// share one public IP (host networking / NAT), and the old per-source-only
// limit silently dropped all but one of them every cycle.
//
// A coarse per-source cap (default 10/s) still bounds a hostile host rotating
// ports or claimed addresses.
class HeartbeatLimiter {
public:
    bool Accept(const std::string& server_key, const std::string& source_ip,
                long long now_ms,
                long long per_server_ms = 1500,
                long long per_source_ms = 100) {
        auto sit = m_by_source.find(source_ip);
        if (sit != m_by_source.end() && now_ms - sit->second < per_source_ms)
            return false;
        auto it = m_by_server.find(server_key);
        if (it != m_by_server.end() && now_ms - it->second < per_server_ms)
            return false;
        m_by_server[server_key] = now_ms;
        m_by_source[source_ip] = now_ms;
        return true;
    }

    // Drop stale keys so a stream of spoofed sources cannot grow the maps
    // without bound. Called from the master's periodic prune.
    void Prune(long long now_ms, long long keep_ms = 60000) {
        for (auto it = m_by_server.begin(); it != m_by_server.end();) {
            if (now_ms - it->second > keep_ms) it = m_by_server.erase(it);
            else ++it;
        }
        for (auto it = m_by_source.begin(); it != m_by_source.end();) {
            if (now_ms - it->second > keep_ms) it = m_by_source.erase(it);
            else ++it;
        }
    }

    size_t server_keys() const { return m_by_server.size(); }
    size_t source_keys() const { return m_by_source.size(); }

private:
    std::unordered_map<std::string, long long> m_by_server;
    std::unordered_map<std::string, long long> m_by_source;
};

// ---------------------------------------------------------------------------
// ResolveBindAddress — accepts a literal IPv4 ("0.0.0.0", "1.2.3.4") or a
// hostname (e.g. a VPS/DuckDNS FQDN) and returns a literal IPv4 string.
// Returns "" when the name cannot be resolved.
// ---------------------------------------------------------------------------
inline std::string ResolveBindAddress(const std::string& bind) {
    if (bind.empty()) return "";
    struct in_addr a4;
    if (inet_pton(AF_INET, bind.c_str(), &a4) == 1) return bind;
    struct addrinfo hints, *res = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(bind.c_str(), nullptr, &hints, &res) == 0 && res) {
        char buf[64] = {0};
        struct sockaddr_in* sin = (struct sockaddr_in*)res->ai_addr;
        inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
        freeaddrinfo(res);
        return buf;
    }
    return "";
}

} // namespace master

#endif // OMEGA_MASTER_PROTOCOL_HPP