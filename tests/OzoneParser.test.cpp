// OzoneParser test — standalone, no raylib dependency
#include "../Source/World/OzoneParser.hpp"
#include "../Source/World/SurfaceFlags.hpp"
#include "../Source/Physics/PhysicsInfo.hpp"
#include "../Source/World/ZoneTypes.hpp"
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

int test_count = 0, pass_count = 0;

static void test_portal() {
    test_count++;
    printf("  TEST portal entity (full)... ");
    auto e = OzoneParser::parse_string(
        "portal EngineTest2 10 20 30 40 50 60 15 25 35 bidir\n");
    bool ok = e.size() == 1 && e[0].type == OzonePrimitiveType::ENTITY_PORTAL
        && e[0].entityType == "EngineTest2"
        && e[0].args.size() == 10
        && e[0].args[0] == 10.0f && e[0].args[5] == 60.0f   // bounds min/max
        && e[0].args[6] == 15.0f && e[0].args[8] == 35.0f;  // spawn xyz
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: size=%zu args=%zu type=%d world=%s\n",
                e.size(), e.size() ? e[0].args.size() : 0,
                e.size() ? (int)e[0].type : -1,
                e.size() ? e[0].entityType.c_str() : "");
}

static void test_portal_minimal() {
    test_count++;
    printf("  TEST portal entity (bounds only)... ");
    auto e = OzoneParser::parse_string("portal OtherLevel 0 0 0 8 12 8\n");
    bool ok = e.size() == 1 && e[0].type == OzonePrimitiveType::ENTITY_PORTAL
        && e[0].args.size() == 6;
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: size=%zu args=%zu\n", e.size(), e.size() ? e[0].args.size() : 0);
}

static void test_portal_short_box_is_not_padded() {
    // A portal is a BOX; both consumers (OzOzoneLoader, the server's HTTP
    // serialiser) gate on 6 numbers and drop anything shorter silently. The
    // parser now WARNS on that shape (the report lives in OzoneParser.cpp), but
    // it must not paper over it by padding the tuple - a short line has to stay
    // short so the loaders keep rejecting it. This exact shape was authored
    // into 13 shipped worlds before the warning existed.
    test_count++;
    printf("  TEST portal with 3 numbers keeps the target, stays short... ");
    auto e = OzoneParser::parse_string("portal SomeNextWorld 0 0 0\n");
    bool ok = e.size() == 1 && e[0].type == OzonePrimitiveType::ENTITY_PORTAL
        && e[0].entityType == "SomeNextWorld"   // still nameable in the warning
        && e[0].args.size() < 6;                // the exact drop predicate
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: size=%zu args=%zu world=%s\n",
                e.size(), e.size() ? e[0].args.size() : 0,
                e.size() ? e[0].entityType.c_str() : "");
}

static void test_trigger_arity() {
    // trigger <def> min(3) max(3) [once] [team] -> the two flags are appended
    // AFTER the bounds, so the loader's >=6 gate reads bounds + flags. A full
    // line therefore yields 8 numbers; a short line must stay under 6 BOUNDS
    // (counted before the flags) or the gate would accept garbage bounds - the
    // warning in OzoneParser.cpp counts the same way.
    test_count++;
    printf("  TEST trigger arity (full=8 args, short stays under the gate)... ");
    auto full = OzoneParser::parse_string(
        "trigger Trigger_EnterVolume 5 5 0 15 15 8 once\n");
    auto shortLine = OzoneParser::parse_string("trigger Trigger_EnterVolume 5 5 0\n");
    bool ok = full.size() == 1
        && full[0].type == OzonePrimitiveType::ENTITY_TRIGGER
        && full[0].args.size() == 8
        && full[0].args[6] == 1.0f                 // once
        && shortLine.size() == 1
        && shortLine[0].type == OzonePrimitiveType::ENTITY_TRIGGER
        && shortLine[0].entityType == "Trigger_EnterVolume"
        // 3 bounds + once + team = 5 total: below the loader's 6-number box
        // check, so the line is dropped rather than mis-read.
        && shortLine[0].args.size() == 5
        && shortLine[0].args.size() - 2 < 6;       // bounds before the flags
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: full=%zu/%zu short=%zu/%zu\n",
                full.size(), full.size() ? full[0].args.size() : 0,
                shortLine.size(), shortLine.size() ? shortLine[0].args.size() : 0);
}

