#include "Mesh.hpp"
#include "../../Package/PackageAssetLoader.hpp"
#include "raymath.h"
#include <cstddef>

namespace oz {

std::string ResolveMeshAsset(const std::string& baseDir, const std::string& rel) {
    if (rel.empty()) return rel;
    if (rel.rfind("GameData/", 0) == 0 || rel.rfind("GameData\\", 0) == 0) return rel;
    bool absolute = (rel.size() > 1 && rel[1] == ':') || rel[0] == '/' || rel[0] == '\\';
    if (absolute) return rel;
    if (!baseDir.empty()) {
        std::string dir = baseDir;
        if (dir.back() != '/' && dir.back() != '\\') dir += '/';
        std::string candidate = dir + rel;
        if (IsPathFile(candidate.c_str())) return candidate;
    }
    return rel;
}

Mesh::~Mesh() {
    Unload();
}

void Mesh::Unload() {
    if (m_model.meshCount > 0 || m_model.meshes != nullptr) {
        UnloadModel(m_model);
    }
    for (auto& tex : m_ownedTextures) {
        if (tex.id > 0) UnloadTexture(tex);
    }
    m_ownedTextures.clear();
    m_baseShaders.clear();
    m_baseDiffuse.clear();
    m_model = Model{0};
    m_valid = false;
}

bool Mesh::Load(const std::string& meshPath, const std::string& texPath,
                const std::string& baseDir, bool applyPointFilter) {
    m_valid = false;
    if (meshPath.empty()) return false;

    std::string meshResolved = ResolveMeshAsset(baseDir, meshPath);
    std::string texResolved  = ResolveMeshAsset(baseDir, texPath);

    // Resolve to a real on-disk path so animation loading can reuse it
    // (raylib has no LoadModelFromMemory; packaged assets land in System/Cache).
    if (IsPathFile(meshResolved.c_str())) {
        m_diskPath = meshResolved;
    } else {
        m_diskPath = PackageAssetLoader::Instance().CacheModelFile(meshResolved.c_str());
    }
    if (m_diskPath.empty() || !IsPathFile(m_diskPath.c_str())) {
        OZ_WARN("Mesh: model '%s' not found", meshResolved.c_str());
        return false;
    }

    m_model = LoadModel(m_diskPath.c_str());
    if (m_model.meshCount <= 0) {
        OZ_WARN("Mesh: failed to load '%s'", m_diskPath.c_str());
        return false;
    }

    if (!texResolved.empty()) {
        Texture2D tex = LoadTextureWithFallback(texResolved.c_str());
        if (tex.id > 0) {
            m_ownedTextures.push_back(tex);
            for (int i = 0; i < m_model.materialCount; i++)
                m_model.materials[i].maps[MATERIAL_MAP_DIFFUSE].texture = tex;
        }
    }

    if (applyPointFilter) {
        for (auto& tex : m_ownedTextures)
            if (tex.id > 0) SetTextureFilter(tex, TEXTURE_FILTER_POINT);
        for (int i = 0; i < m_model.materialCount; i++) {
            Texture2D tex = m_model.materials[i].maps[MATERIAL_MAP_DIFFUSE].texture;
            if (tex.id > 0) SetTextureFilter(tex, TEXTURE_FILTER_POINT);
        }
    }

    m_baseShaders.assign((size_t)(m_model.materialCount > 0 ? m_model.materialCount : 0), Shader{0});
    m_baseDiffuse.resize((size_t)(m_model.materialCount > 0 ? m_model.materialCount : 0));
    for (int i = 0; i < m_model.materialCount; i++) {
        m_baseShaders[(size_t)i] = m_model.materials[i].shader;
        m_baseDiffuse[(size_t)i] = m_model.materials[i].maps[MATERIAL_MAP_DIFFUSE].texture;
    }

    m_bounds = GetModelBoundingBox(m_model);
    m_valid = true;
    OZ_INFO("Mesh: loaded '%s' (%d meshes, %d materials)",
            meshResolved.c_str(), m_model.meshCount, m_model.materialCount);
    return true;
}

namespace {
// Cached gate locations, per shader program (there is one LitFog in practice).
struct MaterialGateLocs {
    unsigned int id = 0;
    int normal = -1;
    int emissive = -1;
};
MaterialGateLocs& GatesFor(Shader shader) {
    static MaterialGateLocs g;
    if (g.id != shader.id) {
        g.id       = shader.id;
        g.normal   = GetShaderLocation(shader, "uHasNormalMap");
        g.emissive = GetShaderLocation(shader, "uHasEmissive");
    }
    return g;
}
} // namespace

// Clear the two per-mesh material gates after a mesh draw.
//
// A GLSL uniform holds its last value for the whole PROGRAM, and these are the
// one pair only oz::Mesh uploads. The same LitFog program also draws OZONE
// brushes (the fast path), EngineBillboard sprites and the portal quad - none of
// which call BindMaterial. So a prop with a baked .ozmat left uHasNormalMap = 1
// (and an unrelated normal map bound) for whatever drew next in that program: in
// the editor, the whole next frame's brushes came out normal-mapped from the
// last prop on screen. Resetting here keeps the "unbound uniform" class closed
// without touching every brush/billboard call site.
static void ClearMaterialGates(Shader shader) {
    if (shader.id == 0) return;
    MaterialGateLocs& g = GatesFor(shader);
    const int zero = 0;
    if (g.normal >= 0)   SetShaderValue(shader, g.normal, &zero, SHADER_UNIFORM_INT);
    if (g.emissive >= 0) SetShaderValue(shader, g.emissive, &zero, SHADER_UNIFORM_INT);
}

void Mesh::BindMaterial(Shader shader) const {
    if (shader.id == 0) return;
    const int hasNormal = (m_material.normal.id > 0) ? 1 : 0;
    const int hasEmissive = (m_material.emissive.id > 0) ? 1 : 0;
    const float strength = m_material.normalStrength;
    const float emissiveScale = m_material.emissiveScale;
    // 1 only on a GPU-skinning build; the CPU path already posed the vertices.
    const int skinning = NeedsShaderSkinning() ? 1 : 0;

    SetShaderValue(shader, GetShaderLocation(shader, "uHasNormalMap"), &hasNormal, SHADER_UNIFORM_INT);
    SetShaderValue(shader, GetShaderLocation(shader, "uNormalStrength"), &strength, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader, GetShaderLocation(shader, "uHasEmissive"), &hasEmissive, SHADER_UNIFORM_INT);
    SetShaderValue(shader, GetShaderLocation(shader, "uEmissiveScale"), &emissiveScale, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader, GetShaderLocation(shader, "uSkinning"), &skinning, SHADER_UNIFORM_INT);
    if (hasNormal)
        SetShaderValueTexture(shader, GetShaderLocation(shader, "normalMap"), m_material.normal);
    if (hasEmissive)
        SetShaderValueTexture(shader, GetShaderLocation(shader, "emissiveMap"), m_material.emissive);
}

void Mesh::ApplyMaterialMaps() {
    for (int i = 0; i < m_model.materialCount; i++) {
        const size_t si = (size_t)i;
        Texture2D tex = m_material.diffuse;
        if (tex.id == 0 && si < m_baseDiffuse.size()) tex = m_baseDiffuse[si];
        if (tex.id > 0) m_model.materials[i].maps[MATERIAL_MAP_DIFFUSE].texture = tex;
    }
}

void Mesh::Draw(const MeshTransform& t, Shader litShader) {
    if (!m_valid || m_model.meshCount <= 0) return;

    for (int i = 0; i < m_model.materialCount; i++) {
        if (litShader.id > 0) {
            m_model.materials[i].shader = litShader;
        } else if ((size_t)i < m_baseShaders.size()) {
            m_model.materials[i].shader = m_baseShaders[(size_t)i];
        }
    }
    ApplyMaterialMaps();
    if (litShader.id > 0) BindMaterial(litShader);

    DrawModelEx(m_model, t.position, {0.0f, 1.0f, 0.0f}, t.yaw, t.scale, WHITE);
    if (litShader.id > 0) ClearMaterialGates(litShader);
}

void Mesh::DrawSubmesh(int index, const MeshTransform& t, Shader litShader, Color tint) {
    if (!m_valid || index < 0 || index >= m_model.meshCount || m_model.materialCount <= 0) return;

    int matIdx = 0;
    // meshMaterial is indexed by MESH index and has meshCount entries, so the
    // bound must be meshCount. It read materialCount, which is a different
    // array's length: with more meshes than materials the correct entry was
    // skipped and the submesh fell back to material 0.
    if (m_model.meshMaterial && index < m_model.meshCount)
        matIdx = m_model.meshMaterial[index];
    if (matIdx < 0 || matIdx >= m_model.materialCount) matIdx = 0;
    Material& mat = m_model.materials[matIdx];

    if (litShader.id > 0) {
        mat.shader = litShader;
        BindMaterial(litShader);
    } else if ((size_t)matIdx < m_baseShaders.size()) {
        mat.shader = m_baseShaders[(size_t)matIdx];
    }
    // Material diffuse for this one slot.
    {
        Texture2D tex = m_material.diffuse;
        if (tex.id == 0 && (size_t)matIdx < m_baseDiffuse.size()) tex = m_baseDiffuse[(size_t)matIdx];
        if (tex.id > 0) mat.maps[MATERIAL_MAP_DIFFUSE].texture = tex;
    }
    mat.maps[MATERIAL_MAP_DIFFUSE].color = tint;

    Matrix matScale = MatrixScale(t.scale.x, t.scale.y, t.scale.z);
    Matrix matRot   = MatrixRotateY(t.yaw * DEG2RAD);
    Matrix matTrans = MatrixTranslate(t.position.x, t.position.y, t.position.z);
    Matrix transform = MatrixMultiply(MatrixMultiply(matScale, matRot), matTrans);
    DrawMesh(m_model.meshes[index], mat, transform);
    if (litShader.id > 0) ClearMaterialGates(litShader);
}

void Mesh::DrawSubmeshMatrix(int index, const Matrix& transform, Shader litShader,
                             Color tint) {
    if (!m_valid || index < 0 || index >= m_model.meshCount || m_model.materialCount <= 0) return;
    int matIdx = 0;
    if (m_model.meshMaterial && index < m_model.meshCount) matIdx = m_model.meshMaterial[index];
    if (matIdx < 0 || matIdx >= m_model.materialCount) matIdx = 0;
    Material& mat = m_model.materials[matIdx];
    if (litShader.id > 0) {
        mat.shader = litShader;
        BindMaterial(litShader);
    } else if ((size_t)matIdx < m_baseShaders.size()) {
        mat.shader = m_baseShaders[(size_t)matIdx];
    }
    {
        Texture2D tex = m_material.diffuse;
        if (tex.id == 0 && (size_t)matIdx < m_baseDiffuse.size()) tex = m_baseDiffuse[(size_t)matIdx];
        if (tex.id > 0) mat.maps[MATERIAL_MAP_DIFFUSE].texture = tex;
    }
    mat.maps[MATERIAL_MAP_DIFFUSE].color = tint;
    DrawMesh(m_model.meshes[index], mat, transform);
    if (litShader.id > 0) ClearMaterialGates(litShader);
}

void Mesh::DrawMatrix(const Matrix& transform, Shader litShader) {
    if (!m_valid || m_model.meshCount <= 0 || m_model.materialCount <= 0) return;
    for (int i = 0; i < m_model.materialCount; i++) {
        if (litShader.id > 0) m_model.materials[i].shader = litShader;
        else if ((size_t)i < m_baseShaders.size()) m_model.materials[i].shader = m_baseShaders[(size_t)i];
    }
    if (litShader.id > 0) BindMaterial(litShader);
    ApplyMaterialMaps();
    for (int i = 0; i < m_model.meshCount; i++) {
        int mi = m_model.meshMaterial ? m_model.meshMaterial[i] : 0;
        if (mi < 0 || mi >= m_model.materialCount) mi = 0;
        DrawMesh(m_model.meshes[i], m_model.materials[mi], transform);
    }
    if (litShader.id > 0) ClearMaterialGates(litShader);
}

} // namespace oz
