#include "MaterialRegistry.hpp"

#include "../Package/PackageAssetLoader.hpp"
#include "../Log.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace ozmat {

namespace {

std::string DirOf(const std::string& path) {
    const size_t s = path.find_last_of("/\\");
    return (s == std::string::npos) ? std::string() : path.substr(0, s + 1);
}

std::string StemOf(const std::string& path) {
    size_t b = path.find_last_of("/\\");
    b = (b == std::string::npos) ? 0 : b + 1;
    size_t e = path.find_last_of('.');
    if (e == std::string::npos || e < b) e = path.size();
    return path.substr(b, e - b);
}

// A baked path is normally GameData-rooted and used verbatim; a hand-written
// relative one resolves beside its .ozmat.
std::string ResolveBaked(const std::string& materialPath, const std::string& baked) {
    if (baked.empty()) return "";
    if (baked.rfind("GameData/", 0) == 0 || baked.rfind("GameData\\", 0) == 0) return baked;
    const bool absolute = (baked.size() > 1 && baked[1] == ':') || baked[0] == '/' || baked[0] == '\\';
    if (absolute) return baked;
    return DirOf(materialPath) + baked;
}

} // namespace

MaterialRegistry& MaterialRegistry::Instance() {
    static MaterialRegistry* instance = new MaterialRegistry();
    return *instance;
}

void MaterialRegistry::Unload() {
    for (MaterialAsset& a : m_assets) {
        if (a.diffuse.id > 0) UnloadTexture(a.diffuse);
        if (a.normal.id > 0) UnloadTexture(a.normal);
        if (a.emissive.id > 0) UnloadTexture(a.emissive);
    }
    m_assets.clear();
}

void MaterialRegistry::Shutdown() {
    Unload();
    m_initialized = false;
}

bool MaterialRegistry::LoadOne(const std::string& path, const std::string& text) {
    Material graph;
    std::string err;
    if (!Parse(text, graph, &err)) {
        OZ_WARN("Material '%s': %s", path.c_str(), err.c_str());
        return false;
    }
    MaterialAsset asset;
    asset.name = graph.name.empty() ? StemOf(path) : graph.name;
    asset.normalStrength = 1.0f;
    asset.emissiveScale = 1.0f;
    for (const Node& n : graph.nodes) {
        if (n.type == NodeType::Normal) asset.normalStrength = n.strength;
    }

    const std::string diffPath = ResolveBaked(path, graph.bakedDiffuse);
    const std::string normPath = ResolveBaked(path, graph.bakedNormal);
    const std::string emisPath = ResolveBaked(path, graph.bakedEmissive);

    if (!diffPath.empty()) asset.diffuse = LoadTextureWithFallback(diffPath.c_str());
    if (!normPath.empty()) asset.normal = LoadTextureWithFallback(normPath.c_str());
    if (!emisPath.empty()) asset.emissive = LoadTextureWithFallback(emisPath.c_str());
    if (asset.normal.id > 0) SetTextureFilter(asset.normal, TEXTURE_FILTER_BILINEAR);
    if (asset.emissive.id > 0) SetTextureFilter(asset.emissive, TEXTURE_FILTER_BILINEAR);

    if (asset.diffuse.id == 0 && asset.normal.id == 0 && asset.emissive.id == 0) {
        OZ_WARN("Material '%s': no baked maps yet (run Material Bake in AngelEd)", asset.name.c_str());
        return false;
    }

    const std::string want = Hash(graph);
    if (!graph.bakeHash.empty() && graph.bakeHash != want) {
        asset.stale = true;
        OZ_WARN("Material '%s': bake is stale (graph changed); re-bake in AngelEd", asset.name.c_str());
    } else if (graph.bakeHash.empty()) {
        asset.stale = true;
    }

    // Later definitions replace earlier ones: a loose file should win over a
    // packaged copy, and the scan feeds loose files first.
    for (MaterialAsset& existing : m_assets) {
        if (existing.name == asset.name) {
            if (existing.diffuse.id > 0) UnloadTexture(existing.diffuse);
            if (existing.normal.id > 0) UnloadTexture(existing.normal);
            if (existing.emissive.id > 0) UnloadTexture(existing.emissive);
            existing = asset;
            return true;
        }
    }
    m_assets.push_back(asset);
    return true;
}

void MaterialRegistry::Init(const char* dir) {
    Unload();
    m_initialized = true;
    m_dir = (dir && *dir) ? dir : "GameData/Materials";

    // Loose files first, then packaged copies of files not present on disk.
    std::vector<std::string> seen;
    std::error_code ec;
    if (fs::exists(m_dir, ec)) {
        std::vector<fs::path> files;
        for (auto& e : fs::directory_iterator(m_dir, ec)) {
            if (e.is_regular_file() && e.path().extension() == ".ozmat")
                files.push_back(e.path());
        }
        std::sort(files.begin(), files.end());
        for (const fs::path& p : files) {
            std::ifstream f(p);
            if (!f.is_open()) continue;
            std::stringstream ss;
            ss << f.rdbuf();
            if (LoadOne(p.generic_string(), ss.str()))
                seen.push_back(p.filename().string());
        }
    } else if (!m_warnedMissingDir) {
        OZ_WARN("MaterialRegistry: '%s' does not exist", m_dir.c_str());
        m_warnedMissingDir = true;
        return;
    }

    for (const std::string& base : PackageAssetLoader::Instance().ListAssetBasenames(".ozmat")) {
        if (std::find(seen.begin(), seen.end(), base) != seen.end()) continue;
        const std::string key = m_dir + "/" + base;
        std::string copy = LoadFileTextWithFallback(key.c_str());
        if (copy.empty()) continue;
        LoadOne(key, copy);
    }

    OZ_INFO("MaterialRegistry: %d material(s) from %s", (int)m_assets.size(), m_dir.c_str());
}

const MaterialAsset* MaterialRegistry::Find(const std::string& name) const {
    for (const MaterialAsset& a : m_assets)
        if (a.name == name) return &a;
    return nullptr;
}

} // namespace ozmat
