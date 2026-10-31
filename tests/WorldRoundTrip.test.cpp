// OZONE round-trip / exporter-parser agreement tests — raylib-free.
//
// Every bug covered here is the same SHAPE: data the editor writes (or a shipped
// world authors) that the loader silently ignores, so opening the level looks
// wrong and the next save DELETES the key. The parser and the exporter are two
// hand-maintained files with no shared schema, so this suite is the schema:
//
//   * both spellings of every surface value
//   * the zone env enable flags (applyFog / applyAmbient)
//   * skybox topTex must not become the brush-wide texture
//   * a short trigger must stay short (the loader gates on 6 numbers)
//   * `door locked=<junk>` must report, not throw
//   * DRIFT GATE: every `key=` the exporter can emit must exist in the parser
//   * a shipped world that authors `surfAlpha=` must actually parse it
//
// Run from the repository root (make test does).

#include "../Source/World/OzoneParser.hpp"
#include "../Source/World/SurfaceFlags.hpp"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <set>
#include <regex>
#include <vector>

int test_count = 0, pass_count = 0;

static void report(bool ok, const char* name, const char* detail = nullptr) {
    test_count++;
    printf("  TEST %s... ", name);
    if (ok) { pass_count++; printf("PASS\n"); }
    else    printf("FAIL%s%s\n", detail ? ": " : "", detail ? detail : "");
}

