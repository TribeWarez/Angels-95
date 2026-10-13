#pragma once
#include "raylib.h"
#include <string>
#include <vector>
#include <unordered_map>

// ---------------------------------------------------------------------------
// AssetMapper — central path-alias registry & texture cache
//
// Aliases map short logical names to real paths under GameData/.
// Two built-in groups are registered at Init():
//   "items"  → GameData/Tex/Global/Items/<alias>.png
//   "engine" → GameData/Tex/Editor/<alias>.png
//
// Example usage:
//   AssetMapper::Instance().PreloadCategory("items");
//   Texture2D t = AssetMapper::Instance().GetTexture("Coin");
// ---------------------------------------------------------------------------

struct AssetMapEntry {
    const char* alias;
    // Owned, NOT a `const char*` borrowed from a side vector. This used to be
    // `m_strings.back().c_str()` pushed into a `std::vector<std::string>`, which
    // frees the backing buffer on every reallocation - so 11 of the 12 engine
    // entries held a dangling pointer and GetTexture() read freed heap to build
    // a texture path. Owning the string removes the whole class of bug.
    std::string path;
    const char* category;
    Texture2D texture;
    bool ownsTexture = true;   // false => the magenta grid, shared + freed once
};

class AssetMapper {
public:
    // Register all known entries (called once at startup). Safe to call again:
    // any previous registration and every texture it owned is released first.
    void Init();

    // Resolve an alias to its full relative path; returns nullptr if unknown
    const char* ResolvePath(const char* alias) const;

    // Get a cached texture, loading it on first access
    Texture2D GetTexture(const char* alias);

    // Bulk preload every entry belonging to the given category
    void PreloadCategory(const char* category);

    // Unload textures for a specific category
    void UnloadCategory(const char* category);

    // Unload every cached texture
    void UnloadAll();

    // Singleton
    static AssetMapper& Instance();

    // Access registered entries (for iteration / debug)
    const std::vector<AssetMapEntry>& Entries() const { return m_entries; }

    // Drop the alias->texture cache without unloading anything. Used when the
    // underlying asset source changed under us (a world reload re-scans
    // packages), so stale Texture2D handles cannot be handed out again.
    void InvalidateCache() { m_cache.clear(); }

private:
    std::vector<AssetMapEntry> m_entries;
    // alias(lower) -> texture. Keyed case-insensitively to match Find(); a
    // case-sensitive key made "Coin" and "coin" each load their own copy of one
    // texture. Holds every texture this class owns, including the shared grid,
    // so UnloadAll() can actually free them all.
    std::unordered_map<std::string, Texture2D> m_cache;
    Texture2D m_missingGrid{0};   // shared magenta grid; created lazily

    AssetMapEntry* Find(const char* alias);
    Texture2D MissingGrid();

    void RegisterItemTextures();
    void RegisterEngineTextures();
};
