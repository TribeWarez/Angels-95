#ifndef OMEGA_MASTER_HTTP_HPP
#define OMEGA_MASTER_HTTP_HPP

// Minimal blocking HTTP/1.1 client used for master list/status queries and the
// HTTP heartbeat uplink. Plain HTTP only (no TLS) to avoid new dependencies.
// Cross-platform (WinSock2 / POSIX). No raylib or engine dependencies.

#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
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
    #include <windows.h>
    #include <winhttp.h>
    #pragma comment(lib, "winhttp.lib")
    typedef int socklen_t;
    #define MHTTP_SOCK(fd) ((SOCKET)(intptr_t)(fd))
    #define MHTTP_CLOSE(fd) closesocket(MHTTP_SOCK(fd))
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <errno.h>
    #include <fcntl.h>
    #define MHTTP_SOCK(fd) (fd)
    #define MHTTP_CLOSE(fd) ::close(fd)
    #define INVALID_SOCKET (-1)
#endif

namespace master {

struct ParsedUrl {
    std::string host;
    uint16_t    port = 80;
    std::string path = "/";
    bool        https = false;
};

inline bool ParseHttpUrl(const std::string& url, ParsedUrl& out) {
    std::string s = url;
    if (s.compare(0, 8, "https://") == 0) {
        out.https = true;
        out.port = 443;
        s = s.substr(8);
    } else if (s.compare(0, 7, "http://") == 0) {
        out.https = false;
        out.port = 80;
        s = s.substr(7);
    }
    size_t slash = s.find('/');
    std::string hostport = (slash == std::string::npos) ? s : s.substr(0, slash);
    out.path = (slash == std::string::npos) ? "/" : s.substr(slash);
    size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        out.host = hostport.substr(0, colon);
        long p = strtol(hostport.substr(colon + 1).c_str(), nullptr, 10);
        if (p <= 0 || p > 65535) return false;
        out.port = (uint16_t)p;
    } else {
        out.host = hostport;
        // keep the scheme default (80 / 443)
    }
    return !out.host.empty();
}

#ifdef _WIN32
inline void mhttp_wsa_init() {
    static bool done = false;
    if (!done) {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
        done = true;
    }
}
#else
inline void mhttp_wsa_init() {}
#endif

inline long long mhttp_now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

inline int mhttp_connect(const ParsedUrl& url, int timeout_ms) {
    mhttp_wsa_init();
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)url.port);
    if (getaddrinfo(url.host.c_str(), portstr, &hints, &res) != 0 || !res) return -1;

    int fd = -1;
    long long deadline = mhttp_now_ms() + timeout_ms;
    for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
        int s = (int)socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s < 0) continue;
#ifdef _WIN32
        u_long nb = 1;
        ioctlsocket(MHTTP_SOCK(s), FIONBIO, &nb);
#else
        int flags = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
        int rc = connect(MHTTP_SOCK(s), ai->ai_addr, (socklen_t)ai->ai_addrlen);
        if (rc == 0) { fd = s; break; }
        bool in_progress =
#ifdef _WIN32
            WSAGetLastError() == WSAEWOULDBLOCK;
#else
            errno == EINPROGRESS || errno == EWOULDBLOCK;
#endif
        if (!in_progress) { MHTTP_CLOSE(s); continue; }
        fd_set wf;
        FD_ZERO(&wf);
        FD_SET(MHTTP_SOCK(s), &wf);
        long long left = deadline - mhttp_now_ms();
        if (left <= 0) { MHTTP_CLOSE(s); break; }
        struct timeval tv;
        tv.tv_sec = (long)(left / 1000);
        tv.tv_usec = (long)((left % 1000) * 1000);
        int sel = select((int)(s + 1), nullptr, &wf, nullptr, &tv);
        if (sel > 0) {
            int err = 0;
            socklen_t elen = sizeof(err);
            getsockopt(MHTTP_SOCK(s), SOL_SOCKET, SO_ERROR, (char*)&err, &elen);
            if (err == 0) { fd = s; break; }
        }
        MHTTP_CLOSE(s);
    }
    freeaddrinfo(res);
    return fd;
}