static void test_levelinfo() {
    test_count++;
    printf("  TEST levelinfo entity... ");
    auto e = OzoneParser::parse_string(
        "levelinfo DEATHMATCH 12 7 1 15 50 1 Textures/Sky.dds\n");
    // DEATHMATCH is not a numeric token; std::stof throws and the parser
    // substitutes 0.0f, so args[0] == 0 (SINGLEPLAYER). The test asserts the
    // parse succeeds with >= 7 args, not that the name resolved.
    bool ok = e.size() == 1 && e[0].type == OzonePrimitiveType::ENTITY_LEVELINFO
        && e[0].args.size() >= 7
        && e[0].args[0] == 0.0f
        && e[0].entityType == "Textures/Sky.dds";
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: size=%zu args=%zu arg0=%f entity=%s\n",
                e.size(), e.size() ? e[0].args.size() : 0,
                e.size() ? e[0].args[0] : -1.0f,
                e.size() ? e[0].entityType.c_str() : "");
}

static void test_levelinfo_gametype_kwarg() {
    test_count++;
    printf("  TEST levelinfo gametype= kwarg... ");
    auto e = OzoneParser::parse_string(
        "levelinfo 0 8 5 0 10 50 0 sky.dds gametype=deathmatch\n");
    bool ok = e.size() == 1 && e[0].type == OzonePrimitiveType::ENTITY_LEVELINFO
        && e[0].args.size() >= 7
        && e[0].args[0] == 0.0f
        && e[0].entityType == "sky.dds"
        && e[0].gametypeKey == "deathmatch";
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: size=%zu args=%zu gametypeKey='%s'\n",
                e.size(), e.size() ? e[0].args.size() : 0,
                e.size() ? e[0].gametypeKey.c_str() : "");
}

static void test_particles() {
    test_count++;
    printf("  TEST particles entity (numeric)... ");
    auto e = OzoneParser::parse_string("particles 2 120 14 0.5 0.6 0.9 -3 1\n");
    bool ok = e.size() == 1 && e[0].type == OzonePrimitiveType::ENTITY_PARTICLES
        && e[0].args.size() == 8
        && e[0].args[0] == 2.0f   // RAIN
        && e[0].args[1] == 120.0f;
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: size=%zu args=%zu\n", e.size(), e.size() ? e[0].args.size() : 0);
}

static void test_particles_named() {
    test_count++;
    printf("  TEST particles entity (named type)... ");
    auto e = OzoneParser::parse_string("particles SNOW 80 5 1 1 1 0 2\n");
    bool ok = e.size() == 1 && e[0].type == OzonePrimitiveType::ENTITY_PARTICLES
        && e[0].args.size() == 8
        && e[0].args[0] == 1.0f;  // SNOW -> 1
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: size=%zu args=%zu\n", e.size(), e.size() ? e[0].args.size() : 0);
}

static void test_mixed_document() {
    test_count++;
    printf("  TEST mixed document... ");
    std::string doc =
        "# comment\n"
        "box 0 0 0 10 10 10 solid\n"
        "playerstart 5 1 5 90\n"
        "portal EngineTest2 10 20 30 40 50 60 15 25 35 bidir\n"
        "levelinfo DEATHMATCH 12 7 1 15 50 1\n"
        "particles SNOW 80 5 1 1 1 0 2\n";    auto e = OzoneParser::parse_string(doc);
    bool ok = e.size() == 5
        && e[0].type == OzonePrimitiveType::BOX
        && e[1].type == OzonePrimitiveType::ENTITY_PLAYERSTART
        && e[2].type == OzonePrimitiveType::ENTITY_PORTAL
        && e[3].type == OzonePrimitiveType::ENTITY_LEVELINFO
        && e[4].type == OzonePrimitiveType::ENTITY_PARTICLES;
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: size=%zu\n", e.size());
}

static void test_mesh_static() {
    test_count++;
    printf("  TEST GameEngine.Mesh.Static entity... ");
    auto e = OzoneParser::parse_string(
        "Mesh.Static GameData/Models/Crate.glb 1 2 3 90 scale=2 tex=GameData/Tex/crate.png\n");
    bool ok = e.size() == 1 && e[0].type == OzonePrimitiveType::ENTITY_MESH_STATIC
        && e[0].meshPath == "GameData/Models/Crate.glb"
        && e[0].texPath == "GameData/Tex/crate.png"
        && e[0].args.size() == 5
        && e[0].args[0] == 1.0f && e[0].args[3] == 90.0f && e[0].args[4] == 2.0f;
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: size=%zu args=%zu mesh=%s\n",
                e.size(), e.size() ? e[0].args.size() : 0,
                e.size() ? e[0].meshPath.c_str() : "");
}

static void test_mesh_skeletal() {
    test_count++;
    printf("  TEST GameEngine.Mesh.Skeletal entity... ");
    auto e = OzoneParser::parse_string(
        "Mesh.Skeletal GameData/Pawns/Walker.glb 0 0 0 45 anim=Walk speed=1.5\n");
    bool ok = e.size() == 1 && e[0].type == OzonePrimitiveType::ENTITY_MESH_SKELETAL
        && e[0].meshPath == "GameData/Pawns/Walker.glb"
        && e[0].animClip == "Walk"
        && e[0].animSpeed == 1.5f
        && e[0].args.size() == 4;
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: size=%zu args=%zu clip=%s speed=%.2f\n",
                e.size(), e.size() ? e[0].args.size() : 0,
                e.size() ? e[0].animClip.c_str() : "", e.size() ? e[0].animSpeed : 0.0f);
}

