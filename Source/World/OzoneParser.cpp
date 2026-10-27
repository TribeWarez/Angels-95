#include "OzoneParser.hpp"
#include "../Package/OzPackage.hpp"
#include "../Log.hpp"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <unordered_map>

std::string StripQuotes(std::string s) {
    if (s.size() >= 2 && s.front() == '\"' && s.back() == '\"')
        return s.substr(1, s.size() - 2);
    return s;
}

// Particle type names -> numeric index (0=none 1=snow 2=rain 3=void 4=psychic)
static int ParticleTypeNameToIndex(const std::string& s) {
    if (s == "NONE" || s == "none") return 0;
    if (s == "SNOW" || s == "snow") return 1;
    if (s == "RAIN" || s == "rain") return 2;
    if (s == "VOID" || s == "VOID_REALM" || s == "void") return 3;
    if (s == "PSYCHIC" || s == "PSYCHIC_REALM" || s == "psychic") return 4;
    try { return std::stoi(s); } catch (...) { return 0; }
}

std::vector<OzonePrimitive> OzoneParser::parse_file(const std::string& path) {
    // Peek at the magic: OZWN/.ozone files may be OzPackage containers
    // (System/Data/Zones/world_<name>.ozone) holding the world text inside.
    {
        uint32_t magic = 0;
        FILE* peek = fopen(path.c_str(), "rb");
        if (peek) {
            if (fread(&magic, sizeof(magic), 1, peek) != 1) magic = 0;
            fclose(peek);
        }
        if (magic == OZ_PACKAGE_MAGIC_WN || magic == OZ_PACKAGE_MAGIC_PK) {
            OzPackageReader reader;
            if (!reader.Open(path.c_str())) return {};
            // Find the world text inside the package (exact or basename)
            std::vector<uint8_t> text;
            size_t got = reader.Read("World.ozone", text);
            if (got == 0) got = reader.ReadBasename("World.ozone", text);
            if (got == 0) return {};
            return parse_string(std::string((const char*)text.data(), text.size()));
        }
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        fprintf(stderr, "OZONE: cannot open %s\n", path.c_str());
        return {};
    }

    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    return parse_string(content);
}

