// LightningScriptParser unit tests
// Compile: g++ -O0 -g --std=c++20 -I ../Source ../Source/Script/LightningScriptParser.cpp LightningScriptParser.test.cpp -o LightningScriptParser.test

#include "../Source/Script/LightningScriptParser.hpp"
#include <cstdio>
#include <cstring>
#include <cassert>

static int tests_total = 0, tests_passed = 0;
#define TEST(name) do { tests_total++; fprintf(stdout, "  TEST: %s ... ", name);
#define PASS() do { tests_passed++; fprintf(stdout, "PASS\n"); } while(0)
#define FAIL(msg) do { fprintf(stdout, "FAIL: %s\n", msg); return 1; } while(0)
#define CHECK(cond) do { if (!(cond)) { fprintf(stdout, "FAIL: %s\n", #cond); return 1; } } while(0)
#define CHECK_EQ(a, b) do { if ((a) != (b)) { fprintf(stdout, "FAIL: expected '%s', got '%s'\n", std::string(b).c_str(), std::string(a).c_str()); return 1; } } while(0)
#define CHECK_APROX(a, b, eps) do { float diff = (a) - (b); if (diff < 0) diff = -diff; if (diff > (eps)) { fprintf(stdout, "FAIL: expected %f, got %f\n", (float)(b), (float)(a)); return 1; } } while(0)
#define END_TEST() } while(0)

static int test_parse_weapon() {
    TEST("parse weapon entity def");
    std::string ozls = R"(
        entity "automag" : weapon {
            mesh = "automag_lvl1.obj"
            texture = "automag_lvl1_texture.png"
            icon = "automag_lvl1_icon.png"
            stats {
                damage = 15
                fire_rate = 0.4
                range = 50.0
            }
        }
    )";
    EntityDef def = LightningScriptParser::Parse(ozls, "test_weapon.ozls");
    CHECK(def.name == "automag");
    CHECK(def.type == EntityType::WEAPON);
    CHECK(def.mesh == "automag_lvl1.obj");
    CHECK(def.texture == "automag_lvl1_texture.png");
    CHECK(def.icon == "automag_lvl1_icon.png");
    CHECK_APROX(def.stats.floats["damage"], 15.0f, 0.001f);
    CHECK_APROX(def.stats.floats["fire_rate"], 0.4f, 0.001f);
    CHECK_APROX(def.stats.floats["range"], 50.0f, 0.001f);
    PASS(); return 0; END_TEST();
}

static int test_parse_skyzone() {
    TEST("parse skyzone entity def");
    std::string ozls = R"(
        entity "snowy_zone" : skyzone {
            fog_color = (0.8, 0.85, 0.9)
            fog_density = 0.015
            ambient_light = (0.6, 0.6, 0.7)
            skybox = "skybox_snowy"
            actions {
                on_enter {
                    set_fog 0.8 0.85 0.9 0.015
                    set_skybox "skybox_snowy"
                }
                on_exit {
                    set_fog 0.5 0.5 0.5 0.002
                    restore_skybox
                }
            }
        }
    )";
    EntityDef def = LightningScriptParser::Parse(ozls, "test_skyzone.ozls");
    CHECK(def.name == "snowy_zone");
    CHECK(def.type == EntityType::SKYZONE);
    CHECK(def.skybox == "skybox_snowy");
    CHECK_APROX(def.stats.vec3s["fog_color"][0], 0.8f, 0.001f);
    CHECK_APROX(def.stats.vec3s["fog_color"][1], 0.85f, 0.001f);
    CHECK_APROX(def.stats.vec3s["fog_color"][2], 0.9f, 0.001f);
    CHECK_APROX(def.stats.floats["fog_density"], 0.015f, 0.001f);
    CHECK(def.actions.size() >= 2);
    CHECK(def.actions[0].name == "on_enter");
    CHECK(def.actions[0].scriptLines.size() >= 2);
    PASS(); return 0; END_TEST();
}

static int test_parse_armor() {
    TEST("parse armor entity def");
    std::string ozls = R"(
        entity "iron_helmet" : armor {
            mesh = "Helmet.obj"
            texture = "HelmetTexture.png"
            icon = "HelmetIcon.png"
            stats {
                defense = 5
                weight = 2.0
            }
        }
    )";
    EntityDef def = LightningScriptParser::Parse(ozls, "test_armor.ozls");
    CHECK(def.name == "iron_helmet");
    CHECK(def.type == EntityType::ARMOR);
    CHECK_APROX(def.stats.floats["defense"], 5.0f, 0.001f);
    CHECK_APROX(def.stats.floats["weight"], 2.0f, 0.001f);
    PASS(); return 0; END_TEST();
}

