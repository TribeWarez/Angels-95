#pragma once
// ---------------------------------------------------------------------------
// Menu/ServerBrowserModel.hpp
//
// The server browser's data layer: merge LAN and internet results, deduplicate
// by ip:port, sort/filter, and remember favourites and recent servers. Pure C++
// so tests/Menu.test.cpp covers the rules without a socket or a window.
//
// The UI owns the two input vectors; this model owns the merged view and the
// persistent state. Favourites/recents are keyed by ip:port (never by display
// name, which two servers may share).
// ---------------------------------------------------------------------------
#include "../IniConfig.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace angelsmenu {

struct ServerKey {
    std::string ip;
    uint16_t port = 0;

    bool operator==(const ServerKey& o) const { return ip == o.ip && port == o.port; }
    bool Empty() const { return ip.empty() || port == 0; }
};

struct ServerRow {
    ServerKey key;
    std::string name;
    std::string map;
    std::string version;       // build id (b94, b97, dev...)
    int protocol = 0;          // wire protocol; 0 = unknown (legacy master)
    int players = 0;
    int maxPlayers = 0;
    int pingMs = -1;          // -1 = unknown
    bool password = false;
    bool live = false;        // status query succeeded
    bool lan = false;         // advertised by LAN discovery
    bool internet = false;    // came from a master
    bool favorite = false;
    bool recent = false;
};

struct ServerFilter {
    enum Source : int { All = 0, Lan = 1, Internet = 2, Favorites = 3 };
    int source = All;
    std::string query;
    bool hideFull = false;
    bool hidePassword = false;
    int sort = 0;             // 0 players, 1 ping, 2 name, 3 map
};

class ServerBrowserModel {
public:
    void SetLan(std::vector<ServerRow> rows) {
        m_lan = std::move(rows);
        Rebuild();
    }

    void SetInternet(std::vector<ServerRow> rows) {
        m_internet = std::move(rows);
        Rebuild();
    }

    const std::vector<ServerRow>& Rows() const { return m_rows; }

    std::vector<size_t> Query(const ServerFilter& filter) const {
        std::vector<size_t> out;
        out.reserve(m_rows.size());
        const std::string q = ToLower(Trim(filter.query));

        for (size_t i = 0; i < m_rows.size(); ++i) {
            const ServerRow& r = m_rows[i];
            if (filter.source == ServerFilter::Lan && !r.lan) continue;
            if (filter.source == ServerFilter::Internet && !r.internet) continue;
            if (filter.source == ServerFilter::Favorites && !r.favorite) continue;
            if (filter.hideFull && r.maxPlayers > 0 && r.players >= r.maxPlayers) continue;
            if (filter.hidePassword && r.password) continue;
            if (!q.empty()) {
                const std::string hay = ToLower(r.name + " " + r.map + " " + r.key.ip);
                if (hay.find(q) == std::string::npos) continue;
            }
            out.push_back(i);
        }

        auto byName = [&](size_t a, size_t b) {
            return ToLower(m_rows[a].name) < ToLower(m_rows[b].name);
        };
        if (filter.sort == 0) {
            std::stable_sort(out.begin(), out.end(), [&](size_t a, size_t b) {
                if (m_rows[a].players != m_rows[b].players)
                    return m_rows[a].players > m_rows[b].players;
                return PingLess(a, b) ? true : (!PingLess(b, a) ? byName(a, b) : false);
            });
        } else if (filter.sort == 1) {
            std::stable_sort(out.begin(), out.end(), [&](size_t a, size_t b) {
                if (PingLess(a, b)) return true;
                if (PingLess(b, a)) return false;
                return byName(a, b);
            });
        } else if (filter.sort == 2) {
            std::stable_sort(out.begin(), out.end(), byName);
        } else {
            std::stable_sort(out.begin(), out.end(), [&](size_t a, size_t b) {
                const std::string ma = ToLower(m_rows[a].map);
                const std::string mb = ToLower(m_rows[b].map);
                if (ma != mb) return ma < mb;
                return byName(a, b);
            });
        }
        return out;
    }

    bool IsFavorite(const ServerKey& key) const {
        const std::string k = KeyString(key);
        return std::find(m_favorites.begin(), m_favorites.end(), k) != m_favorites.end();
    }

    bool ToggleFavorite(const ServerKey& key) {
        if (key.Empty()) return false;
        const std::string k = KeyString(key);
        auto it = std::find(m_favorites.begin(), m_favorites.end(), k);
        bool nowFavorite = false;
        if (it == m_favorites.end()) {
            m_favorites.push_back(k);
            nowFavorite = true;
        } else {
            m_favorites.erase(it);
        }
        Rebuild();
        return nowFavorite;
    }

