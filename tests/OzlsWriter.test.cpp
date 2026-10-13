// OzlsWriter tests - the .ozls write path used by the AngelEd Property Window.
//
// These assert the property that matters most for a text-patching writer: it
// must change ONLY the lines it was asked to change. A writer that reserialises
// the file passes the round-trip tests but silently destroys every comment and
// reorders every key, which is why each case here diffs the untouched content.
#include "../Source/Script/OzlsWriter.hpp"
#include "../Source/Script/LightningScriptParser.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static int g_fail = 0;
static int g_run  = 0;

#define CHECK(cond) do { \
    ++g_run; \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } \
} while (0)

#define CHECK_STR(a, b) do { \
    ++g_run; \
    std::string va = (a), vb = (b); \
    if (va != vb) { \
        printf("FAIL %s:%d\n  got:  [%s]\n  want: [%s]\n", __FILE__, __LINE__, \
               va.c_str(), vb.c_str()); \
        ++g_fail; \
    } \
} while (0)

namespace {

// Scratch file for one case. Deliberately not under GameData/ so a failure
// never leaves a half-written entity def in the shipped data tree.
std::string g_tmp = "ozls_writer_test.tmp.ozls";

void WriteFile(const std::string& text) {
    std::ofstream o(g_tmp, std::ios::binary | std::ios::trunc);
    o << text;
}

std::string ReadFile() {
    std::ifstream i(g_tmp, std::ios::binary);
    std::stringstream ss;
    ss << i.rdbuf();
    return ss.str();
}

EntityDef Reparse() {
    return LightningScriptParser::Parse(ReadFile(), g_tmp);
}

bool HasLine(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------

// The reference document. Every other case starts from a variation of it, so
// they all share the same comments, key order and nested actions block.
const char* kBase =
    "// Leading file comment.\n"
    "entity \"pistol_01\" : weapon {\n"
    "    mesh = \"Pistol_01.glb\"\n"
    "    stats {\n"
    "        // Damage tuning: lowered for the beta pass.\n"
    "        damage = 14\n"
    "        fire_rate = 0.28\n"
    "        projectile_mesh = GameData/x.glb\n"
    "        viewmodel_offset = (0.22, -0.18, 0.45)\n"
    "    }\n"
    "    actions {\n"
    "        on_fire {\n"
    "            set_cooldown 0.28\n"
    "        }\n"
    "    }\n"
    "}\n";

static int test_update_existing() {
    WriteFile(kBase);
    const std::string before = ReadFile();

    auto r = ozls::PatchOzlsStats(g_tmp, {{"damage", "20", ozls::StatKind::Float}});
    CHECK(r.ok);
    CHECK(r.updated == 1);
    CHECK(r.inserted == 0);
    CHECK(r.erased == 0);

    const std::string after = ReadFile();
    CHECK(HasLine(after, "        damage = 20\n"));
    CHECK(!HasLine(after, "damage = 14"));
    // Nothing outside the edited value may move. The expected file is the
    // original with exactly the value "14" replaced by "20".
    const size_t at = before.find("damage = 14");
    CHECK(at != std::string::npos);
    if (at != std::string::npos) {
        const size_t valAt = at + std::string("damage = ").size();   // start of "14"
        CHECK_STR(after, before.substr(0, valAt) + "20" +
                        before.substr(valAt + 2));
    }
    return 0;
}

static int test_preserves_comments_and_order() {
    WriteFile(kBase);
    const std::string before = ReadFile();

    ozls::PatchOzlsStats(g_tmp, {{"fire_rate", "0.5", ozls::StatKind::Float}});
    const std::string after = ReadFile();

    // The comment directly above the block must survive...
    CHECK(HasLine(after, "// Damage tuning: lowered for the beta pass."));
    // ...as must the file header comment and key order.
    CHECK(HasLine(after, "// Leading file comment."));
    size_t d = after.find("damage = 14");
    size_t f = after.find("fire_rate");
    size_t p = after.find("projectile_mesh");
    CHECK(d != std::string::npos && f != std::string::npos && p != std::string::npos);
    CHECK(d < f && f < p);   // order unchanged
    // The nested actions block must be untouched - a naive 'first }' scan would
    // have truncated the stats block at the wrong brace.
    CHECK(HasLine(after, "        on_fire {\n"));
    CHECK(HasLine(after, "            set_cooldown 0.28\n"));
    return 0;
}

static int test_insert_new_key() {
    WriteFile(kBase);
    auto r = ozls::PatchOzlsStats(g_tmp, {
        {"fire_sound", "GameData/Audio/Sounds/Guns/gun.wav", ozls::StatKind::String},
        {"fire_volume", "0.9", ozls::StatKind::Float},
    });
    CHECK(r.ok);
    CHECK(r.inserted == 2);
    CHECK(r.updated == 0);

    const std::string after = ReadFile();
    // Appended INSIDE the stats block, i.e. before its closing brace and before
    // the actions block.
    CHECK(HasLine(after, "        fire_sound = GameData/Audio/Sounds/Guns/gun.wav\n"));
    CHECK(HasLine(after, "        fire_volume = 0.9\n"));
    CHECK(after.find("fire_sound") < after.find("on_fire"));

    // And the new keys must actually parse back out.
    EntityDef def = Reparse();
    auto s = def.stats.strings.find("fire_sound");
    CHECK(s != def.stats.strings.end());
    if (s != def.stats.strings.end())
        CHECK_STR(s->second, "GameData/Audio/Sounds/Guns/gun.wav");
    auto v = def.stats.floats.find("fire_volume");
    CHECK(v != def.stats.floats.end());
    if (v != def.stats.floats.end())
        CHECK(v->second > 0.89f && v->second < 0.91f);
    return 0;
}

static int test_erase_key() {
    WriteFile(kBase);
    auto r = ozls::PatchOzlsStats(g_tmp, {{"fire_rate", "", ozls::StatKind::Float}});
    CHECK(r.ok);
    CHECK(r.erased == 1);
    CHECK(r.updated == 0);

    const std::string after = ReadFile();
    CHECK(!HasLine(after, "fire_rate"));
    // The erase must remove the line, not blank it.
    CHECK(!HasLine(after, "\n\n\n"));
    CHECK(HasLine(after, "        damage = 14\n"));
    CHECK(HasLine(after, "        projectile_mesh = GameData/x.glb\n"));

    EntityDef def = Reparse();
    CHECK(def.stats.floats.find("fire_rate") == def.stats.floats.end());
    CHECK(def.stats.floats.find("damage") != def.stats.floats.end());
    return 0;
}

static int test_vec3_preserved() {
    WriteFile(kBase);
    auto r = ozls::PatchOzlsStats(g_tmp, {
        {"viewmodel_offset", "(0.3, -0.2, 0.5)", ozls::StatKind::Vec3}});
    CHECK(r.ok);
    CHECK(r.updated == 1);
    EntityDef def = Reparse();
    auto v = def.stats.vec3s.find("viewmodel_offset");
    CHECK(v != def.stats.vec3s.end());
    if (v != def.stats.vec3s.end()) {
        CHECK(v->second[0] > 0.29f && v->second[0] < 0.31f);
        CHECK(v->second[2] > 0.49f && v->second[2] < 0.51f);
    }
    // Must not have been demoted to a string.
    CHECK(def.stats.strings.find("viewmodel_offset") == def.stats.strings.end());
    return 0;
}

static int test_multiple_edits_one_pass() {
    WriteFile(kBase);
    auto r = ozls::PatchOzlsStats(g_tmp, {
        {"damage", "30", ozls::StatKind::Float},
        {"swing_sound", "GameData/s.wav", ozls::StatKind::String},
        {"fire_rate", "0.9", ozls::StatKind::Float},
    });
    CHECK(r.ok);
    CHECK(r.updated == 2);
    CHECK(r.inserted == 1);

    EntityDef def = Reparse();
    CHECK(def.stats.floats.find("damage")->second == 30.0f);
    CHECK(def.stats.floats.find("fire_rate")->second > 0.89f);
    CHECK(def.stats.strings.find("swing_sound") != def.stats.strings.end());
    return 0;
}

// An ERASE against a file with no stats block is a caller bug: it asserts the
// key exists, and there is no block for it to exist in. It must fail loudly
// rather than fabricate an empty block. (A non-empty value is an insert and IS
// allowed - see creates_block_on_insert.)
static int test_missing_stats_block_rejects_erase() {
    WriteFile("entity \"empty\" : weapon {\n    mesh = \"a.glb\"\n}\n");
    auto r = ozls::PatchOzlsStats(g_tmp, {{"damage", "", ozls::StatKind::Float}});
    CHECK(!r.ok);
    CHECK(!r.error.empty());
    CHECK(!r.createdStatsBlock);
    // The file must be untouched on failure.
    CHECK_STR(ReadFile(), "entity \"empty\" : weapon {\n    mesh = \"a.glb\"\n}\n");
    return 0;
}

// A pure insert has nothing to patch, so creating the block is the useful
// behaviour. This is the "new def" path.
static int test_creates_stats_block_on_insert() {
    WriteFile("entity \"new\" : weapon {\n    mesh = \"a.glb\"\n}\n");
    auto r = ozls::PatchOzlsStats(g_tmp, {
        {"damage", "11", ozls::StatKind::Float},
        {"fire_sound", "GameData/f.wav", ozls::StatKind::String},
    });
    CHECK(r.ok);
    CHECK(r.createdStatsBlock);
    CHECK(r.inserted == 2);

    const std::string after = ReadFile();
    CHECK(HasLine(after, "stats {"));
    CHECK(HasLine(after, "    mesh = \"a.glb\""));

    EntityDef def = Reparse();
    CHECK(def.name == "new");
    CHECK(def.mesh == "a.glb");
    CHECK(def.stats.floats.find("damage")->second == 11.0f);
    CHECK(def.stats.strings.find("fire_sound") != def.stats.strings.end());
    return 0;
}

// A string value must be written bare. The parser stores stats strings
// verbatim with no quote handling, so quotes would reach the runtime and break
// asset resolution.
static int test_strings_written_unquoted() {
    WriteFile(kBase);
    ozls::PatchOzlsStats(g_tmp, {{"fire_sound", "GameData/a.wav", ozls::StatKind::String}});
    const std::string after = ReadFile();
    CHECK(!HasLine(after, "fire_sound = \""));
    CHECK(HasLine(after, "fire_sound = GameData/a.wav"));
    EntityDef def = Reparse();
    auto s = def.stats.strings.find("fire_sound");
    CHECK(s != def.stats.strings.end());
    if (s != def.stats.strings.end())
        CHECK_STR(s->second, "GameData/a.wav");   // no quotes survived
    return 0;
}

static int test_trailing_comment_on_value_line() {
    WriteFile("entity \"c\" : weapon {\n    stats {\n"
              "        damage = 14 // tune down\n"
              "    }\n}\n");
    auto r = ozls::PatchOzlsStats(g_tmp, {{"damage", "9", ozls::StatKind::Float}});
    CHECK(r.ok);
    CHECK(r.updated == 1);
    EntityDef def = Reparse();
    CHECK(def.stats.floats.find("damage")->second == 9.0f);
    // The rewrite replaces the line, so the old comment is gone with it - the
    // important part is that the value was matched despite the comment.
    return 0;
}

static int test_missing_file_fails() {
    auto r = ozls::PatchOzlsStats("definitely_not_here.ozls",
                                  {{"damage", "1", ozls::StatKind::Float}});
    CHECK(!r.ok);
    CHECK(!r.error.empty());
    return 0;
}

static int test_empty_edits_is_noop() {
    WriteFile(kBase);
    const std::string before = ReadFile();
    auto r = ozls::PatchOzlsStats(g_tmp, {});
    CHECK(r.ok);
    CHECK_STR(ReadFile(), before);
    return 0;
}

static int test_crlf_preserved() {
    WriteFile("entity \"w\" : weapon {\r\n    stats {\r\n        damage = 14\r\n    }\r\n}\r\n");
    auto r = ozls::PatchOzlsStats(g_tmp, {{"damage", "21", ozls::StatKind::Float}});
    CHECK(r.ok);
    const std::string after = ReadFile();
    CHECK(HasLine(after, "damage = 21\r\n"));
    // The rest of the file must keep its CRLFs, not be rewritten to LF.
    CHECK(after.find("stats {\r\n") != std::string::npos);
    CHECK(after.find("entity \"w\"") == 0);
    return 0;
}

static int test_no_trailing_newline_preserved() {
    WriteFile("entity \"n\" : weapon {\n    stats {\n        damage = 14\n    }\n}");
    auto r = ozls::PatchOzlsStats(g_tmp, {{"damage", "5", ozls::StatKind::Float}});
    CHECK(r.ok);
    const std::string after = ReadFile();
    CHECK(after.back() == '}');
    EntityDef def = Reparse();
    CHECK(def.stats.floats.find("damage")->second == 5.0f);
    return 0;
}

// Serialise -> parse must be stable, which is what makes the round-trip usable
// for creating a new def.
static int test_serialize_roundtrip() {
    EntityDef src;
    src.name = "rifle_01";
    src.type = EntityType::WEAPON;
    src.mesh = "Rifle_01.glb";
    src.stats.floats["damage"] = 20.0f;
    src.stats.floats["fire_rate"] = 0.125f;
    src.stats.strings["fire_sound"] = "GameData/Audio/Sounds/Guns/g.wav";
    // vec3s holds a raw float[3]; brace-init into it is not a valid assignment.
    src.stats.vec3s["viewmodel_offset"][0] = 0.3f;
    src.stats.vec3s["viewmodel_offset"][1] = -0.22f;
    src.stats.vec3s["viewmodel_offset"][2] = 0.55f;
    EntityAction a;
    a.name = "on_fire";
    a.scriptLines.push_back("set_cooldown 0.12");
    src.actions.push_back(a);

    const std::string text = ozls::SerializeEntityDef(src);

    EntityDef back = LightningScriptParser::Parse(text, "roundtrip.ozls");
    CHECK(back.name == "rifle_01");
    CHECK(back.type == EntityType::WEAPON);
    CHECK_STR(back.mesh, "Rifle_01.glb");
    CHECK(back.stats.floats.find("damage")->second == 20.0f);
    CHECK(back.stats.floats.find("fire_rate")->second > 0.124f);
    CHECK_STR(back.stats.strings.find("fire_sound")->second,
              "GameData/Audio/Sounds/Guns/g.wav");
    CHECK(back.stats.vec3s.find("viewmodel_offset") != back.stats.vec3s.end());
    CHECK(back.actions.size() == 1);
    CHECK_STR(back.actions[0].name, "on_fire");
    CHECK(back.actions[0].scriptLines.size() == 1);
    return 0;
}

// The writer's output must itself be patchable, otherwise a create-then-edit
// flow breaks on the second save.
static int test_serialize_output_is_patchable() {
    EntityDef src;
    src.name = "x";
    src.type = EntityType::WEAPON;
    src.stats.floats["damage"] = 1.0f;
    WriteFile(ozls::SerializeEntityDef(src));

    auto r = ozls::PatchOzlsStats(g_tmp, {{"damage", "77", ozls::StatKind::Float}});
    CHECK(r.ok);
    EntityDef back = Reparse();
    CHECK(back.stats.floats.find("damage")->second == 77.0f);
    return 0;
}

// A '}' appearing INSIDE a stats value must not be mistaken for the block's
// closing brace. `note = a}b` is the minimal case.
//
// This is what a naive "scan to the first }" implementation gets wrong, and it
// is why FindStatsBlock tracks brace depth AND ignores braces that follow an
// '=' on the line.
//
// NOTE: these cases assert on the written TEXT only and deliberately do not
// re-parse the result. LightningScriptParser cannot currently read a value
// containing '}' - it walks past the end of the content and crashes - so a
// re-parse here would take the whole suite down on a parser bug rather than a
// writer bug. An unquoted brace inside a stats value is not something an author
// can actually write today, so the writer only has to be robust about it, not
// round-trip it.
static int test_brace_inside_value_does_not_end_block() {
    WriteFile("entity \"b\" : weapon {\n"
              "    stats {\n"
              "        note = a}b\n"
              "        damage = 14\n"
              "        fire_rate = 0.28\n"
              "    }\n"
              "}\n");

    auto r = ozls::PatchOzlsStats(g_tmp, {{"damage", "55", ozls::StatKind::Float}});
    CHECK(r.ok);
    // Must be an in-place UPDATE, not an append. If the block were cut short at
    // the '}' in the value, `damage` would fall outside it and be re-inserted.
    CHECK(r.updated == 1);
    CHECK(r.inserted == 0);

    const std::string after = ReadFile();
    // Every key must still be present exactly once, and the value untouched.
    CHECK(HasLine(after, "        note = a}b\n"));
    CHECK(HasLine(after, "        damage = 55\n"));
    CHECK(HasLine(after, "        fire_rate = 0.28\n"));

    size_t first = after.find("damage =");
    CHECK(first != std::string::npos);
    CHECK(first == after.rfind("damage ="));   // exactly one occurrence
    return 0;
}

// An insert must land inside the stats block even when an earlier value
// contains a brace, i.e. appended at the true end of the block.
static int test_insert_with_brace_in_value() {
    WriteFile("entity \"b2\" : weapon {\n"
              "    stats {\n"
              "        note = }x{\n"
              "        damage = 7\n"
              "    }\n"
              "}\n");
    auto r = ozls::PatchOzlsStats(g_tmp, {
        {"fire_sound", "GameData/g.wav", ozls::StatKind::String}});
    CHECK(r.ok);
    CHECK(r.inserted == 1);
    CHECK(r.updated == 0);

    const std::string after = ReadFile();
    // Appended after the last existing key, still inside the block.
    CHECK(HasLine(after, "        note = }x{\n"));
    CHECK(HasLine(after, "        damage = 7\n"));
    CHECK(after.find("fire_sound") > after.find("damage = 7"));
    CHECK(HasLine(after, "        fire_sound = GameData/g.wav\n"));
    return 0;
}

// The `actions` block that FOLLOWS `stats` contains nested braces. The block
// scan must depth-track to stats' own closing brace, or it truncates early and
// the patch lands in the wrong place.
//
// This exists so the depth tracking is proven rather than assumed: an earlier
// mutation that stopped at the first '}' was invisible to every other case,
// because no shipped .ozls nests braces inside `stats`.
static int test_nested_actions_block_not_confused() {
    WriteFile("entity \"n\" : weapon {\n"
              "    stats {\n"
              "        damage = 14\n"
              "        fire_rate = 0.28\n"
              "    }\n"
              "    actions {\n"
              "        on_fire {\n"
              "            if ($x == 0) {\n"
              "                say \"nested\"\n"
              "            }\n"
              "        }\n"
              "    }\n"
              "}\n");
    const std::string before = ReadFile();

    auto r = ozls::PatchOzlsStats(g_tmp, {{"damage", "66", ozls::StatKind::Float}});
    CHECK(r.ok);
    CHECK(r.updated == 1);
    CHECK(r.inserted == 0);

    const std::string after = ReadFile();
    CHECK(HasLine(after, "        damage = 66\n"));
    // The entire nested actions block must be byte-identical.
    const size_t a0 = before.find("    actions {");
    CHECK(a0 != std::string::npos);
    if (a0 != std::string::npos)
        CHECK_STR(after.substr(after.find("    actions {")),
                  before.substr(a0));
    // And fire_rate must not have been touched or duplicated.
    CHECK(HasLine(after, "        fire_rate = 0.28\n"));
    CHECK(after.find("fire_rate") == after.rfind("fire_rate"));
    return 0;
}

// A key whose name merely starts with "stats" must not be mistaken for the
// block header.
static int test_stats_prefix_key_not_confused() {
    WriteFile("entity \"p\" : weapon {\n    statsfoo = 1\n    stats {\n"
              "        damage = 14\n    }\n}\n");
    auto r = ozls::PatchOzlsStats(g_tmp, {{"damage", "3", ozls::StatKind::Float}});
    CHECK(r.ok);
    CHECK(r.updated == 1);
    EntityDef back = Reparse();
    CHECK(back.stats.floats.find("damage")->second == 3.0f);
    return 0;
}

}  // namespace

int main() {
    struct Case { const char* name; int (*fn)(); };
    const Case cases[] = {
        {"update_existing",            test_update_existing},
        {"preserves_comments_order",   test_preserves_comments_and_order},
        {"insert_new_key",             test_insert_new_key},
        {"erase_key",                  test_erase_key},
        {"vec3_preserved",             test_vec3_preserved},
        {"multiple_edits_one_pass",    test_multiple_edits_one_pass},
        {"missing_block_rejects_erase", test_missing_stats_block_rejects_erase},
        {"creates_block_on_insert",    test_creates_stats_block_on_insert},
        {"strings_unquoted",           test_strings_written_unquoted},
        {"trailing_comment",           test_trailing_comment_on_value_line},
        {"missing_file_fails",         test_missing_file_fails},
        {"empty_edits_noop",           test_empty_edits_is_noop},
        {"crlf_preserved",             test_crlf_preserved},
        {"no_trailing_newline",        test_no_trailing_newline_preserved},
        {"serialize_roundtrip",        test_serialize_roundtrip},
        {"serialize_patchable",        test_serialize_output_is_patchable},
        {"brace_in_value",             test_brace_inside_value_does_not_end_block},
        {"insert_with_brace",          test_insert_with_brace_in_value},
        {"nested_actions",             test_nested_actions_block_not_confused},
        {"stats_prefix_not_confused",  test_stats_prefix_key_not_confused},
    };

    printf("=== OzlsWriter Tests ===\n");
    for (const Case& c : cases) {
        printf("  TEST: %s\n", c.name);
        c.fn();
    }

    std::remove(g_tmp.c_str());

    printf("%d/%d checks passed, %d failed\n", g_run - g_fail, g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}