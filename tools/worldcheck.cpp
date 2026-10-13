// worldcheck - static auditor for OZONE world files.
//
// Links the real OzoneParser (not a reimplementation) and reports geometry,
// lighting and placement problems that are cheap to find in text but expensive
// to spot in-engine:
//
//   * referenced texPath=/Mesh.Static paths that do not resolve on disk
//   * brushes and entities outside the declared world/zone bounds
//   * Mesh.Static models whose node transform leaves them below z=0
//   * lights buried inside solid brushes (they light nothing)
//   * playerstart inside a solid brush, or facing a wall
//   * zones missing an explicit name= (auto-generated names collide across
//     worlds because they are per-load counters)
//
// Build (raylib-free, same shape as the other test targets):
//   make worldcheck
//   ./worldcheck GameData/Worlds/World_endless_snow/World.ozone
//
// --glb-bounds <file.json> feeds exact model AABBs in so mesh placement can be
// checked without loading a GLB. tools/glb_bounds.py emits that format.
//
// --asset-refs additionally audits every path-valued key in GameData's
// .ozls/.cfg files (and the levelinfo skybox pair) against the filesystem. It
// is the gate for folder restructures: a moved asset whose reference was not
// updated is a dangling path, and that is the exact class of mistake a
// restructure makes. Paths whose basename is listed in tools/assetrefs-allow.txt
// (the register of pre-existing missing content) are reported as a count, not
// an error, so the check is honest about NEW breakage instead of failing on old
// debt. Invoked once by `make test` and the CI worldcheck step.

#include "../Source/World/OzoneParser.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {

int g_errors = 0;
int g_warnings = 0;

void err(const std::string& msg) { g_errors++; printf("  ERROR   %s\n", msg.c_str()); }
void warn(const std::string& msg) { g_warnings++; printf("  WARNING %s\n", msg.c_str()); }
void note(const std::string& msg) { printf("  info    %s\n", msg.c_str()); }

bool fileExists(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return f.good();
}

