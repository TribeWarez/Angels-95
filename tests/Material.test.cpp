// Material graph: parse / serialize / hash / evaluate. Raylib-free by design —
// the runtime texture decode is injected as a std::function, so the whole graph
// can be exercised with synthetic pixels and no GL context.
//   make test_material && ./test_material
#include "../Source/Material/OzMaterial.hpp"
#include <cmath>
#include <cstdio>
#include <string>

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { g_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static bool Near(float a, float b, float eps = 0.001f) { return std::fabs(a - b) <= eps; }

static const char* kGraph =
    "ozmat 1\n"
    "material \"Rusty Metal\"\n"
    "node tex0 texture \"GameData/Tex/World/metal.png\"\n"
    "node c0   constant 0.5 0.25 0.125 1.0\n"
    "node m0   multiply tex0 c0\n"
    "node n0   normal   tex0 2.0\n"
    "output diffuse m0\n"
    "output alpha tex0.a\n"
    "output normal n0\n"
    "output emissive tex0.r\n"
    "bake 8 8\n"
    "hash deadbeef\n"
    "baked \"GameData/Materials/Rusty Metal/albedo.png\" "
    "\"GameData/Materials/Rusty Metal/normal.png\" -\n";

// 2x2 texture: left column red, right column blue-ish, with a vertical luma ramp.
static bool Loader(const std::string& path, ozmat::Image& out) {
    if (path != "GameData/Tex/World/metal.png") return false;
    out.Resize(2, 2);
    const float px[16] = {
        1.0f, 0.0f, 0.0f, 0.5f,   0.0f, 0.0f, 1.0f, 0.25f,
        1.0f, 1.0f, 0.0f, 0.75f,  0.0f, 1.0f, 0.0f, 1.0f,
    };
    for (int i = 0; i < 16; ++i) out.rgba[i] = px[i];
    return true;
}

static void TestParse() {
    printf("parse\n");
    ozmat::Material m;
    std::string err;
    CHECK(ozmat::Parse(kGraph, m, &err));
    CHECK(err.empty());
    CHECK(m.name == "Rusty Metal");
    CHECK(m.version == 1);
    CHECK(m.nodes.size() == 4);
    CHECK(m.outputs.size() == 4);
    CHECK(m.bakeWidth == 8 && m.bakeHeight == 8);
    CHECK(m.bakeHash == "deadbeef");
    CHECK(m.bakedDiffuse == "GameData/Materials/Rusty Metal/albedo.png");
    CHECK(m.bakedNormal == "GameData/Materials/Rusty Metal/normal.png");
    CHECK(m.bakedEmissive.empty());

    const ozmat::Output* a = m.FindOutput(ozmat::Gate::Alpha);
    CHECK(a && a->source == "tex0" && a->channel == 'a');
    const ozmat::Output* e = m.FindOutput(ozmat::Gate::Emissive);
    CHECK(e && e->source == "tex0" && e->channel == 'r');
}

static void TestSerializeRoundTrip() {
    printf("serialize round-trip\n");
    ozmat::Material m;
    CHECK(ozmat::Parse(kGraph, m, nullptr));
    const std::string text = ozmat::Serialize(m);
    ozmat::Material back;
    CHECK(ozmat::Parse(text, back, nullptr));
    CHECK(back.name == m.name);
    CHECK(back.nodes.size() == m.nodes.size());
    CHECK(back.outputs.size() == m.outputs.size());
    CHECK(back.bakedDiffuse == m.bakedDiffuse);
    // Canonical text is stable across a second save.
    CHECK(ozmat::Serialize(back) == text);
}

static void TestHash() {
    printf("hash\n");
    ozmat::Material a;
    CHECK(ozmat::Parse(kGraph, a, nullptr));
    ozmat::Material b = a;
    CHECK(ozmat::Hash(a) == ozmat::Hash(b));
    // Bake metadata must NOT participate, or every bake would invalidate itself.
    b.bakedDiffuse = "something/else.png";
    b.bakeHash = "0000";
    CHECK(ozmat::Hash(a) == ozmat::Hash(b));
    // A graph edit must.
    b.nodes[1].color[0] = 0.9f;
    CHECK(ozmat::Hash(a) != ozmat::Hash(b));
    // Bake size must.
    ozmat::Material c = a;
    c.bakeWidth = 16;
    CHECK(ozmat::Hash(a) != ozmat::Hash(c));
}

static void TestLayoutPositions() {
    printf("layout positions\n");
    ozmat::Material m;
    CHECK(ozmat::Parse(kGraph, m, nullptr));
    m.nodes[0].px = 10;
    m.nodes[0].py = 20;
    const std::string text = ozmat::Serialize(m);
    CHECK(text.find("pos tex0 10 20") != std::string::npos);
    ozmat::Material back;
    CHECK(ozmat::Parse(text, back, nullptr));
    CHECK(back.nodes[0].px == 10 && back.nodes[0].py == 20);
    // Layout is NOT part of the bake hash: moving a node must not invalidate it.
    const std::string before = ozmat::Hash(m);
    m.nodes[0].px = 500;
    m.nodes[0].py = 400;
    CHECK(ozmat::Hash(m) == before);
}

static void TestEvaluateDiffuse() {
    printf("evaluate diffuse (multiply + alpha gate)\n");
    ozmat::Material m;
    CHECK(ozmat::Parse(kGraph, m, nullptr));
    ozmat::Image out;
    std::string err;
    CHECK(ozmat::Evaluate(m, ozmat::Gate::Diffuse, 2, 2, out, Loader, &err));
    CHECK(out.width == 2 && out.height == 2);
    // Top-left: texel (1,0,0) * constant (0.5,0.25,0.125), alpha from tex .a * .5.
    CHECK(Near(out.rgba[0], 0.5f, 0.01f));
    CHECK(Near(out.rgba[1], 0.0f, 0.01f));
    CHECK(Near(out.rgba[2], 0.0f, 0.01f));
    CHECK(Near(out.rgba[3], 0.25f, 0.02f));
    // Bottom-right: texel (0,1,0) * const, alpha 1 * 1.
    const size_t i = (1 * 2 + 1) * 4;
    CHECK(Near(out.rgba[i + 1], 0.25f, 0.01f));
    CHECK(Near(out.rgba[i + 3], 1.0f, 0.02f));
}

static void TestEvaluateNormal() {
    printf("evaluate normal gate\n");
    // Flat height 0.5 everywhere -> straight up.
    {
        ozmat::Material m;
        CHECK(ozmat::Parse(
            "ozmat 1\nmaterial \"flat\"\n"
            "node c0 constant 0.5 0.5 0.5 1\n"
            "node n0 normal c0 1.0\n"
            "output normal n0\n", m, nullptr));
        ozmat::Image out;
        CHECK(ozmat::Evaluate(m, ozmat::Gate::Normal, 4, 4, out, Loader, nullptr));
        CHECK(Near(out.rgba[0], 0.5f) && Near(out.rgba[1], 0.5f) && Near(out.rgba[2], 1.0f));
        CHECK(Near(out.rgba[3], 1.0f));
    }
    // A left-to-right height ramp must tilt the normal in X.
    {
        ozmat::Material m;
        CHECK(ozmat::Parse(
            "ozmat 1\nmaterial \"ramp\"\n"
            "node t0 texture \"ramp\"\n"
            "node n0 normal t0 4.0\n"
            "output normal n0\n", m, nullptr));
        auto ramp = [](const std::string&, ozmat::Image& out) {
            out.Resize(8, 2);
            for (int y = 0; y < 2; ++y)
                for (int x = 0; x < 8; ++x) {
                    const size_t i = ((size_t)y * 8 + (size_t)x) * 4;
                    const float v = (float)x / 7.0f;
                    out.rgba[i] = out.rgba[i + 1] = out.rgba[i + 2] = v;
                    out.rgba[i + 3] = 1.0f;
                }
            return true;
        };
        ozmat::Image out;
        CHECK(ozmat::Evaluate(m, ozmat::Gate::Normal, 8, 2, out, ramp, nullptr));
        CHECK(!Near(out.rgba[0], 0.5f, 0.02f));   // x term moved
        CHECK(Near(out.rgba[2], 1.0f, 0.05f) || out.rgba[2] > 0.4f);
    }
}

static void TestErrors() {
    printf("errors\n");
    ozmat::Material m;
    std::string err;
    CHECK(!ozmat::Parse("material \"x\"\n", m, &err));
    CHECK(!err.empty());
    CHECK(!ozmat::Parse("ozmat 1\nnode a multiply nope b\n", m, &err));
    CHECK(!ozmat::Parse("ozmat 1\noutput diffuse nope\n", m, &err));
    CHECK(!ozmat::Parse("ozmat 1\noutput sparkle nope\n", m, &err));
    CHECK(!ozmat::Parse(
        "ozmat 1\nnode a texture \"x\"\nnode b texture \"y\"\n"
        "node m multiply a b\nnode loop multiply m loop\noutput diffuse loop\n", m, &err));

    // Missing texture is a bake error, not a black image.
    ozmat::Material ok;
    CHECK(ozmat::Parse("ozmat 1\nnode t texture \"missing\"\noutput diffuse t\n", ok, nullptr));
    ozmat::Image out;
    CHECK(!ozmat::Evaluate(ok, ozmat::Gate::Diffuse, 4, 4, out, Loader, &err));
    CHECK(!err.empty());
    // Gate not declared.
    ozmat::Material noNormal;
    CHECK(ozmat::Parse("ozmat 1\nnode c constant 1 1 1 1\noutput diffuse c\n", noNormal, nullptr));
    CHECK(!ozmat::Evaluate(noNormal, ozmat::Gate::Normal, 4, 4, out, Loader, &err));
}

static void TestMissingGatesRecover() {
    printf("unspecified gates\n");
    ozmat::Material m;
    CHECK(ozmat::Parse(
        "ozmat 1\nmaterial \"solo\"\n"
        "node c constant 0.1 0.2 0.3 1\n"
        "output diffuse c\noutput alpha -\n", m, nullptr));
    CHECK(m.FindOutput(ozmat::Gate::Alpha) == nullptr);
    ozmat::Image out;
    CHECK(ozmat::Evaluate(m, ozmat::Gate::Diffuse, 2, 2, out, Loader, nullptr));
    CHECK(Near(out.rgba[0], 0.1f) && Near(out.rgba[1], 0.2f) && Near(out.rgba[2], 0.3f));
    CHECK(Near(out.rgba[3], 1.0f));
}

int main() {
    TestParse();
    TestSerializeRoundTrip();
    TestHash();
    TestLayoutPositions();
    TestEvaluateDiffuse();
    TestEvaluateNormal();
    TestErrors();
    TestMissingGatesRecover();

    if (g_fail == 0) {
        printf("Material: %d/%d checks passed\n", g_checks, g_checks);
        return 0;
    }
    printf("Material: %d/%d checks FAILED\n", g_fail, g_checks);
    return 1;
}
