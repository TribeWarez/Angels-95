// AngelServ - HTTP map API (extracted from Server.cpp).
#include "ServerInternal.hpp"
#include "../Network/MasterProtocol.hpp"
#include "../Version.hpp"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define TO_SOCK(fd) (SOCKET)(fd)
#define close_sock(fd) closesocket(fd)
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define TO_SOCK(fd) (fd)
#define close_sock(fd) ::close(fd)
#endif
// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char ch : s) {
        switch (ch) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\t': out += "\\t";  break;
            default:   out += ch;
        }
    }
    out += '"';
    return out;
}


void append_ozone_json(std::string& out, const OzonePrimitive& p, bool first) {
    if (!first) out += ',';
    out += R"({"type":)";

    auto esc = [](const std::string& s) { return json_escape(s); };

    switch (p.type) {
        case OzonePrimitiveType::BOX: {
            out += esc("box");
            if (p.args.size() >= 7) {
                char buf[256];
                snprintf(buf, sizeof(buf),
                    R"(,"center":[%.3f,%.3f,%.3f],"size":[%.3f,%.3f,%.3f],"rot":%.3f)",
                    p.args[0], p.args[1], p.args[2],
                    p.args[3], p.args[4], p.args[5],
                    p.args[6]);
                out += buf;
            }
            break;
        }
        case OzonePrimitiveType::CYLINDER: {
            out += esc("cyl");
            if (p.args.size() >= 8) {
                char buf[256];
                snprintf(buf, sizeof(buf),
                    R"(,"center":[%.3f,%.3f,%.3f],"rt":%.3f,"rb":%.3f,"h":%.3f,"slices":%d,"rot":%.3f)",
                    p.args[0], p.args[1], p.args[2],
                    p.args[3], p.args[4],
                    p.args[5],
                    static_cast<int>(p.args[6]),
                    p.args[7]);
                out += buf;
            }
            break;
        }
        case OzonePrimitiveType::SPHERE: {
            out += esc("sph");
            if (p.args.size() >= 4) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                    R"(,"center":[%.3f,%.3f,%.3f],"radius":%.3f,"segments":%d)",
                    p.args[0], p.args[1], p.args[2],
                    p.args[3],
                    p.args.size() >= 5 ? static_cast<int>(p.args[4]) : 16);
                out += buf;
            }
            break;
        }
        case OzonePrimitiveType::PYRAMID: {
            out += esc("pyr");
            if (p.args.size() >= 6) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                    R"(,"center":[%.3f,%.3f,%.3f],"w":%.3f,"d":%.3f,"h":%.3f)",
                    p.args[0], p.args[1], p.args[2],
                    p.args[3], p.args[4], p.args[5]);
                out += buf;
            }
            break;
        }
        case OzonePrimitiveType::PLANE: {
            out += esc("pln");
            if (p.args.size() >= 7) {
                char buf[256];
                snprintf(buf, sizeof(buf),
                    R"(,"center":[%.3f,%.3f,%.3f],"normal":[%.3f,%.3f,%.3f],"dist":%.3f)",
                    p.args[0], p.args[1], p.args[2],
                    p.args[3], p.args[4], p.args[5],
                    p.args[6]);
                out += buf;
            }
            break;
        }
        case OzonePrimitiveType::ENTITY_PLAYERSTART: {
            out += esc("playerstart");
            if (p.args.size() >= 4) {
                char buf[160];
                snprintf(buf, sizeof(buf), R"(,"position":[%.3f,%.3f,%.3f],"yaw":%.3f)",
                         p.args[0], p.args[1], p.args[2], p.args[3]);
                out += buf;
            }
            break;
        }
        case OzonePrimitiveType::ENTITY_PICKUP:
        case OzonePrimitiveType::ENTITY_NPC: {
            out += esc(p.type == OzonePrimitiveType::ENTITY_PICKUP ? "pickup" : "npc");
            out += R"(,"subtype":)" + esc(p.entityType);
            if (p.args.size() >= 3) {
                char buf[128];
                snprintf(buf, sizeof(buf), R"(,"position":[%.3f,%.3f,%.3f])",
                         p.args[0], p.args[1], p.args[2]);
                out += buf;
            }
            if (p.type == OzonePrimitiveType::ENTITY_PICKUP && p.args.size() >= 4) {
                char buf[64];
                snprintf(buf, sizeof(buf), R"(,"respawnTime":%.3f)", p.args[3]);
                out += buf;
            }
            break;
        }
        case OzonePrimitiveType::ENTITY_ZONE: {
            out += esc("zone");
            out += R"(,"subtype":)" + esc(p.entitySubType);
            if (p.args.size() >= 6) {
                char buf[256];
                snprintf(buf, sizeof(buf),
                         R"(,"min":[%.3f,%.3f,%.3f],"max":[%.3f,%.3f,%.3f])",
                         p.args[0], p.args[1], p.args[2], p.args[3], p.args[4], p.args[5]);
                out += buf;
            }
            if (p.args.size() >= 7) {
                char buf[64];
                snprintf(buf, sizeof(buf), R"(,"intensity":%.3f)", p.args[6]);
                out += buf;
            }
            break;
        }
        case OzonePrimitiveType::ENTITY_PORTAL: {
            out += esc("portal");
            out += R"(,"targetWorld":)" + esc(p.entityType);
            if (p.args.size() >= 6) {
                char buf[256];
                snprintf(buf, sizeof(buf),
                         R"(,"min":[%.3f,%.3f,%.3f],"max":[%.3f,%.3f,%.3f])",
                         p.args[0], p.args[1], p.args[2], p.args[3], p.args[4], p.args[5]);
                out += buf;
            }
            if (p.args.size() >= 9) {
                char buf[128];
                snprintf(buf, sizeof(buf), R"(,"spawn":[%.3f,%.3f,%.3f])",
                         p.args[6], p.args[7], p.args[8]);
                out += buf;
            }
            if (p.args.size() >= 10)
                out += p.args[9] != 0.0f ? R"(,"bidirectional":true)" : R"(,"bidirectional":false)";
            break;
        }
        case OzonePrimitiveType::ENTITY_LEVELINFO: {
            out += esc("level_info");
            if (p.args.size() >= 7) {
                char buf[256];
                snprintf(buf, sizeof(buf),
                         R"(,"gameType":%d,"maxPlayers":%d,"respawnTime":%.3f)"
                         R"(,"timeLimitEnabled":%d,"timeLimitMinutes":%.3f,"scoreLimit":%d,"friendlyFire":%d)",
                         (int)p.args[0], (int)p.args[1], p.args[2],
                         p.args[3] != 0.0f ? 1 : 0, p.args[4], (int)p.args[5],
                         p.args[6] != 0.0f ? 1 : 0);
                out += buf;
            }
            if (!p.gametypeKey.empty())
                out += R"(,"gametype":)" + esc(p.gametypeKey);
            if (!p.entityType.empty())
                out += R"(,"skybox":)" + esc(p.entityType);
            break;
        }
        case OzonePrimitiveType::ENTITY_PARTICLES: {
            out += esc("particles");
            if (p.args.size() >= 8) {
                char buf[256];
                snprintf(buf, sizeof(buf),
                         R"(,"particleType":%d,"density":%.3f,"speed":%.3f)"
                         R"(,"r":%d,"g":%d,"b":%d,"windX":%.3f,"windZ":%.3f)",
                         (int)p.args[0], p.args[1], p.args[2],
                         (int)p.args[3], (int)p.args[4], (int)p.args[5],
                         p.args[6], p.args[7]);
                out += buf;
            }
            break;
        }
        default:
            out += esc("unknown");
            break;
    }
    out += '}';
}

