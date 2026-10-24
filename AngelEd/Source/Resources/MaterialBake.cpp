#include "MaterialBake.hpp"

#include "../../../Source/Material/OzMaterial.hpp"
#include "../../../Source/Package/PackageAssetLoader.hpp"
#include "../../../Source/Log.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

static const char* kMaterialDir = "GameData/Materials";
const char* MaterialBakeDir() { return kMaterialDir; }

namespace {

bool DecodeImage(const std::string& path, ozmat::Image& out) {
    ::Image img = LoadImageWithFallback(path.c_str());
    if (img.data == nullptr || img.width <= 0 || img.height <= 0) return false;
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    out.Resize(img.width, img.height);
    const unsigned char* src = (const unsigned char*)img.data;
    const size_t count = std::min(out.rgba.size(), (size_t)img.width * (size_t)img.height * 4u);
    for (size_t i = 0; i < count; ++i) out.rgba[i] = (float)src[i] / 255.0f;
    UnloadImage(img);
    return true;
}

bool WritePng(const ozmat::Image& src, const std::string& path) {
    if (src.width <= 0 || src.height <= 0) return false;
    ::Image img{};
    img.width = src.width;
    img.height = src.height;
    img.mipmaps = 1;
    img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    img.data = MemAlloc((unsigned int)src.rgba.size());
    if (!img.data) return false;
    unsigned char* dst = (unsigned char*)img.data;
    for (size_t i = 0; i < src.rgba.size(); ++i) {
        const float v = std::min(1.0f, std::max(0.0f, src.rgba[i]));
        dst[i] = (unsigned char)(v * 255.0f + 0.5f);
    }
    const bool ok = ExportImage(img, path.c_str());
    UnloadImage(img);
    return ok;
}

// Store an output path the RUNTIME can resolve: GameData-rooted when the
// material lives under GameData, otherwise the path as given.
std::string RuntimePath(const fs::path& outDir, const char* file) {
    fs::path p = outDir / file;
    std::string s = p.generic_string();
    const std::string gd = "GameData/";
    const size_t at = s.find(gd);
    if (at != std::string::npos) return s.substr(at);
    return s;
}

} // namespace

bool BakeMaterialFile(const std::string& ozmatPath, std::string* log) {
    auto say = [&](const std::string& s) {
        if (log) { if (!log->empty()) *log += "\n"; *log += s; }
    };

    std::error_code ec;
    if (!fs::exists(ozmatPath, ec)) {
        say(ozmatPath + ": not a loose file (packaged materials are read-only)");
        return false;
    }
    std::ifstream in(ozmatPath, std::ios::binary);
    if (!in.is_open()) {
        say(ozmatPath + ": could not open");
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    in.close();

    ozmat::Material m;
    std::string err;
    if (!ozmat::Parse(ss.str(), m, &err)) {
        say(ozmatPath + ": " + err);
        return false;
    }

    const fs::path path(ozmatPath);
    const fs::path outDir = path.parent_path() / path.stem();
    fs::create_directories(outDir, ec);

    auto loader = [](const std::string& p, ozmat::Image& img) { return DecodeImage(p, img); };

    std::string bakedDiffuse, bakedNormal, bakedEmissive;
    auto bakeGate = [&](ozmat::Gate gate, const char* file, std::string* outPath) -> bool {
        if (!m.FindOutput(gate)) return true;   // gate not authored
        ozmat::Image img;
        std::string e;
        if (!ozmat::Evaluate(m, gate, m.bakeWidth, m.bakeHeight, img, loader, &e)) {
            say(std::string("gate '") + ozmat::GateName(gate) + "': " + e);
            return false;
        }
        const std::string full = (outDir / file).generic_string();
        if (!WritePng(img, full)) {
            say("could not write " + full);
            return false;
        }
        *outPath = RuntimePath(outDir, file);
        return true;
    };

    if (!bakeGate(ozmat::Gate::Diffuse, "albedo.png", &bakedDiffuse)) return false;
    if (!bakeGate(ozmat::Gate::Normal, "normal.png", &bakedNormal)) return false;
    if (!bakeGate(ozmat::Gate::Emissive, "emissive.png", &bakedEmissive)) return false;

    m.bakedDiffuse = bakedDiffuse;
    m.bakedNormal = bakedNormal;
    m.bakedEmissive = bakedEmissive;
    m.bakeHash = ozmat::Hash(m);

    std::ofstream out(ozmatPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        say(ozmatPath + ": could not write");
        return false;
    }
    out << ozmat::Serialize(m);

    say(path.filename().generic_string() + ": baked " + std::to_string(m.bakeWidth) + "x" +
        std::to_string(m.bakeHeight));
    return true;
}

int BakeAllMaterials(std::string* log) {
    std::error_code ec;
    if (!fs::exists(kMaterialDir, ec)) {
        if (log) *log = std::string(kMaterialDir) + " does not exist";
        return 0;
    }
    std::vector<fs::path> files;
    for (auto& e : fs::directory_iterator(kMaterialDir, ec)) {
        if (e.is_regular_file() && e.path().extension() == ".ozmat") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    int baked = 0;
    for (const fs::path& p : files)
        if (BakeMaterialFile(p.generic_string(), log)) ++baked;
    return baked;
}
