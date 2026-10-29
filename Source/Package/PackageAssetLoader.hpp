#pragma once
// This include is the reason AngelServ has a build-time raylib dependency, and it is NOT
// trivially removable: Source/Script/LightningEntityRegistry.cpp includes this header for
// two pure methods (ListAllFiles, Find), but the *WithFallback helpers below return raylib
// types (Texture2D, Model, Music, Sound, Image, Shader), so hiding raylib.h behind
// OMEGA_TEST_ENV would break every test that compiles with it. Making the server genuinely
// raylib-free means splitting the pure resolver out of this header, not guarding it.
//
// Until then the dependency is declared rather than accidental: SERVER_FLAGS carries
// $(RAYLIB_INC), which the Linux arm of the Makefile now resolves.
#include "raylib.h"
#include "OzPackage.hpp"
#include "../Log.hpp"
#include <string>
#include <vector>
#include <filesystem>
#include <cstring>
#include <fstream>
#include <unordered_map>

namespace fs = std::filesystem;

class PackageAssetLoader {
public:
    static PackageAssetLoader& Instance() {
        static PackageAssetLoader instance;
        return instance;
    }

    void Init() {
        m_readers.clear();
        m_modelCache.clear();
        fs::path dataDir = fs::current_path() / "System" / "Data";
        if (!fs::exists(dataDir)) {
            // This used to be a bare `return`, and it is the single line that made a
            // wrong working directory undiagnosable: every packaged model, texture and
            // sound silently vanished with nothing in the log to say why. Name the path
            // AND the cwd, because the path is relative to it.
            OZ_WARN("package data directory not found: %s (cwd '%s') - "
                    "no .oz* packages will load. Run the editor from the project root "
                    "or from System/.",
                    dataDir.string().c_str(),
                    fs::current_path().string().c_str());
            return;
        }

        for (auto& entry : fs::recursive_directory_iterator(dataDir)) {
            if (entry.is_regular_file()) {
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext.size() >= 3 && ext.substr(0, 3) == ".oz") {
                    OzPackageReader reader;
                    if (reader.Open(entry.path().string().c_str())) {
                        m_readers.push_back(std::move(reader));
                        // Kept index-aligned with m_readers so ResolveInPackage() can
                        // name a package. OzPackageReader deliberately stores no path of
                        // its own, so the loader keeps the association.
                        m_readerFiles.push_back(entry.path().string());
                        OZ_INFO("PackageLoader: loaded %s (%u entries)",
                                entry.path().filename().string().c_str(),
                                m_readers.back().EntryCount());
                    }
                }
            }
        }
    }

    const uint8_t* Find(const char* filename, size_t& outSize) {
        outSize = 0;
        for (auto it = m_readers.rbegin(); it != m_readers.rend(); ++it) {
            const uint8_t* data = it->GetData(filename, outSize);
            if (data) return data;
        }
        return nullptr;
    }

    const uint8_t* FindBasename(const char* basename, size_t& outSize) {
        outSize = 0;
        for (auto it = m_readers.rbegin(); it != m_readers.rend(); ++it) {
            const OzPackageEntry* e = it->FindBasename(basename);
            if (e) {
                outSize = (size_t)e->sizeRaw;
                return (const uint8_t*)(it->GetData(e->filename, outSize));
            }
        }
        return nullptr;
    }

    // Basenames across every loaded package whose filename ends with `suffix`
    // (case-insensitive). For loaders that DISCOVER content rather than name one
    // file: the pawn .cfg defs live in pawns_defs.ozpak so a packaged-only
    // install keeps the authored defs instead of the hardcoded fallbacks.
    std::vector<std::string> ListAssetBasenames(const char* suffix) {
        std::vector<std::string> out;
        std::string suf = suffix;
        for (char& c : suf) c = (char)std::tolower((unsigned char)c);
        for (auto& r : m_readers) {
            uint32_t n = r.EntryCount();
            for (uint32_t i = 0; i < n; i++) {
                const OzPackageEntry* e = r.EntryAt(i);
                if (!e) continue;
                std::string low = e->filename;
                for (char& c : low) c = (char)std::tolower((unsigned char)c);
                if (low.size() < suf.size()) continue;
                if (low.compare(low.size() - suf.size(), suf.size(), suf) != 0) continue;
                const char* slash = strrchr(e->filename, '/');
                out.push_back(slash ? slash + 1 : e->filename);
            }
        }
        return out;
    }

    // Resolve a key inside ONE named package, matched on the package's own filename.
    //
    // Needed wherever a basename is ambiguous across packages. Per-world music is the
    // case that proves it: build-data.ps1 packs each world directory as
    // world_<Name>.ozone with the track keyed "Music/Main.mp3", so EVERY world package
    // holds an entry whose basename is "Main.mp3". A global FindBasename() walks every
    // loaded reader and returns whichever comes first, which is not the world being
    // loaded - so the package-aware path for a world track has to be scoped by name.
    const uint8_t* ResolveInPackage(const char* pkgFileName, const char* key, size_t& outSize) {
        outSize = 0;
        for (size_t i = 0; i < m_readers.size(); i++) {
            if (fs::path(m_readerFiles[i]).filename().string() != pkgFileName) continue;
            return m_readers[i].GetData(key, outSize);
        }
        return nullptr;
    }

    const uint8_t* ResolvePackageKey(const char* path, size_t& outSize) {
        outSize = 0;
        std::string normalized;
        const char* pkgPath = path;
        if (strncmp(path, "GameData/", 9) == 0) pkgPath = path + 9;
        normalized = pkgPath;
        for (auto& c : normalized) if (c == '\\') c = '/';

        const uint8_t* data = Find(normalized.c_str(), outSize);
        if (data) return data;

        const char* slash = normalized.rfind('/') != std::string::npos
            ? &normalized[normalized.rfind('/') + 1] : normalized.c_str();
        data = FindBasename(slash, outSize);
        if (data) return data;

        size_t firstSlash = normalized.find('/');
        if (firstSlash != std::string::npos) {
            std::string stripped = normalized.substr(firstSlash + 1);
            data = Find(stripped.c_str(), outSize);
            if (data) return data;
        }

        return nullptr;
    }

    bool ReadInto(const char* filename, std::vector<uint8_t>& out) {
        size_t sz;
        const uint8_t* data = ResolvePackageKey(filename, sz);
        if (!data) return false;
        out.assign(data, data + sz);
        return true;
    }

    std::string CacheModelFile(const char* path) {
        std::string normalized;
        const char* pkgPath = path;
        if (strncmp(path, "GameData/", 9) == 0) pkgPath = path + 9;
        normalized = pkgPath;
        for (auto& c : normalized) if (c == '\\') c = '/';

        auto it = m_modelCache.find(normalized);
        if (it != m_modelCache.end()) return it->second;

        size_t sz;
        const uint8_t* data = ResolvePackageKey(path, sz);
        if (!data) return "";

        fs::path cacheDir = fs::current_path() / "System" / "Cache";
        fs::create_directories(cacheDir);

        std::string cacheName = normalized;
        for (auto& c : cacheName) if (c == '/') c = '_';
        fs::path cachePath = cacheDir / cacheName;

        std::ofstream ofs(cachePath, std::ios::binary);
        if (!ofs) return "";
        ofs.write((const char*)data, sz);
        ofs.close();

        // A .gltf is JSON that references its buffers (and sometimes images) by
        // relative URI. Extracting only the .gltf leaves e.g. `Predator.bin`
        // missing next to the cached copy, and raylib then reports "Failed to
        // load mesh/material buffers" for a model that loads fine from a loose
        // checkout - the packaged install silently loses the pawn. Pull every
        // relative URI out of the same package into the cache directory.
        std::string ext = cachePath.extension().string();
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext == ".gltf") {
            std::string json((const char*)data, sz);
            std::string keyDir;
            size_t slash = normalized.find_last_of('/');
            if (slash != std::string::npos) keyDir = normalized.substr(0, slash + 1);
            size_t pos = 0;
            while ((pos = json.find("\"uri\"", pos)) != std::string::npos) {
                size_t colon = json.find(':', pos);
                if (colon == std::string::npos) break;
                size_t q1 = json.find('"', colon + 1);
                if (q1 == std::string::npos) break;
                size_t q2 = json.find('"', q1 + 1);
                if (q2 == std::string::npos) break;
                std::string uri = json.substr(q1 + 1, q2 - q1 - 1);
                pos = q2 + 1;
                if (uri.empty() || uri.rfind("data:", 0) == 0) continue;
                if (uri.find("..") != std::string::npos) continue; // no escapes
                size_t bsz = 0;
                const uint8_t* bdata = ResolvePackageKey((keyDir + uri).c_str(), bsz);
                if (!bdata) continue;
                size_t us = uri.find_last_of("/\\");
                std::string base = (us == std::string::npos) ? uri : uri.substr(us + 1);
                std::ofstream bf(cacheDir / base, std::ios::binary);
                if (bf) bf.write((const char*)bdata, bsz);
            }
        }

        m_modelCache[normalized] = cachePath.string();
        return cachePath.string();
    }

    int PackageCount() const { return (int)m_readers.size(); }

    // Load an additional package file at runtime (e.g. via editor "Add Package")
    bool LoadPackageFile(const char* path) {
        OzPackageReader reader;
        if (reader.Open(path)) {
            m_readers.push_back(std::move(reader));
            OZ_INFO("PackageLoader: added %s (%u entries)",
                    fs::path(path).filename().string().c_str(),
                    m_readers.back().EntryCount());
            return true;
        }
        return false;
    }

    // List all filenames across all loaded packages
    void ListAllFiles(std::vector<std::string>& names) const {
        names.clear();
        for (const auto& reader : m_readers) {
            for (const auto& e : reader.Entries()) {
                names.push_back(e.filename);
            }
        }
    }