// Pull a possibly-quoted `key=value` payload out of the token stream.
//
// `texPath="my texture.png"` used to have a quote-aware branch that was
// UNREACHABLE: the plain `texPath=` test ran first and matched the quoted form
// too, so the value was truncated at the first space and silently pointed at a
// file that does not exist. The whole file is tokenised with `>>`, which cannot
// see quotes, so the quoted run has to be re-joined from the stream here.
static std::string ReadQuotedValue(std::istringstream& ls, const std::string& first) {
    // Unquoted: the token is the whole value.
    if (first.empty() || first.front() != '"') return first;

    std::string out = first.substr(1);            // drop the opening quote
    if (!out.empty() && out.back() == '"') {      // single-token form: key="v"
        out.pop_back();
        return out;
    }
    std::string t;
    while (ls >> t) {
        out += " ";
        out += t;
        if (!t.empty() && t.back() == '"') {
            out.pop_back();
            break;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Surface kwargs
//
// Brush-wide:
//   flags=N  surfAlpha=  surfCutoff=  surfGlow=(r,g,b)  surfGlowScale=
//   panU= panV=  surfTexSlot=  surfTex=
// Per-face (one family per face, so every token stays independently parseable
// and order-independent; only faces the author actually touched are emitted):
//   face<px|nx|py|ny|pz|nz>_<flags|alpha|cutoff|glow|glowScale|texSlot|tex|
//                               panU|panV|uvScaleU|uvScaleV|uvOffsetU|uvOffsetV>=value
//
// e.g.  facepy_flags=1024 facepy_tex=3 facepz_pan=0.05,-0.05
// ---------------------------------------------------------------------------
// Non-throwing numeric readers for kwarg VALUES.
//
// The parser called std::stof / std::stoi directly on these, so a typo like
// `gravity=abc` or a bare `effect=` threw std::invalid_argument straight out of
// parse_file with no handler anywhere - the process died with no diagnostic.
// That is the same class as the malformed-vec3 crash; the vec3 path was fixed
// with ReadVec3 and these scalars were left behind.
static bool TryFloat(const std::string& s, float& out) {
    try { out = std::stof(s); return true; } catch (...) { return false; }
}
static bool TryInt(const std::string& s, int& out) {
    try { out = std::stoi(s); return true; } catch (...) { return false; }
}

// Report a malformed surface kwarg value. `line` is 0 when the caller cannot
// supply one. These calls used to throw std::invalid_argument straight out of
// the parser, so `surfAlpha=abc` on any brush line killed the process.
static void WarnBadSurfaceValue(const char* key, const std::string& val, int line) {
    if (line > 0)
        OZ_WARN("OZONE line %d: surface %s='%s' is not a number", line, key, val.c_str());
    else
        OZ_WARN("OZONE: surface %s='%s' is not a number", key, val.c_str());
}

static bool ParseSurfaceField(const std::string& key, const std::string& val,
                              oz::surface::SurfaceProps& p, int line = 0) {
    using namespace oz::surface;
    auto f = [&](const char* k) { return key == k; };

    // A malformed value keeps the field's default and reports, rather than
    // throwing: one bad token must not cost the whole world.
    auto numF = [&](const char* k, float& dst) -> bool {
        if (!f(k)) return false;
        float v;
        if (TryFloat(val, v)) dst = v;
        else WarnBadSurfaceValue(k, val, line);
        return true;
    };
    auto numI = [&](const char* k, int& dst) -> bool {
        if (!f(k)) return false;
        int v;
        if (TryInt(val, v)) dst = v;
        else WarnBadSurfaceValue(k, val, line);
        return true;
    };

    if (f("flags")) {
        int v = 0;
        if (TryInt(val, v)) p.flags = (uint32_t)v;
        else WarnBadSurfaceValue("flags", val, line);
        return true;
    }
    if (numF("alpha", p.alpha))       return true;
    if (numF("cutoff", p.alphaCutoff)) return true;
    if (numF("glowScale", p.glowScale)) return true;
    if (numI("texSlot", p.texSlot))   return true;
    if (numI("surfTexSlot", p.texSlot)) return true;
    if (f("tex") || f("surfTex"))         { p.texPath = val; return true; }
    // Baked material graph: brush-wide `surfMaterial=` / `material=`, or
    // per-face `face<name>_material=`. Value is a material NAME, not a path.
    if (f("material") || f("surfMaterial")) { p.material = val; return true; }
    if (numF("panU", p.panU))         return true;
    if (numF("panV", p.panV))         return true;
    if (f("pan")) {
        // "u,v" form. Needed because the file is tokenised on whitespace, so a
        // `panU=` + `panV=` pair would have to be two tokens and cannot be
        // interleaved with other kwargs without ordering constraints.
        const size_t comma = val.find(',');
        if (comma != std::string::npos) {
            float a, b;
            if (TryFloat(val.substr(0, comma), a)) p.panU = a;
            else WarnBadSurfaceValue("pan", val, line);
            if (TryFloat(val.substr(comma + 1), b)) p.panV = b;
            else WarnBadSurfaceValue("pan", val, line);
        } else if (TryFloat(val, p.panU)) {
            // only U authored
        } else {
            WarnBadSurfaceValue("pan", val, line);
        }
        return true;
    }
    if (numF("uvScaleU", p.uvScaleU))   return true;
    if (numF("texScaleU", p.uvScaleU))  return true;
    if (numF("uvScaleV", p.uvScaleV))   return true;
    if (numF("texScaleV", p.uvScaleV))  return true;
    if (numF("uvOffsetU", p.uvOffsetU)) return true;
    if (numF("texOffsetU", p.uvOffsetU)) return true;
    if (numF("uvOffsetV", p.uvOffsetV)) return true;
    if (numF("texOffsetV", p.uvOffsetV)) return true;
    if (f("glow") || f("surfGlow")) {
        // "(r,g,b)" or "r,g,b". Parsed by stripping the parentheses and then
        // splitting on commas: the previous version looked for the NEXT comma to
        // bound each component, which meant the third component had no closing
        // delimiter and silently came out 0 (so a blue channel never arrived).
        // A malformed triple leaves the colour black rather than throwing out of
        // the parser and losing the whole line.
        std::string nums = val;
        const size_t open = nums.find('(');
        if (open != std::string::npos) {
            const size_t close = nums.find(')', open);
            nums = (close != std::string::npos) ? nums.substr(open + 1, close - open - 1)
                                                : nums.substr(open + 1);
        }
        float rgb[3] = {0.0f, 0.0f, 0.0f};
        size_t pos = 0;
        for (int i = 0; i < 3 && pos <= nums.size(); i++) {
            const size_t comma = nums.find(',', pos);
            const std::string part = (comma == std::string::npos)
                                   ? nums.substr(pos)
                                   : nums.substr(pos, comma - pos);
            if (part.empty()) break;
            try { rgb[i] = std::stof(part); } catch (...) { rgb[i] = 0.0f; }
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        p.glowR = rgb[0]; p.glowG = rgb[1]; p.glowB = rgb[2];
        return true;
    }
    return false;
}

// Returns true when `key` was consumed as a surface kwarg.
static bool ParseSurfaceKwarg(const std::string& key, const std::string& val,
                              oz::surface::BrushSurface& bs, int line = 0) {
    using namespace oz::surface;
    // Per-face? key must look like face<name>_<field>.
    if (key.rfind("face", 0) == 0) {
        size_t us = key.find('_', 4);
        if (us == std::string::npos) return false;
        const std::string faceName = key.substr(4, us - 4);
        const std::string field    = key.substr(us + 1);
        SurfaceFace f = FaceFromName(faceName.c_str());
        if (f == FACE_NONE) return false;   // unknown face name: not ours
        // Start from the brush default so a face only has to state what differs.
        SurfaceProps p = bs.Resolve(f);
        if (!ParseSurfaceField(field, val, p, line)) return false;
        bs.SetFace(f, p);
        return true;
    }
    // Brush-wide, including the legacy texScale*/texOffset* names.
    return ParseSurfaceField(key, val, bs.def, line);
}

static void ParseFloatsAndFlags(std::istringstream& ls, OzonePrimitive& prim) {
    std::string s;
    while (ls >> s) {
        try { prim.args.push_back(std::stof(s)); }
        catch (...) {
            // `key=value` kwargs. The existing texScale/texOffset/flags names are
            // folded into the brush-wide SurfaceProps so there is exactly ONE
            // place that owns UV + surface state; the legacy OzoneRenderable
            // fields are then seeded from it by the loader.
            size_t eq = s.find('=');
            if (eq != std::string::npos && eq > 0) {
                const std::string key = s.substr(0, eq);
                const std::string raw = s.substr(eq + 1);
                std::string val = (raw.size() >= 1 && raw.front() == '"')
                                ? ReadQuotedValue(ls, raw) : raw;
                if (ParseSurfaceKwarg(key, val, prim.surface, prim.line)) continue;
                // texPath is a quoted-or-bare path and is also the face/brush
                // texture; keep the dedicated field for the existing loaders.
                if (key == "texPath") { prim.texPath = val; continue; }
                // Unknown key=value: ignore, as before.
            }
        }
    }
}

// The legacy scalar fields (surfaceFlags / texScale* / texPath) are now VIEWS
// onto BrushSurface rather than a parallel copy written by the tokenizer. They
// stay because DrawWorldGeometry, DrawZoneGeometry, ApplyRenderableUV and
// ExportToOzone all still read them, and the shipped worlds rely on `flags=8`
// painted backdrops - but having one owner means a kwarg cannot be half-applied
// (previously `flags=` and the per-face flags would disagree).
static void DeriveLegacySurfaceFields(OzonePrimitive& prim) {
    using namespace oz::surface;
    // Only the flags the pre-existing pipeline understands are mirrored, so an
    // arbitrary decorative flag can never make a brush invisible or carve it out
    // of CSG by accident. oz::surface::kLegacyPipelineFlags is the single
    // definition of that mask.
    prim.surfaceFlags = DeriveLegacyFlags(prim.surface.def);
    prim.texScaleU  = prim.surface.def.uvScaleU;
    prim.texScaleV  = prim.surface.def.uvScaleV;
    prim.texOffsetU = prim.surface.def.uvOffsetU;
    prim.texOffsetV = prim.surface.def.uvOffsetV;
    if (!prim.surface.def.texPath.empty())
        prim.texPath = prim.surface.def.texPath;
}


std::vector<OzonePrimitive> OzoneParser::parse_string(const std::string& content) {
    std::vector<OzonePrimitive> prims;
    std::istringstream stream(content);
    std::string line;
    int lineNo = 0;

    while (std::getline(stream, line)) {
        lineNo++;
        if (line.empty() || line[0] == '#' || line.rfind("OZONE", 0) == 0 ||
            line.rfind("brushes", 0) == 0)
            continue;

        std::istringstream ls(line);
        std::string type_name;
        ls >> type_name;

        OzonePrimitive prim;
        prim.type = OzonePrimitiveType::UNKNOWN;
        prim.csgOp = 0;
        // Carried so any diagnostic can name the line the author must fix.
        prim.line = lineNo;

        // Check for CSG operation prefix: add/sub/intersect/deresc
        if (type_name == "add" || type_name == "sub" ||
            type_name == "intersect" || type_name == "deresc") {
            if (type_name == "add")        prim.csgOp = 1;
            else if (type_name == "sub")         prim.csgOp = 2;
            else if (type_name == "intersect")   prim.csgOp = 3;
            else if (type_name == "deresc")      prim.csgOp = 4;
            // Read the actual primitive type after the operation
            if (!(ls >> type_name)) continue;
        }

        if (type_name == "box") {
            prim.type = OzonePrimitiveType::BOX;
            // box x y z w h d rot [texSlot] [flags:N]
            ParseFloatsAndFlags(ls, prim);
        } else if (type_name == "cyl") {
            prim.type = OzonePrimitiveType::CYLINDER;
            // cyl x y z r_top r_bot h slices rot [texSlot] [flags:N]
            ParseFloatsAndFlags(ls, prim);
        } else if (type_name == "sph") {
            prim.type = OzonePrimitiveType::SPHERE;
            // sph x y z r [segments] [flags:N]
            ParseFloatsAndFlags(ls, prim);
        } else if (type_name == "pyr") {
            prim.type = OzonePrimitiveType::PYRAMID;
            // pyr x y z w d h [texSlot] [flags:N]
            ParseFloatsAndFlags(ls, prim);
        } else if (type_name == "pln") {
            prim.type = OzonePrimitiveType::PLANE;
            // pln x y z nx ny nz dist [flags:N]
            ParseFloatsAndFlags(ls, prim);
        } else if (type_name == "playerstart") {
            prim.type = OzonePrimitiveType::ENTITY_PLAYERSTART;
            // playerstart x y z yaw
            std::string s;
            while (ls >> s) {
                try { prim.args.push_back(std::stof(s)); }
                catch (...) { break; }
            }
        } else if (type_name == "pickup") {
            prim.type = OzonePrimitiveType::ENTITY_PICKUP;
            // pickup type x y z [respawnTime]
            if (ls >> prim.entityType) {
                std::string s;
                while (ls >> s) {
                    try { prim.args.push_back(std::stof(s)); }
                    catch (...) { break; }
                }
            }
        } else if (type_name == "zone") {
            prim.type = OzonePrimitiveType::ENTITY_ZONE;
            // zone zonetype minX minY minZ maxX maxY maxZ [intensity] [fog...]
            //     [gravity=] [jump=] [terminal=] [water_gravity=] [water_drag=]
            //     [swim_up=] [ladder_speed=] [fly_mult=] [name=label]
            if (ls >> prim.entitySubType) {
                std::string s;
                while (ls >> s) {
                    if (s.rfind("name=", 0) == 0) {
                        prim.name = s.substr(5);
                        continue;
                    }
                    try {
                        prim.args.push_back(std::stof(s));
                        continue;
                    } catch (...) {}
                    // Per-zone physics overrides (named kwargs; absent = defaults)
                    if (s.rfind("gravity_vector=", 0) == 0) {
                        // "(x,y,z)" or "x,y,z" authored in OZONE space (Z-up).
                        // The token arrives with no embedded spaces (the parser
                        // splits on whitespace), so commas delimit. Parens are
                        // stripped before splitting. Stored in ENGINE space
                        // (Y-up): engine(x,y,z) = ozone(x,z,y), matching the
                        // loader's bounds conversion, so PhysicsInfo stays
                        // engine-space end to end.
                        std::string val = s.substr(strlen("gravity_vector="));
                        std::string clean;
                        for (char c : val) if (c != '(' && c != ')') clean += c;
                        float comps[3] = {0.0f, 0.0f, 0.0f}; // ozone (x, y, z)
                        int n = 0;
                        size_t start = 0;
                        while (n < 3) {
                            size_t comma = clean.find(',', start);
                            std::string tok = clean.substr(start,
                                comma == std::string::npos ? std::string::npos : comma - start);
                            if (!tok.empty()) TryFloat(tok, comps[n++]);
                            else n++; // empty token (e.g. leading comma) -> keep 0
                            if (comma == std::string::npos) break;
                            start = comma + 1;
                        }
                        prim.hasPhysics = true;
                        // ozone (x, y, z) -> engine (x, z, y)
                        prim.physics.gravityVector = {comps[0], comps[2], comps[1]};
                        // `gravity` stays the magnitude of "down" (ozone -z).
                        prim.physics.gravity = -comps[2];
                        continue;
                    }
                    if (s.rfind("gravity=", 0) == 0 ||
                        s.rfind("jump=", 0) == 0 ||
                        s.rfind("terminal=", 0) == 0 ||
                        s.rfind("water_gravity=", 0) == 0 ||
                        s.rfind("water_drag=", 0) == 0 ||
                        s.rfind("swim_up=", 0) == 0 ||
                        s.rfind("ladder_speed=", 0) == 0 ||
                        s.rfind("fly_mult=", 0) == 0) {
                        size_t eq = s.find('=');
                        float v;
                        if (!TryFloat(s.substr(eq + 1), v)) {
                            // Used to throw straight out of the parser.
                            OZ_WARN("OZONE line %d: zone physics '%s' is not a number",
                                    lineNo, s.c_str());
                            continue;
                        }
                        std::string key = s.substr(0, eq + 1);
                        prim.hasPhysics = true;
                        if (key == "gravity=") {
                            prim.physics.gravity = v;
                            prim.physics.gravityVector = {0.0f, -v, 0.0f}; // straight down
                        }
                        else if (key == "jump=")        prim.physics.jumpSpeed = v;
                        else if (key == "terminal=")    prim.physics.terminalVelocity = v;
                        else if (key == "water_gravity=") prim.physics.waterGravity = v;
                        else if (key == "water_drag=")  prim.physics.waterDrag = v;
                        else if (key == "swim_up=")     prim.physics.swimUpSpeed = v;
                        else if (key == "ladder_speed=") prim.physics.ladderSpeed = v;
                        else if (key == "fly_mult=")    prim.physics.flySpeedMult = v;
                        continue;
                    }
                    // Unknown token: stop parsing the rest of the line (parity
                    // with the old behaviour for non-numeric tokens) - but say so,
                    // because on an entity line this is almost always a typo.
                    OZ_WARN("OZONE line %d: zone: trailing token '%s' ignored",
                            lineNo, s.c_str());
                    break;
                }
            }
        } else if (type_name == "npc") {
            prim.type = OzonePrimitiveType::ENTITY_NPC;
            // npc npctype x y z
            if (ls >> prim.entityType) {
                std::string s;
                while (ls >> s) {
                    try { prim.args.push_back(std::stof(s)); }
                    catch (...) { break; }
                }
            }
        } else if (type_name == "light") {
            prim.type = OzonePrimitiveType::ENTITY_LIGHT;
            // light point x y z r g b intensity radius [effect] [flare] [corona]
            // light spot x y z tx ty tz r g b intensity radius innerCone
            //          outerCone [effect] [flare] [corona]
            // light directional x y z r g b intensity [flare] [corona]
            // Optional attributes are also accepted as named kwargs, which is
            // the only unambiguous form: effect= flare= corona= name=
            if (ls >> prim.entityType) {
                std::string s;
                while (ls >> s) {
                    if (s.rfind("effect=", 0) == 0) {
                        if (!TryInt(s.substr(7), prim.lightEffect))
                            OZ_WARN("OZONE line %d: light effect '%s' is not a number",
                                    lineNo, s.c_str());
                        continue;
                    }
                    if (s.rfind("flare=", 0)  == 0) {
                        if (!TryInt(s.substr(6), prim.lightFlare))
                            OZ_WARN("OZONE line %d: light flare '%s' is not a number",
                                    lineNo, s.c_str());
                        continue;
                    }
                    if (s.rfind("corona=", 0) == 0) {
                        if (!TryInt(s.substr(7), prim.lightCorona))
                            OZ_WARN("OZONE line %d: light corona '%s' is not a number",
                                    lineNo, s.c_str());
                        continue;
                    }
                    if (s.rfind("name=", 0) == 0) {
                        prim.name = s.substr(5);
                        if (prim.name.size() >= 2 && prim.name.front() == '\"' &&
                            prim.name.back() == '\"')
                            prim.name = prim.name.substr(1, prim.name.size() - 2);
                        continue;
                    }
                    try { prim.args.push_back(std::stof(s)); }
                    catch (...) { break; }
                }
            }
        } else if (type_name == "portal") {
            prim.type = OzonePrimitiveType::ENTITY_PORTAL;
            // portal targetWorld minX minY minZ maxX maxY maxZ [spawnX spawnY spawnZ] [bidir]
            // Coordinates are OZONE Z-up; loaders convert to engine Y-up.
            if (ls >> prim.entityType) {
                std::string s;
                while (ls >> s) {
                    if (s == "bidir" || s == "1") { prim.args.push_back(1.0f); continue; }
                    if (s == "0") { prim.args.push_back(0.0f); continue; }
                    try { prim.args.push_back(std::stof(s)); }
                    catch (...) { break; }
                }
                // A portal is a BOX, not a point. Both consumers (OzOzoneLoader
                // and the server's HTTP serialiser) gate on a full 6-number
                // bounds tuple and drop anything shorter WITHOUT A WORD, so
                // `portal Foo 0 0 0` parses happily, logs nothing, never becomes
                // a portal - and the next editor save drops the line entirely,
                // because a line that never reached ZoneManager has nothing to
                // export. Report it here, where the line number is known.
                if (prim.args.size() < 6)
                    OZ_WARN("OZONE line %d: portal '%s' needs 6 box numbers "
                            "(minX minY minZ maxX maxY maxZ), got %d; the portal is dropped",
                            lineNo, prim.entityType.c_str(), (int)prim.args.size());
            }
        } else if (type_name == "emitter") {
            prim.type = OzonePrimitiveType::ENTITY_EMITTER;
            // emitter sound|music x y z   (Z-up coordinates)
            if (ls >> prim.entityType) {
                std::string s;
                while (ls >> s) {
                    try { prim.args.push_back(std::stof(s)); }
                    catch (...) { break; }
                }
            }
        } else if (type_name == "Mesh.Static" || type_name == "Mesh.Skeletal") {
            prim.type = (type_name == "Mesh.Skeletal")
                ? OzonePrimitiveType::ENTITY_MESH_SKELETAL
                : OzonePrimitiveType::ENTITY_MESH_STATIC;
            // Mesh.Static   <meshPath> x y z yaw [scale=N] [tex=path]
            // Mesh.Skeletal <meshPath> x y z yaw [scale=N] [tex=path] [anim=Clip] [speed=N]
            if (ls >> prim.meshPath) {
                std::string s;
                while (ls >> s) {
                    float mv;
                    if (s.rfind("scale=", 0) == 0) {
                        if (TryFloat(s.substr(6), mv)) prim.args.push_back(mv);
                        else OZ_WARN("OZONE line %d: mesh scale '%s' is not a number", lineNo, s.c_str());
                        continue;
                    }
                    if (s.rfind("tex=", 0) == 0) { prim.texPath = s.substr(4); continue; }
                    if (s.rfind("material=", 0) == 0) { prim.materialName = s.substr(9); continue; }
                    if (s.rfind("animfile=", 0) == 0) { prim.animFile = s.substr(9); continue; }
                    if (s.rfind("anim=", 0) == 0) { prim.animClip = s.substr(5); continue; }
                    if (s.rfind("speed=", 0) == 0) {
                        if (!TryFloat(s.substr(6), prim.animSpeed))
                            OZ_WARN("OZONE line %d: mesh speed '%s' is not a number", lineNo, s.c_str());
                        continue;
                    }
                    if (s == "wind" || s.rfind("wind=", 0) == 0) { prim.meshWind = true; continue; }
                    try { prim.args.push_back(std::stof(s)); }
                    catch (...) { break; }
                }
            }
        } else if (type_name == "WindZone") {
            prim.type = OzonePrimitiveType::ENTITY_WIND_ZONE;
            // WindZone minX minY minZ maxX maxY maxZ dirX dirY dirZ strength [freq]
            std::string s;
            while (ls >> s) {
                try { prim.args.push_back(std::stof(s)); }
                catch (...) { break; }
            }
        } else if (type_name == "door") {
            prim.type = OzonePrimitiveType::ENTITY_DOOR;
            // door <meshPath> x y z yaw [key=<name>] [anim=<clip>] [locked=1]
            std::string tok;
            if (ls >> tok) prim.meshPath = tok;
            float v;
            for (int i = 0; i < 5 && (ls >> v); i++) prim.args.push_back(v);
            std::string s;
            while (ls >> s) {
                if (s.rfind("key=", 0) == 0) { prim.doorKey = s.substr(4); }
                else if (s.rfind("anim=", 0) == 0) { prim.doorAnim = s.substr(5); }
                else if (s.rfind("locked=", 0) == 0) { prim.doorLocked = (std::stof(s.substr(7)) != 0.0f); }
                else {
                    try { prim.args.push_back(std::stof(s)); } catch (...) { break; }
                }
            }
        } else if (type_name == "trigger") {
            prim.type = OzonePrimitiveType::ENTITY_TRIGGER;
            // trigger <def> minX minY minZ maxX maxY maxZ [once] [team=N]
            // args[0..5] = bounds, args[6] = once (0/1), args[7] = team.
            if (ls >> prim.entityType) {
                std::string s;
                float once = 0.0f, team = 0.0f;
                while (ls >> s) {
                    if (s == "once") { once = 1.0f; continue; }
                    if (s.rfind("team=", 0) == 0) {
                        float tv;
                        if (TryFloat(s.substr(5), tv)) team = tv;
                        continue;
                    }
                    float v;
                    if (TryFloat(s, v)) prim.args.push_back(v);
                    else break;
                }
                const size_t boundsCount = prim.args.size();
                prim.args.push_back(once);
                prim.args.push_back(team);
                // Same silent-drop class as `portal`: the loader gates on a full
                // 6-number box, and the two flags appended just above would push
                // a SHORT tuple past that check - 4 bounds + once + team = 6
                // numbers, which reads as a valid box built out of garbage. Count
                // the bounds BEFORE appending so the report is honest.
                if (boundsCount < 6)
                    OZ_WARN("OZONE line %d: trigger '%s' needs 6 box numbers "
                            "(minX minY minZ maxX maxY maxZ), got %d; the trigger is dropped",
                            lineNo, prim.entityType.c_str(), (int)boundsCount);
            }
        } else if (type_name == "ParticleEmitter") {
            prim.type = OzonePrimitiveType::ENTITY_PARTICLE_EMITTER;
            // ParticleEmitter <type> x y z [rate life speed spread sizeStart sizeEnd
            //   r g b rEnd gEnd bEnd gravity radius dirX dirY dirZ yaw] [tex=path]
            // Plus optional `key=value` overrides of that tail (see
            // OzonePrimitive::particleOverrides). The indices below are the
            // positional slots the loader reads, so a kwarg and its slot name
            // the same thing.
            if (ls >> prim.entityType) {
                // name -> positional slot. Keep in step with the loader's
                // ENTITY_PARTICLE_EMITTER arg() indices.
                static const std::unordered_map<std::string, int> kSlots = {
                    {"rate", 3},   {"life", 4},   {"speed", 5},   {"spread", 6},
                    {"sizestart", 7}, {"sizeend", 8},
                    {"r", 9},      {"g", 10},     {"b", 11},
                    {"rend", 12},  {"gend", 13},  {"bend", 14},
                    {"gravity", 15}, {"radius", 16},
                    {"dirx", 17},  {"diry", 18},  {"dirz", 19},  {"yaw", 20},
                };
                std::string s;
                while (ls >> s) {
                    if (s.rfind("tex=", 0) == 0) { prim.texPath = s.substr(4); continue; }
                    const size_t eq = s.find('=');
                    if (eq != std::string::npos && eq > 0) {
                        std::string key = s.substr(0, eq);
                        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
                        auto it = kSlots.find(key);
                        if (it != kSlots.end()) {
                            try {
                                prim.particleOverrides.emplace_back(
                                    it->second, std::stof(s.substr(eq + 1)));
                                continue;
                            } catch (...) { /* fall through: a non-numeric value is not an arg */ }
                        }
                        continue;  // unknown key: ignore the token, keep the rest parseable
                    }
                    try { prim.args.push_back(std::stof(s)); }
                    catch (...) { break; }
                }
            }
        } else if (type_name == "PathNode") {
            prim.type = OzonePrimitiveType::ENTITY_PATH_NODE;
            // PathNode <name> x y z [radius=R] [next=a,b,c] [loop]
            if (ls >> prim.entityType) {
                std::string s;
                while (ls >> s) {
                    float pv;
                    if (s.rfind("radius=", 0) == 0) {
                        if (TryFloat(s.substr(7), pv)) prim.args.push_back(pv);
                        else OZ_WARN("OZONE line %d: pathnode radius '%s' is not a number",
                                     lineNo, s.c_str());
                        continue;
                    }
                    if (s.rfind("next=", 0) == 0) { prim.entitySubType = s.substr(5); continue; }
                    if (s == "loop") { prim.pathLoop = true; continue; }
                    try { prim.args.push_back(std::stof(s)); }
                    catch (...) { break; }
                }
            }
        } else if (type_name == "levelinfo") {
            prim.type = OzonePrimitiveType::ENTITY_LEVELINFO;
            // levelinfo gameType maxPlayers respawnTime timeLimitEnabled timeLimitMinutes
            //           scoreLimit friendlyFire skyboxPath
            for (int c = 0; c < 7; c++) {
                std::string s;
                if (!(ls >> s)) break;
                try { prim.args.push_back(std::stof(s)); }
                catch (...) { prim.args.push_back(0.0f); }
            }
            // Tail: skybox, side skybox, then optional gametype=<name>.
            // The gametype= kwarg must be recognised before the skybox reads
            // consume it as a path, so the tail is a loop that classifies each
            // token rather than two blind `>>` reads.
            std::string tok;
            bool gotSkybox = false, gotSideSky = false;
            while (ls >> tok) {
                if (tok.rfind("gametype=", 0) == 0) {
                    prim.gametypeKey = tok.substr(9);
                    continue;
                }
                if (!gotSkybox) { prim.entityType = tok; gotSkybox = true; continue; }
                if (!gotSideSky) { prim.entitySubType = tok; gotSideSky = true; continue; }
                break;
            }
        } else if (type_name == "particles") {
            prim.type = OzonePrimitiveType::ENTITY_PARTICLES;
            // particles type density speed r g b windX windZ
            // type is numeric (0=none 1=snow 2=rain 3=void 4=psychic); names accepted for robustness
            std::string s;
            bool first = true;
            while (ls >> s) {
                if (first) {
                    first = false;
                    prim.args.push_back((float)ParticleTypeNameToIndex(s));
                    continue;
                }
                try { prim.args.push_back(std::stof(s)); }
                catch (...) { break; }
            }
        } else if (type_name == "skybox") {
            prim.type = OzonePrimitiveType::SKYBOX;
            // skybox <texPath> cx cy cz size [panU=] [panV=] [scale=] [topTex=]
            // The six inner faces are built by OzoneLoader::BuildSkybox and get
            // their UVs from the vertex's direction relative to the cube centre,
            // so the authored textures read as one coherent projected sky rather
            // than six stretched billboards. topTex= optionally overrides the
            // ceiling face.
            if (ls >> prim.entityType) {
                std::string s;
                while (ls >> s) {
                    float sv;
                    if (s.rfind("panU=", 0) == 0) {
                        if (TryFloat(s.substr(5), sv)) prim.surface.def.panU = sv;
                        else OZ_WARN("OZONE line %d: skybox panU '%s' is not a number", lineNo, s.c_str());
                        continue;
                    }
                    if (s.rfind("panV=", 0) == 0) {
                        if (TryFloat(s.substr(5), sv)) prim.surface.def.panV = sv;
                        else OZ_WARN("OZONE line %d: skybox panV '%s' is not a number", lineNo, s.c_str());
                        continue;
                    }
                    if (s.rfind("scale=", 0) == 0) {
                        if (TryFloat(s.substr(6), sv)) prim.args.push_back(sv);
                        else OZ_WARN("OZONE line %d: skybox scale '%s' is not a number", lineNo, s.c_str());
                        continue;
                    }
                    if (s.rfind("topTex=", 0) == 0) { prim.surface.def.texPath = s.substr(7); continue; }
                    try { prim.args.push_back(std::stof(s)); }
                    catch (...) { break; }
                }
            }
            // A skybox is a room, not a light: it must render unlit and it must
            // never be carved away by CSG, or the sky disappears when the author
            // subtracts the surrounding shell.
            prim.surface.def.Set(SURF_UNLIT, true);
            prim.surface.def.Set(SURF_TWO_SIDED, true);
            prim.surface.def.Set(SURF_NO_FOG, true);
            prim.surface.def.Set(SURF_NO_BSP_CUTS, true);
        } else if (type_name == "heightmap") {
            prim.type = OzonePrimitiveType::HEIGHTMAP;
            // heightmap imagePath texturePath x y z scale sizeX sizeY sizeZ
            ls >> prim.entityType;   // image path
            std::string texPath;
            ls >> texPath;           // texture path
            prim.entitySubType = texPath;
            {
                std::string s;
                while (ls >> s) {
                    try { prim.args.push_back(std::stof(s)); }
                    catch (...) { break; }
                }
            }
        }

        if (prim.type != OzonePrimitiveType::UNKNOWN) {
            DeriveLegacySurfaceFields(prim);
            prims.push_back(std::move(prim));
        } else {
            // A line whose FIRST token is unrecognised used to vanish with no
            // message at all, so a typo'd primitive was indistinguishable from a
            // blank line. Name the line and the token.
            OZ_WARN("OZONE line %d: unknown primitive '%s' skipped",
                    lineNo, type_name.c_str());
        }
    }

    return prims;
}