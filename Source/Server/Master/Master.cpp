// AngelMaster ÔÇö standalone master server for Angels95 / OmegaTech.
//
// Collects heartbeats from public game servers and serves the live list to
// clients (HTTP/JSON) ÔÇö a lightweight GameSpy/Unreal-style master.
//
//   UDP  heartbeat : OZHEARTBEAT:gamename=angels95;...
//   HTTP GET       : /api/servers?gamename=angels95
//   HTTP POST      : /api/heartbeat   (same payload as UDP)
//
// Raw sockets only, no raylib. Usage:
//   AngelMaster [--port 27900] [--http-port 27950] [--max-servers 4096]

#include "../../Network/MasterProtocol.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
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
    typedef int socklen_t;
    #define MSOCK(fd) ((SOCKET)(intptr_t)(fd))
    #define MCLOSE(fd) closesocket(MSOCK(fd))
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    #define MSOCK(fd) (fd)
    #define MCLOSE(fd) ::close(fd)
#endif

// Log.hpp after the platform headers: on Windows, <windows.h> defines an
// ERROR macro that would otherwise mangle LogLevel::ERROR.
#include "../../Log.hpp"
#include "../../Version.hpp"

using namespace master;

static std::atomic<bool> g_running{true};
static std::mutex g_state_mutex;
static std::mutex g_print_mutex;
static std::unordered_map<std::string, ServerEntry> g_servers;   // key: ip:port
static HeartbeatLimiter g_limiter;   // keyed by ip:port, coarse cap by source
static int g_max_servers = MAX_SERVERS;
static std::string g_gamename = GAMENAME;
// Bind address for the UDP heartbeat + HTTP API. Default loopback; a VPS build
// must pass --bind 0.0.0.0 to accept heartbeats/queries from the internet.
static std::string g_bind_ip = "127.0.0.1";

static long long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
static long long now_epoch() { return (long long)time(nullptr); }

static void log_line(LogLevel level, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Log::write(level, __FILE__, __LINE__, "%s", buf);
}

static void signal_handler(int) { g_running = false; }

static bool init_winsock() {
#ifdef _WIN32
    static bool done = false;
    if (!done) {
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d) != 0) return false;
        done = true;
    }
#endif
    return true;
}

// LAN/private-range policy lives in MasterProtocol.hpp (ResolveAnnouncedIp)
// so the wire-level rule is covered by tests/Master.test.cpp.

static std::string resolve_bind_address(const std::string& bind_ip) {
    return ResolveBindAddress(bind_ip);
}

// ---------------------------------------------------------------------------
// Registration (shared by UDP + HTTP heartbeat paths)
// ---------------------------------------------------------------------------
static std::string heartbeat_key(const std::string& ip, uint16_t port) {
    return ip + ":" + std::to_string(port);
}

// Returns false when rate-limited, invalid or over capacity.
static bool apply_heartbeat(const Heartbeat& hb, const std::string& source_ip,
                            bool from_http) {
    if (hb.port == 0) return false;

    // A claimed public_ip is only trusted from a private (LAN) source; a public
    // source already IS the address that must be listed. See
    // master::ResolveAnnouncedIp for the full rule (and tests/Master.test.cpp).
    const std::string ip = ResolveAnnouncedIp(source_ip, hb.public_ip);
    if (ip.empty()) return false;

    const std::string key = heartbeat_key(ip, hb.port);
    const long long now = now_ms();
    bool is_new = false, accepted = false, full = false, limited = false;
    ServerEntry snapshot;

    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        auto it = g_servers.find(key);
        is_new = (it == g_servers.end());
        if (is_new && (int)g_servers.size() >= g_max_servers) {
            full = true;
        } else if (!g_limiter.Accept(key, source_ip, now)) {
            // Per-(ip,port) limit (1.5 s) with a coarse per-source cap. This
            // is what lets several servers behind one public IP all stay
            // listed: the old limiter keyed only on the source address.
            limited = true;
        } else {
            ServerEntry& e = g_servers[key];
            e.ip = ip;
            e.port = hb.port;
            e.http_port = hb.http_port;
            e.name = hb.host.empty() ? ("Angels95 Server " + ip) : hb.host;
            e.map = hb.map;
            e.gamever = hb.gamever.empty() ? GAMEVER_DEFAULT : hb.gamever;
            e.gamename = hb.gamename;
            e.protocol = hb.protocol;
            e.players = hb.players;
            e.maxplayers = hb.maxplayers ? hb.maxplayers : 32;
            e.password = hb.password;
            e.last_seen = (double)now_epoch();
            snapshot = e;
            accepted = true;
        }
    }

    if (full) {
        log_line(LogLevel::WARN, "Master: rejected %s:%u from %s (server cap %d)",
                 ip.c_str(), (unsigned)hb.port, source_ip.c_str(), g_max_servers);
    } else if (limited) {
        // Visible on purpose: the old code returned false silently, which is
        // why a permanently-starved server looked like it was never announcing.
        log_line(LogLevel::INFO, "Master: rate-limited %s:%u from %s%s",
                 ip.c_str(), (unsigned)hb.port, source_ip.c_str(),
                 from_http ? " [http]" : "");
    } else if (accepted) {
        log_line(LogLevel::INFO,
                 "Master: %s heartbeat %s:%u (%u/%u, map=%s, %s, proto=%u)%s",
                 is_new ? "new" : "update", snapshot.ip.c_str(),
                 (unsigned)snapshot.port, snapshot.players, snapshot.maxplayers,
                 snapshot.map.c_str(), snapshot.gamever.c_str(),
                 (unsigned)snapshot.protocol, from_http ? " [http]" : "");
    }
    return accepted;
}

