// ============================================================================
// Resources/AssetScope.cpp
//
// See AssetScope.hpp for why this exists and why it is its own translation unit.
// Verified as a pure move: every line came from UI/Panels/TexturePanel.cpp. The two
// helpers keep `static` (they are private to this file now, which is MORE correct
// than before - LowerAscii was a file-static in a 1000-line panel that happened to
// hold them).
// ============================================================================
#include "AssetScope.hpp"

#include <algorithm>
#include <cctype>
static std::string LowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)::tolower(c); });
    return s;
}

// Find or create the child of `parent` labelled `name`.
static AssetScopeNode& ChildFor(AssetScopeNode& parent, const std::string& name) {
    for (auto& c : parent.children)
        if (c.label == name && c.entryIndex < 0) return c;
    parent.children.push_back(AssetScopeNode{});
    parent.children.back().label = name;
    return parent.children.back();
}

// Split "dir/sub/file.ext" into its directory segments, dropping the leading
// anchor (drive letter, "GameData/", or a package key's own first segment).
static std::vector<std::string> DirSegments(const std::string& path) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

AssetScopeNode BuildAssetScope(const std::vector<AssetScopeItem>& items,
                               const std::string& search) {
    AssetScopeNode root;
    root.label = "Assets";

    AssetScopeNode gameData;  gameData.label  = "(GameData)";
    AssetScopeNode packages;  packages.label  = "(Packages)";

    const std::string needle = LowerAscii(search);
    const bool filtering = !needle.empty();

    for (size_t i = 0; i < items.size(); ++i) {
        const AssetScopeItem& it = items[i];
        if (filtering &&
            LowerAscii(it.name).find(needle) == std::string::npos &&
            LowerAscii(it.path).find(needle) == std::string::npos)
            continue;

        AssetScopeNode* scope = it.fromPackage ? &packages : &gameData;

        // Group by folder. Package keys are stored with '/' separators by
        // PackageAssetLoader; loose files may use '\' on Windows.
        std::vector<std::string> segs = DirSegments(it.path);
        if (!segs.empty()) segs.pop_back();          // drop the file name itself

        // A loose path scanned from disk is absolute
        // ("C:/repo/GameData/Tex/Global/Gun/x.glb"), so hide everything up to and
        // including the GameData anchor: the visible hierarchy is just the part
        // under GameData/. Package keys have no such anchor and are used whole.
        size_t start = 0;
        if (!it.fromPackage) {
            for (size_t k = 0; k < segs.size(); ++k) {
                if (LowerAscii(segs[k]) == "gamedata") { start = k + 1; break; }
            }
        }

        AssetScopeNode* cur = scope;
        for (size_t k = start; k < segs.size(); ++k)
            cur = &ChildFor(*cur, segs[k]);

        cur->children.push_back(AssetScopeNode{});
        cur->children.back().label = it.name;
        cur->children.back().entryIndex = (int)i;
    }

    // Prune folders that ended up with no leaves (common while filtering).
    struct Pruner {
        static bool Keep(AssetScopeNode& n) {
            if (n.entryIndex >= 0) return true;
            std::vector<AssetScopeNode> kept;
            for (auto& c : n.children) if (Keep(c)) kept.push_back(std::move(c));
            n.children = std::move(kept);
            return !n.children.empty();
        }
    };
    Pruner::Keep(gameData);
    Pruner::Keep(packages);

    // Sort: folders first, then leaves, each alphabetically (case-insensitive).
    struct Sorter {
        static bool Less(const AssetScopeNode& a, const AssetScopeNode& b) {
            bool af = a.entryIndex < 0, bf = b.entryIndex < 0;
            if (af != bf) return af;
            return LowerAscii(a.label) < LowerAscii(b.label);
        }
        static void Go(AssetScopeNode& n) {
            std::sort(n.children.begin(), n.children.end(), Less);
            for (auto& c : n.children) Go(c);
        }
    };
    Sorter::Go(gameData);
    Sorter::Go(packages);

    if (!gameData.children.empty()) root.children.push_back(std::move(gameData));
    if (!packages.children.empty()) root.children.push_back(std::move(packages));
    return root;
}

// Dedupe identity: display name PLUS lowercase extension.
//
// Keying on the bare stem collapsed `Bricks.bmp` onto `Bricks.png` (or vice
// versa) and kept only whichever sorted first, which is why an imported .bmp
// "disappeared" whenever a same-stem PNG already existed. The extension is taken
// from the path (the package key and the on-disk path both end in one), so a
// package copy of the SAME filename still collapses onto the real file.
static std::string AssetItemKey(const AssetScopeItem& a) {
    std::string base = a.path;
    const size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    const size_t dot = base.rfind('.');
    std::string ext = (dot == std::string::npos) ? std::string() : base.substr(dot);
    for (auto& c : ext)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return a.name + ext;
}

void DedupeAssetItemsByName(std::vector<AssetScopeItem>& items) {
    // stable_sort so two items with the same key AND the same fromPackage keep
    // enumeration order - the sort below only decides which of a same-named PAIR
    // is a package copy, not how two real files order against each other.
    std::stable_sort(items.begin(), items.end(),
        [](const AssetScopeItem& a, const AssetScopeItem& b) {
            const std::string ka = AssetItemKey(a);
            const std::string kb = AssetItemKey(b);
            if (ka != kb) return ka < kb;
            // false (0, a real file) sorts before true (1, a package copy), so after
            // std::unique keeps the FIRST of each run it is the real file that
            // survives. Reversing this silently makes packaged assets shadow local
            // ones, which is the bug this rule exists to prevent.
            return static_cast<int>(a.fromPackage) < static_cast<int>(b.fromPackage);
        });
    auto last = std::unique(items.begin(), items.end(),
        [](const AssetScopeItem& a, const AssetScopeItem& b) {
            return AssetItemKey(a) == AssetItemKey(b);
        });
    items.erase(last, items.end());
}