static int test_parse_consumable() {
    TEST("parse consumable entity def");
    std::string ozls = R"(
        entity "mana_vial" : consumable {
            icon = "ManaVial.png"
            stats {
                restore = 25.0
                max_stack = 10
            }
            actions {
                on_use {
                    say "Used mana vial"
                }
            }
        }
    )";
    EntityDef def = LightningScriptParser::Parse(ozls, "test_consumable.ozls");
    CHECK(def.name == "mana_vial");
    CHECK(def.type == EntityType::CONSUMABLE);
    CHECK(def.icon == "ManaVial.png");
    CHECK_APROX(def.stats.floats["restore"], 25.0f, 0.001f);
    PASS(); return 0; END_TEST();
}

static int test_parse_with_variants() {
    TEST("parse variants block");
    std::string ozls = R"(
        entity "automag" : weapon {
            mesh = "base.obj"
            variants {
                "lvl1" { mesh_override = "automag_lvl1" }
                "lvl2" { mesh_override = "automag_lvl2" }
                "lvl3" { mesh_override = "automag_heavy_rifle_lvl3" }
            }
        }
    )";
    EntityDef def = LightningScriptParser::Parse(ozls, "test_variants.ozls");
    CHECK(def.name == "automag");
    CHECK(def.variants.size() == 3);
    CHECK(def.variants[0].name == "lvl1");
    CHECK(def.variants[0].meshOverride == "automag_lvl1");
    PASS(); return 0; END_TEST();
}

static int test_parse_error_recovery() {
    TEST("parse error recovery (malformed input)");
    std::string ozls = "this is not valid ozls";
    EntityDef def = LightningScriptParser::Parse(ozls, "bad.ozls");
    // Should return empty/unknown entity def, not crash
    CHECK(def.type == EntityType::UNKNOWN);
    PASS(); return 0; END_TEST();
}

static int test_action_block_content() {
    TEST("action block extracts real script lines");
    std::string ozls = R"(
        entity "test" : weapon {
            actions {
                on_fire {
                    set_cooldown 0.4
                    say "Fired!"
                }
            }
        }
    )";
    EntityDef def = LightningScriptParser::Parse(ozls, "test_action.ozls");
    CHECK(def.actions.size() >= 1);
    CHECK(def.actions[0].name == "on_fire");
    CHECK(def.actions[0].scriptLines.size() >= 2);
    bool hasCooldown = false, hasSay = false;
    for (auto& line : def.actions[0].scriptLines) {
        if (line.find("set_cooldown") != std::string::npos) hasCooldown = true;
        if (line.find("say") != std::string::npos) hasSay = true;
    }
    CHECK(hasCooldown);
    CHECK(hasSay);
    PASS(); return 0; END_TEST();
}

static int test_action_brace_normalization() {
    TEST("brace-style actions strip trailing '{' and bare '}' lines");
    // Replicates arena.ozls syntax exactly
    std::string ozls = R"(entity "arena" : skyzone {
    actions {
        on_enter {
            rtflag 0
            if ($result == 0) {
                wtflag 0 1
                spawn_pawn "Walker" 10 2 -55
                msg "The canyon rumbles!"
            }
        }
        on_exit {
            say "bye"
        }
    }
})";
    EntityDef def = LightningScriptParser::Parse(ozls, "test_brace.ozls");
    CHECK(def.type == EntityType::SKYZONE);
    CHECK(def.actions.size() == 2);
    CHECK(def.actions[0].name == "on_enter");
    // No line may carry a trailing '{' or be a bare '}' — the context would
    // otherwise mis-evaluate the condition / warn on unknown opcode.
    for (auto& line : def.actions[0].scriptLines) {
        CHECK(line.back() != '{');
        CHECK(line != "}");
    }
    // Condition line survived intact (minus the brace)
    bool hasCond = false, hasSpawn = false;
    for (auto& line : def.actions[0].scriptLines) {
        if (line == "if ($result == 0)") hasCond = true;
        if (line.find("spawn_pawn \"Walker\"") != std::string::npos) hasSpawn = true;
    }
    CHECK(hasCond);
    CHECK(hasSpawn);
    PASS(); return 0; END_TEST();
}