private:
    std::vector<OzPackageReader> m_readers;
    // Source path of each entry in m_readers, same index. See ResolveInPackage().
    std::vector<std::string> m_readerFiles;
    std::unordered_map<std::string, std::string> m_modelCache;
};

inline Texture2D LoadTextureWithFallback(const char* path) {
    if (IsPathFile(path))
        return LoadTexture(path);

    size_t sz;
    const uint8_t* data = PackageAssetLoader::Instance().ResolvePackageKey(path, sz);
    if (data) {
        std::string normalized = path;
        for (auto& c : normalized) if (c == '\\') c = '/';
        const size_t dot = normalized.rfind('.');
        if (dot == std::string::npos) return Texture2D{0};   // no extension: cannot pick a decoder
        std::string ext = normalized.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        // Raylib's LoadImageFromMemory (which LoadTextureFromImage uses) needs
        // the extension *with* its leading dot, e.g. ".png". Passing "png"
        // without the dot tripped "IMAGE: Data format not supported" (the old
        // workaround wrote them to System/Cache just to use the file-based
        // loader). Every shipped texture is now PNG/GIF - the .dds library was
        // converted by tools/convert_dds.py - so ".dds" is no longer listed.
        const char* fmt = nullptr;
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
            ext == ".bmp" || ext == ".tga" || ext == ".gif")
            fmt = ext.c_str();
        if (fmt) {
            Image img = LoadImageFromMemory(fmt, data, (int)sz);
            Texture2D tex = LoadTextureFromImage(img);
            UnloadImage(img);
            return tex;
        }
    }
    return Texture2D{0};
}