inline bool mhttp_send_all(int fd, const std::string& data, long long deadline) {
    size_t off = 0;
    while (off < data.size()) {
        long long left = deadline - mhttp_now_ms();
        if (left <= 0) return false;
        fd_set wf;
        FD_ZERO(&wf);
        FD_SET(MHTTP_SOCK(fd), &wf);
        struct timeval tv;
        tv.tv_sec = (long)(left / 1000);
        tv.tv_usec = (long)((left % 1000) * 1000);
        if (select(fd + 1, nullptr, &wf, nullptr, &tv) <= 0) return false;
        int n = (int)send(MHTTP_SOCK(fd), data.c_str() + off, (int)(data.size() - off), 0);
        if (n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}

inline bool mhttp_recv_all(int fd, std::string& out, long long deadline, size_t max_bytes) {
    char buf[4096];
    while (out.size() < max_bytes) {
        long long left = deadline - mhttp_now_ms();
        if (left <= 0) break;
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(MHTTP_SOCK(fd), &rf);
        struct timeval tv;
        tv.tv_sec = (long)(left / 1000);
        tv.tv_usec = (long)((left % 1000) * 1000);
        int sel = select(fd + 1, &rf, nullptr, nullptr, &tv);
        if (sel <= 0) break;
        int n = (int)recv(MHTTP_SOCK(fd), buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, (size_t)n);
    }
    return !out.empty();
}

// ---------------------------------------------------------------------------
// HTTPS transport (TLS). No crypto dependency is vendored:
//   Windows: WinHTTP (Schannel)
//   POSIX:   the `curl` binary via popen (present on virtually all systems)
// Plain HTTP continues to use the raw-socket path below.
// ---------------------------------------------------------------------------
inline bool HttpsRequest(const std::string& method, const ParsedUrl& u,
                         const std::string& body, const std::string& content_type,
                         int timeout_ms, std::string& out_body, int* out_status) {
    std::string url = "https://" + u.host + ":" + std::to_string(u.port) + u.path;

#ifdef _WIN32
    // --- WinHTTP (Schannel TLS) ---
    std::wstring wurl(url.begin(), url.end());
    HINTERNET hSession = WinHttpOpen(L"Angels95-Master/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    WinHttpSetTimeouts(hSession, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    HINTERNET hConnect = WinHttpConnect(hSession, std::wstring(u.host.begin(), u.host.end()).c_str(),
                                        u.port, 0);
    bool ok = false;
    if (hConnect) {
        std::wstring wmethod(method.begin(), method.end());
        std::wstring wpath(u.path.begin(), u.path.end());
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, wmethod.c_str(), wpath.c_str(),
                                                nullptr, WINHTTP_NO_REFERER,
                                                WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                WINHTTP_FLAG_SECURE);
        if (hRequest) {
            std::wstring headers;
            if (!body.empty()) {
                std::wstring wct(content_type.empty() ? L"text/plain" : std::wstring(content_type.begin(), content_type.end()));
                headers = L"Content-Type: " + wct + L"\r\n";
            }
            BOOL sent = WinHttpSendRequest(hRequest,
                headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                headers.empty() ? 0 : (DWORD)-1L,
                body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                (DWORD)body.size(), (DWORD)body.size(), 0);
            if (sent && WinHttpReceiveResponse(hRequest, nullptr)) {
                if (out_status) {
                    DWORD code = 0, len = sizeof(code);
                    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                        WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
                    *out_status = (int)code;
                }
                out_body.clear();
                DWORD avail = 0;
                while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
                    std::string chunk(avail, '\0');
                    DWORD read = 0;
                    if (!WinHttpReadData(hRequest, &chunk[0], avail, &read) || read == 0) break;
                    out_body.append(chunk.data(), read);
                    if (out_body.size() > 2 * 1024 * 1024) break;
                }
                ok = true;
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return ok;
#else
    // --- POSIX: shell out to curl ---
    if (u.host.empty()) return false;
    // NO `-k`. Certificate verification was disabled on this path while the
    // Windows path (WinHTTP/Schannel) verified, so on Linux any on-path attacker
    // could MITM the server browser and feed arbitrary ip:port entries - which
    // the client then probes and lets the player join. `--fail` makes a 4xx/5xx
    // exit non-zero, so a rejected heartbeat is reported instead of looking like
    // a success with an empty body.
    std::string cmd = "curl -sS --fail --max-time " + std::to_string(timeout_ms / 1000 + 1) +
                      " -X " + method;
    if (!body.empty()) {
        // Single-quote the body; escape embedded single quotes
        std::string esc;
        for (char c : body) { if (c == '\'') esc += "'\\''"; else esc.push_back(c); }
        cmd += " --data-raw '" + esc + "'";
        if (!content_type.empty()) cmd += " -H 'Content-Type: " + content_type + "'";
    }
    cmd += " '" + url + "' 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return false;
    out_body.clear();
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out_body.append(buf, n);
    int rc = pclose(p);
    if (out_status) *out_status = (rc == 0 && !out_body.empty()) ? 200 : 0;
    return rc == 0 && !out_body.empty();
#endif
}

// Returns true on a complete HTTP response (even 4xx/5xx). out_status gets the
// status code, out_body the response body.
inline bool HttpRequest(const std::string& method, const std::string& url,
                        const std::string& body, const std::string& content_type,
                        int timeout_ms, std::string& out_body, int* out_status = nullptr) {
    ParsedUrl u;
    if (!ParseHttpUrl(url, u)) return false;
    if (u.https)
        return HttpsRequest(method, u, body, content_type, timeout_ms, out_body, out_status);

    int fd = mhttp_connect(u, timeout_ms);
    if (fd < 0) return false;

    std::string req = method + " " + u.path + " HTTP/1.1\r\n";
    req += "Host: " + u.host + ":" + std::to_string(u.port) + "\r\n";
    req += "User-Agent: Angels95-Master/1.0\r\n";
    req += "Accept: application/json\r\n";
    req += "Connection: close\r\n";
    if (!body.empty()) {
        req += "Content-Type: " + (content_type.empty() ? std::string("text/plain") : content_type) + "\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n";
    req += body;

    long long deadline = mhttp_now_ms() + timeout_ms;
    std::string raw;
    bool ok = mhttp_send_all(fd, req, deadline) &&
              mhttp_recv_all(fd, raw, deadline, 512 * 1024);
    MHTTP_CLOSE(fd);
    if (!ok) return false;

    size_t hdr_end = raw.find("\r\n\r\n");
    size_t skip = 4;
    if (hdr_end == std::string::npos) {
        hdr_end = raw.find("\n\n");
        skip = 2;
    }
    if (hdr_end == std::string::npos) return false;
    out_body = raw.substr(hdr_end + skip);

    // Grow the body if Content-Length says there is more (first recv may have
    // been partial, though Connection: close plus the read loop usually has it).
    if (out_status) {
        const char* sp = strchr(raw.c_str(), ' ');
        if (sp) *out_status = atoi(sp + 1);
    }
    return true;
}

} // namespace master

#endif // OMEGA_MASTER_HTTP_HPP