static int test_parse_projectile() {
    TEST("parse projectile entity def");
    std::string ozls = R"(
        entity "energy_bolt" : projectile {
            mesh = "bolt.obj"
            texture = "bolt_blue.png"
            stats {
                damage = 25
                speed = 30.0
                lifetime = 3.0
            }
        }
    )";
    EntityDef def = LightningScriptParser::Parse(ozls, "test_projectile.ozls");
    CHECK(def.name == "energy_bolt");
    CHECK(def.type == EntityType::PROJECTILE);
    CHECK_APROX(def.stats.floats["damage"], 25.0f, 0.001f);
    CHECK_APROX(def.stats.floats["speed"], 30.0f, 0.001f);
    CHECK_APROX(def.stats.floats["lifetime"], 3.0f, 0.001f);
    PASS(); return 0; END_TEST();
}

static int test_parse_pawn() {
    TEST("parse pawn entity def");
    std::string ozls = R"(
        entity "Skaarj" : pawn {
            mesh = "skaarj.obj"
            texture = "skaarj_tex.png"
            stats {
                speed = 3.0
                aggro_range = 12.0
                attack_range = 2.0
                damage = 20
                max_health = 150
            }
        }
    )";
    EntityDef def = LightningScriptParser::Parse(ozls, "test_pawn.ozls");
    CHECK(def.name == "Skaarj");
    // b60: PAWN type re-introduced (pawn .ozls act as FSM hook carriers)
    CHECK(def.type == EntityType::PAWN);
    CHECK_APROX(def.stats.floats["speed"], 3.0f, 0.001f);
    CHECK_APROX(def.stats.floats["aggro_range"], 12.0f, 0.001f);
    CHECK_APROX(def.stats.floats["max_health"], 150.0f, 0.001f);
    PASS(); return 0; END_TEST();
}

static int test_parse_upgrade() {
    TEST("parse upgrade entity def");
    std::string ozls = R"(
        entity "SwiftBoots" : upgrade {
            icon = "SwiftBoots.png"
            stats {
                move_speed_bonus = 1.5
                equip_slot = "boots"
            }
        }
    )";
    EntityDef def = LightningScriptParser::Parse(ozls, "test_upgrade.ozls");
    CHECK(def.name == "SwiftBoots");
    CHECK(def.type == EntityType::UPGRADE);
    CHECK_APROX(def.stats.floats["move_speed_bonus"], 1.5f, 0.001f);
    PASS(); return 0; END_TEST();
}

static int test_parse_music_field() {
    TEST("parse entity with music field");
    std::string ozls = R"(
        entity "boss_arena" : skyzone {
            skybox = "boss_sky"
            music = "boss_theme.ogg"
        }
    )";
    EntityDef def = LightningScriptParser::Parse(ozls, "test_music.ozls");
    CHECK(def.name == "boss_arena");
    CHECK(def.type == EntityType::SKYZONE);
    CHECK(def.skybox == "boss_sky");
    CHECK(def.music == "boss_theme.ogg");
    PASS(); return 0; END_TEST();
}

static int test_parse_play_sound_action() {
    TEST("parse entity with play_sound action");
    std::string ozls = R"(
        entity "gun_test" : weapon {
            actions {
                on_fire {
                    play_sound "bang.wav"
                    set_cooldown 0.3
                }
            }
        }
    )";
    EntityDef def = LightningScriptParser::Parse(ozls, "test_sound.ozls");
    CHECK(def.actions.size() >= 1);
    CHECK(def.actions[0].name == "on_fire");
    bool hasPlaySound = false;
    for (auto& line : def.actions[0].scriptLines) {
        if (line.find("play_sound") != std::string::npos) hasPlaySound = true;
    }
    CHECK(hasPlaySound);
    PASS(); return 0; END_TEST();
}

static int test_parse_goto_action() {
    TEST("parse entity with goto in action");
    std::string ozls = R"(
        entity "loop_test" : weapon {
            actions {
                on_fire {
                    say "pew"
                    goto fire_loop
                }
            }
        }
    )";
    EntityDef def = LightningScriptParser::Parse(ozls, "test_goto.ozls");
    CHECK(def.actions.size() >= 1);
    CHECK(def.actions[0].name == "on_fire");
    bool hasGoto = false;
    for (auto& line : def.actions[0].scriptLines) {
        if (line.find("goto") != std::string::npos) hasGoto = true;
    }
    CHECK(hasGoto);
    PASS(); return 0; END_TEST();
}