static void test_particle_emitter() {
    test_count++;
    printf("  TEST GameEngine.ParticleEmitter entity... ");
    auto e = OzoneParser::parse_string(
        "ParticleEmitter fire 1 2 3 30 0.8 2.5 0.5 0.4 0.0 "
        "255 180 80 40 10 0 0 0.2 0 1 0 0 tex=GameData/EFX/fire.png\n");
    bool ok = e.size() == 1 && e[0].type == OzonePrimitiveType::ENTITY_PARTICLE_EMITTER
        && e[0].entityType == "fire"
        && e[0].texPath == "GameData/EFX/fire.png"
        && e[0].args.size() == 21
        && e[0].args[0] == 1.0f && e[0].args[2] == 3.0f   // position
        && e[0].args[3] == 30.0f;                          // rate
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: size=%zu args=%zu type=%s\n",
                e.size(), e.size() ? e[0].args.size() : 0,
                e.size() ? e[0].entityType.c_str() : "");
}

// Named `key=value` overrides on ParticleEmitter. The point is retuning ONE
// value (gravity, say) without restating twenty-two positional floats and
// risking a transposed pair. The override is recorded against the positional
// slot it names; the loader applies it last so it always wins.
static void test_particle_emitter_kwargs() {
    test_count++;
    printf("  TEST ParticleEmitter named kwargs... ");
    auto e = OzoneParser::parse_string(
        "ParticleEmitter smoke 1 2 3 20 1 2 0.4 0.4 0.0 "
        "255 180 80 60 20 10 0 0 0 1 0 0 gravity=-0.8 radius=2.5 rate=45 tex=x.png\n");
    bool ok = e.size() == 1 && e[0].type == OzonePrimitiveType::ENTITY_PARTICLE_EMITTER;
    // Two overrides recorded: gravity (slot 15) and radius (slot 16); rate
    // (slot 3) is a third.
    // Exact-ish comparison via a helper rather than int casts: (int)(-0.8f) is
    // 0, so any integer truncation here would silently pass for the wrong
    // value. 1e-3 tolerance on floats the parser produced from text.
    auto near = [](float got, float want) { return fabsf(got - want) < 1e-3f; };
    float ovGravity = 999, ovRadius = 999, ovRate = 999;
    for (const auto& ov : e[0].particleOverrides) {
        if (ov.first == 15) ovGravity = ov.second;
        if (ov.first == 16) ovRadius = ov.second;
        if (ov.first == 3)  ovRate   = ov.second;
    }
    ok = ok && e[0].texPath == "x.png"
        && e[0].particleOverrides.size() == 3
        && near(ovGravity, -0.8f) && near(ovRadius, 2.5f) && near(ovRate, 45.0f)
        // The kwargs did NOT get pushed into args: args still holds exactly the
        // 21 positionals, so the loader's index math is untouched.
        && e[0].args.size() == 21
        && e[0].args[3] == 20.0f;   // positional rate unchanged; kwarg overrides it
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: size=%zu args=%zu overrides=%zu\n",
                e.size(), e.size() ? e[0].args.size() : 0,
                e.size() ? e[0].particleOverrides.size() : 0);
}

static void test_mesh_animfile() {
    test_count++;
    printf("  TEST Mesh.Skeletal animfile=/speed=... ");
    auto e = OzoneParser::parse_string(
        "Mesh.Skeletal GameData/M.glb 0 0 0 0 animfile=Environment/Anims/m.ozanim speed=2.5\n");
    bool ok = e.size() == 1 && e[0].type == OzonePrimitiveType::ENTITY_MESH_SKELETAL
        && e[0].animFile == "Environment/Anims/m.ozanim"
        && e[0].animSpeed == 2.5f
        && e[0].args.size() == 4;
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: size=%zu animfile=%s speed=%.2f\n",
                e.size(), e.size() ? e[0].animFile.c_str() : "",
                e.size() ? e[0].animSpeed : 0.0f);
}

static void test_path_node() {
    test_count++;
    printf("  TEST GameEngine.PathNode entity... ");
    auto e = OzoneParser::parse_string(
        "PathNode A 0 0 0 radius=1.5 next=B,C loop\n");
    bool ok = e.size() == 1 && e[0].type == OzonePrimitiveType::ENTITY_PATH_NODE
        && e[0].entityType == "A"
        && e[0].entitySubType == "B,C"
        && e[0].pathLoop
        && e[0].args.size() == 4
        && e[0].args[0] == 0.0f && e[0].args[3] == 1.5f;
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: size=%zu args=%zu name=%s next=%s loop=%d\n",
                e.size(), e.size() ? e[0].args.size() : 0,
                e.size() ? e[0].entityType.c_str() : "",
                e.size() ? e[0].entitySubType.c_str() : "",
                e.size() ? (int)e[0].pathLoop : -1);
}

