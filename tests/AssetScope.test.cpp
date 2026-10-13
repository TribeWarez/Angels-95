// Headless tests for Resources/AssetScope.cpp - the two-root (GameData)/(Packages)
// asset tree shared by the Model Browser and the Texture Manager.
//
// WHY THIS SUITE EXISTS. This is the first automated coverage the asset browser has
// ever had. BuildAssetScope was declared in UI/UiPanels.hpp and implemented inside
// UI/Panels/TexturePanel.cpp while UI/Panels/ModelPanel.cpp also called it - shared
// code living inside one of its two consumers, reachable only because the UI layer is
// a single translation unit. AGENTS.md asserted the two panels "cannot drift" because
// the tree is built once, but nothing checked that the tree is CORRECT. It is now
// Resources/AssetScope.{hpp,cpp}, and this suite pins the invariants AGENTS.md states
// in prose but which had no test behind them.
//
// Raylib-free and Win32-free by construction: AssetScope.hpp includes only <string>
// and <vector>. That is the property that makes this suite possible at all, and it is
// why the header carries no engine or platform include.

#include "AngelEd/Source/Resources/AssetScope.hpp"

#include <cstdio>
#include <string>
#include <vector>

static int g_pass = 0, g_fail = 0;

static void check(bool cond, const char* what) {
    if (cond) { g_pass++; return; }
    g_fail++;
    printf("  FAIL  %s\n", what);
}

// ---------------------------------------------------------------------------
// Tree navigation helpers. Deliberately written against the public shape only
// (label / entryIndex / children), because that is all a panel ever sees.
// ---------------------------------------------------------------------------

static const AssetScopeNode* Root(const AssetScopeNode& r, const char* label) {
    for (const auto& c : r.children) if (c.label == label) return &c;
    return nullptr;
}

static const AssetScopeNode* Child(const AssetScopeNode& n, const char* label) {
    for (const auto& c : n.children) if (c.label == label) return &c;
    return nullptr;
}

// Depth-first search for a leaf with the given display name.
static const AssetScopeNode* FindLeaf(const AssetScopeNode& n, const std::string& name) {
    if (n.entryIndex >= 0 && n.label == name) return &n;
    for (const auto& c : n.children) { if (const AssetScopeNode* f = FindLeaf(c, name)) return f; }
    return nullptr;
}

static int CountLeaves(const AssetScopeNode& n) {
    if (n.entryIndex >= 0) return 1;
    int t = 0;
    for (const auto& c : n.children) t += CountLeaves(c);
    return t;
}

// The full dotted path of a node, for readable assertions.
static void Path(const AssetScopeNode& n, const std::string& prefix, std::string& out) {
    std::string here = prefix.empty() ? n.label : prefix + "/" + n.label;
    if (n.entryIndex >= 0) { out += here + ";"; return; }
    for (const auto& c : n.children) Path(c, here, out);
}

static std::string AllPaths(const AssetScopeNode& root) {
    std::string out;
    for (const auto& c : root.children) Path(c, "", out);
    return out;
}

// Paths of everything BENEATH `parent`, excluding parent's own label. Use this to
// compare one scope's contents; AllPaths includes the node it is given.
static std::string PathsUnder(const AssetScopeNode& parent) {
    std::string out;
    for (const auto& c : parent.children) Path(c, "", out);
    return out;
}

static AssetScopeItem Loose(const char* name, const char* path) {
    AssetScopeItem i; i.name = name; i.path = path; i.fromPackage = false; return i;
}
static AssetScopeItem Pkg(const char* name, const char* key) {
    AssetScopeItem i; i.name = name; i.path = key; i.fromPackage = true; return i;
}

// ---------------------------------------------------------------------------

// The root is a single untyped node holding at most two typed scopes.
static void test_root_shape() {
    printf("root shape\n");
    std::vector<AssetScopeItem> items = {
        Loose("hero", "C:/repo/GameData/Pawn/Player/hero.glb"),
    };
    AssetScopeNode r = BuildAssetScope(items, "");
    check(r.label == "Assets", "root is labelled Assets");
    check(r.entryIndex == -1, "root is a folder (entryIndex -1)");
    check(r.children.size() == 1, "only the (GameData) scope appears when no packages");

    std::vector<AssetScopeItem> both = {
        Loose("hero", "C:/repo/GameData/Pawn/Player/hero.glb"),
        Pkg("hero", "Pawn/Player/hero.glb"),
    };
    AssetScopeNode r2 = BuildAssetScope(both, "");
    check(r2.children.size() == 2, "both scopes appear when there is a package entry");
    check(r2.children[0].label == "(GameData)", "(GameData) sorts before (Packages)");
    check(r2.children[1].label == "(Packages)", "(Packages) is second");
}

