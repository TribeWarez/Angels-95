// AssetPath test - standalone, no raylib dependency.
//   make test_assetpath
//
// Pins the two halves of AngelEd/Source/Core/AssetPath.hpp.
//
// The regression: the exporter wrote RESOLVED (absolute) paths into World.ozone
// for Mesh.Static/Mesh.Skeletal and their tex=, so a committed world held
// `G:\projects\...\GameData\Environment\Props\Bush_Dry_B.glb`. It resolves on the
// machine that authored it, so `make test` was green locally while the Linux CI
// failed `worldcheck --asset-refs` - a platform split that only shows up in CI
// unless the audit rejects the absolute form explicitly.
#include "../AngelEd/Source/Core/AssetPath.hpp"
#include <cstdio>
#include <string>

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { g_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static void ExpectPath(const char* in, const char* want) {
    g_checks++;
    const std::string got = ozasset::ToPortablePath(in);
    if (got != want) {
        g_fail++;
        printf("  FAIL ToPortablePath(\"%s\")\n         expected \"%s\"\n         got      \"%s\"\n",
               in, want, got.c_str());
    }
}

static void TestPortableRewrite() {
    // The exact shape that broke CI.
    ExpectPath("G:\\projects\\angels95\\angels-95-omega-tech\\GameData\\Environment\\Props\\Bush_Dry_B.glb",
               "GameData/Environment/Props/Bush_Dry_B.glb");
    ExpectPath("C:/work/GameData/Tex/Tileset/Jungle/Canopy_1.png",
               "GameData/Tex/Tileset/Jungle/Canopy_1.png");
    // Already portable: untouched.
    ExpectPath("GameData/Environment/Props/Bush_Dry_B.glb",
               "GameData/Environment/Props/Bush_Dry_B.glb");
    // GameData/ wins when present: it is the most portable form and the loader
    // resolves it directly, so a world-local path is NOT shortened further.
    ExpectPath("D:\\w\\GameData\\Worlds\\Sektor_01\\oztex\\tileset\\03.png",
               "GameData/Worlds/Sektor_01/oztex/tileset/03.png");
    ExpectPath("/home/x/GameData/Worlds/Sektor_01/Models/fence.glb",
               "GameData/Worlds/Sektor_01/Models/fence.glb");
    // Without a GameData prefix the world-relative subdirectories are the form.
    ExpectPath("D:\\w\\Sektor_01\\oztex\\tileset\\03.png", "oztex/tileset/03.png");
    ExpectPath("D:\\w\\Worlds\\Sektor_01\\Models\\fence.glb", "Models/fence.glb");
    // Nothing to strip: leave it alone (a def-relative path).
    ExpectPath("Environment/Props/Bush_Dry_B.glb", "Environment/Props/Bush_Dry_B.glb");
    ExpectPath("", "");
}

static void TestMachineAnchored() {
    CHECK(ozasset::IsMachineAnchored("G:\\projects\\x.glb"));
    CHECK(ozasset::IsMachineAnchored("c:/work/x.glb"));
    CHECK(ozasset::IsMachineAnchored("/home/x/GameData/y.png"));
    CHECK(ozasset::IsMachineAnchored("\\\\server\\share\\x.glb"));

    CHECK(!ozasset::IsMachineAnchored("GameData/Environment/Props/Bush_Dry_B.glb"));
    CHECK(!ozasset::IsMachineAnchored("oztex/tileset/03.png"));
    CHECK(!ozasset::IsMachineAnchored("Models/fence.glb"));
    CHECK(!ozasset::IsMachineAnchored(""));

    // Round trip: whatever the rewrite produces must be portable, and anything
    // the rewrite changed must have been machine-anchored to begin with.
    const char* inputs[] = {
        "G:\\projects\\GameData\\a\\b.glb",
        "C:/work/GameData/a/b.png",
        "GameData/a/b.png",
        "oztex/tileset/x.png",
    };
    for (const char* in : inputs) {
        CHECK(!ozasset::IsMachineAnchored(ozasset::ToPortablePath(in)));
    }
}

int main() {
    printf("AssetPath\n=========\n");
    TestPortableRewrite();
    TestMachineAnchored();
    printf("=========\n%d/%d checks passed, %d failed\n",
           g_checks - g_fail, g_checks, g_fail);
    return g_fail;
}