// ---------------------------------------------------------------------------
// HTTP server
// ---------------------------------------------------------------------------
static int http_listen(int port) {
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (!sock_fd_good(fd)) return -1;
    int yes = 1;
    setsockopt(TO_SOCK(fd), SOL_SOCKET, SO_REUSEADDR, sock_set_opt(&yes, sizeof(yes)));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    // g_bind_ip is the already-resolved literal IPv4 ("" = all interfaces)
    if (g_bind_ip.empty() || g_bind_ip == "0.0.0.0")
        addr.sin_addr.s_addr = INADDR_ANY; // 0.0.0.0 — all interfaces
    else
        inet_pton(AF_INET, g_bind_ip.c_str(), &addr.sin_addr);
    addr.sin_port = htons(port);
    if (bind(TO_SOCK(fd), (struct sockaddr*)&addr, sizeof(addr)) < 0) { close_sock(fd); return -1; }
    if (listen(TO_SOCK(fd), 8) < 0) { close_sock(fd); return -1; }
    return fd;
}

static void http_write_all(int fd, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t w = send(TO_SOCK(fd), sock_sendto_buf(data + off, len - off), 0);
        if (w <= 0) break;
        off += (size_t)w;
    }
}

static void http_response(int fd, int code, const char* status,
                          const char* body, const char* type) {
    char hdr[512];
    int hl = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n",
        code, status, type, strlen(body));
    http_write_all(fd, hdr, (size_t)hl);
    http_write_all(fd, body, strlen(body));
}