// A loose path scanned from disk is absolute, so everything up to and including the
// GameData anchor is hidden; package keys have no anchor and are used whole.
static void test_path_anchoring() {
    printf("path anchoring\n");
    std::vector<AssetScopeItem> items = {
        Loose("hero", "C:/repo/GameData/Pawn/Player/hero.glb"),
        Pkg("hero", "Pawn/Player/hero.glb"),
    };
    AssetScopeNode r = BuildAssetScope(items, "");

    const AssetScopeNode* gd = Root(r, "(GameData)");
    const AssetScopeNode* pk = Root(r, "(Packages)");
    check(gd && pk, "both scopes present");

    // The loose side must NOT show "C:" or "repo" or "GameData" as folders.
    check(gd && Child(*gd, "C:") == nullptr, "drive letter is not shown as a folder");
    check(gd && Child(*gd, "repo") == nullptr, "the repo directory is not shown");
    check(gd && Child(*gd, "GameData") == nullptr, "the GameData anchor itself is hidden");

    const AssetScopeNode* g = gd ? Child(*gd, "Pawn") : nullptr;
    const AssetScopeNode* p = g ? Child(*g, "Player") : nullptr;
    const AssetScopeNode* leaf = p ? Child(*p, "hero") : nullptr;
    check(leaf != nullptr, "loose file nests under Pawn/Player");
    check(leaf && leaf->entryIndex == 0, "loose leaf carries its index into the items vector");

    // Same file, same visible path - only the root differs.
    const AssetScopeNode* pg = pk ? Child(*pk, "Pawn") : nullptr;
    check(pg && Child(*pg, "Player") && Child(*pg, "Player")->children.size() == 1,
          "package key is used whole, with no anchor to strip");
    std::string gdPaths = gd ? PathsUnder(*gd) : "";
    std::string pkPaths = pk ? PathsUnder(*pk) : "";
    check(gdPaths == "Pawn/Player/hero;", "GameData side path is relative to GameData/");
    check(pkPaths == "Pawn/Player/hero;", "Packages side path is the package key verbatim");
}

// THE invariant from AGENTS.md: a package holding a same-named copy does not make the
// on-disk file disappear. Both must appear, under different roots.
static void test_same_name_in_both_scopes() {
    printf("same name on disk AND in a package\n");
    std::vector<AssetScopeItem> items = {
        Loose("hero", "C:/repo/GameData/Global/hero.glb"),
        Pkg("hero", "Global/hero.glb"),
    };
    AssetScopeNode r = BuildAssetScope(items, "");
    const AssetScopeNode* gd = Root(r, "(GameData)");
    const AssetScopeNode* pk = Root(r, "(Packages)");
    check(gd && FindLeaf(*gd, "hero") != nullptr, "the on-disk file is still listed");
    check(pk && FindLeaf(*pk, "hero") != nullptr, "the packaged copy is listed too");
    check(CountLeaves(r) == 2, "two distinct leaves, one per scope");

    // Their entryIndex values must differ, or selecting one would select the other.
    int gdIdx = gd ? FindLeaf(*gd, "hero")->entryIndex : -99;
    int pkIdx = pk ? FindLeaf(*pk, "hero")->entryIndex : -99;
    check(gdIdx != pkIdx, "the two leaves carry different entryIndex values");
    check((gdIdx == 0 && pkIdx == 1) || (gdIdx == 1 && pkIdx == 0),
          "entryIndex points back into the caller's own vector");
}

// A .oz* file ON DISK is still a real file - being a package extension must not make
// the enumerator treat it as packaged. fromPackage is the only input that decides.
static void test_package_extension_on_disk_is_not_a_package() {
    printf("a .oz* file on disk stays a loose file\n");
    std::vector<AssetScopeItem> items = {
        Loose("models", "C:/repo/System/Data/models.ozpak"),
        Pkg("hero", "Global/hero.glb"),
    };
    AssetScopeNode r = BuildAssetScope(items, "");
    const AssetScopeNode* gd = Root(r, "(GameData)");
    const AssetScopeNode* pk = Root(r, "(Packages)");
    check(gd && gd->children.size() == 1, "the .ozpak path lands under (GameData)");
    // ...and with no GameData segment in it, the path is used as-is from the root.
    std::string p = gd ? PathsUnder(*gd) : "";
    check(p.find("System") != std::string::npos,
          "a path with no GameData anchor is not truncated - nothing is hidden");
}