static void test_wind_zone() {
    test_count++;
    printf("  TEST WindZone entity... ");
    auto e = OzoneParser::parse_string(
        "WindZone -10 -5 -10 10 25 10 1 0 0 2.5 0.8\n");
    bool ok = e.size() == 1 && e[0].type == OzonePrimitiveType::ENTITY_WIND_ZONE
        && e[0].args.size() == 11
        && e[0].args[0] == -10.0f && e[0].args[9] == 2.5f && e[0].args[10] == 0.8f;
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: size=%zu args=%zu\n", e.size(), e.size() ? e[0].args.size() : 0);
}

static void test_mesh_wind_flag() {
    test_count++;
    printf("  TEST GameEngine.Mesh wind flag... ");
    auto e = OzoneParser::parse_string(
        "Mesh.Static GameData/Models/Tree.glb 0 0 0 0 wind=1 tex=x.png\n");
    bool ok = e.size() == 1 && e[0].type == OzonePrimitiveType::ENTITY_MESH_STATIC
        && e[0].meshWind
        && e[0].meshPath == "GameData/Models/Tree.glb";
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: size=%zu wind=%d\n", e.size(), e.size() ? (int)e[0].meshWind : -1);
}

static void test_zone_physics_kwargs() {
    test_count++;
    printf("  TEST zone physics kwargs... ");
    auto e = OzoneParser::parse_string(
        "zone water -8 -8 0 8 8 2 1 gravity=14 jump=6.5 terminal=45 "
        "water_gravity=5 water_drag=0.9 swim_up=4 ladder_speed=7 fly_mult=2 "
        "name=zone_water_0\n");
    auto& p = e[0];
    bool ok = e.size() == 1 && p.type == OzonePrimitiveType::ENTITY_ZONE
        && p.entitySubType == "water"
        && p.name == "zone_water_0"
        && p.args.size() == 7                     // bounds + intensity
        && p.hasPhysics
        && p.physics.gravity == 14.0f
        && p.physics.jumpSpeed == 6.5f
        && p.physics.terminalVelocity == 45.0f
        && p.physics.waterGravity == 5.0f
        && p.physics.waterDrag == 0.9f
        && p.physics.swimUpSpeed == 4.0f
        && p.physics.ladderSpeed == 7.0f
        && p.physics.flySpeedMult == 2.0f;
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: type=%d args=%zu hasPhysics=%d g=%f\n",
                e.size() ? (int)p.type : -1, e.size() ? p.args.size() : 0,
                e.size() ? (int)p.hasPhysics : -1,
                e.size() ? p.physics.gravity : -1.0f);
}

static void test_zone_default_physics() {
    test_count++;
    printf("  TEST zone without physics kwargs keeps defaults... ");
    auto e = OzoneParser::parse_string(
        "zone water -3 30 -8 3 36 -2 1 name=acidpool\n");
    auto& p = e[0];
    // Assert against the engine defaults themselves, not hardcoded literals,
    // so re-tuning PhysicsInfo does not require editing this test.
    const oz::physics::PhysicsInfo d;
    bool ok = e.size() == 1 && p.type == OzonePrimitiveType::ENTITY_ZONE
        && !p.hasPhysics
        && p.physics.gravity == d.gravity
        && p.physics.jumpSpeed == d.jumpSpeed
        && p.physics.ladderSpeed == d.ladderSpeed;
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: hasPhysics=%d g=%f\n",
                e.size() ? (int)p.hasPhysics : -1,
                e.size() ? p.physics.gravity : -1.0f);
}

static void test_zone_gravity_vector() {
    test_count++;
    printf("  TEST zone gravity_vector kwarg... ");
    auto e = OzoneParser::parse_string(
        "zone gravity -8 -8 -8 8 8 8 1 gravity_vector=(0,0,-18) name=grav_shaft\n");
    auto& p = e[0];
    // OZONE Z-up (0,0,-18) == "straight down" -> engine Y-up (0,-18,0).
    bool ok = e.size() == 1 && p.type == OzonePrimitiveType::ENTITY_ZONE
        && p.entitySubType == "gravity"
        && p.name == "grav_shaft"
        && p.hasPhysics
        && p.physics.gravityVector.x == 0.0f
        && p.physics.gravityVector.y == -18.0f
        && p.physics.gravityVector.z == 0.0f
        && p.physics.gravity == 18.0f;
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: gv=(%f,%f,%f) g=%f\n",
                e.size() ? p.physics.gravityVector.x : -1.0f,
                e.size() ? p.physics.gravityVector.y : -1.0f,
                e.size() ? p.physics.gravityVector.z : -1.0f,
                e.size() ? p.physics.gravity : -1.0f);
}

