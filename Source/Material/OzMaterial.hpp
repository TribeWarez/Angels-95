#pragma once
// ---------------------------------------------------------------------------
// Material/OzMaterial — the authoring half of the material graph, deliberately
// RAYLIB-FREE so it can be parsed, evaluated and tested headlessly.
//
// A material is a small DAG of nodes feeding four output GATES:
//
//   diffuse   base colour (RGB) + opacity (A)
//   alpha     opacity only, layered over the diffuse alpha
//   normal    tangent-space normal map (a `normal` node derives one from height)
//   emissive  self-illumination added after lighting
//
// Node vocabulary is intentionally tiny — texture / constant / multiply /
// normal — because the runtime is baked, not a shader compiler: the editor
// evaluates the graph on the CPU and writes PNGs, and the game only samples
// those. Keeping the graph small is what makes that bake deterministic.
//
// Textures are decoded by a CALLER-SUPPLIED loader (std::function), so the
// parser/evaluator never touch raylib and tests can feed synthetic pixels.
//
// File format (`GameData/Materials/<Name>.ozmat`):
//
//   ozmat 1
//   material "RustyMetal"
//   node tex0 texture "GameData/Tex/World/metal.png"
//   node c0   constant 1.0 0.6 0.2 1.0
//   node m0   multiply tex0 c0
//   node n0   normal   tex0 2.0
//   output diffuse  m0
//   output alpha    tex0.a
//   output normal   n0
//   output emissive -
//   bake 256 256
//   hash <fnv1a of the graph + bake size>
//   baked "GameData/Materials/RustyMetal/albedo.png" \
//         "GameData/Materials/RustyMetal/normal.png" -
// ---------------------------------------------------------------------------

#include <functional>
#include <string>
#include <vector>

namespace ozmat {

// Row-major RGBA float image, straight (non-premultiplied) alpha, 0..1.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<float> rgba;

    void Resize(int w, int h);
    void Fill(float r, float g, float b, float a);
    // Bilinear sample with texture-coordinate wrapping (REPEAT).
    void Sample(float u, float v, float out[4]) const;
    // Luminance at a pixel, used by the height->normal conversion.
    float Luma(int x, int y) const;
};

enum class NodeType {
    Texture,
    Constant,
    Multiply,
    Normal,
};

struct Node {
    std::string id;
    NodeType type = NodeType::Constant;
    std::string path;             // Texture
    std::string a;                // Multiply input A (node id)
    std::string b;                // Multiply input B (node id)
    std::string source;           // Normal input (node id)
    float color[4] = {1, 1, 1, 1}; // Constant
    float strength = 1.0f;        // Normal (height scale)
    // Editor layout hint, persisted as `pos <id> <x> <y>`. -1 = no saved
    // position (the panel auto-lays-out and stores it on first drag). Layout is
    // NOT part of Hash(), so moving a node never invalidates a bake.
    int px = -1;
    int py = -1;
};

enum class Gate {
    Diffuse = 0,
    Alpha,
    Normal,
    Emissive,
    Count
};

const char* GateName(Gate g);
bool GateFromName(const std::string& name, Gate* out);

struct Output {
    Gate gate = Gate::Diffuse;
    std::string source; // node id
    // Optional channel selector on the source: 0 = RGB, 'r'/'g'/'b'/'a' = that
    // channel, 'l' = luminance. A texture with no `.channel` is used as-is.
    char channel = 0;
};

struct Material {
    std::string name;
    int version = 1;
    std::vector<Node> nodes;
    std::vector<Output> outputs;
    int bakeWidth = 256;
    int bakeHeight = 256;

    // Bake metadata. `bakeHash` is compared against Hash() to detect a stale
    // bake; the paths are what the runtime loads.
    std::string bakeHash;
    std::string bakedDiffuse;
    std::string bakedNormal;
    std::string bakedEmissive;

    Node* FindNode(const std::string& id);
    const Node* FindNode(const std::string& id) const;
    const Output* FindOutput(Gate g) const;
    std::vector<Output>& Outputs() { return outputs; }
};

// Parse a .ozmat document. On failure `err` names the line.
bool Parse(const std::string& text, Material& out, std::string* err);

// Serialize back to the canonical layout above (comments are not preserved; the
// graph is machine-authored/edited, unlike .ozls which is hand-authored).
std::string Serialize(const Material& m);

// FNV-1a over the GRAPH ONLY (nodes + outputs + bake size), i.e. excluding the
// bake metadata itself. That is what makes "is this bake stale?" answerable.
std::string Hash(const Material& m);

// Evaluate one gate into an RGBA image. `loader` must decode an image path into
// straight RGBA floats (it is responsible for the real file/package lookup).
//
// Diffuse combines the `diffuse` gate with the `alpha` gate when one exists.
// `normal` expects either a `normal` node (height -> tangent normal) or an
// image already in tangent space (copied through).
bool Evaluate(const Material& m, Gate gate, int width, int height, Image& out,
              const std::function<bool(const std::string&, Image&)>& loader,
              std::string* err);

} // namespace ozmat
