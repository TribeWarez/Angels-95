#ifndef OMEGA_INTERNET_BROWSER_HPP
#define OMEGA_INTERNET_BROWSER_HPP

// Client-side internet server browser. Fetches the live list from one or more
// master servers (HTTP/JSON) on a background thread, then queries each game
// server's own /status endpoint for live player counts and real RTT.
//
// Improvements over the original sequential scan:
//   * the master list is published immediately, so the UI shows servers while
//     status probes are still running;
//   * status probes run in a small thread pool (max 8) with a 700 ms timeout,
//     instead of up to 48 sequential 1.5 s requests (a worst case of >70 s);
//   * Update() copies partial results while the worker is alive.
//
// All network work stays off the render thread; the menu calls Refresh() to
// start and Update() each frame to pick up results.

#include "../Network/MasterProtocol.hpp"
#include "../Network/MasterHttp.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <cstdio>

namespace master {

struct OnlineServer {
    ServerEntry entry;
    bool live = false;    // /status query succeeded
    int  ping_ms = -1;    // RTT of the /status request (real round trip)
};

class InternetBrowser {
public:
    InternetBrowser() = default;
    ~InternetBrowser() { Cancel(); }
    InternetBrowser(const InternetBrowser&) = delete;
    InternetBrowser& operator=(const InternetBrowser&) = delete;

    void SetMasters(std::vector<std::string> urls) { m_urls = std::move(urls); }

    bool IsScanning() const { return m_running.load(); }
    bool HasResult() const { return m_have_result; }
    double LastScanSeconds() const { return m_last_seconds; }
    const std::vector<OnlineServer>& Servers() const { return m_servers; }
    const std::string& Error() const { return m_error; }
    int ProgressDone() const { return m_progressDone.load(); }
    int ProgressTotal() const { return m_progressTotal.load(); }

    void Refresh() {
        if (m_running.load()) return;
        Cancel();
        m_cancel.store(false);
        m_finished.store(false);
        m_running.store(true);
        m_last_seconds = 0.0;
        m_progressDone.store(0);
        m_progressTotal.store(0);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_staged.clear();
            m_staged_error.clear();
            m_staged_have = false;
        }
        m_thread = std::thread([this] { Worker(); });
    }

    // Non-blocking; copies partial results while the worker runs and joins it
    // once it has finished.
    void Update() {
        if (!m_running.load()) return;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_servers = m_staged;
            m_error = m_staged_error;
            if (m_staged_have) m_have_result = true;
        }

        if (m_finished.load()) {
            m_running.store(false);
            if (m_thread.joinable()) m_thread.join();
        }
    }

    void Cancel() {
        m_cancel.store(true);
        if (m_thread.joinable()) m_thread.join();
        m_running.store(false);
        m_finished.store(false);
    }

private:
    static std::string ServersUrl(const std::string& base) {
        // Explicit endpoint supplied (already points at /api/servers)
        if (base.find("/api/servers") != std::string::npos) return base;

        // Otherwise append the endpoint to the base, handling trailing slashes
        // and path prefixes such as "https://host/master".
        std::string b = base;
        while (!b.empty() && b.back() == '/') b.pop_back();
        return b + "/api/servers?gamename=" + std::string(GAMENAME);
    }

    void Publish(const std::vector<OnlineServer>& found, const std::string& err,
                 bool have, int done, int total) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_staged = found;
        if (m_staged_error.empty()) m_staged_error = err;
        m_staged_have = m_staged_have || have;
        m_progressDone.store(done);
        m_progressTotal.store(total);
    }

    void Probe(OnlineServer& os) {
        if (os.entry.http_port == 0) return;
        std::string url = "http://" + os.entry.ip + ":" +
                          std::to_string(os.entry.http_port) + "/status";
        long long q0 = mhttp_now_ms();
        std::string body;
        int status = 0;
        if (HttpRequest("GET", url, "", "", 700, body, &status) && status == 200) {
            os.ping_ms = (int)(mhttp_now_ms() - q0);
            os.live = true;
            uint32_t v = 0;
            if (JsonGetUInt(body, "players", v)) os.entry.players = v;
            if (JsonGetUInt(body, "max_players", v)) os.entry.maxplayers = v;
            std::string sname;
            if (JsonGetString(body, "server_name", sname) && !sname.empty())
                os.entry.name = sname;
            std::string smap;
            if (JsonGetString(body, "map", smap) && !smap.empty())
                os.entry.map = smap;
        }
    }

    void Worker() {
        long long t0 = mhttp_now_ms();
        std::vector<OnlineServer> found;
        std::string err;

        for (const auto& base : m_urls) {
            if (m_cancel.load()) break;
            std::string body;
            int status = 0;
            if (!HttpRequest("GET", ServersUrl(base), "", "", 2500, body, &status) ||
                status != 200) {
                if (err.empty()) err = "master unreachable: " + base;
                continue;
            }
            std::vector<ServerEntry> list;
            ParseServerListJson(body, list);
            for (auto& e : list) {
                bool dup = false;
                for (auto& f : found) {
                    if (f.entry.ip == e.ip && f.entry.port == e.port) {
                        f.entry = e;
                        dup = true;
                        break;
                    }
                }
                if (!dup) found.push_back(OnlineServer{e, false, -1});
            }
        }

        // Publish the master list before probing so the browser has rows to
        // show immediately (ping = —, live = false).
        Publish(found, err, true, 0, (int)found.size());

        const size_t kMaxStatus = 48;
        const size_t cap = std::min(found.size(), kMaxStatus);
        const int kThreads = (int)std::min<size_t>(8, std::max<size_t>(1, cap));
        std::atomic<size_t> next{0};
        std::atomic<int> done{0};

        if (cap > 0) {
            std::vector<std::thread> workers;
            workers.reserve(kThreads);
            for (int t = 0; t < kThreads; ++t) {
                workers.emplace_back([&] {
                    while (!m_cancel.load()) {
                        size_t i = next.fetch_add(1);
                        if (i >= cap) break;
                        Probe(found[i]);
                        done.fetch_add(1);
                    }
                });
            }
            for (auto& w : workers) w.join();
        }

        m_last_seconds = (mhttp_now_ms() - t0) / 1000.0;
        Publish(found, err, true, (int)cap, (int)cap);
        m_finished.store(true);
    }

    std::vector<std::string> m_urls;
    std::vector<OnlineServer> m_servers;
    std::string m_error;
    bool m_have_result = false;
    double m_last_seconds = 0.0;

    std::vector<OnlineServer> m_staged;
    std::string m_staged_error;
    bool m_staged_have = false;
    std::mutex m_mutex;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_finished{false};
    std::atomic<bool> m_cancel{false};
    std::atomic<int> m_progressDone{0};
    std::atomic<int> m_progressTotal{0};
    std::thread m_thread;
};

} // namespace master

#endif // OMEGA_INTERNET_BROWSER_HPP