// Search is case-insensitive over BOTH name and path; empty folders are pruned.
static void test_search_filter() {
    printf("search filter\n");
    std::vector<AssetScopeItem> items = {
        Loose("Hero", "C:/repo/GameData/Pawn/Player/Hero.glb"),
        Loose("prop", "C:/repo/GameData/Global/Props/rock.glb"),
        Pkg("Hero", "Pawn/Player/Hero.glb"),
    };

    AssetScopeNode byName = BuildAssetScope(items, "hero");
    check(CountLeaves(byName) == 2, "matching by display name, case-insensitively, finds both copies");
    check(FindLeaf(byName, "Hero") != nullptr, "the matching leaf survives");

    // A path-only match: "props" appears in the path but in neither name.
    AssetScopeNode byPath = BuildAssetScope(items, "props");
    check(CountLeaves(byPath) == 1, "matching by path substring finds the prop");
    check(byPath.children.size() == 1, "the (Packages) scope is dropped entirely when empty");
    const AssetScopeNode* gd = Root(byPath, "(GameData)");
    check(gd && FindLeaf(*gd, "prop") != nullptr, "the path match is the loose prop");

    // No match at all: both scopes pruned, so the root has no children.
    AssetScopeNode none = BuildAssetScope(items, "zzzz-nothing");
    check(none.children.empty(), "an empty result set prunes both scopes");

    // Pruning must be recursive - an intermediate folder with no match disappears
    // entirely rather than surviving as an empty parent.
    check(Child(*gd, "Global") != nullptr, "the surviving branch keeps its folders");
    check(Child(*Child(*gd, "Global"), "Player") == nullptr,
          "a folder whose only leaf was filtered out is pruned");
}

// entryIndex must address the caller's vector, so it is the INDEX and not an ordinal
// among survivors. This is what treeview lParam carries.
static void test_entry_index_addresses_input_vector() {
    printf("entryIndex addresses the input vector\n");
    std::vector<AssetScopeItem> items = {
        Loose("a", "C:/repo/GameData/X/a.glb"),
        Loose("b", "C:/repo/GameData/X/b.glb"),
        Loose("c", "C:/repo/GameData/Y/c.glb"),
    };
    AssetScopeNode r = BuildAssetScope(items, "");
    const AssetScopeNode* gd = Root(r, "(GameData)");
    check(gd && FindLeaf(*gd, "a") && FindLeaf(*gd, "a")->entryIndex == 0, "a -> index 0");
    check(gd && FindLeaf(*gd, "b") && FindLeaf(*gd, "b")->entryIndex == 1, "b -> index 1");
    check(gd && FindLeaf(*gd, "c") && FindLeaf(*gd, "c")->entryIndex == 2, "c -> index 2");

    // Filtering must NOT renumber: the leaf still has to point at its own entry, or a
    // filtered tree would resolve to the wrong asset on click.
    AssetScopeNode f = BuildAssetScope(items, "c");
    const AssetScopeNode* fgd = Root(f, "(GameData)");
    check(fgd && FindLeaf(*fgd, "c") && FindLeaf(*fgd, "c")->entryIndex == 2,
          "filtering preserves the original index rather than renumbering from 0");
}

// Folders before leaves, each alphabetically, case-insensitively.
static void test_sort_order() {
    printf("sort order\n");
    std::vector<AssetScopeItem> items = {
        Loose("zebra", "C:/repo/GameData/zeta/zebra.glb"),
        Loose("Apple", "C:/repo/GameData/zeta/Apple.glb"),
        Loose("mango", "C:/repo/GameData/zeta/mango.glb"),
    };
    AssetScopeNode r = BuildAssetScope(items, "");
    const AssetScopeNode* gd = Root(r, "(GameData)");
    const AssetScopeNode* z = gd ? Child(*gd, "zeta") : nullptr;
    check(z && z->children.size() == 3, "all three leaves under one folder");
    if (z && z->children.size() == 3) {
        check(z->children[0].label == "Apple", "case-insensitive: Apple first");
        check(z->children[1].label == "mango", "mango second");
        check(z->children[2].label == "zebra", "zebra third");
    }

    // A folder must precede a leaf that sorts after it alphabetically.
    std::vector<AssetScopeItem> mixed = {
        Loose("aaa_leaf", "C:/repo/GameData/aaa_leaf.glb"),
        Loose("zzz_in_folder", "C:/repo/GameData/mmm/zzz_in_folder.glb"),
    };
    AssetScopeNode r2 = BuildAssetScope(mixed, "");
    const AssetScopeNode* gd2 = Root(r2, "(GameData)");
    check(gd2 && gd2->children.size() == 2, "folder and leaf are siblings");
    if (gd2 && gd2->children.size() == 2) {
        check(gd2->children[0].entryIndex < 0 && gd2->children[0].label == "mmm",
              "the folder comes first even though the leaf name sorts earlier");
        check(gd2->children[1].entryIndex >= 0, "the leaf comes last");
    }
}

