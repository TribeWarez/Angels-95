#include "OzMaterial.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace ozmat {

// ---------------------------------------------------------------------------
// Image
// ---------------------------------------------------------------------------
void Image::Resize(int w, int h) {
    width = std::max(0, w);
    height = std::max(0, h);
    rgba.assign((size_t)width * (size_t)height * 4u, 0.0f);
}

void Image::Fill(float r, float g, float b, float a) {
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        rgba[i + 0] = r;
        rgba[i + 1] = g;
        rgba[i + 2] = b;
        rgba[i + 3] = a;
    }
}

static float WrapCoord(float v) {
    v -= std::floor(v);
    return v;
}

void Image::Sample(float u, float v, float out[4]) const {
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (width <= 0 || height <= 0 || rgba.empty()) return;
    u = WrapCoord(u);
    v = WrapCoord(v);
    const float fx = u * (float)width - 0.5f;
    const float fy = v * (float)height - 0.5f;
    const int x0 = (int)std::floor(fx);
    const int y0 = (int)std::floor(fy);
    const float tx = fx - (float)x0;
    const float ty = fy - (float)y0;
    auto fetch = [&](int x, int y, int c) -> float {
        x = ((x % width) + width) % width;
        y = ((y % height) + height) % height;
        return rgba[((size_t)y * (size_t)width + (size_t)x) * 4u + (size_t)c];
    };
    for (int c = 0; c < 4; ++c) {
        const float a = fetch(x0, y0, c), b = fetch(x0 + 1, y0, c);
        const float d = fetch(x0, y0 + 1, c), e = fetch(x0 + 1, y0 + 1, c);
        const float top = a + (b - a) * tx;
        const float bot = d + (e - d) * tx;
        out[c] = top + (bot - top) * ty;
    }
}

float Image::Luma(int x, int y) const {
    if (width <= 0 || height <= 0 || rgba.empty()) return 0.0f;
    x = ((x % width) + width) % width;
    y = ((y % height) + height) % height;
    const size_t i = ((size_t)y * (size_t)width + (size_t)x) * 4u;
    return 0.2126f * rgba[i] + 0.7152f * rgba[i + 1] + 0.0722f * rgba[i + 2];
}

// ---------------------------------------------------------------------------
// Gate names
// ---------------------------------------------------------------------------
const char* GateName(Gate g) {
    switch (g) {
        case Gate::Diffuse:  return "diffuse";
        case Gate::Alpha:    return "alpha";
        case Gate::Normal:   return "normal";
        case Gate::Emissive: return "emissive";
        default:             return "?";
    }
}

