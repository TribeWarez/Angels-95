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

#include "../Source/World/OzoneParser.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: worldcheck <world.ozone> [--glb-bounds <file.json>]\n");
        return 2;
    }
    std::string worldPath = argv[1];
    std::string glbJson;
    for (int i = 2; i + 1 < argc; i++)
        if (std::strcmp(argv[i], "--glb-bounds") == 0) glbJson = argv[i + 1];

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
    // Mirrors OzoneLoader::ResolveWorldAssetPath: try the path as-is, then a
    // repo-relative GameData/ path, then an absolute path, and only then treat
    // it as world-relative. Without the as-is/GameData cases a perfectly valid
    // repo-relative ref like GameData/Worlds/X/oztex/foo.dds gets joined onto
    // the world dir and reported missing.
    auto resolves = [&](const std::string& raw) {
        if (raw.empty()) return true;
        if (fileExists(raw)) return true;
        if (raw.rfind("GameData/", 0) == 0 || raw.rfind("GameData\\", 0) == 0)
            return fileExists(raw);
        if ((raw.size() > 1 && raw[1] == ':') || raw[0] == '/' || raw[0] == '\\')
            return fileExists(raw);
        return fileExists(joinPath(worldDir, raw));
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
    if (missingRefs.empty()) note("all referenced assets resolve");

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
                                        if (traps <= 8) {
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

    printf("\n=== %d error(s), %d warning(s) ===\n", g_errors, g_warnings);
    return g_errors ? 1 : 0;
}