static void test_zone_gravity_type() {
    test_count++;
    printf("  TEST ZoneType gravity mapping... ");
    bool ok = ZoneTypeFromString("gravity") == ZoneType::ZONE_GRAVITY
        && ZoneTypeFromString("GRAVITY") == ZoneType::ZONE_GRAVITY
        && std::string(ZoneTypeName(ZoneType::ZONE_GRAVITY)) == "gravity";
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL\n");
}

// ZONE_CRT drives the PostFX warp pass. It is named "crt" in OZONE (the zone line
// token), and the name must round-trip or the editor's exporter would write a
// token the parser maps back to water.
static void test_zone_crt_type() {
    test_count++;
    printf("  TEST ZoneType crt mapping... ");
    bool ok = ZoneTypeFromString("crt") == ZoneType::ZONE_CRT
        && ZoneTypeFromString("CRT") == ZoneType::ZONE_CRT
        && std::string(ZoneTypeName(ZoneType::ZONE_CRT)) == "crt"
        && (int)ZoneType::ZONE_CRT == 7;
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL\n");
}

// A quoted texPath used to be truncated at the first space: the plain
// `texPath=` test matched the quoted form before the quote-aware branch could
// run, so the value silently became `"my` and pointed at a missing file.
static void test_texpath_quoted_with_space() {
    test_count++;
    printf("  TEST brush texPath quoted with space... ");
    auto e = OzoneParser::parse_string(
        "box 0 0 0 8 8 8 0 texPath=\"oztex/my stone wall.png\"\n");
    bool ok = e.size() == 1 && e[0].texPath == "oztex/my stone wall.png";
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: texPath='%s'\n", e.size() ? e[0].texPath.c_str() : "");
}

static void test_texpath_unquoted_still_works() {
    test_count++;
    printf("  TEST brush texPath unquoted... ");
    auto e = OzoneParser::parse_string(
        "box 0 0 0 8 8 8 0 texPath=oztex/plain.png\n");
    bool ok = e.size() == 1 && e[0].texPath == "oztex/plain.png";
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: texPath='%s'\n", e.size() ? e[0].texPath.c_str() : "");
}

// The quoted value must not swallow the kwargs that follow it on the same line.
static void test_texpath_quoted_then_flags() {
    test_count++;
    printf("  TEST brush texPath quoted followed by flags=... ");
    auto e = OzoneParser::parse_string(
        "box 0 0 0 8 8 8 0 texPath=\"a b.png\" flags=8 texScaleU=2\n");
    bool ok = e.size() == 1
        && e[0].texPath == "a b.png"
        && e[0].surfaceFlags == 8
        && e[0].texScaleU == 2.0f;
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: texPath='%s' flags=%d u=%f\n",
                e.size() ? e[0].texPath.c_str() : "",
                e.size() ? e[0].surfaceFlags : -1,
                e.size() ? e[0].texScaleU : -1.0f);
}

// `flags=16` is the bit that was actually lost: AngelEd's exporter emitted `flags=`
// only when surface.def.flags DISAGREED with the legacy mirror, but that mirror is
// derived from the owner, so the two were always equal and every `flags=` was
// dropped on re-export. GameData/Worlds/TestMap lost all 230 AutoConvex proxies
// that way. Pinned here so the owner and the mirror can never drift apart again.
static void test_collision_proxy_flag_roundtrip() {
    using namespace oz::surface;
    test_count++;
    printf("  TEST brush flags=16 sets BOTH the owner and the derived mirror... ");
    auto e = OzoneParser::parse_string("add box -4 -12 4.75 8.04 8.04 8.04 0 flags=16\n");
    bool ok = e.size() == 1
        && e[0].surface.def.Has(SURF_COLLISION_PROXY)
        && e[0].surfaceFlags == (int)SURF_COLLISION_PROXY
        && e[0].csgOp == 1;   // `add` -> CsgOp::ADD. Note AppendAutoConvexCollision
                              // passes CsgOp::SOLID explicitly (also additive to the
                              // processor) and the exporter prints its name, so a
                              // generated proxy round-trips as `add box ... flags=16`.
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: defFlags=0x%X surfaceFlags=%d csgOp=%d\n",
                e.size() ? e[0].surface.def.flags : 0,
                e.size() ? e[0].surfaceFlags : -1,
                e.size() ? e[0].csgOp : -1);

    // A plain brush must stay at zero on both, so the exporter writes no flags=
    // kwarg for it and does not grow noise in the world file.
    test_count++;
    printf("  TEST a plain brush has no flags on either field... ");
    auto p = OzoneParser::parse_string("add box 0 0 0 8 8 8 0\n");
    bool ok2 = p.size() == 1
        && p[0].surface.def.flags == 0
        && p[0].surfaceFlags == 0
        && !NeedsFlagsKwarg(p[0].surface.def.flags);
    if (ok2) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: defFlags=0x%X surfaceFlags=%d\n",
                p.size() ? p[0].surface.def.flags : 0,
                p.size() ? p[0].surfaceFlags : -1);
}

