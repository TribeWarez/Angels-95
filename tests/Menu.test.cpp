// Menu logic test - raylib-free. Covers the responsive layout, the character
// roster parser and the server browser model.
//   make test_menu
#include "../Source/Menu/MenuMetrics.hpp"
#include "../Source/Menu/CharacterRoster.hpp"
#include "../Source/Menu/ServerBrowserModel.hpp"
#include "../Source/IniConfig.hpp"
#include <cstdio>

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { g_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static bool Inside(const angelsmenu::UiRect& inner, const angelsmenu::UiRect& outer) {
    return inner.x >= outer.x - 0.01f && inner.y >= outer.y - 0.01f &&
           inner.Right() <= outer.Right() + 0.01f &&
           inner.Bottom() <= outer.Bottom() + 0.01f;
}

static void TestMetrics() {
    printf("MenuMetrics\n");
    struct Case { float w, h, dpi, ui; };
    const Case cases[] = {
        { 960, 600, 1.0f, 1.0f },
        { 1280, 720, 1.0f, 1.0f },
        { 1920, 1080, 1.0f, 1.0f },
        { 2560, 1440, 1.5f, 1.0f },
        { 3840, 2160, 2.0f, 1.0f },
        { 1920, 1080, 1.25f, 1.5f },
    };
    for (const Case& c : cases) {
        auto m = angelsmenu::MenuMetrics::Compute(c.w, c.h, c.dpi, c.ui);
        CHECK(m.scale >= 0.75f && m.scale <= 2.5f);
        CHECK(m.frame.w > 0 && m.frame.h > 0);
        CHECK(Inside(m.titleBar, m.frame));
        CHECK(Inside(m.navBar, m.frame));
        CHECK(Inside(m.content, m.frame));
        CHECK(Inside(m.footer, m.frame));
        CHECK(m.content.h > 0);
        CHECK(m.titleBar.Bottom() <= m.navBar.y + 0.01f);
        CHECK(m.navBar.Bottom() <= m.content.y + 0.01f);
        CHECK(m.content.Bottom() <= m.footer.y + 0.01f);
        CHECK(m.footer.Bottom() <= m.frame.Bottom() + 0.01f);
        CHECK(m.fontSm > 0 && m.fontMd >= m.fontSm && m.fontLg >= m.fontMd);

        // Navigation items tile the nav bar without overlapping.
        float lastRight = m.navBar.x;
        for (int i = 0; i < 4; i++) {
            auto r = m.NavItem(i, 4);
            CHECK(r.w > 0 && r.h > 0);
            CHECK(r.x >= lastRight - 0.01f);
            CHECK(Inside(r, m.navBar));
            lastRight = r.Right();
        }
        CHECK(lastRight <= m.navBar.Right() + 0.01f);
    }

    auto compact = angelsmenu::MenuMetrics::Compute(960, 600, 1.0f, 1.0f);
    auto wide = angelsmenu::MenuMetrics::Compute(1920, 1080, 1.0f, 1.0f);
    CHECK(compact.compact);
    CHECK(!wide.compact);
    CHECK(wide.wide);
}

static void TestIniParseText() {
    printf("IniConfig::ParseText\n");
    IniConfig cfg;
    const char* text =
        "; comment\n"
        "[Roster]\n"
        "Order=a,b\n"
        "Name = spaced value \n";
    CHECK(cfg.ParseText(text));
    CHECK(cfg.Get("Roster", "Order", "") == "a,b");
    CHECK(cfg.Get("Roster", "Name", "") == "spaced value");
}

static void TestRoster() {
    printf("CharacterRoster\n");
    const char* text =
        "[Roster]\n"
        "Order=arcanist,killer,missing\n"
        "\n"
        "[arcanist]\n"
        "name=Plague Arcanist\n"
        "mesh=GameData/Pawn/Player/Plague_Arcanist.glb\n"
        "mesh_type=static\n"
        "bio=Keeper of the ninth seal.\n"
        "accent=6\n"
        "\n"
        "[killer]\n"
        "name=Killer\n"
        "mesh=GameData/Pawn/Player/Character_Killer_01.glb\n"
        "texture=GameData/Pawn/Player/Character_Killer_01.png\n"
        "accent=1\n"
        "\n"
        "[invalid id]\n"
        "name=Bad\n"
        "mesh=x.glb\n";

    angelsmenu::CharacterRoster roster;
    CHECK(roster.LoadFromText(text));
    CHECK(roster.All().size() == 2);
    CHECK(roster.All()[0].id == "arcanist");
    CHECK(roster.All()[0].name == "Plague Arcanist");
    CHECK(roster.All()[0].accent == 6);
    CHECK(roster.All()[1].id == "killer");
    CHECK(roster.Find("arcanist") != nullptr);
    CHECK(roster.Find("missing") == nullptr);
    CHECK(!roster.Warnings().empty());

    // Legacy profile values that are asset paths map back to the roster id.
    CHECK(roster.FindByMesh("GameData/Pawn/Player/Plague_Arcanist.glb") != nullptr);
    CHECK(roster.ResolveSelection("GameData/Pawn/Player/Character_Killer_01.glb") == "killer");
    CHECK(roster.ResolveSelection("arcanist") == "arcanist");
    CHECK(roster.ResolveSelection("nope") == "");
    CHECK(roster.ResolveSelection("") == "");

    // Missing mesh / name entries are skipped, not shown as dead rows.
    angelsmenu::CharacterRoster bad;
    CHECK(!bad.LoadFromText("[Roster]\nOrder=a\n[a]\nname=NoMesh\n"));
    CHECK(bad.All().empty());

    // Empty input produces the synthetic default, never a null row.
    angelsmenu::CharacterRoster empty;
    CHECK(!empty.LoadFromText(""));
    CHECK(empty.Default().id == "default");
    CHECK(empty.Default().mesh.empty());

    // Id validation matches what the server will accept on the wire.
    CHECK(angelsmenu::CharacterRoster::IsValidId("arcanist"));
    CHECK(angelsmenu::CharacterRoster::IsValidId("a_b-9"));
    CHECK(!angelsmenu::CharacterRoster::IsValidId(""));
    CHECK(!angelsmenu::CharacterRoster::IsValidId("../evil"));
    CHECK(!angelsmenu::CharacterRoster::IsValidId("has space"));
    CHECK(!angelsmenu::CharacterRoster::IsValidId("this_id_is_far_too_long_to_fit_on_the_wire"));
}

static angelsmenu::ServerRow Row(const char* ip, uint16_t port, const char* name,
                                 const char* map, int players, int maxp, int ping,
                                 bool lan, bool internet, bool pw = false) {
    angelsmenu::ServerRow r;
    r.key = {ip, port};
    r.name = name;
    r.map = map;
    r.players = players;
    r.maxPlayers = maxp;
    r.pingMs = ping;
    r.lan = lan;
    r.internet = internet;
    r.password = pw;
    r.live = true;
    return r;
}

static void TestServerBrowser() {
    printf("ServerBrowserModel\n");
    angelsmenu::ServerBrowserModel model;
    model.SetLan({
        Row("192.168.1.5", 27015, "LAN Coop", "CitadelRuins", 2, 8, -1, true, false),
        Row("192.168.1.6", 27015, "LAN Empty", "TestMap", 0, 8, -1, true, false),
    });
    model.SetInternet({
        Row("192.168.1.5", 27015, "LAN Coop", "CitadelRuins", 3, 8, 12, false, true),
        Row("203.0.113.9", 27016, "Internet One", "World_endless_snow", 7, 16, 80, false, true, true),
    });
    CHECK(model.Rows().size() == 3);  // duplicate ip:port merged

    // The merged LAN row keeps both sources and takes the live internet fields.
    const auto& merged = model.Rows()[0];
    CHECK(merged.lan && merged.internet);
    CHECK(merged.players == 3);
    CHECK(merged.pingMs == 12);
    CHECK(merged.map == "CitadelRuins");

    angelsmenu::ServerFilter all;
    auto allRows = model.Query(all);
    CHECK(allRows.size() == 3);
    CHECK(model.Rows()[allRows[0]].players == 7); // players descending

    angelsmenu::ServerFilter lan;
    lan.source = angelsmenu::ServerFilter::Lan;
    CHECK(model.Query(lan).size() == 2);

    angelsmenu::ServerFilter net;
    net.source = angelsmenu::ServerFilter::Internet;
    CHECK(model.Query(net).size() == 2);

    angelsmenu::ServerFilter search;
    search.query = "endless";
    CHECK(model.Query(search).size() == 1);

    angelsmenu::ServerFilter noPw;
    noPw.hidePassword = true;
    CHECK(model.Query(noPw).size() == 2);

    angelsmenu::ServerFilter full;
    full.hideFull = true;
    full.query = "LAN";
    CHECK(model.Query(full).size() == 2); // 3/8 and 0/8 are both below capacity

    // Correct the full check with a genuinely full server. Clear the internet
    // rows first: a filter with source=All sees both lists.
    model.SetInternet({});
    model.SetLan({
        Row("10.0.0.1", 27015, "Full", "Map", 8, 8, -1, true, false),
        Row("10.0.0.2", 27015, "Open", "Map", 7, 8, -1, true, false),
    });
    angelsmenu::ServerFilter onlyFull;
    onlyFull.hideFull = true;
    CHECK(model.Query(onlyFull).size() == 1);
    CHECK(model.Rows()[model.Query(onlyFull)[0]].name == "Open");

    // Favourites and recents survive a serialize/deserialize round-trip.
    model.ToggleFavorite({"10.0.0.1", 27015});
    model.AddRecent({"10.0.0.2", 27015});
    const std::string state = model.SerializeState();
    angelsmenu::ServerBrowserModel restored;
    CHECK(restored.DeserializeState(state));
    CHECK(restored.IsFavorite({"10.0.0.1", 27015}));
    CHECK(restored.Recent().size() == 1);
    CHECK(restored.Recent()[0] == "10.0.0.2:27015");

    angelsmenu::ServerFilter fav;
    fav.source = angelsmenu::ServerFilter::Favorites;
    // The restored model has no server rows, only favourite keys; the UI will
    // still show a favourite entry as offline once it has a row. The state
    // itself is what this test pins.
    CHECK(restored.Query(fav).empty());
    CHECK(restored.Favorites().size() == 1);

    // Ping sort puts known pings before -1.
    angelsmenu::ServerBrowserModel pings;
    pings.SetLan({
        Row("10.0.0.3", 1, "Slow", "M", 0, 8, 120, true, false),
        Row("10.0.0.4", 1, "Fast", "M", 0, 8, 20, true, false),
        Row("10.0.0.5", 1, "Unknown", "M", 0, 8, -1, true, false),
    });
    angelsmenu::ServerFilter byPing;
    byPing.sort = 1;
    auto sorted = pings.Query(byPing);
    CHECK(pings.Rows()[sorted[0]].name == "Fast");
    CHECK(pings.Rows()[sorted[1]].name == "Slow");
    CHECK(pings.Rows()[sorted[2]].name == "Unknown");
}

int main() {
    TestMetrics();
    TestIniParseText();
    TestRoster();
    TestServerBrowser();
    printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
