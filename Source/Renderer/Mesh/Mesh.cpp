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
    for (int i = 0; i < m_model.materialCount; i++)
        m_baseShaders[(size_t)i] = m_model.materials[i].shader;

    m_bounds = GetModelBoundingBox(m_model);
    m_valid = true;
    OZ_INFO("Mesh: loaded '%s' (%d meshes, %d materials)",
            meshResolved.c_str(), m_model.meshCount, m_model.materialCount);
    return true;
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

    DrawModelEx(m_model, t.position, {0.0f, 1.0f, 0.0f}, t.yaw, t.scale, WHITE);
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
    } else if ((size_t)matIdx < m_baseShaders.size()) {
        mat.shader = m_baseShaders[(size_t)matIdx];
    }
    mat.maps[MATERIAL_MAP_DIFFUSE].color = tint;

    Matrix matScale = MatrixScale(t.scale.x, t.scale.y, t.scale.z);
    Matrix matRot   = MatrixRotateY(t.yaw * DEG2RAD);
    Matrix matTrans = MatrixTranslate(t.position.x, t.position.y, t.position.z);
    Matrix transform = MatrixMultiply(MatrixMultiply(matScale, matRot), matTrans);
    DrawMesh(m_model.meshes[index], mat, transform);
}

void Mesh::DrawMatrix(const Matrix& transform, Shader litShader) {
    if (!m_valid || m_model.meshCount <= 0 || m_model.materialCount <= 0) return;
    for (int i = 0; i < m_model.materialCount; i++) {
        if (litShader.id > 0) m_model.materials[i].shader = litShader;
        else if ((size_t)i < m_baseShaders.size()) m_model.materials[i].shader = m_baseShaders[(size_t)i];
    }
    for (int i = 0; i < m_model.meshCount; i++) {
        int mi = m_model.meshMaterial ? m_model.meshMaterial[i] : 0;
        if (mi < 0 || mi >= m_model.materialCount) mi = 0;
        DrawMesh(m_model.meshes[i], m_model.materials[mi], transform);
    }
}

} // namespace oz