    void AddRecent(const ServerKey& key, size_t cap = 10) {
        if (key.Empty() || cap == 0) return;
        const std::string k = KeyString(key);
        m_recent.erase(std::remove(m_recent.begin(), m_recent.end(), k), m_recent.end());
        m_recent.insert(m_recent.begin(), k);
        if (m_recent.size() > cap) m_recent.resize(cap);
        Rebuild();
    }

    const std::vector<std::string>& Favorites() const { return m_favorites; }
    const std::vector<std::string>& Recent() const { return m_recent; }

    std::string SerializeState() const {
        std::string out = "[Browser]\n";
        out += "Favorites=" + Join(m_favorites) + "\n";
        out += "Recent=" + Join(m_recent) + "\n";
        return out;
    }

    bool DeserializeState(const std::string& text) {
        IniConfig cfg;
        if (!cfg.ParseText(text)) return false;
        m_favorites.clear();
        m_recent.clear();
        SplitList(cfg.Get("Browser", "Favorites", ""), m_favorites);
        SplitList(cfg.Get("Browser", "Recent", ""), m_recent);
        Rebuild();
        return true;
    }

    bool LoadState(const char* path) {
        IniConfig cfg;
        if (!cfg.Load(path)) return false;
        m_favorites.clear();
        m_recent.clear();
        SplitList(cfg.Get("Browser", "Favorites", ""), m_favorites);
        SplitList(cfg.Get("Browser", "Recent", ""), m_recent);
        Rebuild();
        return true;
    }

    bool SaveState(const char* path) const {
        std::ofstream f(path, std::ios::trunc);
        if (!f.is_open()) return false;
        f << SerializeState();
        return true;
    }

    static std::string KeyString(const ServerKey& key) {
        return key.ip + ":" + std::to_string((unsigned)key.port);
    }

    static ServerKey ParseKey(const std::string& text) {
        ServerKey k;
        size_t colon = text.rfind(':');
        if (colon == std::string::npos) return k;
        k.ip = Trim(text.substr(0, colon));
        const std::string p = Trim(text.substr(colon + 1));
        if (p.empty()) return k;
        try {
            int v = std::stoi(p);
            if (v > 0 && v <= 65535) k.port = (uint16_t)v;
            else k.ip.clear();
        } catch (...) {
            k.ip.clear();
        }
        return k;
    }

private:
    static std::string ToLower(std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    static std::string Trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return {};
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    static std::string Join(const std::vector<std::string>& v) {
        std::string out;
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) out += '|';
            out += v[i];
        }
        return out;
    }

    static void SplitList(const std::string& s, std::vector<std::string>& out) {
        std::string cur;
        for (char c : s) {
            if (c == '|') {
                std::string t = Trim(cur);
                if (!t.empty()) out.push_back(t);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        std::string t = Trim(cur);
        if (!t.empty()) out.push_back(t);
    }

    bool PingLess(size_t a, size_t b) const {
        const int pa = m_rows[a].pingMs;
        const int pb = m_rows[b].pingMs;
        if (pa < 0 && pb < 0) return false;
        if (pa < 0) return false;
        if (pb < 0) return true;
        return pa < pb;
    }

    void Rebuild() {
        m_rows.clear();
        std::unordered_map<std::string, size_t> index;

        auto merge = [&](const std::vector<ServerRow>& src) {
            for (const ServerRow& in : src) {
                if (in.key.Empty()) continue;
                const std::string k = KeyString(in.key);
                auto it = index.find(k);
                if (it == index.end()) {
                    index[k] = m_rows.size();
                    m_rows.push_back(in);
                    continue;
                }
                ServerRow& r = m_rows[it->second];
                if (r.name.empty()) r.name = in.name;
                if (r.map.empty()) r.map = in.map;
                if (r.version.empty()) r.version = in.version;
                if (r.protocol == 0) r.protocol = in.protocol;
                if (in.live) {
                    r.live = true;
                    if (in.players > 0) r.players = in.players;
                    if (in.maxPlayers > 0) r.maxPlayers = in.maxPlayers;
                    if (in.pingMs >= 0) r.pingMs = in.pingMs;
                    if (!in.map.empty()) r.map = in.map;
                } else {
                    r.players = std::max(r.players, in.players);
                    r.maxPlayers = std::max(r.maxPlayers, in.maxPlayers);
                }
                r.password = r.password || in.password;
                r.lan = r.lan || in.lan;
                r.internet = r.internet || in.internet;
            }
        };

        merge(m_lan);
        merge(m_internet);

        for (ServerRow& r : m_rows) {
            const std::string k = KeyString(r.key);
            r.favorite = std::find(m_favorites.begin(), m_favorites.end(), k) != m_favorites.end();
            r.recent = std::find(m_recent.begin(), m_recent.end(), k) != m_recent.end();
        }
    }

    std::vector<ServerRow> m_lan;
    std::vector<ServerRow> m_internet;
    std::vector<ServerRow> m_rows;
    std::vector<std::string> m_favorites;
    std::vector<std::string> m_recent;
};

} // namespace angelsmenu