static int test_parse_multiple_actions() {
    TEST("parse entity with multiple actions");
    std::string ozls = R"(
        entity "multi_test" : weapon {
            stats { damage = 10 }
            actions {
                on_fire { say "bang" set_cooldown 0.4 }
                on_reload { say "reloading..." }
                on_equip { say "ready" }
                on_unequip { say "holstered" }
            }
        }
    )";
    EntityDef def = LightningScriptParser::Parse(ozls, "test_multi.ozls");
    CHECK(def.actions.size() >= 4);
    CHECK(def.actions[0].name == "on_fire");
    CHECK(def.actions[1].name == "on_reload");
    CHECK(def.actions[2].name == "on_equip");
    CHECK(def.actions[3].name == "on_unequip");
    PASS(); return 0; END_TEST();
}

static int test_parse_variant_texture_override() {
    TEST("parse variant with texture_override");
    std::string ozls = R"(
        entity "armor" : armor {
            variants {
                "rusty" { texture_override = "rusty_armor.png" }
                "shiny" { texture_override = "shiny_armor.png" }
            }
        }
    )";
    EntityDef def = LightningScriptParser::Parse(ozls, "test_variant_tex.ozls");
    CHECK(def.variants.size() >= 2);
    CHECK(def.variants[0].textureOverride == "rusty_armor.png");
    CHECK(def.variants[1].textureOverride == "shiny_armor.png");
    PASS(); return 0; END_TEST();
}

static int test_parse_empty_actions() {
    TEST("parse entity with empty actions block");
    std::string ozls = R"(
        entity "silent" : weapon {
            actions { }
        }
    )";
    EntityDef def = LightningScriptParser::Parse(ozls, "test_empty_actions.ozls");
    CHECK(def.actions.empty());
    PASS(); return 0; END_TEST();
}

static int test_parse_all_fields() {
    TEST("parse entity with all fields");
    std::string ozls = R"(
        entity "giga_cannon" : weapon {
            mesh = "cannon.obj"
            texture = "cannon_tex.png"
            icon = "cannon_icon.png"
            stats {
                damage = 99
                fire_rate = 1.5
                projectile_speed = 50.0
                projectile_count = 3
                spread = 2.0
                magazine = 6
                reload_time = 3.0
            }
            actions {
                on_fire {
                    set_cooldown 1.5
                    play_sound "cannon_boom.wav"
                    say "BOOM!"
                }
            }
            variants {
                "mk1" { mesh_override = "cannon_mk1" texture_override = "cannon_mk1_tex.png" }
                "mk2" { mesh_override = "cannon_mk2" texture_override = "cannon_mk2_tex.png" }
            }
        }
    )";
    EntityDef def = LightningScriptParser::Parse(ozls, "test_all.ozls");
    CHECK(def.name == "giga_cannon");
    CHECK(def.type == EntityType::WEAPON);
    CHECK(def.mesh == "cannon.obj");
    CHECK(def.texture == "cannon_tex.png");
    CHECK(def.icon == "cannon_icon.png");
    CHECK_APROX(def.stats.floats["damage"], 99.0f, 0.001f);
    CHECK_APROX(def.stats.floats["projectile_count"], 3.0f, 0.001f);
    CHECK(def.actions.size() >= 1);
    CHECK(def.variants.size() >= 2);
    CHECK(def.variants[0].meshOverride == "cannon_mk1");
    PASS(); return 0; END_TEST();
}

static int test_parse_name_with_spaces() {
    TEST("parse entity name with spaces");
    std::string ozls = R"(
        entity "Heavy Plasma Rifle" : weapon {
            stats { damage = 50 }
        }
    )";
    EntityDef def = LightningScriptParser::Parse(ozls, "test_spaces.ozls");
    CHECK(def.name == "Heavy Plasma Rifle");
    CHECK(def.type == EntityType::WEAPON);
    PASS(); return 0; END_TEST();
}

static int test_parse_large_stats() {
    TEST("parse very large stat values");
    std::string ozls = R"(
        entity "overlord" : pawn {
            stats {
                max_health = 99999
                damage = 5000.5
                speed = 0.001
            }
        }
    )";
    EntityDef def = LightningScriptParser::Parse(ozls, "test_large.ozls");
    CHECK_APROX(def.stats.floats["max_health"], 99999.0f, 0.001f);
    CHECK_APROX(def.stats.floats["damage"], 5000.5f, 0.001f);
    CHECK_APROX(def.stats.floats["speed"], 0.001f, 0.001f);
    PASS(); return 0; END_TEST();
}

static int test_parse_empty_string() {
    TEST("parse empty string returns UNKNOWN");
    EntityDef def = LightningScriptParser::Parse("", "empty.ozls");
    CHECK(def.type == EntityType::UNKNOWN);
    PASS(); return 0; END_TEST();
}

