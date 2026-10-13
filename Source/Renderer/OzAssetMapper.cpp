#include "OzAssetMapper.hpp"
#include "../Package/PackageAssetLoader.hpp"
#include "../Log.hpp"
#include <cstring>
#include <cctype>
#include <cstdio>
#include <algorithm>
#include <vector>

// ---------------------------------------------------------------------------
// Case-insensitive filename matching helper
// ---------------------------------------------------------------------------
static bool iequals(const char* a, const char* b) {
    while (*a && *b) {
        if (std::tolower((unsigned char)*a) != std::tolower((unsigned char)*b))
            return false;
        a++; b++;
    }
    return *a == *b;
}

static void to_lower_inplace(std::string& s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
}

static std::string to_lower_copy(const char* s) {
    std::string out(s ? s : "");
    to_lower_inplace(out);
    return out;
}

// ---------------------------------------------------------------------------
// Fallback file probe: try path as-is, then lowercase, then .png fallback
// Returns the first path that exists, or the original path (caller handles).
// ---------------------------------------------------------------------------
static std::string probe_path(const char* baseDir, const char* name, const char* ext) {
    std::string path = std::string(baseDir) + "/" + name + ext;
    if (IsPathFile(path.c_str()))
        return path;

    // Try lowercase filename
    std::string lower = name;
    to_lower_inplace(lower);
    std::string path2 = std::string(baseDir) + "/" + lower + ext;
    if (IsPathFile(path2.c_str()))
        return path2;

    // Try .png fallback if ext was .gif
    if (strcmp(ext, ".gif") == 0) {
        std::string pngPath = std::string(baseDir) + "/" + name + ".png";
        if (IsPathFile(pngPath.c_str()))
            return pngPath;
        std::string pngPath2 = std::string(baseDir) + "/" + lower + ".png";
        if (IsPathFile(pngPath2.c_str()))
            return pngPath2;
    }

    // Return original probe — caller will get a fallback texture
    return path;
}

// ---------------------------------------------------------------------------
// Generate a small "missing" grid texture
// ---------------------------------------------------------------------------
static Texture2D MakeGridTexture() {
    const int w = 16, h = 16;
    Image img = GenImageChecked(w, h, 4, 4, (Color){80, 0, 80, 255}, (Color){40, 0, 40, 255});
    Texture2D t = LoadTextureFromImage(img);
    UnloadImage(img);
    return t;
}

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------
AssetMapper& AssetMapper::Instance() {
    static AssetMapper instance;
    return instance;
}