static void http_respond_json(int fd, const char* body) {
    http_response(fd, 200, "OK", body, "application/json");
}

static void http_respond_404(int fd) {
    http_response(fd, 404, "Not Found", "{\"ok\":false,\"error\":\"not found\"}",
                  "application/json");
}

static const char* get_query_param(const char* path, const char* key,
                                    char* out, size_t out_sz) {
    const char* q = strchr(path, '?');
    if (!q) return nullptr;
    ++q;
    size_t klen = strlen(key);
    while (*q) {
        if (strncmp(q, key, klen) == 0 && q[klen] == '=') {
            q += klen + 1;
            size_t i = 0;
            while (*q && *q != '&' && i + 1 < out_sz) out[i++] = *q++;
            out[i] = '\0';
            return out;
        }
        while (*q && *q != '&') ++q;
        if (*q == '&') ++q;
    }
    return nullptr;
}

// Bearer-token check for the HTTP API. When no auth token is configured the
// server runs in open dev mode (everything allowed).
static bool http_bearer_ok(const char* req) {
    if (g_auth_token.empty()) return true; // dev mode
    // Scan header lines for "Authorization: Bearer <token>"
    const char* p = req;
    while ((p = strstr(p, "Authorization:")) != nullptr) {
        const char* v = p + strlen("Authorization:");
        while (*v == ' ') ++v;
        if (strncmp(v, "Bearer ", 7) == 0 || strncmp(v, "bearer ", 7) == 0) {
            const char* tok = v + 7;
            size_t len = strcspn(tok, "\r\n");
            if (len == g_auth_token.size() &&
                strncmp(tok, g_auth_token.c_str(), len) == 0)
                return true;
        }
        ++p;
    }
    return false;
}

static bool http_path_is_protected(const char* path) {
    return strncmp(path, "/map", 4) == 0 ||
           strcmp(path, "/worlds") == 0 ||
           strcmp(path, "/status") == 0 ||
           strcmp(path, "/players") == 0;
}