inline Sound LoadSoundWithFallback(const char* path) {
    if (IsPathFile(path))
        return LoadSound(path);

    size_t sz;
    const uint8_t* data = PackageAssetLoader::Instance().ResolvePackageKey(path, sz);
    if (data) {
        std::string normalized = path;
        for (auto& c : normalized) if (c == '\\') c = '/';
        // Raylib memory loaders expect the extension including its dot.
        std::string ext = normalized.substr(normalized.rfind('.'));
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        Wave w = LoadWaveFromMemory(ext.c_str(), data, (int)sz);
        Sound s = LoadSoundFromWave(w);
        UnloadWave(w);
        return s;
    }
    return Sound{0};
}

inline Music LoadMusicWithFallback(const char* path) {
    if (IsPathFile(path))
        return LoadMusicStream(path);

    size_t sz;
    const uint8_t* data = PackageAssetLoader::Instance().ResolvePackageKey(path, sz);
    if (data) {
        std::string normalized = path;
        for (auto& c : normalized) if (c == '\\') c = '/';
        std::string ext = normalized.substr(normalized.rfind('.'));
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        return LoadMusicStreamFromMemory(ext.c_str(), data, (int)sz);
    }
    return Music{0};
}

// Load a music asset from ONE named package, by package filename + in-package key.
// Use this instead of LoadMusicWithFallback whenever the asset's basename is shared
// across packages - per-world "Music/Main.mp3" being the case that forced this.
inline Music LoadMusicFromPackage(const char* pkgFileName, const char* key) {
    size_t sz = 0;
    const uint8_t* data = PackageAssetLoader::Instance().ResolveInPackage(pkgFileName, key, sz);
    if (!data) return Music{0};
    std::string k = key;
    const size_t dot = k.rfind('.');
    if (dot == std::string::npos) return Music{0};
    std::string ext = k.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return LoadMusicStreamFromMemory(ext.c_str(), data, (int)sz);
}

