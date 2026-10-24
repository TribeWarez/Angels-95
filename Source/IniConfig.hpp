#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <exception>   // std::exception, caught by GetInt/GetFloat (see below)

class IniConfig {
public:
    using Section = std::unordered_map<std::string, std::string>;
    using Data = std::unordered_map<std::string, Section>;

    Data data;

    bool Load(const char* path) {
        data.clear();
        std::ifstream f(path);
        if (!f.is_open()) return false;
        return ParseStream(f);
    }

    // Parse already-loaded text. Used by package-aware loaders (the character
    // roster may live inside player.ozpak) and by headless tests, which must not
    // depend on the process working directory.
    bool ParseText(const std::string& text) {
        data.clear();
        std::istringstream f(text);
        return ParseStream(f);
    }

private:
    bool ParseStream(std::istream& f) {
        std::string line, currentSection;
        bool first = true;
        while (std::getline(f, line)) {
            // Strip a UTF-8 BOM. Windows PowerShell's `Set-Content -Encoding UTF8`
            // writes one, and so does Notepad's "UTF-8 with BOM" - and the BOM's
            // bytes are not in the trim set below, so the first section header
            // failed to parse and EVERY key in it was silently dropped. That was
            // cosmetic while the first section was the unused [Video] template
            // block; it is not once it holds real settings.
            if (first && line.size() >= 3 &&
                (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB &&
                (unsigned char)line[2] == 0xBF)
                line.erase(0, 3);
            first = false;
            // Trim
            line.erase(0, line.find_first_not_of(" \t\r\n"));
            size_t end = line.find_last_not_of(" \t\r\n");
            if (end != std::string::npos) line = line.substr(0, end + 1);
            // Skip empty / comment
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;
            // Section header
            if (line[0] == '[') {
                size_t close = line.find(']');
                if (close != std::string::npos)
                    currentSection = line.substr(1, close - 1);
                continue;
            }
            // Key=Value
            size_t eq = line.find('=');
            if (eq != std::string::npos) {
                std::string key = line.substr(0, eq);
                std::string val = line.substr(eq + 1);
                key.erase(0, key.find_first_not_of(" \t"));
                key.erase(key.find_last_not_of(" \t") + 1);
                val.erase(0, val.find_first_not_of(" \t"));
                val.erase(val.find_last_not_of(" \t\r") + 1);
                if (!currentSection.empty())
                    data[currentSection][key] = val;
            }
        }
        return true;
    }

public:
    bool Save(const char* path) {
        std::ofstream f(path);
        if (!f.is_open()) return false;
        for (auto& [section, kv] : data) {
            f << "[" << section << "]" << std::endl;
            for (auto& [key, val] : kv)
                f << key << "=" << val << std::endl;
            f << std::endl;
        }
        return true;
    }

    // Deterministic, hand-editable variant of Save().
    //
    // Save() walks two unordered_maps, so the section order and the key order
    // inside a section change from run to run. That is fine for a file only the
    // program reads, but System/Angels95.ini is documented as hand-editable, and
    // a file that reshuffles itself on every exit is not.
    //
    // `sectionOrder` lists the sections to write first, in that order. Keys are
    // written alphabetically inside every section. Any section NOT named is
    // appended afterwards (also sorted) rather than dropped, so a caller that
    // only owns part of the file cannot silently delete the rest of it - the
    // exact bug that used to wipe [MasterServers] on every client exit.
    bool SaveOrdered(const char* path,
                     const std::vector<std::string>& sectionOrder,
                     const char* header = nullptr) {
        std::ofstream f(path);
        if (!f.is_open()) return false;
        if (header && *header) f << header;
        for (const std::string& section : sectionOrder) {
            auto it = data.find(section);
            if (it != data.end()) WriteSection(f, it->first, it->second);
        }
        std::vector<std::string> rest;
        for (const auto& [section, kv] : data) {
            if (std::find(sectionOrder.begin(), sectionOrder.end(), section) == sectionOrder.end())
                rest.push_back(section);
        }
        std::sort(rest.begin(), rest.end());
        for (const std::string& section : rest)
            WriteSection(f, section, data.at(section));
        return true;
    }

    // Getters with default
    std::string Get(const char* section, const char* key, const char* def = "") {
        if (data.count(section) && data[section].count(key))
            return data[section][key];
        return def;
    }

    // True when the key is PRESENT, even if its value is empty. Get()/GetInt()
    // return the caller's default for both cases, which is not enough when a
    // setting has a legacy fallback section: `key=` present must still win over
    // the old location, rather than silently reading the fallback.
    bool Has(const char* section, const char* key) const {
        auto s = data.find(section);
        return s != data.end() && s->second.find(key) != s->second.end();
    }

    // std::stoi / std::stof THROW std::invalid_argument on a non-numeric string and
    // std::out_of_range on an unparsable one, and neither was caught. These files are
    // user-editable and this header is included by the CLIENT's startup path, so a
    // typo like `window_width = abc` in System/Angels95.ini threw straight out of
    // LoadClientSettings() — which runs BEFORE InitWindow, so the process died with no
    // window, no message and no log line, for a file the docs invite you to hand-edit.
    // A malformed value now falls back to the caller's default, which is the same
    // path a MISSING key already takes.
    int GetInt(const char* section, const char* key, int def = 0) {
        std::string v = Get(section, key);
        if (v.empty()) return def;
        try {
            return std::stoi(v);
        } catch (const std::exception&) {
            return def;
        }
    }

    float GetFloat(const char* section, const char* key, float def = 0.0f) {
        std::string v = Get(section, key);
        if (v.empty()) return def;
        try {
            return std::stof(v);
        } catch (const std::exception&) {
            return def;
        }
    }

    bool GetBool(const char* section, const char* key, bool def = false) {
        std::string v = Get(section, key);
        if (v.empty()) return def;
        std::transform(v.begin(), v.end(), v.begin(), ::tolower);
        return v == "true" || v == "1" || v == "yes";
    }

    // Setters
    void Set(const char* section, const char* key, const std::string& val) {
        data[section][key] = val;
    }

    void SetInt(const char* section, const char* key, int val) {
        data[section][key] = std::to_string(val);
    }

    void SetFloat(const char* section, const char* key, float val) {
        data[section][key] = std::to_string(val);
    }

    void SetBool(const char* section, const char* key, bool val) {
        data[section][key] = val ? "True" : "False";
    }

private:
    static void WriteSection(std::ostream& f, const std::string& name,
                             const Section& kv) {
        f << "[" << name << "]\n";
        std::vector<std::string> keys;
        keys.reserve(kv.size());
        for (const auto& [key, val] : kv) keys.push_back(key);
        std::sort(keys.begin(), keys.end());
        for (const std::string& key : keys)
            f << key << "=" << kv.at(key) << "\n";
        f << "\n";
    }
};