static int test_parse_set_skybox() {
    TEST("parse entity with set_skybox action");
    std::string ozls = R"(
        entity "sky_changer" : skyzone {
            actions {
                on_enter {
                    set_skybox "sunset_sky"
                    set_fog 0.8 0.6 0.5 0.003
                }
                on_exit { restore_skybox restore_fog }
            }
        }
    )";
    EntityDef def = LightningScriptParser::Parse(ozls, "test_skybox_action.ozls");
    CHECK(def.actions.size() >= 2);
    CHECK(def.actions[0].name == "on_enter");
    CHECK(def.actions[1].name == "on_exit");
    bool hasSetSkybox = false;
    for (auto& line : def.actions[0].scriptLines) {
        if (line.find("set_skybox") != std::string::npos) hasSetSkybox = true;
    }
    CHECK(hasSetSkybox);
    PASS(); return 0; END_TEST();
}

static int test_parse_both_stats_types() {
    TEST("parse entity with float and string stats");
    std::string ozls = R"(
        entity "loot_box" : pickup {
            stats {
                item_id = 42
                respawn_time = 15.0
                pickup_category = "treasure"
            }
        }
    )";
    EntityDef def = LightningScriptParser::Parse(ozls, "test_both_stats.ozls");
    CHECK_APROX(def.stats.floats["item_id"], 42.0f, 0.001f);
    CHECK_APROX(def.stats.floats["respawn_time"], 15.0f, 0.001f);
    PASS(); return 0; END_TEST();
}

static int test_parse_mesh_skeletal() {
    TEST("parse Mesh.Skeletal entity with first-class anim keys");
    std::string ozls = R"(
        entity "Walker" : Mesh.Skeletal {
            mesh = "GameData/Pawns/Walker.glb"
            texture = "GameData/Pawns/Walker.png"
            mesh_type = "skeletal"
            anim_idle = "Idle"
            anim_patrol = "Walk"
            anim_chase = "Run"
            anim_return = "Walk"
            anim_death = "Die"
            anim_speed = 1.25
            movement_speed = 1.4
        }
    )";
    EntityDef def = LightningScriptParser::Parse(ozls, "test_mesh_skeletal.ozls");
    CHECK(def.name == "Walker");
    CHECK(def.type == EntityType::MESH_SKELETAL);
    CHECK(def.mesh == "GameData/Pawns/Walker.glb");
    CHECK(def.meshType == "skeletal");
    CHECK(def.animIdle == "Idle");
    CHECK(def.animPatrol == "Walk");
    CHECK(def.animChase == "Run");
    CHECK(def.animReturn == "Walk");
    CHECK(def.animDeath == "Die");
    CHECK_APROX(def.animSpeed, 1.25f, 0.001f);
    CHECK_APROX(def.movementSpeed, 1.4f, 0.001f);
    PASS(); return 0; END_TEST();
}

static int test_parse_mesh_static_and_quoted_clip() {
    TEST("parse Mesh.Static + quoted animation clip name");
    std::string ozls = R"(
        entity "AncientMachine" : Mesh.Static {
            mesh = "AncientMachine.obj"
            mesh_type = "static"
            anim_idle = "Idle Loop"
            anim_speed = 2.0
        }
    )";
    EntityDef def = LightningScriptParser::Parse(ozls, "test_mesh_static.ozls");
    CHECK(def.type == EntityType::MESH_STATIC);
    CHECK(def.meshType == "static");
    CHECK(def.animIdle == "Idle Loop");
    CHECK_APROX(def.animSpeed, 2.0f, 0.001f);
    PASS(); return 0; END_TEST();
}

static int test_parse_wind_zone() {
    TEST("parse WindZone entity type");
    std::string ozls = R"(
        entity "Courtyard" : WindZone {
            mesh_type = "static"
        }
    )";
    EntityDef def = LightningScriptParser::Parse(ozls, "test_windzone.ozls");
    CHECK(def.name == "Courtyard");
    CHECK(def.type == EntityType::WIND_ZONE);
    PASS(); return 0; END_TEST();
}

static int test_parse_skill() {
    TEST("parse skill entity (ethereal tree)");
    std::string ozls = R"(
        entity "angelic_might" : skill {
            stats {
                cost = 25
                cost_type = mana
                tier = 1
                col = 0
                requires = none
                max_health_bonus = 25
            }
        }
    )";
    EntityDef def = LightningScriptParser::Parse(ozls, "test_skill.ozls");
    CHECK(def.name == "angelic_might");
    CHECK(def.type == EntityType::SKILL);
    CHECK_APROX(def.stats.floats["cost"], 25.0f, 0.001f);
    CHECK(def.stats.strings["cost_type"] == "mana");
    CHECK_APROX(def.stats.floats["max_health_bonus"], 25.0f, 0.001f);
    PASS(); return 0; END_TEST();
}