static void prune_expired_locked() {
    long long now = now_epoch();
    for (auto it = g_servers.begin(); it != g_servers.end();) {
        if (IsExpired(it->second, now)) {
            log_line(LogLevel::INFO, "Master: expired %s (no heartbeat for %ds)",
                     it->first.c_str(), EXPIRE_SECONDS);
            it = g_servers.erase(it);
        } else {
            ++it;
        }
    }
    // The limiter maps are keyed by announced ip:port and by source address;
    // without this they grow forever under a spoofed-source flood.
    g_limiter.Prune(now_ms());
}

static std::vector<ServerEntry> snapshot_servers(const std::string& gamename) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    prune_expired_locked();
    std::vector<ServerEntry> out;
    for (auto& kv : g_servers) {
        if (!gamename.empty() && kv.second.gamename != gamename) continue;
        out.push_back(kv.second);
    }
    std::sort(out.begin(), out.end(), [](const ServerEntry& a, const ServerEntry& b) {
        if (a.players != b.players) return a.players > b.players;
        return a.name < b.name;
    });
    return out;
}

// ---------------------------------------------------------------------------
// HTTP server
// ---------------------------------------------------------------------------
static int http_listen(int port, const std::string& bind_ip) {
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int yes = 1;
    setsockopt(MSOCK(fd), SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    // --bind applies to BOTH listeners (the help/docs always said so). This
    // used to be INADDR_ANY unconditionally, so --bind 127.0.0.1 left the API
    // exposed on every interface.
    if (bind_ip.empty() || bind_ip == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (inet_pton(AF_INET, bind_ip.c_str(), &addr.sin_addr) != 1) {
        MCLOSE(fd);
        return -1;
    }
    addr.sin_port = htons((u_short)port);
    if (bind(MSOCK(fd), (struct sockaddr*)&addr, sizeof(addr)) < 0) { MCLOSE(fd); return -1; }
    if (listen(MSOCK(fd), 16) < 0) { MCLOSE(fd); return -1; }
    return fd;
}

static void write_all(int fd, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        int n = (int)send(MSOCK(fd), data + off, (int)(len - off), 0);
        if (n <= 0) break;
        off += (size_t)n;
    }
}

static void http_response(int fd, int code, const char* status,
                          const std::string& body, const char* type) {
    char hdr[512];
    int hl = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n",
        code, status, type, body.size());
    write_all(fd, hdr, (size_t)hl);
    write_all(fd, body.c_str(), body.size());
}

static std::string get_query_param(const char* path, const char* key) {
    const char* q = strchr(path, '?');
    if (!q) return "";
    ++q;
    size_t klen = strlen(key);
    while (*q) {
        if (strncmp(q, key, klen) == 0 && q[klen] == '=') {
            q += klen + 1;
            std::string out;
            while (*q && *q != '&') out.push_back(*q++);
            return out;
        }
        while (*q && *q != '&') ++q;
        if (*q == '&') ++q;
    }
    return "";
}

// Content-Length from the header block, case-insensitive, 0 when absent.
static size_t header_content_length(const std::string& req, size_t header_end) {
    static const char kKey[] = "content-length:";
    const size_t kLen = sizeof(kKey) - 1;
    size_t pos = 0;
    while (pos < header_end) {
        size_t eol = req.find('\n', pos);
        if (eol == std::string::npos || eol > header_end) eol = header_end;
        std::string line = req.substr(pos, eol - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.size() >= kLen) {
            bool match = true;
            for (size_t i = 0; i < kLen; ++i) {
                if (std::tolower((unsigned char)line[i]) != kKey[i]) { match = false; break; }
            }
            if (match) {
                const char* p = line.c_str() + kLen;
                while (*p == ' ' || *p == '\t') ++p;
                return (size_t)strtoul(p, nullptr, 10);
            }
        }
        pos = eol + 1;
    }
    return 0;
}