static const OzonePrimitive* first_of(const std::vector<OzonePrimitive>& v,
                                      OzonePrimitiveType t) {
    for (const auto& p : v) if (p.type == t) return &p;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Surface value spellings
// ---------------------------------------------------------------------------
// The exporter and AGENTS.md use `surfAlpha=` / `surfCutoff=` / `surfGlowScale=`;
// the parser only knew `alpha=` / `cutoff=` / `glowScale=`. ~60 shipped lines use
// the surf* form, so water rendered opaque and every glow strip ignored its
// scale - and re-exporting dropped the keys entirely because surface.def still
// held the defaults.
static void test_surface_value_aliases() {
    bool ok = true;
    std::string detail;

    struct Case { const char* kwarg; float want; float oz::surface::SurfaceProps::*field; };
    const Case cases[] = {
        { "alpha=0.55",          0.55f, &oz::surface::SurfaceProps::alpha },
        { "surfAlpha=0.55",      0.55f, &oz::surface::SurfaceProps::alpha },
        { "cutoff=0.4",          0.4f,  &oz::surface::SurfaceProps::alphaCutoff },
        { "surfCutoff=0.4",      0.4f,  &oz::surface::SurfaceProps::alphaCutoff },
        { "glowScale=0.2",       0.2f,  &oz::surface::SurfaceProps::glowScale },
        { "surfGlowScale=0.2",   0.2f,  &oz::surface::SurfaceProps::glowScale },
    };
    for (const Case& c : cases) {
        auto e = OzoneParser::parse_string(
            std::string("add box 0 0 0 4 1 4 0 ") + c.kwarg + "\n");
        const OzonePrimitive* p = first_of(e, OzonePrimitiveType::BOX);
        const float got = p ? p->surface.def.*(c.field) : -999.0f;
        if (!p || std::fabs(got - c.want) > 1e-4f) {
            ok = false;
            detail = std::string("kwarg '") + c.kwarg + "' did not set the field";
            break;
        }
    }
    report(ok, "both surface-value spellings parse", ok ? nullptr : detail.c_str());
}

// Per-face overrides must accept the same spellings, since the exporter emits
// `face<n>_<field>=` built from the same table.
static void test_per_face_alias() {
    auto e = OzoneParser::parse_string(
        "add box 0 0 0 4 1 4 0 facepy_surfAlpha=0.25\n");
    const OzonePrimitive* p = first_of(e, OzonePrimitiveType::BOX);
    using namespace oz::surface;
    bool ok = p && p->surface.IsFaceOverridden(FACE_PY)
              && std::fabs(p->surface.face[FACE_PY].alpha - 0.25f) < 1e-4f;
    report(ok, "per-face surfAlpha alias parses");
}

// ---------------------------------------------------------------------------
// Zone env enable flags
// ---------------------------------------------------------------------------
// The 12-value positional env block turns fog AND ambient on. A zone that only
// wants one of them had no way to say so, and reloaded with fog at the panel
// default density (1.0 = near-solid). The explicit kwargs win.
static void test_zone_apply_flags() {
    // zone <type> min(3) max(3) intensity, then 12 env values:
    // fogR G B density start end ambR G B intensity reverbMix reverbDecay
    auto e = OzoneParser::parse_string(
        "zone water 0 0 0 10 10 10 1 179 179 204 0.004 10 100 40 40 48 1 0 0 "
        "applyFog=0 applyAmbient=1\n");
    const OzonePrimitive* p = first_of(e, OzonePrimitiveType::ENTITY_ZONE);
    bool ok = p && p->zoneApplyFog == 0 && p->zoneApplyAmbient == 1
              && p->args.size() >= 17;
    report(ok, "zone applyFog=/applyAmbient= kwargs parse");

    // Absent: -1 means "infer from the positional block" (both on), which is
    // the pre-existing behaviour every shipped zone relies on.
    auto e2 = OzoneParser::parse_string(
        "zone water 0 0 0 10 10 10 1 179 179 204 0.004 10 100 40 40 48 1 0 0\n");
    const OzonePrimitive* p2 = first_of(e2, OzonePrimitiveType::ENTITY_ZONE);
    report(p2 && p2->zoneApplyFog == -1 && p2->zoneApplyAmbient == -1,
           "zone flags absent stay unspecified");
}

// ---------------------------------------------------------------------------
// Skybox
// ---------------------------------------------------------------------------
// `topTex=` used to be stored in surface.def.texPath, which is the BRUSH-WIDE
// texture - so DrawSurface overrode all six faces with the ceiling image.
static void test_skybox_top_tex_is_separate() {
    auto e = OzoneParser::parse_string(
        "skybox Models/Sky.png 0 0 32 512 topTex=Models/Ceiling.png\n");
    const OzonePrimitive* p = first_of(e, OzonePrimitiveType::SKYBOX);
    bool ok = p && p->entityType == "Models/Sky.png"
              && p->skyTopTex == "Models/Ceiling.png"
              && p->surface.def.texPath.empty();
    report(ok, "skybox topTex does not become the brush-wide texture");
}

// ---------------------------------------------------------------------------
// Trigger arity
// ---------------------------------------------------------------------------
// The two flags are appended AFTER the bounds; appending them to a 4-number
// tuple made 6 - which the loader accepts as a box built out of garbage.
static void test_trigger_short_stays_short() {
    auto e = OzoneParser::parse_string("trigger Vol 5 5 0 once\n");
    const OzonePrimitive* p = first_of(e, OzonePrimitiveType::ENTITY_TRIGGER);
    // 3 bounds + nothing appended: stays below the loader's >= 6 gate.
    bool ok = p && p->args.size() < 6;
    char d[96];
    snprintf(d, sizeof(d), "args=%zu (must stay < 6)", p ? p->args.size() : 0);
    report(ok, "short trigger is not padded past the loader gate", ok ? nullptr : d);

    // A complete box still gets its flags.
    auto e2 = OzoneParser::parse_string(
        "trigger Vol 0 0 0 10 10 10 once team=2\n");
    const OzonePrimitive* p2 = first_of(e2, OzonePrimitiveType::ENTITY_TRIGGER);
    report(p2 && p2->args.size() == 8 && p2->args[6] == 1.0f && p2->args[7] == 2.0f,
           "complete trigger keeps once/team");
}

// ---------------------------------------------------------------------------
// Door
// ---------------------------------------------------------------------------
// `locked=yes` was a raw std::stof: an uncaught throw out of the parser, i.e.
// the process died with no diagnostic (0xC0000409 on the local toolchain).
static void test_door_locked_typo_does_not_throw() {
    auto e = OzoneParser::parse_string(
        "door Models/Door.glb 1 2 3 90 locked=yes\n"
        "add box 0 0 1 2 2 2 0\n");
    const OzonePrimitive* d = first_of(e, OzonePrimitiveType::ENTITY_DOOR);
    bool ok = d && !d->doorLocked && !e.empty()
              && e.back().type == OzonePrimitiveType::BOX;  // parsing continued
    report(ok, "door locked=<non-number> is reported, not fatal");
}

// ---------------------------------------------------------------------------
// Exporter/parser drift gate
// ---------------------------------------------------------------------------
// Extract every `key=` the editor's exporter can emit and require the parser to
// mention it. This is what would have caught surfAlpha= (and the next key
// someone adds to one file and forgets in the other) - the two lists are
// hand-maintained and had already drifted.
static void test_exporter_keys_are_parsed() {
    auto slurp = [](const char* path) -> std::string {
        std::ifstream f(path, std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    };
    const std::string exp = slurp("AngelEd/Source/Subsystems/OzoneExport.cpp");
    const std::string par = slurp("Source/World/OzoneParser.cpp");
    if (exp.empty() || par.empty()) {
        report(false, "exporter keys are parsed by OzoneParser",
               "could not read the sources (run from the repo root)");
        return;
    }

    std::set<std::string> keys;
    // output << " key=" ...
    std::regex plain("<<\\s*\" ([A-Za-z_][A-Za-z0-9_]*)=");
    // output << " " << pfx << "key=" ...
    std::regex perface("<<\\s*pfx\\s*<<\\s*\"([A-Za-z_][A-Za-z0-9_]*)=");
    for (auto it = std::sregex_iterator(exp.begin(), exp.end(), plain);
         it != std::sregex_iterator(); ++it)
        keys.insert((*it)[1].str());
    for (auto it = std::sregex_iterator(exp.begin(), exp.end(), perface);
         it != std::sregex_iterator(); ++it)
        keys.insert((*it)[1].str());

    std::vector<std::string> missing;
    for (const std::string& k : keys) {
        // Accepted spellings are string literals in the parser, either bare
        // ("panU") for the numF() table or with the '=' ("name=") for rfind.
        if (par.find("\"" + k + "\"") == std::string::npos &&
            par.find("\"" + k + "=") == std::string::npos)
            missing.push_back(k);
    }

    bool ok = !keys.empty() && missing.empty();
    std::string detail;
    if (keys.empty()) detail = "no keys extracted from the exporter";
    else if (!missing.empty()) {
        detail = "exporter writes keys the parser ignores:";
        for (const auto& m : missing) detail += " " + m;
    }
    report(ok, "every exporter key is parsed by OzoneParser", ok ? nullptr : detail.c_str());
}

// ---------------------------------------------------------------------------
// Shipped content
// ---------------------------------------------------------------------------
// A whole-file spot check on worlds that author the surf* spellings. Before the
// alias fix this returned zero brushes with alpha < 1.0, and the water planes
// rendered as opaque slabs.
static void test_shipped_worlds_parse_surf_values() {
    const char* kWorlds[] = {
        "GameData/Worlds/KryoArchipel_Severny/World.ozone",   // surfAlpha 0.55..0.9
        "GameData/Worlds/Sektor_01_SilentPines/World.ozone",  // surfAlpha + surfGlowScale
    };
    bool anyAlpha = false, anyGlowScale = false, anyFile = false;
    for (const char* w : kWorlds) {
        auto e = OzoneParser::parse_file(w);
        if (e.empty()) continue;
        anyFile = true;
        for (const auto& p : e) {
            if (p.surface.def.alpha < 0.999f) anyAlpha = true;
            if (p.surface.def.glowScale < 0.999f) anyGlowScale = true;
        }
    }
    bool ok = anyFile && anyAlpha && anyGlowScale;
    report(ok, "shipped worlds' surfAlpha/surfGlowScale actually load");
}

int main() {
    printf("WorldRoundTrip tests:\n");
    test_surface_value_aliases();
    test_per_face_alias();
    test_zone_apply_flags();
    test_skybox_top_tex_is_separate();
    test_trigger_short_stays_short();
    test_door_locked_typo_does_not_throw();
    test_exporter_keys_are_parsed();
    test_shipped_worlds_parse_surf_values();

    printf("\nResults: %d/%d passed\n", pass_count, test_count);
    return (pass_count == test_count) ? 0 : 1;
}