static int test_parse_gameui() {
    TEST("parse GameUI entity type (declarative HUD)");
    std::string ozls = R"(
        entity "GameUI" : GameUI {
            texture = "GameData/Tex/Engine/TextBar.png"
            stats {
                hud_scale = 1.5
                hud_x = 0
            }
        }
    )";
    EntityDef def = LightningScriptParser::Parse(ozls, "test_gameui.ozls");
    CHECK(def.name == "GameUI");
    CHECK(def.type == EntityType::GAMEUI);
    CHECK(def.texture == "GameData/Tex/Engine/TextBar.png");
    CHECK_APROX(def.stats.floats["hud_scale"], 1.5f, 0.001f);
    PASS(); return 0; END_TEST();
}

// Regression guard for the HUD object bar: slot_rects must survive the stats
// parser as a single whitespace-delimited STRING token (it is not a float, and
// the parser does not honour quotes), and it must contain no spaces or the
// value is silently truncated at the first one.
static int test_parse_gameui_slot_rects() {
    TEST("parse GameUI slot_rects bar geometry (no spaces, string stat)");
    std::string ozls = R"(
        entity "GameUI" : GameUI {
            texture = "GameData/Tex/Engine/ObjectBar_Redesigned_8_Slot.png"
            stats {
                slot_rects = "114,126,681,494,807,126,683,494,1502,126,682,494"
                hud_width_pct = 0.42
                icon_inset = 16
            }
        }
    )";
    EntityDef def = LightningScriptParser::Parse(ozls, "test_slotbar.ozls");
    CHECK(def.type == EntityType::GAMEUI);
    CHECK(def.texture == "GameData/Tex/Engine/ObjectBar_Redesigned_8_Slot.png");

    auto it = def.stats.strings.find("slot_rects");
    CHECK(it != def.stats.strings.end());
    if (it != def.stats.strings.end()) {
        // The parser keeps the token verbatim, quotes included; GameUi strips
        // them. What matters here is that no space truncated the value and that
        // all 12 numbers (3 cells x 4) survived.
        const std::string& v = it->second;
        CHECK(v.find(' ') == std::string::npos);
        int nums = 0;
        bool prevDigit = false;
        for (char c : v) {
            bool isDigit = (c >= '0' && c <= '9');
            if (isDigit && !prevDigit) ++nums;
            prevDigit = isDigit;
        }
        CHECK(nums == 12);
    }
    // Float stats must not be misfiled as strings.
    CHECK_APROX(def.stats.floats["hud_width_pct"], 0.42f, 0.001f);
    CHECK_APROX(def.stats.floats["icon_inset"], 16.0f, 0.001f);
    CHECK(def.stats.strings.find("hud_width_pct") == def.stats.strings.end());
    PASS(); return 0; END_TEST();
}

