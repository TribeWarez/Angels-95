#pragma once
// ---------------------------------------------------------------------------
// Material/MaterialRegistry — runtime side of a baked material graph.
//
// A `.ozmat` describes the graph; the editor bakes the gates to PNGs; this
// registry loads those PNGs and hands the draw path a MeshMaterial with the
// normal + emissive maps. Nothing here evaluates the graph.
// ---------------------------------------------------------------------------

#include "raylib.h"
#include "OzMaterial.hpp"
#include <string>
#include <vector>

namespace ozmat {

struct MaterialAsset {
    std::string name;
    Texture2D diffuse{0};
    Texture2D normal{0};
    Texture2D emissive{0};
    float normalStrength = 1.0f;
    float emissiveScale = 1.0f;
    bool stale = false;   // bake hash != graph hash; still usable, just warned
};

class MaterialRegistry {
public:
    static MaterialRegistry& Instance();

    // Scan `dir` for *.ozmat (loose + packaged) and load their baked maps.
    // Safe to call more than once; a reload unloads the previous textures.
    void Init(const char* dir = "GameData/Materials");
    void Shutdown();
    void Reload() { Init(m_dir.c_str()); }

    const MaterialAsset* Find(const std::string& name) const;
    const std::vector<MaterialAsset>& All() const { return m_assets; }
    bool Loaded() const { return m_initialized; }

private:
    MaterialRegistry() = default;
    bool LoadOne(const std::string& path, const std::string& text);
    void Unload();

    std::string m_dir = "GameData/Materials";
    std::vector<MaterialAsset> m_assets;
    bool m_initialized = false;
    bool m_warnedMissingDir = false;
};

} // namespace ozmat