// ---------------------------------------------------------------------------
// Find entry by alias (case-insensitive)
// ---------------------------------------------------------------------------
AssetMapEntry* AssetMapper::Find(const char* alias) {
    for (auto& e : m_entries) {
        if (iequals(e.alias, alias))
            return &e;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Init — register all known Engine and Items textures
// ---------------------------------------------------------------------------
void AssetMapper::Init() {
    // Idempotent: a second Init() used to append a duplicate copy of every
    // entry (m_entries grew unbounded) and leave the previously loaded textures
    // orphaned on the GPU.
    UnloadAll();
    m_entries.clear();
    RegisterEngineTextures();
    RegisterItemTextures();
}

// ---------------------------------------------------------------------------
// Register engine icons (always .png, never .gif)
// ---------------------------------------------------------------------------
void AssetMapper::RegisterEngineTextures() {
    struct { const char* alias; } engineIcons[] = {
        {"Light"},
        {"Music"},
        {"PawnNode"},
        {"PlayerStart"},
        {"Portal"},
        {"Sound"},
        {"ZoneInfo"},
        {"ZoneWater"},
        {"ZoneLadder"},
        {"ZoneSky"},
        {"ZoneSound"},
        {"ZoneReverb"},
    };
    const char* baseDir = "GameData/Global/Engine";
    for (auto& ic : engineIcons) {
        m_entries.push_back({ic.alias, probe_path(baseDir, ic.alias, ".png"),
                             "engine", {0}, true});
    }
}

// ---------------------------------------------------------------------------
// Register item textures (some have .gif variants for animated billboards)
// ---------------------------------------------------------------------------
void AssetMapper::RegisterItemTextures() {
    // Items with both .png and .gif
    struct { const char* alias; } itemIcons[] = {
        {"Coin"},
        {"EnergyCrystal"},
        {"HealthVial"},
        {"ManaVial"},
        {"Powerup"},
    };
    const char* baseDir = "GameData/Tex/Global/Items";
    for (auto& ic : itemIcons) {
        // Try .gif first (animated billboard), fall back to .png
        m_entries.push_back({ic.alias, probe_path(baseDir, ic.alias, ".gif"),
                             "items", {0}, true});
    }

    // Key — file on disk is lowercase "key.png"
    m_entries.push_back({"Key", probe_path(baseDir, "Key", ".png"), "items", {0}, true});

    // alpha_key — file on disk is lowercase "alpha_key.png"
    m_entries.push_back({"alpha_key", probe_path(baseDir, "alpha_key", ".png"),
                         "items", {0}, true});
}

// ---------------------------------------------------------------------------
// ResolvePath — returns full relative path for alias, or nullptr
// ---------------------------------------------------------------------------
const char* AssetMapper::ResolvePath(const char* alias) const {
    if (!alias) return nullptr;
    for (auto& e : m_entries) {
        if (iequals(e.alias, alias))
            return e.path.c_str();
    }
    return nullptr;
}

// Shared magenta grid for unknown aliases. Lazily created ONCE: the old code
// minted a fresh grid per unknown alias into m_cache, and because those grids
// were never referenced from m_entries, neither UnloadCategory() nor
// UnloadAll() could ever free them - one leaked GL texture per unknown alias.
Texture2D AssetMapper::MissingGrid() {
    if (m_missingGrid.id == 0) m_missingGrid = MakeGridTexture();
    return m_missingGrid;
}

// ---------------------------------------------------------------------------
// GetTexture — load on first access, cache, fallback to grid
// ---------------------------------------------------------------------------
Texture2D AssetMapper::GetTexture(const char* alias) {
    if (!alias) return MissingGrid();
    const std::string key = to_lower_copy(alias);

    // Check cache first
    auto it = m_cache.find(key);
    if (it != m_cache.end())
        return it->second;

    // Find entry
    AssetMapEntry* e = Find(alias);
    if (!e) {
        // Unknown alias — the shared grid. Still cached (and still owned by
        // m_cache, so UnloadAll frees it) so we do not re-hit the map each draw.
        Texture2D grid = MissingGrid();
        m_cache[key] = grid;
        return grid;
    }

    // If texture already loaded on entry, return it
    if (e->texture.id > 0) {
        m_cache[key] = e->texture;
        return e->texture;
    }

    // Try loading the path (filesystem first, then packages)
    Texture2D tex = LoadTextureWithFallback(e->path.c_str());
    if (tex.id > 0)
        SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);

    // .gif -> .png fallback for items (package may have .png where .gif was probed)
    if (tex.id == 0 && strcmp(e->category, "items") == 0) {
        std::string pngPath = e->path;
        size_t dot = pngPath.rfind(".gif");
        if (dot != std::string::npos) {
            pngPath.replace(dot, 4, ".png");
            tex = LoadTextureWithFallback(pngPath.c_str());
            if (tex.id > 0)
                SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
        }
    }

    // Fallback to grid
    if (tex.id == 0) {
        OZ_WARN("AssetMapper: missing texture '%s' at '%s' — using grid", alias, e->path.c_str());
        tex = MissingGrid();
        e->ownsTexture = false;
    } else {
        e->ownsTexture = true;
    }

    e->texture = tex;
    m_cache[key] = tex;
    return tex;
}

// ---------------------------------------------------------------------------
// PreloadCategory — eagerly load every texture in a category
// ---------------------------------------------------------------------------
void AssetMapper::PreloadCategory(const char* category) {
    for (auto& e : m_entries) {
        if (category && strcmp(e.category, category) == 0)
            GetTexture(e.alias);
    }
}

// ---------------------------------------------------------------------------
// UnloadCategory
// ---------------------------------------------------------------------------
void AssetMapper::UnloadCategory(const char* category) {
    if (!category) return;
    for (auto& e : m_entries) {
        if (strcmp(e.category, category) == 0) {
            // The grid is shared by every unknown alias; never unload it here,
            // it outlives the category and UnloadAll() frees it once.
            if (e.texture.id > 0 && e.ownsTexture) {
                UnloadTexture(e.texture);
                e.texture = {0};
            }
            e.texture = {0};
        }
    }
    // Drop every cache row that resolves into this category. Comparing against
    // the (now reset) entries still works because we only need the alias set.
    std::vector<std::string> drop;
    for (const auto& e : m_entries) {
        if (strcmp(e.category, category) == 0) drop.push_back(to_lower_copy(e.alias));
    }
    for (const auto& k : drop) {
        // Only erase if the cached texture is not the shared grid.
        auto it = m_cache.find(k);
        if (it == m_cache.end()) continue;
        if (it->second.id != m_missingGrid.id) UnloadTexture(it->second);
        m_cache.erase(it);
    }
}

// ---------------------------------------------------------------------------
// UnloadAll
// ---------------------------------------------------------------------------
void AssetMapper::UnloadAll() {
    // m_cache is the single record of every texture this class owns, including
    // the shared grid and every previously-mapped fallback.
    // m_cache is the SINGLE owner record: every texture this class created is
    // inserted there exactly once (including the shared grid and every
    // unknown-alias fallback), and an entry's `texture` is only ever a copy of
    // a cached handle. Iterating both lists would double-free, so only the
    // cache is unloaded here and the entries are just reset.
    //
    // Dedupe by id: the shared grid is reachable from every unknown alias, so a
    // naive loop would unload it once per alias.
    std::vector<int> freed;
    freed.reserve(m_cache.size() + 1);
    for (auto& kv : m_cache) {
        if (kv.second.id <= 0) continue;
        if (std::find(freed.begin(), freed.end(), kv.second.id) != freed.end()) continue;
        freed.push_back(kv.second.id);
        UnloadTexture(kv.second);
    }
    if (m_missingGrid.id > 0 &&
        std::find(freed.begin(), freed.end(), m_missingGrid.id) == freed.end()) {
        UnloadTexture(m_missingGrid);
    }
    m_missingGrid = {0};
    m_cache.clear();

    for (auto& e : m_entries) {
        e.texture = {0};
        e.ownsTexture = true;
    }
}