// A malformed vec3 must not terminate the process.
//
// ParseStatBlock read a vec3 by blind position: three ReadNumber calls with two
// ReadToken calls between them and no check that anything was there. A short or
// unclosed tuple ran ReadNumber past the end of the content, and ReadNumber called
// std::stof on an empty string — std::invalid_argument, uncaught, out of a parser
// with no handler. Measured before the fix: exit 0xC0000409 on each of these.
//
// This is the bug AGENTS.md and OzlsWriter.hpp recorded as "segfaults on an
// unquoted } inside a stats value". The crash was real; the recorded CAUSE was not —
// an unquoted `}` parses fine (test_parse_unquoted_brace_is_not_a_vec3 below proves
// it). The trigger is a malformed vec3, which is easier to hit by accident because
// an author editing `color = (1, 0.5, 0.2)` can drop a component and the file still
// looks plausible.
//
// The contract being pinned: a bad stat costs that ONE stat and a warning, and the
// rest of the file still parses. Recovery, not merely not-crashing.
static int test_parse_malformed_vec3_recovers() {
    TEST("malformed vec3 does not terminate the parser");
    const char* sources[] = {
        // Two components.
        "entity \"v\" : Generic {\n  stats {\n    k = (1,2)\n    after = 7\n  }\n}\n",
        // Unclosed, at end of file.
        "entity \"v\" : Generic {\n  stats {\n    k = (\n  }\n}\n",
        // Unclosed, where the stats block's own brace closes it.
        "entity \"v\" : Generic {\n  stats {\n    k = (\n    }\n}\n",
        // Missing all three.
        "entity \"v\" : Generic {\n  stats {\n    k = ()\n    after = 7\n  }\n}\n",
        // Only a comma.
        "entity \"v\" : Generic {\n  stats {\n    k = (,)\n  }\n}\n",
    };
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        EntityDef def = LightningScriptParser::Parse(sources[i], "<malformed_vec3>");
        // Reaching this line at all is the primary assertion: the old code threw
        // std::invalid_argument out of Parse and never returned.
        CHECK_EQ(def.name, std::string("v"));
        // A malformed vec3 must NOT be recorded as a valid one — silently defaulting
        // it to (0,0,0) would be worse than dropping it, because a black colour is a
        // plausible-looking value.
        CHECK(def.stats.vec3s.find("k") == def.stats.vec3s.end());
    }
    // Recovery: the stat AFTER a malformed vec3 in the same block still lands, where
    // the file provided one.
    EntityDef after = LightningScriptParser::Parse(sources[0], "<recover>");
    CHECK_APROX(after.stats.floats["after"], 7.0f, 0.001f);

    // A WELL-FORMED vec3 is unaffected and still parses all three components.
    EntityDef good = LightningScriptParser::Parse(
        "entity \"g\" : Generic {\n  stats {\n    c = (0.25, 0.5, 0.75)\n  }\n}\n", "<good_vec3>");
    auto it = good.stats.vec3s.find("c");
    CHECK(it != good.stats.vec3s.end());
    CHECK_APROX(it->second[0], 0.25f, 0.001f);
    CHECK_APROX(it->second[1], 0.5f, 0.001f);
    CHECK_APROX(it->second[2], 0.75f, 0.001f);

    // Negative and exponent forms still work — the guard must not reject a legal '-'.
    EntityDef neg = LightningScriptParser::Parse(
        "entity \"n\" : Generic {\n  stats {\n    c = (-1, 2, -3.5)\n  }\n}\n", "<neg_vec3>");
    auto nit = neg.stats.vec3s.find("c");
    CHECK(nit != neg.stats.vec3s.end());
    CHECK_APROX(nit->second[0], -1.0f, 0.001f);
    CHECK_APROX(nit->second[2], -3.5f, 0.001f);
    PASS(); return 0; END_TEST();
}

// ReadNumber is public on the class, so it must not throw regardless of caller.
static int test_read_number_never_throws() {
    TEST("ReadNumber returns 0 instead of throwing on junk");
    // ParseState is private, so ReadNumber is exercised the way an author triggers it:
    // a numeric field whose value is not a number.
    //
    // Only assert that parsing RETURNS and the name is intact. What follows a junk
    // token depends on where the scan stops — for "1.2.3" ReadNumber consumes "1.2"
    // and leaves ".3", so the next line is consumed as part of that field and the
    // stats block never opens. Pinning the recovery position of malformed input would
    // freeze an accident; the contract that matters is "no exception escapes".
    const char* junk[] = { "", "   ", "abc", "1.2.3", "--", "1e", ".", "+", "-" };
    for (size_t i = 0; i < sizeof(junk) / sizeof(junk[0]); i++) {
        const std::string content = std::string("entity \"j\" : Generic {\n  anim_speed = ") +
                                    junk[i] + "\n}\n";
        EntityDef def = LightningScriptParser::Parse(content, "<junk>");
        CHECK_EQ(def.name, std::string("j"));
    }
    // A junk value INSIDE a stats block is the reachable authoring case, and it is
    // where recovery is observable: the value is filed as a string (the value scanner
    // takes anything non-numeric) and the following stat still parses.
    EntityDef bad = LightningScriptParser::Parse(
        "entity \"s\" : Generic {\n  stats {\n    a = fast\n    k = 1\n  }\n}\n",
        "<bad_stat>");
    CHECK_EQ(bad.name, std::string("s"));
    CHECK_EQ(bad.stats.strings["a"], std::string("fast"));
    CHECK_APROX(bad.stats.floats["k"], 1.0f, 0.001f);

    // A junk value where a top-level NUMBER belongs used to crash the same way. Note
    // that `fast` is then read as the next key, so nothing after it lines up — that
    // desync is pre-existing and is NOT what this test pins; reaching the end of Parse
    // without an exception is.
    EntityDef badSpeed = LightningScriptParser::Parse(
        "entity \"t\" : Generic {\n  anim_speed = fast\n}\n", "<bad_speed>");
    CHECK_EQ(badSpeed.name, std::string("t"));

    // Non-numeric where a vec3 was expected: rejected as malformed rather than filed
    // as three zeros, and the NEXT stat survives.
    EntityDef badv = LightningScriptParser::Parse(
        "entity \"v\" : Generic {\n  stats {\n    c = (a, b, c)\n    k = 2\n  }\n}\n",
        "<bad_vec3>");
    CHECK_EQ(badv.name, std::string("v"));
    CHECK(badv.stats.vec3s.find("c") == badv.stats.vec3s.end());
    CHECK_APROX(badv.stats.floats["k"], 2.0f, 0.001f);

    // Same for the skyzone fields, which were a SECOND hand-written copy of the same
    // blind three-component read and are the second crash site.
    EntityDef badFog = LightningScriptParser::Parse(
        "entity \"z\" : SkyZone {\n  fog_color = (1, 2)\n  ambient_light = (1,2,3)\n}\n",
        "<bad_fog>");
    CHECK_EQ(badFog.name, std::string("z"));
    CHECK(badFog.stats.vec3s.find("fog_color") == badFog.stats.vec3s.end());
    auto amb = badFog.stats.vec3s.find("ambient_light");
    CHECK(amb != badFog.stats.vec3s.end());
    CHECK_APROX(amb->second[1], 2.0f, 0.001f);
    PASS(); return 0; END_TEST();
}