inline Model LoadModelWithFallback(const char* path) {
    if (IsPathFile(path))
        return LoadModel(path);

    std::string cachePath = PackageAssetLoader::Instance().CacheModelFile(path);
    if (!cachePath.empty() && IsPathFile(cachePath.c_str()))
        return LoadModel(cachePath.c_str());

    return Model{0};
}

inline Font LoadFontWithFallback(const char* path) {
    if (IsPathFile(path))
        return LoadFont(path);

    size_t sz;
    const uint8_t* data = PackageAssetLoader::Instance().ResolvePackageKey(path, sz);
    if (data) {
        std::string normalized = path;
        for (auto& c : normalized) if (c == '\\') c = '/';
        std::string ext = normalized.substr(normalized.rfind('.'));
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        return LoadFontFromMemory(ext.c_str(), data, (int)sz, 32, 0, 0);
    }
    return Font{0};
}

inline std::string LoadFileTextWithFallback(const char* path) {
    if (IsPathFile(path)) {
        char* text = LoadFileText(path);
        if (text) {
            std::string result(text);
            UnloadFileText(text);
            return result;
        }
    }
    size_t sz;
    const uint8_t* data = PackageAssetLoader::Instance().ResolvePackageKey(path, sz);
    if (data) {
        return std::string((const char*)data, sz);
    }
    return "";
}

inline Image LoadImageWithFallback(const char* path) {
    if (IsPathFile(path))
        return LoadImage(path);

    size_t sz;
    const uint8_t* data = PackageAssetLoader::Instance().ResolvePackageKey(path, sz);
    if (data) {
        std::string normalized = path;
        for (auto& c : normalized) if (c == '\\') c = '/';
        const size_t dot = normalized.rfind('.');
        if (dot == std::string::npos) return Image{0};   // no extension: cannot pick a decoder
        std::string ext = normalized.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        // Extension (including the leading dot) is required by raylib's
        // LoadImageFromMemory.
        const char* fmt = nullptr;
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
            ext == ".bmp" || ext == ".tga" || ext == ".gif")
            fmt = ext.c_str();
        if (fmt)
            return LoadImageFromMemory(fmt, data, (int)sz);
    }
    return Image{0};
}

inline Shader LoadShaderWithFallback(const char* vsPath, const char* fsPath) {
    std::string vsCode, fsCode;
    bool vsFromPkg = false, fsFromPkg = false;

    if (vsPath && IsPathFile(vsPath)) {
        char* text = LoadFileText(vsPath);
        if (text) { vsCode = text; UnloadFileText(text); }
    } else if (vsPath) {
        size_t sz;
        const uint8_t* data = PackageAssetLoader::Instance().ResolvePackageKey(vsPath, sz);
        if (data) { vsCode.assign((const char*)data, sz); vsFromPkg = true; }
    }

    if (fsPath && IsPathFile(fsPath)) {
        char* text = LoadFileText(fsPath);
        if (text) { fsCode = text; UnloadFileText(text); }
    } else if (fsPath) {
        size_t sz;
        const uint8_t* data = PackageAssetLoader::Instance().ResolvePackageKey(fsPath, sz);
        if (data) { fsCode.assign((const char*)data, sz); fsFromPkg = true; }
    }

    if (!vsCode.empty() || !fsCode.empty()) {
        const char* vs = vsCode.empty() ? nullptr : vsCode.c_str();
        const char* fs = fsCode.empty() ? nullptr : fsCode.c_str();
        return LoadShaderFromMemory(vs, fs);
    }

    return LoadShader(vsPath, fsPath);
}
