#pragma once
// ---------------------------------------------------------------------------
// Menu/CharacterRoster.hpp
//
// Data-driven player characters. The roster is authored in
// GameData/Pawn/Player/characters.cfg (packed by the existing player.ozpak
// rule), parsed through IniConfig and kept raylib-free so the headless test
// harness can cover it.
//
// Format:
//
//   [Roster]
//   Order=arcanist,killer
//
//   [arcanist]
//   name=Plague Arcanist
//   mesh=GameData/Pawn/Player/Plague_Arcanist.glb
//   mesh_type=static
//   bio=...
//   accent=6
//
// `mesh`, `texture` and `portrait` are exact asset-reference keys, so
// `worldcheck --asset-refs` validates them. A section not listed in Order is
// appended in name order with a warning; a section listed in Order but missing
// is skipped with a warning. Invalid entries never become selectable rows.
//
// PlayerProfile.modelPath stores the section id. A legacy value that is an
// asset path is mapped through FindByMesh() so old profiles keep their model.
// ---------------------------------------------------------------------------
#include "../IniConfig.hpp"
#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace angelsmenu {

struct CharacterDef {
    std::string id;
    std::string name;
    std::string mesh;
    std::string texture;
    std::string meshType = "static";   // "static" | "skeletal"
    std::string portrait;              // optional authored override
    std::string bio;
    std::string animIdle;              // optional, for skeletal characters
    std::string animChase;
    int accent = 0;                    // palette index 0..7
};

class CharacterRoster {
public:
    static CharacterRoster& Instance() {
        static CharacterRoster roster;
        return roster;
    }

    void Clear() {
        m_entries.clear();
        m_index.clear();
        m_warnings.clear();
        m_loaded = false;
    }

    bool LoadFromText(const std::string& text) {
        Clear();
        IniConfig cfg;
        if (!cfg.ParseText(text)) {
            m_warnings.push_back("roster: could not parse characters.cfg");
            return false;
        }

        std::vector<std::string> order;
        SplitList(cfg.Get("Roster", "Order", ""), order);

        // Walk Order first so display order is deterministic even though
        // IniConfig stores sections in an unordered_map.
        for (const std::string& id : order) AddSection(cfg, id, true);

        // Any section not named in Order is still usable; append it sorted by
        // id so a hand-added character does not silently disappear.
        std::vector<std::string> leftovers;
        for (const auto& [section, _] : cfg.data) {
            if (section == "Roster") continue;
            if (std::find(order.begin(), order.end(), section) != order.end()) continue;
            leftovers.push_back(section);
        }
        std::sort(leftovers.begin(), leftovers.end());
        for (const std::string& id : leftovers) AddSection(cfg, id, false);

        m_loaded = !m_entries.empty();
        if (!m_loaded)
            m_warnings.push_back("roster: no valid characters found");
        return m_loaded;
    }

    bool Loaded() const { return m_loaded; }
    const std::vector<CharacterDef>& All() const { return m_entries; }
    const std::vector<std::string>& Warnings() const { return m_warnings; }

    const CharacterDef* Find(const std::string& id) const {
        auto it = m_index.find(id);
        return (it == m_index.end()) ? nullptr : &m_entries[it->second];
    }

    const CharacterDef* FindByMesh(const std::string& mesh) const {
        if (mesh.empty()) return nullptr;
        for (const auto& c : m_entries)
            if (c.mesh == mesh) return &c;
        return nullptr;
    }

    // Resolve a stored profile value to a valid roster id. Empty means "use the
    // engine default" (the Player .ozls def / Plague Arcanist fallback).
    std::string ResolveSelection(const std::string& stored) const {
        if (stored.empty()) return {};
        if (Find(stored)) return stored;
        if (const CharacterDef* byMesh = FindByMesh(stored)) return byMesh->id;
        return {};
    }

    // A built-in entry used when the file is missing entirely. Its mesh is empty
    // so callers fall through to the Player def, which is the same chain the
    // renderer used before the roster existed.
    static CharacterDef MakeDefault() {
        CharacterDef c;
        c.id = "default";
        c.name = "Default";
        c.meshType = "static";
        return c;
    }

    const CharacterDef& Default() const {
        static const CharacterDef fallback = MakeDefault();
        return m_entries.empty() ? fallback : m_entries.front();
    }

    static bool IsValidId(const std::string& id) {
        if (id.empty() || id.size() > 31) return false;
        for (unsigned char c : id) {
            if (!(std::isalnum(c) || c == '_' || c == '-')) return false;
        }
        return true;
    }

private:
    void AddSection(IniConfig& cfg, const std::string& rawId, bool listed) {
        std::string id = Trim(rawId);
        if (!IsValidId(id)) {
            m_warnings.push_back("roster: invalid character id '" + rawId + "'");
            return;
        }
        if (m_index.count(id)) {
            m_warnings.push_back("roster: duplicate character id '" + id + "'");
            return;
        }
        if (!cfg.data.count(id)) {
            if (listed) m_warnings.push_back("roster: missing section '" + id + "'");
            return;
        }

        CharacterDef c;
        c.id = id;
        c.name = Trim(cfg.Get(id.c_str(), "name", ""));
        c.mesh = Trim(cfg.Get(id.c_str(), "mesh", ""));
        c.texture = Trim(cfg.Get(id.c_str(), "texture", ""));
        c.meshType = Trim(cfg.Get(id.c_str(), "mesh_type", "static"));
        c.portrait = Trim(cfg.Get(id.c_str(), "portrait", ""));
        c.bio = Trim(cfg.Get(id.c_str(), "bio", ""));
        c.animIdle = Trim(cfg.Get(id.c_str(), "anim_idle", ""));
        c.animChase = Trim(cfg.Get(id.c_str(), "anim_chase", ""));
        c.accent = std::clamp(cfg.GetInt(id.c_str(), "accent", 0), 0, 7);

        if (c.name.empty()) {
            m_warnings.push_back("roster: '" + id + "' has no name");
            return;
        }
        if (c.mesh.empty()) {
            m_warnings.push_back("roster: '" + id + "' has no mesh");
            return;
        }
        if (c.meshType != "skeletal") c.meshType = "static";

        m_index[id] = (int)m_entries.size();
        m_entries.push_back(std::move(c));
    }

    static std::string Trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return {};
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    static void SplitList(const std::string& s, std::vector<std::string>& out) {
        std::string cur;
        for (char c : s) {
            if (c == ',') {
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

    std::vector<CharacterDef> m_entries;
    std::unordered_map<std::string, int> m_index;
    std::vector<std::string> m_warnings;
    bool m_loaded = false;
};

} // namespace angelsmenu