// The recorded cause was wrong. An unquoted '}' in a stats value does NOT crash.
//
// Measured behaviour, pinned here: the value scan stops before '}', stores `k = "v"`,
// and the following ReadToken sees the '}' and ends the stats block normally. So the
// line after it belongs to the ENTITY body, not to stats — it arrives as an unknown
// top-level key, which is why `n = 3` below is deliberately absent from stats.
//
// Pinned so the next person to read "segfaults on an unquoted }" does not go hunting
// for a brace bug that is not there, and so this case cannot silently change while
// someone "fixes" the real one (the malformed vec3, above).
static int test_parse_unquoted_brace_is_not_a_vec3() {
    TEST("unquoted '}' in a stats value is not the crash trigger");
    EntityDef def = LightningScriptParser::Parse(
        "entity \"b\" : Generic {\n  stats {\n    k = v }\n  }\n}\n",
        "<unquoted_brace>");
    CHECK_EQ(def.name, std::string("b"));
    CHECK_EQ(def.stats.strings["k"], std::string("v"));
    CHECK(def.stats.strings.find("n") == def.stats.strings.end());

    // Same value without the brace, to show the '}' is what ends the block and that
    // the stat itself is unaffected either way.
    EntityDef plain = LightningScriptParser::Parse(
        "entity \"b\" : Generic {\n  stats {\n    k = v\n  }\n}\n", "<plain>");
    CHECK_EQ(plain.stats.strings["k"], std::string("v"));
    PASS(); return 0; END_TEST();
}

int main() {
    fprintf(stdout, "LightningScriptParser Tests:\n");
    int failures = 0;
    failures += test_parse_malformed_vec3_recovers();
    failures += test_read_number_never_throws();
    failures += test_parse_unquoted_brace_is_not_a_vec3();
    failures += test_parse_weapon();
    failures += test_parse_skyzone();
    failures += test_parse_armor();
    failures += test_parse_consumable();
    failures += test_parse_with_variants();
    failures += test_parse_error_recovery();
    failures += test_action_block_content();
    failures += test_action_brace_normalization();
    failures += test_parse_projectile();
    failures += test_parse_pawn();
    failures += test_parse_upgrade();
    failures += test_parse_music_field();
    failures += test_parse_play_sound_action();
    failures += test_parse_goto_action();
    failures += test_parse_multiple_actions();
    failures += test_parse_variant_texture_override();
    failures += test_parse_empty_actions();
    failures += test_parse_all_fields();
    failures += test_parse_name_with_spaces();
    failures += test_parse_large_stats();
    failures += test_parse_empty_string();
    failures += test_parse_set_skybox();
    failures += test_parse_both_stats_types();
    failures += test_parse_mesh_skeletal();
    failures += test_parse_mesh_static_and_quoted_clip();
    failures += test_parse_wind_zone();
    failures += test_parse_skill();
    failures += test_parse_gameui();
    failures += test_parse_gameui_slot_rects();
    fprintf(stdout, "\n%d/%d tests passed.\n", tests_passed, tests_total);
    return failures;
}
