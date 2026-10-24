// Client settings INI schema + legacy migration. Raylib-free on purpose: the
// runtime statics live in Settings.hpp (raylib/raygui), but the two parts that
// can silently corrupt a player's config are plain data handling -
//
//   1. the section names Load and Save must agree on, and
//   2. the per-key fallback to the old flat [Settings] block,
//
// so they live in Source/ClientSettingsSchema.hpp and are covered here.
// IniConfig::SaveOrdered is exercised too: a hand-editable file must come back
// in the same shape it went in.
#include "../Source/IniConfig.hpp"
#include "../Source/ClientSettingsSchema.hpp"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { g_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static const char* kTmp = "test_clientsettings.tmp";

static std::string ReadAll(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

// An existing install, i.e. the flat layout this pass migrates away from.
static void TestLegacyFallback() {
    printf("legacy [Settings] fallback\n");
    IniConfig cfg;
    CHECK(cfg.ParseText(
        "[Settings]\n"
        "vsync=True\n"
        "window_width=1920\n"
        "crosshair_size=30\n"));

    CHECK(clientcfg::Bool(cfg, clientcfg::kRender, "vsync", false) == true);
    CHECK(clientcfg::Int(cfg, clientcfg::kWindow, "window_width", 1280) == 1920);
    CHECK(clientcfg::Int(cfg, clientcfg::kHud, "crosshair_size", 12) == 30);

    // A key present nowhere keeps the caller's default (missing key and
    // malformed value are the same path).
    CHECK(clientcfg::Int(cfg, clientcfg::kRender, "msaa", 7) == 7);
}

// PowerShell's `Set-Content -Encoding UTF8` and Notepad both write a BOM. Its
// bytes are not whitespace, so before IniConfig stripped it the first section
// header in the file never parsed and that whole section was dropped.
static void TestUtf8Bom() {
    printf("UTF-8 BOM does not eat the first section\n");
    IniConfig cfg;
    CHECK(cfg.ParseText("\xEF\xBB\xBF" "[AngelTech.Render]\nvsync=True\n"));
    CHECK(cfg.GetBool(clientcfg::kRender, "vsync", false) == true);
}

static void TestScopedWins() {
    printf("scoped section wins over legacy\n");
    IniConfig cfg;
    CHECK(cfg.ParseText(
        "[Settings]\n"
        "vsync=False\n"
        "jitter=True\n"
        "[AngelTech.Render]\n"
        "vsync=True\n"
        "jitter=\n"));   // present but empty

    CHECK(clientcfg::Bool(cfg, clientcfg::kRender, "vsync", false) == true);
    // An empty value in the scoped section must NOT fall through to the legacy
    // value: the key is present, so the caller's default is the answer.
    CHECK(clientcfg::Bool(cfg, clientcfg::kRender, "jitter", false) == false);
}

static void TestSaveOrdered() {
    printf("SaveOrdered layout\n");
    IniConfig cfg;
    // Deliberately inserted out of section and key order.
    cfg.SetInt(clientcfg::kHud, "ui_scale", 1);
    cfg.SetInt(clientcfg::kAudio, "audio_volume", 50);
    cfg.SetInt(clientcfg::kWindow, "window_width", 800);
    cfg.Set("AngelTech.Render", "vsync", "True");
    cfg.Set("AngelTech.Render", "msaa", "False");
    cfg.SetBool(clientcfg::kDebug, "debug", false);
    cfg.Set("Zzz", "kept", "1");            // a section the client does not own

    CHECK(cfg.SaveOrdered(kTmp, clientcfg::kSectionOrder, "; header\n\n"));

    IniConfig back;
    CHECK(back.Load(kTmp));
    CHECK(back.GetInt(clientcfg::kAudio, "audio_volume", -1) == 50);
    CHECK(back.GetInt(clientcfg::kHud, "ui_scale", -1) == 1);
    CHECK(back.GetBool(clientcfg::kRender, "vsync", false) == true);
    CHECK(back.Get("Zzz", "kept", "") == "1");

    const std::string text = ReadAll(kTmp);
    auto at = [&](const char* s) { return text.find(s); };
    CHECK(text.rfind("; header", 0) == 0);
    // Declared section order, even though they were inserted differently.
    CHECK(at("[AngelTech.Window]") < at("[AngelTech.Render]"));
    CHECK(at("[AngelTech.Render]") < at("[AngelTech.Audio]"));
    CHECK(at("[AngelTech.Audio]") < at("[AngelTech.Debug]"));
    // Keys are alphabetical inside a section: msaa was set after vsync.
    CHECK(at("msaa=False") < at("vsync=True"));
    // The unowned section survives, after every named one.
    CHECK(at("[Zzz]") != std::string::npos);
    CHECK(at("[Zzz]") > at("[AngelTech.Debug]"));
}

// Mirrors what SaveClientSettings does to a legacy file: read through the
// fallback, rewrite into the scoped sections, drop the old block.
static void TestMigration() {
    printf("legacy file migrates on save\n");
    IniConfig cfg;
    CHECK(cfg.ParseText(
        "[Settings]\n"
        "vsync=True\n"
        "music_volume=40.000000\n"
        "crosshair_style=2\n"
        "\n"
        "[MasterServers]\n"
        "Master=http://127.0.0.1:27950\n"));

    const bool vsync  = clientcfg::Bool(cfg, clientcfg::kRender, "vsync", false);
    const float music = clientcfg::Float(cfg, clientcfg::kAudio, "music_volume", 100.0f);
    const int style   = clientcfg::Int(cfg, clientcfg::kHud, "crosshair_style", 0);

    cfg.data.erase(clientcfg::kLegacy);
    cfg.SetBool(clientcfg::kRender, "vsync", vsync);
    cfg.SetFloat(clientcfg::kAudio, "music_volume", music);
    cfg.SetInt(clientcfg::kHud, "crosshair_style", style);
    CHECK(cfg.SaveOrdered(kTmp, clientcfg::kSectionOrder));

    IniConfig back;
    CHECK(back.Load(kTmp));
    CHECK(back.GetBool(clientcfg::kRender, "vsync", false) == true);
    CHECK(back.GetFloat(clientcfg::kAudio, "music_volume", 0.0f) == 40.0f);
    CHECK(back.GetInt(clientcfg::kHud, "crosshair_style", 0) == 2);
    // The stale duplicate must not survive the migration, and the section the
    // client merely reads must not be dropped.
    CHECK(!back.Has(clientcfg::kLegacy, "vsync"));
    CHECK(back.Get("MasterServers", "Master", "") == "http://127.0.0.1:27950");
}

// A section constant that Save forgets is appended at the end of the file
// instead of in place - visible, but only after a user notices the layout
// drifted. Keep the list honest.
static void TestSchemaListsEverySection() {
    printf("schema covers every section\n");
    const char* owned[] = { clientcfg::kWindow, clientcfg::kRender,
                            clientcfg::kAudio, clientcfg::kGameplay,
                            clientcfg::kInput, clientcfg::kHud,
                            clientcfg::kDebug };
    for (const char* s : owned) {
        int hits = 0;
        for (const std::string& listed : clientcfg::kSectionOrder)
            if (listed == s) hits++;
        CHECK(hits == 1);
    }
    // Bindings and master URLs are read/written by their own modules, but the
    // client's writer must emit them in a known place too.
    CHECK(clientcfg::kSectionOrder.size() >= 9);
}

int main() {
    TestLegacyFallback();
    TestUtf8Bom();
    TestScopedWins();
    TestSaveOrdered();
    TestMigration();
    TestSchemaListsEverySection();
    std::remove(kTmp);

    if (g_fail == 0) {
        printf("ClientSettings: %d/%d checks passed\n", g_checks, g_checks);
        return 0;
    }
    printf("ClientSettings: %d/%d checks FAILED\n", g_fail, g_checks);
    return 1;
}