static void handle_http_client(int cfd) {
    char req[8192];
    ssize_t n = recv(TO_SOCK(cfd), sock_recvfrom_buf(req, sizeof(req) - 1), 0);
    if (n <= 0) { close_sock(cfd); return; }
    req[n] = '\0';

    char method[16] = {0}, path[1024] = {0};
    sscanf(req, "%15s %1023s", method, path);

    // Auth gate (only when --auth-token/OZ_AUTH_TOKEN is set). /auth/login
    // itself stays open — it is how clients obtain the token.
    if (http_path_is_protected(path) && !http_bearer_ok(req)) {
        http_response(cfd, 401, "Unauthorized",
                      "{\"ok\":false,\"error\":\"unauthorized\"}", "application/json");
        close_sock(cfd);
        return;
    }

    if (strcmp(method, "GET") == 0) {
        if (strcmp(path, "/map") == 0 || strncmp(path, "/map?", 5) == 0) {
            char name[256] = {0};
            get_query_param(path, "name", name, sizeof(name));
            if (name[0] == '\0') {
                std::string json = "{\"ok\":true,\"worlds\":[";
                for (size_t i = 0; i < g_world_list.size(); ++i) {
                    if (i) json += ',';
                    json += json_escape(g_world_list[i]);
                }
                json += "]}";
                http_respond_json(cfd, json.c_str());
                close_sock(cfd);
                return;
            }
            // OZONE is the only world format
            std::string ozone_path = g_gamedata_dir + "/Worlds/" + name + "/World.ozone";
            auto ozone_prims = OzoneParser::parse_file(ozone_path);
            if (!ozone_prims.empty()) {
                std::string json = "{\"ok\":true,\"format\":\"ozone\",\"world\":";
                json += json_escape(name);
                json += ",\"brushes\":[";
                bool first = true;
                for (const auto& p : ozone_prims) {
                    append_ozone_json(json, p, first);
                    first = false;
                }
                json += "]}";
                http_respond_json(cfd, json.c_str());
                close_sock(cfd);
                return;
            }

            http_respond_404(cfd);
            close_sock(cfd);
            return;
        }

        if (strcmp(path, "/status") == 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - g_server_start_time).count();
            std::string json = "{\"ok\":true,\"server_name\":";
            json += json_escape(g_server_name);
            json += ",\"players\":";
            json += std::to_string(g_game_server->player_count());
            json += ",\"max_players\":";
            // The WORLD's cap (levelinfo maxPlayers), not net::MAX_PLAYERS.
            // The two differ on every shipped level (8 vs 32), so the browser
            // used to advertise 24 slots the server refuses.
            json += std::to_string(g_game_state.max_players());
            json += ",\"uptime_seconds\":";
            json += std::to_string(elapsed);
            json += ",\"worlds\":";
            json += std::to_string(g_game_state.world_count());
            json += ",\"map\":";
            json += json_escape(current_map_name());
            json += ",\"gamever\":";
            json += json_escape(oz::kBuildVersion);
            json += ",\"proto\":";
            json += std::to_string((unsigned)net::PROTOCOL_VERSION);
            json += ",\"password\":false}";
            http_respond_json(cfd, json.c_str());
            close_sock(cfd);
            return;
        }

        if (strcmp(path, "/worlds") == 0) {
            std::string json = "{\"ok\":true,\"worlds\":[";
            for (size_t i = 0; i < g_world_list.size(); ++i) {
                if (i) json += ',';
                json += json_escape(g_world_list[i]);
            }
            json += "]}";
            http_respond_json(cfd, json.c_str());
            close_sock(cfd);
            return;
        }

        if (strcmp(path, "/players") == 0) {
            std::string json = "{\"ok\":true,\"players\":[";
            bool first = true;
            for (const auto& p : g_game_server->players()) {
                if (!p.connected) continue;
                if (!first) json += ',';
                first = false;
                json += "{\"id\":";
                json += std::to_string(p.id);
                json += ",\"name\":";
                json += json_escape(p.name);
                json += ",\"ip\":";
                json += json_escape(p.ip_address);
                json += "}";
            }
            json += "]}";
            http_respond_json(cfd, json.c_str());
            close_sock(cfd);
            return;
        }
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/auth/login") == 0) {
        if (!g_auth_token.empty()) {
            // Extract body after headers (\r\n\r\n)
            const char* body = std::strstr(req, "\r\n\r\n");
            if (!body) body = std::strstr(req, "\n\n");
            body = body ? body + (body[0] == '\r' ? 4 : 2) : "";
            // Find "token" (quoted or bare), then ':' — value may be quoted
            // or bare (tolerant: some thin clients strip quotes).
            bool valid = false;
            const char* t = std::strstr(body, "token");
            if (t) {
                t += 5; // past "token"
                while (*t == ' ' || *t == ':' || *t == '"') ++t;
                size_t len = 0;
                while (t[len] && t[len] != '"' && t[len] != '}' && t[len] != ',' &&
                       t[len] != '\r' && t[len] != '\n') ++len;
                if (len > 0 && len == g_auth_token.size() &&
                    strncmp(t, g_auth_token.c_str(), len) == 0)
                    valid = true;
            }
            if (!valid) {
                http_respond_json(cfd, "{\"ok\":false,\"error\":\"invalid_token\"}");
                close_sock(cfd);
                return;
            }
        }
        http_respond_json(cfd, (std::string("{\"ok\":true,\"token\":\"") + g_auth_token + "\"}").c_str());
        close_sock(cfd);
        return;
    }

    http_respond_404(cfd);
    close_sock(cfd);
}

void http_server_thread(int port) {
    int lfd = http_listen(port);
    if (lfd < 0) {
        OZ_ERROR("HTTP: failed to listen on port %d", port);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_print_mutex);
        OZ_INFO("HTTP: map server on http://127.0.0.1:%d", port);
    }

    fd_set master_fds;
    FD_ZERO(&master_fds);
    FD_SET(lfd, &master_fds);
    int max_fd = lfd;

    while (g_running) {
        fd_set read_fds = master_fds;
        struct timeval tv = {1, 0};
        if (select(max_fd + 1, &read_fds, nullptr, nullptr, &tv) < 0) {
            if (g_running) break;
            continue;
        }

        if (FD_ISSET(lfd, &read_fds)) {
            struct sockaddr_in addr;
            socklen_t addrlen = sizeof(addr);
            int cfd = accept(lfd, (struct sockaddr*)&addr, &addrlen);
            if (cfd >= 0) {
                handle_http_client(cfd);
            }
        }
    }

    close_sock(lfd);
}