bool GateFromName(const std::string& name, Gate* out) {
    if (!out) return false;
    for (int i = 0; i < (int)Gate::Count; ++i) {
        if (name == GateName((Gate)i)) {
            *out = (Gate)i;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Material lookup
// ---------------------------------------------------------------------------
const Node* Material::FindNode(const std::string& id) const {
    for (const Node& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}

Node* Material::FindNode(const std::string& id) {
    for (Node& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}

const Output* Material::FindOutput(Gate g) const {
    for (const Output& o : outputs)
        if (o.gate == g) return &o;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Tokenizer / parse
// ---------------------------------------------------------------------------
namespace {

std::string Trim(std::string s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Double-quoted tokens keep their spaces (asset names ship with spaces). A `//`
// that follows whitespace starts a comment, so a Windows path is safe unless it
// is unquoted AND contains " //".
std::vector<std::string> Tokenize(const std::string& line) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace((unsigned char)line[i])) ++i;
        if (i >= line.size()) break;
        if (line[i] == '"') {
            ++i;
            std::string tok;
            while (i < line.size() && line[i] != '"') tok.push_back(line[i++]);
            if (i < line.size()) ++i;
            out.push_back(tok);
            continue;
        }
        std::string tok;
        while (i < line.size() && !std::isspace((unsigned char)line[i])) tok.push_back(line[i++]);
        out.push_back(tok);
    }
    return out;
}

bool ToFloat(const std::string& s, float* out) {
    if (!out || s.empty()) return false;
    try {
        size_t used = 0;
        *out = std::stof(s, &used);
        return used > 0;
    } catch (...) {
        return false;
    }
}

bool ToInt(const std::string& s, int* out) {
    if (!out || s.empty()) return false;
    try {
        size_t used = 0;
        *out = std::stoi(s, &used);
        return used > 0;
    } catch (...) {
        return false;
    }
}

std::string Quote(const std::string& s) {
    return "\"" + s + "\"";
}

// "tex0.a" -> source "tex0", channel 'a'. A bare node id keeps channel 0.
void SplitChannel(const std::string& tok, std::string* source, char* channel) {
    *source = tok;
    *channel = 0;
    const size_t dot = tok.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= tok.size()) return;
    const std::string tail = tok.substr(dot + 1);
    if (tail.size() != 1) return;
    const char c = (char)std::tolower((unsigned char)tail[0]);
    if (c == 'r' || c == 'g' || c == 'b' || c == 'a' || c == 'l') {
        *source = tok.substr(0, dot);
        *channel = c;
    }
}

std::string ChannelSuffix(char c) {
    if (!c) return "";
    return std::string(".") + c;
}

} // namespace

bool Parse(const std::string& text, Material& out, std::string* err) {
    out = Material{};
    std::istringstream f(text);
    std::string line;
    int lineNo = 0;
    bool sawHeader = false;
    struct PendingPos { std::string id; int x; int y; };
    std::vector<PendingPos> pendingPos;

    while (std::getline(f, line)) {
        ++lineNo;
        line = Trim(line);
        if (line.empty()) continue;
        if (line.rfind("//", 0) == 0 || line.rfind(";", 0) == 0 || line.rfind("#", 0) == 0)
            continue;
        // Trailing comment after whitespace.
        const size_t cmt = line.find(" //");
        if (cmt != std::string::npos) line = Trim(line.substr(0, cmt));

        std::vector<std::string> tok = Tokenize(line);
        if (tok.empty()) continue;

        auto fail = [&](const std::string& why) {
            if (err) *err = "line " + std::to_string(lineNo) + ": " + why;
            return false;
        };

        if (!sawHeader) {
            if (tok[0] != "ozmat" || tok.size() < 2) return fail("expected 'ozmat <version>'");
            int ver = 1;
            if (!ToInt(tok[1], &ver) || ver < 1) return fail("bad ozmat version");
            out.version = ver;
            sawHeader = true;
            continue;
        }

        if (tok[0] == "material") {
            if (tok.size() < 2) return fail("material needs a name");
            out.name = tok[1];
        } else if (tok[0] == "node") {
            if (tok.size() < 3) return fail("node needs <id> <type>");
            Node n;
            n.id = tok[1];
            if (out.FindNode(n.id)) return fail("duplicate node id '" + n.id + "'");
            const std::string& type = tok[2];
            if (type == "texture") {
                n.type = NodeType::Texture;
                if (tok.size() < 4 || tok[3].empty()) return fail("texture node needs a path");
                n.path = tok[3];
            } else if (type == "constant") {
                n.type = NodeType::Constant;
                if (tok.size() < 6) return fail("constant needs r g b a");
                for (int i = 0; i < 4; ++i)
                    if (!ToFloat(tok[3 + i], &n.color[i])) return fail("bad constant component");
            } else if (type == "multiply") {
                n.type = NodeType::Multiply;
                if (tok.size() < 5) return fail("multiply needs two node ids");
                n.a = tok[3];
                n.b = tok[4];
            } else if (type == "normal") {
                n.type = NodeType::Normal;
                if (tok.size() < 4) return fail("normal needs a source node id");
                n.source = tok[3];
                if (tok.size() >= 5 && !ToFloat(tok[4], &n.strength))
                    return fail("bad normal strength");
                if (n.strength <= 0.0f) n.strength = 1.0f;
            } else {
                return fail("unknown node type '" + type + "'");
            }
            out.nodes.push_back(n);
        } else if (tok[0] == "output") {
            if (tok.size() < 3) return fail("output needs <gate> <source>");
            Output o;
            if (!GateFromName(tok[1], &o.gate)) return fail("unknown gate '" + tok[1] + "'");
            if (out.FindOutput(o.gate)) return fail("duplicate gate '" + tok[1] + "'");
            if (tok[2] == "-" || tok[2] == "none") continue; // gate deliberately unset
            SplitChannel(tok[2], &o.source, &o.channel);
            out.outputs.push_back(o);
        } else if (tok[0] == "bake") {
            if (tok.size() < 3) return fail("bake needs <width> <height>");
            if (!ToInt(tok[1], &out.bakeWidth) || !ToInt(tok[2], &out.bakeHeight))
                return fail("bad bake size");
            out.bakeWidth = std::clamp(out.bakeWidth, 1, 4096);
            out.bakeHeight = std::clamp(out.bakeHeight, 1, 4096);
        } else if (tok[0] == "pos") {
            if (tok.size() < 4) return fail("pos needs <node-id> <x> <y>");
            int x = 0, y = 0;
            if (!ToInt(tok[2], &x) || !ToInt(tok[3], &y)) return fail("bad pos coordinates");
            pendingPos.push_back({tok[1], x, y});
        } else if (tok[0] == "hash") {
            if (tok.size() >= 2) out.bakeHash = tok[1];
        } else if (tok[0] == "baked") {
            if (tok.size() >= 4) {
                if (tok[1] != "-") out.bakedDiffuse = tok[1];
                if (tok[2] != "-") out.bakedNormal = tok[2];
                if (tok[3] != "-") out.bakedEmissive = tok[3];
            }
        } else {
            return fail("unknown directive '" + tok[0] + "'");
        }
    }

    if (err) err->clear();
    if (!sawHeader) {
        if (err) *err = "missing 'ozmat <version>' header";
        return false;
    }
    // Apply layout hints (tolerant: an unknown id is just a stale hint).
    for (const PendingPos& p : pendingPos) {
        if (Node* n = out.FindNode(p.id)) {
            n->px = p.x;
            n->py = p.y;
        }
    }
    // References must resolve; catching them at load time turns a typo into one
    // clear message instead of a bake that silently uses black.
    for (const Output& o : out.outputs) {
        if (!out.FindNode(o.source)) {
            if (err) *err = std::string("gate '") + GateName(o.gate) + "' references unknown node '" +
                            o.source + "'";
            return false;
        }
    }
    for (const Node& n : out.nodes) {
        auto check = [&](const std::string& ref) {
            if (ref.empty()) return true;
            if (out.FindNode(ref)) return true;
            if (err) *err = "node '" + n.id + "' references unknown node '" + ref + "'";
            return false;
        };
        if (n.type == NodeType::Multiply && (!check(n.a) || !check(n.b))) return false;
        if (n.type == NodeType::Normal && !check(n.source)) return false;
    }
    // A cycle would make Evaluate recurse until it gives up; catching it here
    // names the offending node instead.
    {
        std::unordered_map<std::string, int> state; // 0 new, 1 visiting, 2 done
        std::function<bool(const std::string&)> visit = [&](const std::string& id) {
            int& s = state[id];
            if (s == 1) {
                if (err) *err = "cycle through node '" + id + "'";
                return false;
            }
            if (s == 2) return true;
            s = 1;
            if (const Node* n = out.FindNode(id)) {
                if (n->type == NodeType::Multiply) {
                    if (!visit(n->a) || !visit(n->b)) return false;
                } else if (n->type == NodeType::Normal) {
                    if (!visit(n->source)) return false;
                }
            }
            s = 2;
            return true;
        };
        for (const Node& n : out.nodes)
            if (!visit(n.id)) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Serialize / hash
// ---------------------------------------------------------------------------
std::string Serialize(const Material& m) {
    std::ostringstream o;
    o << "ozmat " << m.version << "\n";
    o << "material " << Quote(m.name) << "\n\n";

    for (const Node& n : m.nodes) {
        o << "node " << n.id << " ";
        switch (n.type) {
            case NodeType::Texture:
                o << "texture " << Quote(n.path);
                break;
            case NodeType::Constant:
                o << "constant " << n.color[0] << " " << n.color[1] << " "
                  << n.color[2] << " " << n.color[3];
                break;
            case NodeType::Multiply:
                o << "multiply " << n.a << " " << n.b;
                break;
            case NodeType::Normal:
                o << "normal " << n.source << " " << n.strength;
                break;
        }
        o << "\n";
    }
    for (const Node& n : m.nodes) {
        if (n.px >= 0 && n.py >= 0)
            o << "pos " << n.id << " " << n.px << " " << n.py << "\n";
    }
    o << "\n";

    // Gates in enum order so a re-save is byte-stable.
    for (int g = 0; g < (int)Gate::Count; ++g) {
        const Output* out = m.FindOutput((Gate)g);
        o << "output " << GateName((Gate)g) << " ";
        if (!out) o << "-";
        else o << out->source << ChannelSuffix(out->channel);
        o << "\n";
    }
    o << "\nbake " << m.bakeWidth << " " << m.bakeHeight << "\n";
    if (!m.bakeHash.empty()) o << "hash " << m.bakeHash << "\n";
    o << "baked "
      << (m.bakedDiffuse.empty() ? "-" : Quote(m.bakedDiffuse)) << " "
      << (m.bakedNormal.empty() ? "-" : Quote(m.bakedNormal)) << " "
      << (m.bakedEmissive.empty() ? "-" : Quote(m.bakedEmissive)) << "\n";
    return o.str();
}

std::string Hash(const Material& m) {
    std::ostringstream canon;
    canon << "v" << m.version << "|" << m.name << "|" << m.bakeWidth << "x" << m.bakeHeight;
    for (const Node& n : m.nodes) {
        canon << "|n:" << n.id << ":" << (int)n.type << ":" << n.path << ":" << n.a << ":"
              << n.b << ":" << n.source << ":" << n.color[0] << "," << n.color[1] << ","
              << n.color[2] << "," << n.color[3] << ":" << n.strength;
    }
    for (int g = 0; g < (int)Gate::Count; ++g) {
        const Output* o = m.FindOutput((Gate)g);
        canon << "|o:" << g << ":";
        if (o) canon << o->source << ChannelSuffix(o->channel);
    }
    // FNV-1a 64-bit, printed as 16 hex chars.
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : canon.str()) {
        h ^= (uint64_t)c;
        h *= 1099511628211ull;
    }
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
    return buf;
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------
namespace {

float ChannelOf(const Image& img, int x, int y, char ch) {
    if (ch == 'l') return img.Luma(x, y);
    const int c = (ch == 'r') ? 0 : (ch == 'g') ? 1 : (ch == 'b') ? 2 : (ch == 'a') ? 3 : -1;
    if (c < 0 || img.width <= 0 || img.height <= 0) return 0.0f;
    x = ((x % img.width) + img.width) % img.width;
    y = ((y % img.height) + img.height) % img.height;
    return img.rgba[((size_t)y * (size_t)img.width + (size_t)x) * 4u + (size_t)c];
}

struct EvalCtx {
    const Material& mat;
    const std::function<bool(const std::string&, Image&)>& loader;
    int w = 0, h = 0;
    std::unordered_map<std::string, Image> cache;
    std::unordered_set<std::string> visiting;
    std::string* err = nullptr;

    bool Fail(const std::string& why) {
        if (err) *err = why;
        return false;
    }

    bool Eval(const std::string& id, Image& out) {
        auto cached = cache.find(id);
        if (cached != cache.end()) {
            out = cached->second;
            return true;
        }
        const Node* n = mat.FindNode(id);
        if (!n) return Fail("unknown node '" + id + "'");
        if (!visiting.insert(id).second)
            return Fail("cycle through node '" + id + "'");

        bool ok = false;
        switch (n->type) {
            case NodeType::Texture: {
                Image tex;
                if (!loader(n->path, tex) || tex.width <= 0 || tex.height <= 0)
                    return Fail("texture '" + n->path + "' could not be loaded");
                out.Resize(w, h);
                for (int y = 0; y < h; ++y) {
                    for (int x = 0; x < w; ++x) {
                        const float u = ((float)x + 0.5f) / (float)w;
                        const float v = ((float)y + 0.5f) / (float)h;
                        float px[4];
                        tex.Sample(u, v, px);
                        const size_t i = ((size_t)y * (size_t)w + (size_t)x) * 4u;
                        for (int c = 0; c < 4; ++c) out.rgba[i + (size_t)c] = px[c];
                    }
                }
                ok = true;
                break;
            }
            case NodeType::Constant:
                out.Resize(w, h);
                out.Fill(n->color[0], n->color[1], n->color[2], n->color[3]);
                ok = true;
                break;
            case NodeType::Multiply: {
                Image a, b;
                if (!Eval(n->a, a) || !Eval(n->b, b)) return false;
                out.Resize(w, h);
                for (size_t i = 0; i < out.rgba.size(); ++i)
                    out.rgba[i] = a.rgba[i] * b.rgba[i];
                ok = true;
                break;
            }
            case NodeType::Normal: {
                Image height;
                if (!Eval(n->source, height)) return false;
                out.Resize(w, h);
                const float s = (n->strength > 0.0f) ? n->strength : 1.0f;
                for (int y = 0; y < h; ++y) {
                    for (int x = 0; x < w; ++x) {
                        // Image rows run top-down while UV v runs bottom-up; the
                        // Y term is negated so the baked normal map agrees with
                        // the engine's UV convention.
                        const float dx = (height.Luma(x - 1, y) - height.Luma(x + 1, y)) * s;
                        const float dy = (height.Luma(x, y - 1) - height.Luma(x, y + 1)) * s;
                        const float inv = 1.0f / std::sqrt(dx * dx + dy * dy + 1.0f);
                        const size_t i = ((size_t)y * (size_t)w + (size_t)x) * 4u;
                        out.rgba[i + 0] = dx * inv * 0.5f + 0.5f;
                        out.rgba[i + 1] = dy * inv * 0.5f + 0.5f;
                        out.rgba[i + 2] = inv * 0.5f + 0.5f;
                        out.rgba[i + 3] = 1.0f;
                    }
                }
                ok = true;
                break;
            }
        }
        visiting.erase(id);
        if (!ok) return false;
        cache[id] = out;
        return true;
    }

    // Apply a channel selector to a gate image.
    void ApplyChannel(Image& img, char ch, bool alphaGate) {
        if (!ch) return;
        for (int y = 0; y < img.height; ++y) {
            for (int x = 0; x < img.width; ++x) {
                const float v = ChannelOf(img, x, y, ch);
                const size_t i = ((size_t)y * (size_t)img.width + (size_t)x) * 4u;
                if (alphaGate) {
                    img.rgba[i + 0] = img.rgba[i + 1] = img.rgba[i + 2] = 1.0f;
                    img.rgba[i + 3] = v;
                } else {
                    img.rgba[i + 0] = img.rgba[i + 1] = img.rgba[i + 2] = v;
                }
            }
        }
    }
};

} // namespace

bool Evaluate(const Material& m, Gate gate, int width, int height, Image& out,
              const std::function<bool(const std::string&, Image&)>& loader,
              std::string* err) {
    if (err) err->clear();
    if (width <= 0 || height <= 0) {
        if (err) *err = "invalid bake size";
        return false;
    }
    const Output* o = m.FindOutput(gate);
    if (!o) {
        if (err) *err = std::string("no '") + GateName(gate) + "' gate";
        return false;
    }

    EvalCtx ctx{m, loader, width, height, {}, {}, err};
    Image src;
    if (!ctx.Eval(o->source, src)) return false;

    out = src;
    if (gate == Gate::Alpha) ctx.ApplyChannel(out, o->channel ? o->channel : 'l', true);
    else if (gate == Gate::Diffuse) {
        ctx.ApplyChannel(out, o->channel, false);
        // Layer the alpha gate, if any, over the diffuse alpha.
        if (const Output* a = m.FindOutput(Gate::Alpha)) {
            Image alpha;
            if (!ctx.Eval(a->source, alpha)) return false;
            const char ch = a->channel ? a->channel : 'l';
            const int w = out.width, h = out.height;
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const size_t i = ((size_t)y * (size_t)w + (size_t)x) * 4u;
                    out.rgba[i + 3] *= ChannelOf(alpha, x, y, ch);
                }
            }
        }
    } else if (gate == Gate::Normal || gate == Gate::Emissive) {
        ctx.ApplyChannel(out, o->channel, false);
        for (size_t i = 3; i < out.rgba.size(); i += 4) out.rgba[i] = 1.0f;
    }
    return true;
}

} // namespace ozmat