std::string dirName(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

std::string joinPath(const std::string& dir, const std::string& rel) {
    if (rel.empty()) return dir;
    if (rel[0] == '/' || (rel.size() > 1 && rel[1] == ':')) return rel; // absolute
    return dir + "/" + rel;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

struct Aabb {
    float lo[3] = {1e30f, 1e30f, 1e30f};
    float hi[3] = {-1e30f, -1e30f, -1e30f};
    bool valid = false;

    void add(float x, float y, float z) {
        lo[0] = std::min(lo[0], x); hi[0] = std::max(hi[0], x);
        lo[1] = std::min(lo[1], y); hi[1] = std::max(hi[1], y);
        lo[2] = std::min(lo[2], z); hi[2] = std::max(hi[2], z);
        valid = true;
    }
    // span [a,b] on one axis (0=X, 1=Y, 2=Z)
    void span(int axis, float a, float b) {
        lo[axis] = std::min(lo[axis], a);
        hi[axis] = std::max(hi[axis], b);
        valid = true;
    }
    bool contains(float x, float y, float z, float pad = 0.0f) const {
        return valid && x >= lo[0] - pad && x <= hi[0] + pad &&
               y >= lo[1] - pad && y <= hi[1] + pad &&
               z >= lo[2] - pad && z <= hi[2] + pad;
    }
};

// Brush volume in OZONE authoring order (z is the authoring vertical axis).
// Mirrors OzoneLoader::BuildFromPrimitive + ParseOzoneEntity exactly:
//   position = {args[0], args[2], args[1]}   (Z-up -> engine Y-up)
//   box    x y z w h d rot      -- centre-based
//   cyl    x y z rTop rBot h slices rot -- cz is the TOP, mesh sits bottom-at-0
//                                       and the loader re-centres by -= h/2
//   sph    x y z r segments
Aabb brushBounds(const OzonePrimitive& p) {
    Aabb b;
    const std::vector<float>& a = p.args;
    switch (p.type) {
        case OzonePrimitiveType::BOX:
            if (a.size() >= 6) {
                // Loader: position = {a0, a2, a1}, BuildBox(w=a3, h=a4, d=a5).
                // Engine X span = a3; engine Y (up) span = a4; engine Z span = a5.
                // Invert the Z-up swap: authoring Y = engine Z, authoring Z = engine Y.
                b.span(0, a[0] - a[3] * 0.5f, a[0] + a[3] * 0.5f);
                b.span(1, a[1] - a[5] * 0.5f, a[1] + a[5] * 0.5f);
                b.span(2, a[2] - a[4] * 0.5f, a[2] + a[4] * 0.5f);
            }
            break;
        case OzonePrimitiveType::CYLINDER:
            if (a.size() >= 6) {
                float r = std::max(a[3], a[4]), h = a[5];
                b.span(0, a[0] - r, a[0] + r);
                b.span(1, a[1] - r, a[1] + r);
                b.span(2, a[2] - h, a[2]);   // authored cz is the top
            }
            break;
        case OzonePrimitiveType::SPHERE:
            if (a.size() >= 4) {
                float r = a[3];
                for (int i = 0; i < 3; i++) b.span(i, a[i] - r, a[i] + r);
            }
            break;
        case OzonePrimitiveType::PYRAMID:
            if (a.size() >= 6) {
                b.span(0, a[0] - a[3] / 2, a[0] + a[3] / 2);
                b.span(1, a[1] - a[4] / 2, a[1] + a[4] / 2);
                b.span(2, a[2] - a[5], a[2]);
            }
            break;
        default:
            break;
    }
    return b;
}

// Authoring-space Z of a mesh placement (loader: position = {a0, a2, a1}).
inline void meshPos(const OzonePrimitive& p, float& x, float& y, float& z) {
    x = p.args[0];
    y = p.args[2];   // engine up
    z = p.args[1];
}

struct Box3 { float x0, y0, z0, x1, y1, z1; };

// Exact model AABB loaded from JSON produced by tools/glb_bounds.py.
struct ModelBounds {
    std::map<std::string, Box3> models;
};

// ============================================================================
// --asset-refs: audit every path-valued key in .ozls/.cfg (and the levelinfo
// skybox pair) against the filesystem.
// ============================================================================

// Keys whose value is an asset path. This is the vocabulary the engine reads
// through ResolveMeshAsset / Load*WithFallback; keys outside this set (stats,
// numbers, names) are not paths and must not be reported.
bool isAssetRefKey(const std::string& k) {
    static const char* kKeys[] = {
        "icon", "mesh", "mesh_override", "texture",
        "model_path", "model_texture", "sprite_path", "scream_path",
        "fire_sound", "swing_sound", "hit_sound", "reload_sound", "equip_sound",
        "cast_sound", "jump_sound", "land_sound", "walk_sound", "run_sound",
        "hurt_sound", "death_sound",
        "projectile_mesh", "viewmodel_mesh", "viewmodel_texture",
        "skybox", "set_skybox",
        "music_on_enter", "music_on_exit", "ambience_loop", "sfx_on_enter",
        "play_sound",
    };
    for (const char* s : kKeys)
        if (k == s) return true;
    return false;
}

std::string toLower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool endsWithCI(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    return toLower(s.substr(s.size() - suffix.size())) == toLower(suffix);
}

std::string baseName(const std::string& p) {
    size_t slash = p.find_last_of("/\\");
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

// Strip a trailing `// comment` (and `; comment` in .cfg). Paths never contain
// "//", so a plain find is safe.
std::string stripLineComment(std::string s) {
    size_t p = s.find("//");
    if (p != std::string::npos) s = s.substr(0, p);
    p = s.find(';');
    if (p != std::string::npos) s = s.substr(0, p);
    return trim(s);
}

std::string unquote(std::string v) {
    v = trim(v);
    if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') ||
                          (v.front() == '\'' && v.back() == '\'')))
        v = v.substr(1, v.size() - 2);
    return trim(v);
}

// A token from a levelinfo tail is a path if it has a slash or a known asset
// extension; the numeric game-rule args are not paths.
bool looksLikePath(const std::string& v) {
    if (v.empty()) return false;
    if (v.find('/') != std::string::npos || v.find('\\') != std::string::npos) return true;
    static const char* kExts[] = {
        ".png", ".dds", ".jpg", ".jpeg", ".bmp", ".tga", ".gif",
        ".obj", ".glb", ".gltf", ".iqm", ".vox", ".m3d",
        ".wav", ".mp3", ".ogg", ".ttf", ".ozanim", ".ozone",
    };
    for (const char* e : kExts)
        if (endsWithCI(v, e)) return true;
    return false;
}


// ---------------------------------------------------------------------------
// Case-EXACT resolution. Windows' filesystem is case-insensitive, so
// fileExists(".../key.png") happily accepts a file stored as "Key.png" - and
// the same reference then fails on Linux CI (`key.png` in PistonPart.ozls vs
// the on-disk Key.png was the case that proved it, and only the Linux job saw
// it until this check existed). Walk every component against the directory
// listing: exact walk first, then a case-insensitive walk purely to EXPLAIN a
// miss ("on disk it is Key.png").
// ---------------------------------------------------------------------------
enum class RefState { Exact, Missing, CaseMismatch };

RefState resolveCaseWalk(const std::string& raw, bool caseInsensitive, std::string* onDisk) {
    namespace fs = std::filesystem;
    std::string norm = raw;
    for (char& c : norm)
        if (c == '\\') c = '/';
    if (norm.empty()) return RefState::Exact;
    if (norm[0] == '/' || (norm.size() > 1 && norm[1] == ':')) {
        // Absolute path: no portable walk (and dev trees never use them).
        return fileExists(norm) ? RefState::Exact : RefState::Missing;
    }
    std::error_code ec;
    fs::path cur;
    size_t start = 0;
    while (start <= norm.size()) {
        size_t slash = norm.find('/', start);
        std::string seg = norm.substr(start, slash == std::string::npos
                                                ? std::string::npos : slash - start);
        if (!seg.empty() && seg != "." && seg != "..") {
            fs::path parent = cur.empty() ? fs::path(".") : cur;
            bool found = false;
            std::string spelling;
            for (auto& e : fs::directory_iterator(parent, ec)) {
                std::string name = e.path().filename().string();
                if (caseInsensitive) {
                    if (name.size() != seg.size()) continue;
                    bool same = true;
                    for (size_t k = 0; k < name.size(); k++)
                        if (std::tolower((unsigned char)name[k]) !=
                            std::tolower((unsigned char)seg[k])) { same = false; break; }
                    if (same) { found = true; spelling = name; break; }
                } else if (name == seg) {
                    found = true; spelling = name; break;
                }
            }
            if (!found) return RefState::Missing;
            if (spelling != seg && onDisk) {
                *onDisk = (cur / spelling).generic_string();
            }
            cur /= spelling;
        } else if (seg == "..") {
            cur /= "..";
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    if (!fs::is_regular_file(cur, ec)) return RefState::Missing;
    if (onDisk) *onDisk = cur.generic_string();
    return RefState::Exact;
}

// Exact-or-explained: Exact when every component matches the on-disk spelling;
// CaseMismatch when only a case-insensitive walk reaches it (which works on
// Windows and breaks on Linux); Missing otherwise.
RefState resolveRefCase(const std::string& value, const std::string& fileDir,
                        const std::string& repoRoot, std::string& attempted,
                        std::string& onDisk) {
    std::string v = value;
    for (char& c : v)
        if (c == '\\') c = '/';
    std::vector<std::string> cands;
    if ((v.size() > 1 && v[1] == ':') || (!v.empty() && v[0] == '/')) {
        cands.push_back(v);
    } else {
        cands.push_back(v);
        cands.push_back(joinPath(repoRoot, v));
        cands.push_back(joinPath(fileDir, v));
    }
    bool sawMismatch = false;
    for (const std::string& c : cands) {
        std::string od;
        RefState st = resolveCaseWalk(c, false, &od);
        if (st == RefState::Exact) return RefState::Exact;
        if (st == RefState::Missing) {
            std::string loose;
            if (resolveCaseWalk(c, true, &loose) == RefState::Exact) {
                sawMismatch = true;
                attempted = c;
                onDisk = loose;
            }
        }
    }
    return sawMismatch ? RefState::CaseMismatch : RefState::Missing;
}

// <root>/Worlds/<Name>/World.ozone -> <root>, with a GameData fallback.
// Probes for real files: an ifstream cannot open a directory, so testing
// fileExists("<root>/Worlds") would always fail.
std::string guessGameDataRoot(const std::string& worldPath) {
    std::string d1 = dirName(worldPath);      // .../Worlds/<Name>
    std::string d2 = dirName(d1);             // .../Worlds
    std::string d3 = dirName(d2);             // root
    if (baseName(d2) == "Worlds" &&
        fileExists(joinPath(joinPath(joinPath(d3, "Worlds"), baseName(d1)), "World.ozone")))
        return d3;
    if (fileExists("GameData/Launch.conf")) return "GameData";
    return std::string();
}

// C++ source literals: report CASE mismatches only. A literal that does not
// exist at all (synthetic test-style paths, examples in comments) stays silent,
// so the pass is zero-false-positive; what it catches is the real slip -
// `Items.hpp` asking for `Key.png` beside a `key.png` on disk, which works on
// Windows and silently loses the icon on Linux.
void auditCodeLiterals(const std::string& repoRoot) {
    namespace fs = std::filesystem;
    const char* kRoots[] = { "Source", "AngelEd/Source" };
    int checked = 0, mismatches = 0;
    std::error_code ec;
    for (const char* r : kRoots) {
        std::string root = joinPath(repoRoot, r);
        for (fs::recursive_directory_iterator it(root, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            std::string ext = toLower(it->path().extension().string());
            if (ext != ".cpp" && ext != ".hpp") continue;
            std::ifstream f(it->path());
            if (!f.is_open()) continue;
            std::string text((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
            std::string rel = it->path().generic_string();
            if (rel.rfind("./", 0) == 0) rel = rel.substr(2);
            size_t pos = 0;
            while ((pos = text.find("GameData/", pos)) != std::string::npos) {
                size_t endp = pos;
                while (endp < text.size()) {
                    char c = text[endp];
                    if (c == '"' || c == '\\' || c == ')' || c == '\n' ||
                        c == ' ' || c == '\t' || c == ';' || c == ',') break;
                    endp++;
                }
                std::string lit = text.substr(pos, endp - pos);
                pos = endp > pos ? endp : pos + 1;
                if (lit.size() < 12 || lit.find('.') == std::string::npos) continue;
                checked++;
                std::string attempted, onDisk;
                RefState st = resolveRefCase(lit, ".", repoRoot, attempted, onDisk);
                if (st != RefState::CaseMismatch) continue;
                mismatches++;
                int line = 1 + (int)std::count(text.begin(), text.begin() + (ptrdiff_t)pos, '\n');
                char buf[640];
                snprintf(buf, sizeof buf,
                         "code literal differs only in CASE: %s:%d  \"%s\"  ->  on disk "
                         "it is '%s' (Windows accepts this, Linux does not)",
                         rel.c_str(), line, lit.c_str(), onDisk.c_str());
                err(buf);
            }
        }
    }
    char buf[192];
    snprintf(buf, sizeof buf, "code literals: %d path literal(s) scanned, %d case mismatch(es)",
             checked, mismatches);
    note(buf);
}

void auditAssetRefs(const std::string& gameDataRoot) {
    namespace fs = std::filesystem;
    std::string repoRoot = dirName(gameDataRoot);
    std::string allowPath = joinPath(joinPath(repoRoot, "tools"), "assetrefs-allow.txt");

    std::set<std::string> allow;
    if (fileExists(allowPath)) {
        std::ifstream f(allowPath);
        std::string line;
        while (std::getline(f, line)) {
            std::string t = stripLineComment(line);
            if (t.empty() || t[0] == '#') continue;
            allow.insert(baseName(t));
        }
    }

    int defFiles = 0, refs = 0, allowed = 0, broken = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(gameDataRoot, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string p = it->path().generic_string();
        std::string ext = toLower(it->path().extension().string());
        bool isDef = (ext == ".ozls" || ext == ".cfg");
        bool isWorld = (ext == ".ozone");
        if (!isDef && !isWorld) continue;
        defFiles++;
        std::string fileDir = dirName(p);
        std::ifstream f(p);
        std::string line;
        int lineNo = 0;
        while (std::getline(f, line)) {
            lineNo++;
            std::string t = trim(line);
            if (t.empty()) continue;
            if (t.rfind("//", 0) == 0 || t[0] == '#' || t[0] == ';') continue;

            std::string key, value;
            size_t eq = t.find('=');
            if (eq != std::string::npos) {
                key = trim(t.substr(0, eq));
                value = t.substr(eq + 1);
            } else {
                size_t sp = t.find_first_of(" \t");
                if (sp == std::string::npos) continue;
                key = t.substr(0, sp);
                value = t.substr(sp + 1);
            }

            if (isWorld) {
                if (key != "levelinfo") continue;
                // levelinfo <7 game-rule numbers> <skybox> [<side skybox>] [gametype=]
                std::istringstream ts(value);
                std::string tok;
                int idx = 0;
                while (ts >> tok) {
                    idx++;
                    if (idx <= 7) continue;
                    if (tok.rfind("gametype=", 0) == 0) continue;
                    std::string v = unquote(stripLineComment(tok));
                    if (!looksLikePath(v)) continue;
                    refs++;
                    std::string attempted, onDisk;
                    RefState st = resolveRefCase(v, fileDir, repoRoot, attempted, onDisk);
                    if (st == RefState::Exact) continue;
                    if (st == RefState::CaseMismatch) {
                        broken++;
                        char buf[560];
                        snprintf(buf, sizeof buf,
                                 "asset reference differs only in CASE: %s:%d  levelinfo "
                                 "skybox = %s  ->  on disk it is '%s' (Windows accepts "
                                 "this, Linux does not)",
                                 p.c_str(), lineNo, v.c_str(), onDisk.c_str());
                        err(buf);
                        continue;
                    }
                    if (allow.count(baseName(v))) allowed++;
                    else {
                        broken++;
                        char buf[512];
                        snprintf(buf, sizeof buf,
                                 "unresolved asset reference: %s:%d  levelinfo skybox = %s",
                                 p.c_str(), lineNo, v.c_str());
                        err(buf);
                    }
                }
                continue;
            }

            if (!isAssetRefKey(key)) continue;
            std::string v = unquote(stripLineComment(value));
            if (v.empty()) continue;
            refs++;
            std::string attempted, onDisk;
            RefState st = resolveRefCase(v, fileDir, repoRoot, attempted, onDisk);
            if (st == RefState::Exact) continue;
            if (st == RefState::CaseMismatch) {
                broken++;
                char buf[620];
                snprintf(buf, sizeof buf,
                         "asset reference differs only in CASE: %s:%d  %s = %s  ->  on "
                         "disk it is '%s' (Windows accepts this, Linux does not)",
                         p.c_str(), lineNo, key.c_str(), v.c_str(), onDisk.c_str());
                err(buf);
                continue;
            }
            if (allow.count(baseName(v))) { allowed++; continue; }
            broken++;
            char buf[512];
            snprintf(buf, sizeof buf, "unresolved asset reference: %s:%d  %s = %s",
                     p.c_str(), lineNo, key.c_str(), v.c_str());
            err(buf);
        }
    }

    char buf[256];
    snprintf(buf, sizeof buf, "scanned %d def/world files, %d asset references", defFiles, refs);
    note(buf);
    auditCodeLiterals(repoRoot);
    if (allowed) {
        snprintf(buf, sizeof buf,
                 "%d unresolved reference(s) are pre-existing missing content "
                 "(tools/assetrefs-allow.txt)", allowed);
        note(buf);
    }
    if (broken == 0) {
        note("no new dangling asset references");
    } else {
        snprintf(buf, sizeof buf, "%d dangling asset reference(s) - fix the paths above", broken);
        err(buf);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: worldcheck <world.ozone> [--glb-bounds <file.json>] "
               "[--headroom-all] [--asset-refs]\n");
        return 2;
    }
    std::string worldPath = argv[1];
    std::string glbJson;
    // List every headroom trap instead of the first 8. The count is what CI
    // sees; a full list is what you need to actually go and fix them.
    bool headroomAll = false;
    // Audit GameData's .ozls/.cfg path keys against the filesystem. Run once
    // (not per world) - it is GameData-wide, not world-scoped.
    bool assetRefs = false;
    for (int i = 2; i < argc; i++) {
        if (std::strcmp(argv[i], "--glb-bounds") == 0 && i + 1 < argc) glbJson = argv[i + 1];
        else if (std::strcmp(argv[i], "--headroom-all") == 0)    headroomAll = true;
        else if (std::strcmp(argv[i], "--asset-refs") == 0)      assetRefs = true;
    }

    std::string worldDir = dirName(worldPath);
    printf("=== worldcheck: %s ===\n", worldPath.c_str());

    auto prims = OzoneParser::parse_file(worldPath);
    if (prims.empty()) {
        err("parser returned no primitives - file empty or unreadable?");
        printf("\n%d error(s), %d warning(s)\n", g_errors, g_warnings);
        return 1;
    }
    printf("  parsed %zu primitives\n\n", prims.size());

    // ---- minimal JSON scan for glb bounds: "path" + "min":[..] "max":[..]
    std::map<std::string, Box3> models;
    if (!glbJson.empty() && fileExists(glbJson)) {
        std::ifstream f(glbJson);
        std::stringstream ss;
        ss << f.rdbuf();
        std::string all = ss.str();
        size_t pos = 0;
        while (true) {
            size_t p = all.find("\"path\"", pos);
            if (p == std::string::npos) break;
            size_t q1 = all.find('"', all.find(':', p) + 1);
            size_t q2 = all.find('"', q1 + 1);
            std::string key = all.substr(q1 + 1, q2 - q1 - 1);
            size_t mp = all.find("\"min\"", q2);
            size_t xp = all.find("\"max\"", mp);
            if (mp == std::string::npos || xp == std::string::npos) break;
            float v[6] = {0};
            size_t lb = all.find('[', mp), rb = all.find(']', lb);
            {
                std::string nums = all.substr(lb + 1, rb - lb - 1);
                std::replace(nums.begin(), nums.end(), ',', ' ');
                std::istringstream is(nums);
                for (int k = 0; k < 3; k++) is >> v[k];
            }
            lb = all.find('[', xp); rb = all.find(']', lb);
            {
                std::string nums = all.substr(lb + 1, rb - lb - 1);
                std::replace(nums.begin(), nums.end(), ',', ' ');
                std::istringstream is(nums);
                for (int k = 0; k < 3; k++) is >> v[3 + k];
            }
            models[key] = {v[0], v[1], v[2], v[3], v[4], v[5]};
            pos = xp + 8;
        }
        printf("  loaded %zu model bounds from %s\n\n", models.size(), glbJson.c_str());
    }

    // ---------------- pass 1: references + bounds ----------------
    printf("--- references ---\n");
    std::set<std::string> missingRefs;
    std::vector<std::string> caseRefs;
    // Mirrors OzoneLoader::ResolveWorldAssetPath: try the path as-is, then a
    // repo-relative GameData/ path, then an absolute path, and only then treat
    // it as world-relative. Without the as-is/GameData cases a perfectly valid
    // repo-relative ref like GameData/Worlds/X/oztex/foo.dds gets joined onto
    // the world dir and reported missing. Case-exact on purpose: a texPath that
    // only resolves case-insensitively renders on Windows and vanishes on Linux.
    auto resolves = [&](const std::string& raw) {
        if (raw.empty()) return true;
        std::string attempted, onDisk;
        RefState st = resolveRefCase(raw, worldDir, ".", attempted, onDisk);
        if (st == RefState::CaseMismatch) caseRefs.push_back(raw + "  ->  " + onDisk);
        return st == RefState::Exact;
    };
    for (const auto& p : prims) {
        if (!p.texPath.empty()) {
            if (!resolves(p.texPath)) missingRefs.insert(p.texPath);
        }
        if (!p.meshPath.empty()) {
            if (!resolves(p.meshPath)) missingRefs.insert(p.meshPath);
        }
    }
    for (const auto& m : missingRefs) {
        char buf[512];
        snprintf(buf, sizeof buf, "referenced asset does not exist: %s", m.c_str());
        err(buf);
    }
    for (const auto& c : caseRefs) {
        char buf[560];
        snprintf(buf, sizeof buf,
                 "referenced asset differs only in CASE: %s (Windows accepts this, "
                 "Linux does not)", c.c_str());
        err(buf);
    }
    if (missingRefs.empty() && caseRefs.empty()) note("all referenced assets resolve");

    // ---------------- pass 2: solid brushes, playerstart, lights -------
    printf("\n--- geometry / lighting ---\n");
    std::vector<Aabb> solids;
    std::vector<std::string> solidDesc;
    // floor-ish brushes (very flat and wide) are walkable, not solid blockers
    for (const auto& p : prims) {
        if (p.csgOp != 1) continue;   // only 'add' contributes to collision
        if (p.type != OzonePrimitiveType::BOX &&
            p.type != OzonePrimitiveType::CYLINDER &&
            p.type != OzonePrimitiveType::SPHERE) continue;
        Aabb b = brushBounds(p);
        if (!b.valid) continue;
        float dx = b.hi[0] - b.lo[0], dy = b.hi[1] - b.lo[1], dz = b.hi[2] - b.lo[2];
        // A walkable pad/floor is a wide, thin slab. OZONE `box x y z w h d`
        // puts its height on the authoring Y axis, so a ground pad comes out
        // thin in Y, not Z (e.g. `add box 0 0 -0.5 210 1 210` is 210x1x210).
        // Treat any axis as the thin one.
        float smallest = std::min(dx, std::min(dy, dz));
        float largest = std::max(dx, std::max(dy, dz));
        bool floorLike = smallest <= 1.5f && largest > 20.0f;
        if (floorLike) continue;
        solids.push_back(b);
        const char* nm = p.type == OzonePrimitiveType::BOX ? "box"
                       : p.type == OzonePrimitiveType::CYLINDER ? "cyl" : "sph";
        solidDesc.push_back(nm);
    }
    printf("  %zu solid (non-floor) brushes\n", solids.size());

    // playerstart
    int starts = 0;
    for (const auto& p : prims) {
        if (p.type != OzonePrimitiveType::ENTITY_PLAYERSTART) continue;
        starts++;
        if (p.args.size() < 3) { err("playerstart has too few args"); continue; }
        // Authoring-space point. The loader remaps to {args[0], args[2], args[1]}
        // for ENGINE Y-up rendering, but brushBounds() is authored in OZONE
        // Z-up space, so containment tests must use the raw authoring triple.
        float ax = p.args[0], ay = p.args[1], az = p.args[2];
        float yaw = p.args.size() >= 4 ? p.args[3] : 0.0f;
        char buf[256];
        snprintf(buf, sizeof buf,
                 "playerstart #%d at authored (%.1f, %.1f, %.1f) up=%.1f yaw=%.0f",
                 starts, ax, ay, az, az, yaw);
        note(buf);

        int buried = 0;
        for (size_t i = 0; i < solids.size(); i++)
            if (solids[i].contains(ax, ay, az, -0.05f)) buried++;
        if (buried) {
            snprintf(buf, sizeof buf,
                     "playerstart is inside %d solid brush(es) - spawn is sealed", buried);
            err(buf);
        }
        // Player is 3u tall with a 2u eye height; report whether anything
        // encloses the spawn overhead.
        bool enclosed = false;
        for (size_t i = 0; i < solids.size(); i++) {
            const Aabb& s = solids[i];
            if (ax >= s.lo[0] && ax <= s.hi[0] &&
                ay >= s.lo[1] && ay <= s.hi[1] &&
                az >= s.lo[2] && az <= s.hi[2]) continue;
            bool overX = ax >= s.lo[0] && ax <= s.hi[0];
            bool overY = ay >= s.lo[1] && ay <= s.hi[1];
            if (overX && overY && s.lo[2] > az + 0.5f && s.lo[2] < az + 6.0f)
                enclosed = true;
        }
        if (!enclosed) note("spawn is open to the sky (nothing above it within 6u)");
    }
    if (starts == 0) err("no playerstart - the level has no defined first frame");
    if (starts > 1) warn("multiple playerstarts; only #1 is ever used");

    // lights
    int buriedLights = 0, dirLights = 0, pointLights = 0;
    for (const auto& p : prims) {
        // OzoneParser reads the light subtype into entityType and leaves args
        // as the pure float list; the loader uses arg(0)/arg(2)/arg(1).
        if (p.type != OzonePrimitiveType::ENTITY_LIGHT) continue;
        if (p.args.size() < 3) continue;
        float x = p.args[0], y = p.args[1], z = p.args[2]; // authoring (Z-up)
        if (p.entityType == "directional") {
            dirLights++;
            float len = std::sqrt(x * x + y * y + z * z);
            if (len < 1.0f)
                warn("directional light authored at/near the origin - it aims at the "
                     "world origin so it will have no defined direction");
            continue;
        }
        pointLights++;
        for (size_t i = 0; i < solids.size(); i++) {
            if (solids[i].contains(x, y, z, -0.05f)) {
                char buf[256];
                snprintf(buf, sizeof buf,
                         "point light at (%.1f, %.1f, %.1f) is buried inside a solid "
                         "%s - it lights nothing",
                         x, y, z, solidDesc[i].c_str());
                warn(buf);
                buriedLights++;
                break;
            }
        }
    }
    printf("  lights: %d directional, %d point (%d buried in solids)\n",
           dirLights, pointLights, buriedLights);

    // mesh placement vs ground and model bounds
    int meshCount = 0, belowGround = 0;
    for (const auto& p : prims) {
        if (p.type != OzonePrimitiveType::ENTITY_MESH_STATIC &&
            p.type != OzonePrimitiveType::ENTITY_MESH_SKELETAL) continue;
        meshCount++;
        if (p.args.size() < 4) { err("Mesh entity has too few args"); continue; }
        float x, y, z;
        meshPos(p, x, y, z);
        float scale = p.args.size() >= 5 ? p.args[4] : 1.0f;
        auto it = models.find(p.meshPath);
        if (it != models.end()) {
            // NOTE: model-local axes are NOT reliably Z-up. Blender-exported
            // assets carry a baked Y->Z node rotation, but the rock pack also
            // bakes a random tumble rotation and a 100x scale, so a model's
            // local bounds cannot be compared against world up directly. The
            // only placement mistake that is unambiguous from bounds alone is
            // a scale that cancels (or grossly over-cancels) the bake, so that
            // is what we check.
            //
            // tools/glb_bounds.py bakes node transforms, so a GLB authored in
            // centimetres at 100x arrives already at world scale: a "2m rock"
            // spans ~336 units. Multiplying that by scale=0.01 yields 3.4
            // units for a boulder, and by scale=0.01 on an already-100x model
            // yields a 0.12u building - effectively invisible.
            const Box3& b = it->second;
            float h = (b.z1 - b.z0) * scale;
            float span = std::max(b.x1 - b.x0, std::max(b.y1 - b.y0, b.z1 - b.z0));
            float hWorld = span * scale;
            char buf[360];
            if (scale > 0.0f && hWorld < 1.0f) {
                snprintf(buf, sizeof buf,
                         "mesh %s is %.2fu across after scale=%g (raw span %.1fu) - "
                         "a fraction of a unit is invisible. GLBs from these packs "
                         "bake a 100x node scale, so an extra scale=0.0N cancels it",
                         p.meshPath.c_str(), hWorld, scale, span);
                err(buf);
                belowGround++;
            } else if (scale > 0.0f && hWorld < 3.0f) {
                snprintf(buf, sizeof buf,
                         "mesh %s is only %.2fu across after scale=%g - likely too small",
                         p.meshPath.c_str(), hWorld, scale);
                warn(buf);
            }
            (void)h;
        }
        if (scale <= 0.0f)
            err("mesh has non-positive scale");
    }
    printf("  meshes: %d placed (%d implausibly small)\n", meshCount, belowGround);

    // ---------------- pass 2c: headroom ----------------
    //
    // A gap the player can STAND in but not JUMP in is a trap. The body is
    // Height 3.0 anchored at the FEET and a jump rises jumpSpeed^2 / (2*gravity)
    // = 81 / 36 = 2.25, so jumping freely needs 5.25 of clearance. A lower
    // ceiling does not merely clip the jump: OverlapsObstacle rejects the move
    // and RestorePosition puts the player back, so they appear to stick to the
    // wall they jumped beside - which reads as broken collision, not as a low
    // ceiling. Every EngineTest interior ceiling sat at 4.0 (a 1.0 clearance)
    // for exactly that reason.
    //
    // The test is a COLUMN scan: sample the floor plane, and at each sample take
    // the solids that actually contain the point, sort their z-spans, merge, and
    // look at every free interval. That formulation is what avoids the false
    // positives a naive "lowest solid above this surface" test produces: the
    // inside of a stepped pyramid, or a 1200-unit ground slab under a building,
    // is not a standing spot at all because the sample is buried inside another
    // solid - and a column sees that immediately while a pair test does not.
    {
        constexpr float kBodyHeight = 3.0f;               // PlayerMovement::Height
        constexpr float kJumpRise   = 2.25f;              // 9^2 / (2 * 18)
        constexpr float kNeedClear  = kBodyHeight + kJumpRise;
        constexpr uint32_t kNoBspCuts = 1u << 28;         // SURF_NO_BSP_CUTS

        std::vector<Aabb> all;
        for (const auto& p : prims) {
            // sub/intersect/de_resc carve rather than add, so they are not ceilings.
            if (p.csgOp == 2 || p.csgOp == 3 || p.csgOp == 4) continue;
            if (p.type != OzonePrimitiveType::BOX &&
                p.type != OzonePrimitiveType::CYLINDER &&
                p.type != OzonePrimitiveType::SPHERE &&
                p.type != OzonePrimitiveType::PYRAMID) continue;
            if (p.surface.def.flags & kNoBspCuts) continue;   // opts out of collision
            Aabb b = brushBounds(p);
            if (b.valid) all.push_back(b);
        }

        int traps = 0;
        if (!all.empty()) {
            std::vector<std::pair<float, float>> spans;   // (bottom, top) per column
            std::vector<std::pair<float, float>> hotspots; // dedupe by location

            // Sample each surface's footprint rather than the whole world: the
            // step is per-surface so a huge ground slab costs a bounded number of
            // samples while a small prop is still examined closely.
            for (const Aabb& surf : all) {
                const float top = surf.hi[2];
                const float fx = surf.hi[0] - surf.lo[0];
                const float fy = surf.hi[1] - surf.lo[1];
                if (fx < 2.0f || fy < 2.0f) continue;   // too small to stand on
                const float fmax = std::max(fx, fy);
                const float step = std::max(2.0f, std::min(8.0f, fmax / 64.0f));

                for (float x = surf.lo[0] + step * 0.5f; x < surf.hi[0]; x += step) {
                    for (float y = surf.lo[1] + step * 0.5f; y < surf.hi[1]; y += step) {
                        spans.clear();
                        for (const Aabb& b : all)
                            if (x >= b.lo[0] && x <= b.hi[0] &&
                                y >= b.lo[1] && y <= b.hi[1])
                                spans.emplace_back(b.lo[2], b.hi[2]);
                        if (spans.size() < 2) continue;   // nothing above: open sky
                        std::sort(spans.begin(), spans.end());

                        float floorTop = spans[0].second;
                        for (size_t k = 1; k < spans.size(); k++) {
                            if (spans[k].first > floorTop + 0.01f) {
                                const float gap = spans[k].first - floorTop;
                                if (gap >= kBodyHeight - 0.01f && gap < kNeedClear - 0.01f) {
                                    bool dup = false;
                                    for (const auto& h : hotspots) {
                                        if (std::fabs(h.first - x) < 4.0f &&
                                            std::fabs(h.second - y) < 4.0f) { dup = true; break; }
                                    }
                                    if (!dup) {
                                        hotspots.emplace_back(x, y);
                                        traps++;
                                        // A WARNING, not an error: the shipped worlds carry many
                                        // pre-existing instances (crates under a roof, wall tops
                                        // under a pergola), and failing CI on those would block
                                        // every unrelated change. Reported with a count and the
                                        // first few locations so it stays actionable.
                                        if (headroomAll || traps <= 8) {
                                            char buf[340];
                                            snprintf(buf, sizeof buf,
                                                     "headroom %.2fu at (%.1f, %.1f) over a floor top at "
                                                     "z=%.2f: a jump rises %.2f on a %.1f-tall body, so the "
                                                     "move is rolled back and the player sticks against the "
                                                     "wall beside them. Give it >= %.2fu of clearance",
                                                     gap, x, y, floorTop, kJumpRise, kBodyHeight, kNeedClear);
                                            warn(buf);
                                        }
                                    }
                                }
                            }
                            floorTop = std::max(floorTop, spans[k].second);
                        }
                    }
                }
            }
        }
        printf("  headroom traps: %d (a standable gap under %.2fu cannot be jumped in)\n",
               traps, kNeedClear);
    }
    // ---------------- pass 3: zones ----------------
    printf("\n--- zones ---\n");
    int zones = 0, unnamed = 0;
    Aabb worldBounds;
    for (const auto& p : prims) {
        if (p.type != OzonePrimitiveType::ENTITY_ZONE) continue;
        zones++;
        if (p.name.empty()) {
            char buf[128];
            snprintf(buf, sizeof buf,
                     "zone #%d (subtype '%s') has no name= - auto-generated names are "
                     "per-load counters and collide across worlds",
                     zones, p.entitySubType.c_str());
            err(buf);
            unnamed++;
        }
        if (p.args.size() >= 6) {
            // zone x1 y1 z1 x2 y2 z2, loader remaps {arg0, arg2, arg1} so the
            // authored third/fourth components are the vertical (Z) range.
            worldBounds.add(p.args[0], p.args[2], p.args[1]);
            worldBounds.add(p.args[3], p.args[5], p.args[4]);
        }
    }
    printf("  zones: %d (%d missing name=)\n", zones, unnamed);
    if (worldBounds.valid) {
        char buf[256];
        snprintf(buf, sizeof buf,
                 "world/zone bounds: x %.0f..%.0f  y %.0f..%.0f  z %.0f..%.0f",
                 worldBounds.lo[0], worldBounds.hi[0], worldBounds.lo[1],
                 worldBounds.hi[1], worldBounds.lo[2], worldBounds.hi[2]);
        note(buf);
        // anything sticking out of the sky zone is a likely authoring slip
        for (const auto& p : prims) {
            if (p.type != OzonePrimitiveType::ENTITY_MESH_STATIC) continue;
            if (p.args.size() < 4) continue;
            float x = p.args[0], y = p.args[2], z = p.args[1]; // engine Y-up (matches worldBounds)
            if (x < worldBounds.lo[0] - 1 || x > worldBounds.hi[0] + 1 ||
                y < worldBounds.lo[1] - 1 || y > worldBounds.hi[1] + 1) {
                char buf[256];
                snprintf(buf, sizeof buf,
                         "mesh %s at (%.0f,%.0f) lies outside the sky-zone bounds",
                         p.meshPath.c_str(), x, y);
                warn(buf);
            }
        }
    }

    // ---------------- pass 4: asset references (.ozls / .cfg / levelinfo) ----
    if (assetRefs) {
        printf("\n--- asset references ---\n");
        std::string gdRoot = guessGameDataRoot(worldPath);
        if (gdRoot.empty())
            warn("--asset-refs: could not locate the GameData root (run from the "
                 "repo root or pass a GameData/Worlds/<Name>/World.ozone path)");
        else
            auditAssetRefs(gdRoot);
    }

    printf("\n=== %d error(s), %d warning(s) ===\n", g_errors, g_warnings);
    return g_errors ? 1 : 0;
}