// Brush material graph name. `surfMaterial=` is brush-wide, `face<name>_material=`
// is the per-face override; both are plain strings (a material NAME, not a path)
// and a material-only face must count as non-default so it takes the per-face
// draw path that binds the material.
static void test_surface_material_kwarg() {
    using namespace oz::surface;
    test_count++;
    printf("  TEST brush surfMaterial= and facepy_material=... ");
    auto e = OzoneParser::parse_string(
        "add box 0 0 0 8 8 8 0 surfMaterial=RustyMetal facepy_material=GlowSign\n");
    bool ok = e.size() == 1
        && e[0].surface.def.material == "RustyMetal"
        && e[0].surface.IsFaceOverridden(FACE_PY)
        && e[0].surface.Resolve(FACE_PY).material == "GlowSign"
        && e[0].surface.def.IsNonDefault();
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: def='%s' faceOverridden=%d face='%s'\n",
                e.size() ? e[0].surface.def.material.c_str() : "",
                e.size() ? (int)e[0].surface.IsFaceOverridden(FACE_PY) : -1,
                e.size() ? e[0].surface.Resolve(FACE_PY).material.c_str() : "");

    test_count++;
    printf("  TEST surfMaterial= aliases material=... ");
    auto a = OzoneParser::parse_string("add box 0 0 0 8 8 8 0 material=Stone\n");
    bool ok2 = a.size() == 1 && a[0].surface.def.material == "Stone";
    if (ok2) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: def='%s'\n", a.size() ? a[0].surface.def.material.c_str() : "");
}

// ---------------------------------------------------------------------------
// Surface properties
// ---------------------------------------------------------------------------
static void test_surface_brush_flags() {
    test_count++;
    printf("  TEST brush flags= maps onto the surface default... ");
    auto e = OzoneParser::parse_string("box 0 0 0 8 8 8 0 flags=8\n");
    using namespace oz::surface;
    bool ok = e.size() == 1
        && e[0].surface.def.Has(SURF_FAKEBACKDROP)
        // The legacy scalar must be DERIVED from the surface default, or the two
        // can disagree and DrawWorldGeometry would skip a brush the surface
        // block says is a backdrop (or vice versa).
        && e[0].surfaceFlags == 8;
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: defFlags=0x%X surfaceFlags=%d\n",
                e.size() ? e[0].surface.def.flags : 0,
                e.size() ? e[0].surfaceFlags : -1);
}

static void test_surface_per_face_kwargs() {
    test_count++;
    printf("  TEST per-face surface kwargs... ");
    auto e = OzoneParser::parse_string(
        "box 0 0 0 8 8 8 0 flags=2048 facepy_flags=12 facepz_texSlot=3 "
        "facepx_pan=0.05,-0.1 facepx_tex=oztex/custom.png\n");
    using namespace oz::surface;
    bool ok = e.size() == 1
        && e[0].surface.IsFaceOverridden(FACE_PY)
        && e[0].surface.Resolve(FACE_PY).flags == 12
        && e[0].surface.IsFaceOverridden(FACE_PZ)
        // texSlot= is the 1-based tileset index the Alignment tab's combo
        // writes; tex= is a free-placement PATH. They are different fields, so
        // conflating them would make a tileset index load as a filename.
        && e[0].surface.Resolve(FACE_PZ).texSlot == 3
        && e[0].surface.Resolve(FACE_PZ).texPath.empty()
        && e[0].surface.IsFaceOverridden(FACE_PX)
        && e[0].surface.Resolve(FACE_PX).panU == 0.05f
        && e[0].surface.Resolve(FACE_PX).panV == -0.1f
        && e[0].surface.Resolve(FACE_PX).texPath == "oztex/custom.png"
        // A face the author never mentioned must NOT become an override.
        && !e[0].surface.IsFaceOverridden(FACE_NX)
        && !e[0].surface.IsFaceOverridden(FACE_NY)
        && !e[0].surface.IsFaceOverridden(FACE_NZ);
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: py=%d pz=%d px=%d nxOver=%d panU=%.4f panV=%.4f tex=%d\n",
                e.size() ? (int)e[0].surface.IsFaceOverridden(FACE_PY) : -1,
                e.size() ? (int)e[0].surface.IsFaceOverridden(FACE_PZ) : -1,
                e.size() ? (int)e[0].surface.IsFaceOverridden(FACE_PX) : -1,
                e.size() ? (int)e[0].surface.IsFaceOverridden(FACE_NX) : -1,
                e.size() ? e[0].surface.Resolve(FACE_PX).panU : -99.0f,
                e.size() ? e[0].surface.Resolve(FACE_PX).panV : -99.0f,
                e.size() ? e[0].surface.Resolve(FACE_PZ).texSlot : -1);
}