static void handle_http_client(int cfd, const std::string& client_ip) {
    // Read the WHOLE request, not just the first segment. A POST body split
    // across TCP segments used to be truncated, which silently dropped the
    // optional tail (publicip/proto) of an HTTP heartbeat.
    std::string req;
    char buf[4096];
    size_t header_end = std::string::npos;
    size_t want = 0;
    while (req.size() < 8192) {
        int n = (int)recv(MSOCK(cfd), buf, sizeof(buf), 0);
        if (n <= 0) break;
        req.append(buf, (size_t)n);
        if (header_end == std::string::npos) {
            header_end = req.find("\r\n\r\n");
            size_t skip = 4;
            if (header_end == std::string::npos) {
                header_end = req.find("\n\n");
                skip = 2;
            }
            if (header_end != std::string::npos)
                want = header_end + skip + header_content_length(req, header_end);
        }
        if (header_end != std::string::npos && req.size() >= want) break;
    }
    if (req.empty()) { MCLOSE(cfd); return; }

    char method[16] = {0}, path[1024] = {0};
    sscanf(req.c_str(), "%15s %1023s", method, path);

    if (strcmp(method, "GET") == 0 &&
        (strcmp(path, "/api/servers") == 0 || strncmp(path, "/api/servers?", 12) == 0)) {
        std::string gn = get_query_param(path, "gamename");
        if (gn.empty()) gn = g_gamename;
        auto servers = snapshot_servers(gn);
        std::string json = BuildServerListJson(servers, gn);
        http_response(cfd, 200, "OK", json, "application/json");
        MCLOSE(cfd);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/stats") == 0) {
        auto servers = snapshot_servers(g_gamename);
        uint32_t players = 0;
        for (auto& s : servers) players += s.players;
        std::string json = "{\"ok\":true,\"gamename\":\"" + std::string(g_gamename) +
                           "\",\"servers\":" + std::to_string(servers.size()) +
                           ",\"players\":" + std::to_string(players) + "}";
        http_response(cfd, 200, "OK", json, "application/json");
        MCLOSE(cfd);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/heartbeat") == 0) {
        // Body is the same key=value payload as the UDP heartbeat.
        std::string body;
        if (header_end != std::string::npos) {
            size_t skip = (req.compare(header_end, 4, "\r\n\r\n") == 0) ? 4 : 2;
            size_t body_pos = header_end + skip;
            size_t end = (want > body_pos) ? want : req.size();
            if (end > req.size()) end = req.size();
            if (body_pos < end) body = req.substr(body_pos, end - body_pos);
        }
        Heartbeat hb;
        if (DecodeHeartbeat(body, hb) && apply_heartbeat(hb, client_ip, true)) {
            http_response(cfd, 200, "OK", "{\"ok\":true}", "application/json");
        } else {
            http_response(cfd, 400, "Bad Request",
                          "{\"ok\":false,\"error\":\"invalid_or_rate_limited\"}",
                          "application/json");
        }
        MCLOSE(cfd);
        return;
    }

    http_response(cfd, 404, "Not Found", "{\"ok\":false,\"error\":\"not found\"}",
                  "application/json");
    MCLOSE(cfd);
}