// Degenerate inputs must not crash: an empty item list is what a browser shows before
// its first scan.
static void test_degenerate_inputs() {
    printf("degenerate inputs\n");
    AssetScopeNode empty = BuildAssetScope({}, "");
    check(empty.children.empty(), "an empty item list yields an empty root, not a crash");

    AssetScopeNode blank = BuildAssetScope({Loose("x", "")}, "");
    // An empty path yields no directory segments, so the leaf lands directly on the
    // scope. It must still be reachable.
    check(CountLeaves(blank) == 1, "an item with an empty path still produces one leaf");

    // Windows backslash separators must split the same as forward slashes: loose
    // paths scanned from disk use '\'.
    std::vector<AssetScopeItem> win = {
        Loose("hero", "C:\\repo\\GameData\\Global\\hero.glb"),
    };
    AssetScopeNode w = BuildAssetScope(win, "");
    const AssetScopeNode* gd = Root(w, "(GameData)");
    check(gd && Child(*gd, "Global") != nullptr, "backslash separators split like forward slashes");
    check(gd && Child(*gd, "GameData") == nullptr, "anchor still stripped with backslashes");
}

// The enumeration side of the same invariant. DedupeAssetItemsByName is what
// Resources/AssetScan.cpp calls once it has walked both the filesystem and the package
// loader, so this is where "a package copy never shadows the real file" is actually
// decided - the tree builder above only displays the result.
static void test_dedupe_prefers_real_file() {
    printf("dedupe by name, preferring the real file\n");

    // The exact case the old code had to guess at: a packaged copy listed FIRST.
    std::vector<AssetScopeItem> v = {
        Pkg("hero", "Global/hero.glb"),
        Loose("hero", "C:/repo/GameData/Global/hero.glb"),
    };
    DedupeAssetItemsByName(v);
    check(v.size() == 1, "a same-named package copy does not duplicate the real file");
    check(!v[0].fromPackage, "the surviving entry is the real file");
    check(v[0].path == "C:/repo/GameData/Global/hero.glb", "the real file's path survives");

    // And the reverse order, because the sort has to not care about input order.
    std::vector<AssetScopeItem> v2 = {
        Loose("hero", "C:/repo/GameData/Global/hero.glb"),
        Pkg("hero", "Global/hero.glb"),
    };
    DedupeAssetItemsByName(v2);
    check(v2.size() == 1 && !v2[0].fromPackage, "same result regardless of input order");

    // Distinct names are all kept, sorted by name.
    std::vector<AssetScopeItem> v3 = {
        Pkg("zebra", "z.glb"), Loose("Apple", "a.glb"), Pkg("mango", "m.glb"),
    };
    DedupeAssetItemsByName(v3);
    check(v3.size() == 3, "distinct names are all kept");
    check(v3[0].name == "Apple" && v3[1].name == "mango" && v3[2].name == "zebra",
          "the result is sorted by name");

    // Two real files with the same stem in different folders: the dedup key is the
    // display name, so this COLLAPSES them. That is the pre-existing behaviour of both
    // browsers (they both deduped on name) and is pinned here rather than left
    // implicit - it is a real limitation of a name-keyed asset browser.
    std::vector<AssetScopeItem> v4 = {
        Loose("rock", "C:/repo/GameData/A/rock.glb"),
        Loose("rock", "C:/repo/GameData/B/rock.glb"),
    };
    DedupeAssetItemsByName(v4);
    check(v4.size() == 1, "same-stem files in different folders collapse (name is the key)");

    // Two package copies with one name: stable_sort must still leave exactly one.
    std::vector<AssetScopeItem> v5 = {
        Pkg("rock", "A/rock.glb"), Pkg("rock", "B/rock.glb"),
    };
    DedupeAssetItemsByName(v5);
    check(v5.size() == 1, "two package copies of one name collapse to one");
    check(v5[0].path == "A/rock.glb",
          "stable_sort keeps the first package copy when neither is a real file");

    // Empty input is what a browser sees before its first scan.
    std::vector<AssetScopeItem> none;
    DedupeAssetItemsByName(none);
    check(none.empty(), "an empty list dedupes to empty");
}

int main() {
    printf("AssetScope tests\n\n");
    test_root_shape();
    test_path_anchoring();
    test_same_name_in_both_scopes();
    test_package_extension_on_disk_is_not_a_package();
    test_search_filter();
    test_entry_index_addresses_input_vector();
    test_sort_order();
    test_degenerate_inputs();
    test_dedupe_prefers_real_file();

    printf("\nResults: %d/%d passed\n", g_pass, g_pass + g_fail);
    return g_fail == 0 ? 0 : 1;
}