// A face kwarg inherits everything it does not restate, so an author can flip
// one flag on a face without restating the whole surface.
static void test_surface_face_inherits_default() {
    test_count++;
    printf("  TEST face override inherits the brush default... ");
    auto e = OzoneParser::parse_string("box 0 0 0 8 8 8 0 flags=8 facepy_flags=9\n");
    using namespace oz::surface;
    bool ok = e.size() == 1
        && e[0].surface.Resolve(FACE_PY).flags == 9
        && e[0].surface.Resolve(FACE_NX).flags == 8
        && e[0].surface.IsFaceOverridden(FACE_PY)
        && !e[0].surface.IsFaceOverridden(FACE_NX);
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: py=0x%X nx=0x%X\n",
                e.size() ? e[0].surface.Resolve(FACE_PY).flags : 0,
                e.size() ? e[0].surface.Resolve(FACE_NX).flags : 0);
}

static void test_surface_uv_kwargs_still_derive() {
    test_count++;
    printf("  TEST legacy texScale*/texOffset* derive into the surface... ");
    auto e = OzoneParser::parse_string(
        "box 0 0 0 8 8 8 0 texScaleU=2 texScaleV=3 texOffsetU=0.25 texOffsetV=0.5\n");
    bool ok = e.size() == 1
        && e[0].surface.def.uvScaleU == 2.0f && e[0].surface.def.uvScaleV == 3.0f
        && e[0].surface.def.uvOffsetU == 0.25f && e[0].surface.def.uvOffsetV == 0.5f
        && e[0].texScaleU == 2.0f && e[0].texScaleV == 3.0f
        && e[0].texOffsetU == 0.25f && e[0].texOffsetV == 0.5f;
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: defU=%f legacyU=%f\n",
                e.size() ? e[0].surface.def.uvScaleU : -1.0f,
                e.size() ? e[0].texScaleU : -1.0f);
}

static void test_surface_glow_triple() {
    test_count++;
    printf("  TEST surfGlow=(r,g,b) triple... ");
    auto e = OzoneParser::parse_string("box 0 0 0 8 8 8 0 surfGlow=(1,0.5,0.25)\n");
    const auto& g = e.size() ? e[0].surface.def : oz::surface::SurfaceProps{};
    bool ok = e.size() == 1 && g.glowR == 1.0f && g.glowG == 0.5f && g.glowB == 0.25f;
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: glow=(%f,%f,%f)\n", g.glowR, g.glowG, g.glowB);
}

static void test_surface_unknown_face_ignored() {
    test_count++;
    printf("  TEST unknown face name is ignored, not a crash... ");
    auto e = OzoneParser::parse_string("box 0 0 0 8 8 8 0 faceqq_flags=1\n");
    bool ok = e.size() == 1 && !e[0].surface.NeedsPerFaceDraw();
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: needsPerFace=%d\n", e.size() ? (int)e[0].surface.NeedsPerFaceDraw() : -1);
}

static void test_skybox_primitive() {
    test_count++;
    printf("  TEST skybox primitive... ");
    auto e = OzoneParser::parse_string("skybox Models/Sky.png 0 0 32 512\n");
    using namespace oz::surface;
    bool ok = e.size() == 1
        && e[0].type == OzonePrimitiveType::SKYBOX
        && e[0].entityType == "Models/Sky.png"
        && e[0].args.size() == 4
        && e[0].args[3] == 512.0f
        // A skybox must be unlit, two-sided, fog-free and CSG-exempt by
        // construction, or the sky vanishes when the shell is subtracted.
        && e[0].surface.def.Has(SURF_UNLIT)
        && e[0].surface.def.Has(SURF_TWO_SIDED)
        && e[0].surface.def.Has(SURF_NO_FOG)
        && e[0].surface.def.Has(SURF_NO_BSP_CUTS);
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: type=%d nargs=%zu flags=0x%X\n",
                e.size() ? (int)e[0].type : -1,
                e.size() ? e[0].args.size() : 0,
                e.size() ? e[0].surface.def.flags : 0);
}