static void http_server_thread(int port, std::string bind_ip) {
    int lfd = http_listen(port, bind_ip);
    if (lfd < 0) {
        log_line(LogLevel::ERROR, "Master: HTTP listen failed on %s:%d",
                 bind_ip.empty() ? "0.0.0.0" : bind_ip.c_str(), port);
        return;
    }
    log_line(LogLevel::INFO, "Master: HTTP server on port %d (/api/servers, /api/heartbeat)", port);

    fd_set master_fds;
    FD_ZERO(&master_fds);
    FD_SET(MSOCK(lfd), &master_fds);
    while (g_running) {
        fd_set read_fds = master_fds;
        struct timeval tv = {1, 0};
        if (select(lfd + 1, &read_fds, nullptr, nullptr, &tv) < 0) {
            if (g_running) break;
            continue;
        }
        if (!FD_ISSET(MSOCK(lfd), &read_fds)) continue;
        struct sockaddr_in addr;
        socklen_t alen = sizeof(addr);
        int cfd = (int)accept(MSOCK(lfd), (struct sockaddr*)&addr, &alen);
        if (cfd < 0) continue;
        char ip[64] = {0};
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        handle_http_client(cfd, ip);
    }
    MCLOSE(lfd);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    int udp_port = DEFAULT_UDP_PORT;
    int http_port = DEFAULT_HTTP_PORT;

    for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)           udp_port = atoi(argv[++i]);
    else if (strcmp(argv[i], "--http-port") == 0 && i + 1 < argc) http_port = atoi(argv[++i]);
    else if (strcmp(argv[i], "--max-servers") == 0 && i + 1 < argc) g_max_servers = atoi(argv[++i]);
    else if (strcmp(argv[i], "--gamename") == 0 && i + 1 < argc) g_gamename = argv[++i];
    else if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc) g_bind_ip = argv[++i];
    else if (strcmp(argv[i], "--version") == 0) {
        printf("AngelMaster %s\n", oz::kBuildVersion);
        return 0;
    }
    else if (strcmp(argv[i], "--help") == 0) {
        printf("AngelMaster %s -- Angels95 master server\n", oz::kBuildVersion);
        printf("Usage: AngelMaster [--port P] [--http-port P] [--max-servers N]"
               " [--gamename NAME] [--bind IP|HOST|0.0.0.0]\n");
        printf("  --bind  Interface to bind UDP+HTTP to (default 127.0.0.1).\n");
        printf("          Use 0.0.0.0 on a VPS to accept remote heartbeats/queries.\n");
        return 0;
    }
    }
    if (g_max_servers <= 0) g_max_servers = MAX_SERVERS;

    // Resolve --bind (literal IP or hostname such as a DuckDNS/VPS FQDN)
    std::string resolved_bind;
    if (!g_bind_ip.empty() && g_bind_ip != "0.0.0.0") {
        resolved_bind = resolve_bind_address(g_bind_ip);
        if (resolved_bind.empty()) {
            fprintf(stderr, "AngelMaster: cannot resolve --bind '%s' (falling back to 0.0.0.0)\n",
                    g_bind_ip.c_str());
        }
    }

    if (!init_winsock()) { fprintf(stderr, "AngelMaster: winsock init failed\n"); return 1; }
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // UDP heartbeat socket (non-blocking).
    int ufd = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (ufd < 0) { fprintf(stderr, "AngelMaster: UDP socket failed\n"); return 1; }
    {
        int reuse = 1;
        setsockopt(MSOCK(ufd), SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
#ifdef _WIN32
        u_long nb = 1;
        ioctlsocket(MSOCK(ufd), FIONBIO, &nb);
#else
        int flags = fcntl(ufd, F_GETFL, 0);
        fcntl(ufd, F_SETFL, flags | O_NONBLOCK);
#endif
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        if (resolved_bind.empty())
            addr.sin_addr.s_addr = INADDR_ANY;             // 0.0.0.0
        else
            inet_pton(AF_INET, resolved_bind.c_str(), &addr.sin_addr);
        addr.sin_port = htons((u_short)udp_port);
        if (bind(MSOCK(ufd), (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            fprintf(stderr, "AngelMaster: UDP bind failed on %d\n", udp_port);
            return 1;
        }
    }

    log_line(LogLevel::INFO, "AngelMaster starting (gamename=%s)", g_gamename.c_str());
    printf("Master UDP:  %d\n", udp_port);
    printf("Master HTTP: %d\n", http_port);

    std::thread http_thread(http_server_thread, http_port, resolved_bind);

    long long last_prune = 0;
    while (g_running) {
        struct sockaddr_in sender;
        socklen_t slen = sizeof(sender);
        char buf[1024];
        int n;
        while ((n = (int)recvfrom(MSOCK(ufd), buf, sizeof(buf) - 1, 0,
                                  (struct sockaddr*)&sender, &slen)) > 0) {
            buf[n] = '\0';
            Heartbeat hb;
            if (!DecodeHeartbeat(buf, hb)) continue;
            char ip[64] = {0};
            inet_ntop(AF_INET, &sender.sin_addr, ip, sizeof(ip));
            if (apply_heartbeat(hb, ip, false)) {
                const char* ack = "OZMASTEROK";
                sendto(MSOCK(ufd), ack, (int)strlen(ack), 0,
                       (struct sockaddr*)&sender, slen);
            }
        }
        long long now = now_ms();
        if (now - last_prune > 5000) {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            prune_expired_locked();
            last_prune = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    printf("Shutting down master...\n");
    MCLOSE(ufd);
    if (http_thread.joinable()) http_thread.join();
    return 0;
}