// Malformed input must not kill the process.
//
// The parser called std::stof / std::stoi directly on kwarg VALUES, so a typo
// like `gravity=abc` or `surfAlpha=abc` threw std::invalid_argument straight out
// of parse_string with no handler anywhere. Everything below threw before this
// was fixed - and because the failure was an uncaught exception, not a parse
// error, the process died with no diagnostic at all.
//
// The rule the fix implements: a malformed value keeps the field's default,
// reports, and the REST of the document still parses.
static void test_malformed_values_do_not_throw() {
    test_count++;
    printf("  TEST malformed values are reported, not fatal... ");

    auto e = OzoneParser::parse_string(
        // zone physics value is not a number
        "zone water 0 0 0 10 10 10 1 gravity=abc\n"
        // a bare effect= (empty value)
        "light point 0 5 0 1 1 1 1 10 effect=\n"
        // mesh scale / speed typos
        "Mesh.Static droid.glb 0 0 0 0 scale=wide speed=fast\n"
        // a surface kwarg that is not a number, on a real brush
        "add box 0 0 0 4 1 4 0 surfAlpha=opaque\n"
        // a skybox pan typo
        "skybox sky.png 0 0 0 100 panU=left\n"
        // a completely unknown primitive
        "wat 1 2 3\n"
        // a known line AFTER all of the above: it must still be parsed
        "add box 0 0 1 2 2 2 0\n");

    bool ok = true;
    // The trailing box is the last primitive and must have survived.
    if (e.empty()) { ok = false; printf("FAIL: nothing parsed\n"); }
    else {
        const OzonePrimitive& last = e.back();
        if (!(last.type == OzonePrimitiveType::BOX && last.args.size() >= 6)) {
            ok = false;
            printf("FAIL: last prim type=%d args=%zu\n", (int)last.type, last.args.size());
        }
        // ...and the malformed brush must still be a box, with its default alpha
        // (not a wiped line).
        bool foundBox = false;
        for (const auto& p : e) {
            if (p.type == OzonePrimitiveType::BOX && p.surface.def.alpha == 1.0f &&
                p.surface.def.uvScaleU == 1.0f)
                foundBox = true;
        }
        if (!foundBox) { ok = false; printf("FAIL: malformed brush line lost or corrupted\n"); }
    }

    // Line numbers are carried for diagnostics.
    for (const auto& p : e) {
        if (p.type == OzonePrimitiveType::BOX && p.line == 0) {
            ok = false; printf("FAIL: primitive has no source line\n");
        }
    }

    if (ok) { pass_count++; printf("PASS\n"); }
}

static void test_sektor01_scar_content() {
    auto e = OzoneParser::parse_file("GameData/Worlds/Sektor_01_SilentPines/World.ozone");
    if (e.empty()) { printf("  SKIP Sektor_01 scar content (world not found from cwd)\n"); return; }
    test_count++;
    printf("  TEST Sektor_01 scar uses gravity/CRT/quantum... ");
    bool gravity = false, crt = false;
    int quantum = 0;
    bool hasScarSky = false, hasOuterSky = false;
    for (const auto& p : e) {
        if (p.type == OzonePrimitiveType::ENTITY_ZONE) {
            if (p.entitySubType == "gravity" && p.physics.gravityVector.y > 0.1f)
                gravity = true;   // engine +Y = straight up
            if (p.entitySubType == "crt") crt = true;
            if (p.entitySubType == "sky" && p.name == "silentpines_scar") hasScarSky = true;
            if (p.entitySubType == "sky" && p.name == "silentpines_sky") hasOuterSky = true;
        }
        if (p.surface.def.flags & oz::surface::SURF_QUANTUM) quantum++;
    }
    // The interior sky pocket must be smaller than the outer world sky so the
    // runtime's smallest-containing-volume rule resolves it indoors.
    bool ok = gravity && crt && quantum >= 2 && hasScarSky && hasOuterSky;
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL: gravity=%d crt=%d quantum=%d scarSky=%d outerSky=%d\n",
                gravity, crt, quantum, hasScarSky, hasOuterSky);
}

int main() {
    printf("OzoneParser tests:\n");
    test_portal();
    test_portal_minimal();
    test_portal_short_box_is_not_padded();
    test_trigger_arity();
    test_levelinfo();
    test_levelinfo_gametype_kwarg();
    test_particles();
    test_particles_named();
    test_mesh_static();
    test_mesh_skeletal();
    test_particle_emitter();
    test_particle_emitter_kwargs();
    test_path_node();
    test_wind_zone();
    test_mesh_wind_flag();
    test_malformed_values_do_not_throw();
    test_mesh_animfile();
    test_mixed_document();
    test_zone_physics_kwargs();
    test_zone_default_physics();
    test_zone_gravity_vector();
    test_zone_gravity_type();
    test_zone_crt_type();
    test_texpath_quoted_with_space();
    test_texpath_unquoted_still_works();
    test_texpath_quoted_then_flags();
    test_surface_brush_flags();
    test_collision_proxy_flag_roundtrip();
    test_surface_material_kwarg();
    test_surface_per_face_kwargs();
    test_surface_face_inherits_default();
    test_surface_uv_kwargs_still_derive();
    test_surface_glow_triple();
    test_surface_unknown_face_ignored();
    test_skybox_primitive();
    test_sektor01_scar_content();

    printf("\nResults: %d/%d passed\n", pass_count, test_count);
    return (pass_count == test_count) ? 0 : 1;
}
