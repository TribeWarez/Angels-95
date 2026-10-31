#include "Mesh.hpp"
#include "../../Package/PackageAssetLoader.hpp"
#include "../../Diagnostics/FrameProfiler.hpp"
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
//
// BindMaterial runs once per lit mesh draw, and it used to resolve five uniform
// locations with GetShaderLocation on every one of them - ~600 lookups/frame in
// a Mesh.*-heavy world. Uniform locations are fixed after link, so the whole set
// is resolved once per program id.
struct MaterialGateLocs {
    unsigned int id = 0;
    int normal = -1;
    int normalStrength = -1;
    int emissive = -1;
    int emissiveScale = -1;
    int skinning = -1;
    int normalMap = -1;
    int emissiveMap = -1;

    // Last value actually uploaded to THIS program. GLSL uniforms persist for
    // the program's lifetime, so an upload whose value is unchanged is pure
    // overhead. The pair (normal, emissive) is reset after every mesh draw so a
    // normal-mapped prop cannot leak its gate onto the brushes that follow; the
    // cache makes that reset a no-op in the common case.
    int lastNormal = 0;
    int lastEmissive = 0;
    int lastSkinning = 0;
    float lastStrength = 1.0f;
    float lastEmissiveScale = 1.0f;
};
MaterialGateLocs& GatesFor(Shader shader) {
    static MaterialGateLocs g;
    if (g.id != shader.id) {
        g = MaterialGateLocs{};
        g.id             = shader.id;
        g.normal         = GetShaderLocation(shader, "uHasNormalMap");
        g.normalStrength = GetShaderLocation(shader, "uNormalStrength");
        g.emissive       = GetShaderLocation(shader, "uHasEmissive");
        g.emissiveScale  = GetShaderLocation(shader, "uEmissiveScale");
        g.skinning       = GetShaderLocation(shader, "uSkinning");
        g.normalMap      = GetShaderLocation(shader, "normalMap");
        g.emissiveMap    = GetShaderLocation(shader, "emissiveMap");
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
    if (g.lastNormal != 0) {
        if (g.normal >= 0) SetShaderValue(shader, g.normal, &zero, SHADER_UNIFORM_INT);
        g.lastNormal = 0;
    }
    if (g.lastEmissive != 0) {
        if (g.emissive >= 0) SetShaderValue(shader, g.emissive, &zero, SHADER_UNIFORM_INT);
        g.lastEmissive = 0;
    }
    // Skinning is a per-mesh property too; a brush after a skinned prop must not
    // keep the branch on (its mesh has no bone attributes, but the shader would
    // still evaluate the branch).
    if (g.lastSkinning != 0) {
        if (g.skinning >= 0) SetShaderValue(shader, g.skinning, &zero, SHADER_UNIFORM_INT);
        g.lastSkinning = 0;
    }
}

void Mesh::BindMaterial(Shader shader) const {
    if (shader.id == 0) return;
    MaterialGateLocs& g = GatesFor(shader);
    const int hasNormal = (m_material.normal.id > 0) ? 1 : 0;
    const int hasEmissive = (m_material.emissive.id > 0) ? 1 : 0;
    const float strength = m_material.normalStrength;
    const float emissiveScale = m_material.emissiveScale;
    // 1 only on a GPU-skinning build; the CPU path already posed the vertices.
    const int skinning = NeedsShaderSkinning() ? 1 : 0;

    int uploads = 0;
    auto setInt = [&](int loc, int& last, int value) {
        if (last == value) return;
        if (loc >= 0) SetShaderValue(shader, loc, &value, SHADER_UNIFORM_INT);
        last = value;
        ++uploads;
    };
    setInt(g.normal, g.lastNormal, hasNormal);
    setInt(g.emissive, g.lastEmissive, hasEmissive);
    setInt(g.skinning, g.lastSkinning, skinning);
    if (g.lastStrength != strength) {
        if (g.normalStrength >= 0)
            SetShaderValue(shader, g.normalStrength, &strength, SHADER_UNIFORM_FLOAT);
        g.lastStrength = strength;
        ++uploads;
    }
    if (g.lastEmissiveScale != emissiveScale) {
        if (g.emissiveScale >= 0)
            SetShaderValue(shader, g.emissiveScale, &emissiveScale, SHADER_UNIFORM_FLOAT);
        g.lastEmissiveScale = emissiveScale;
        ++uploads;
    }
    if (hasNormal) {
        SetShaderValueTexture(shader, g.normalMap, m_material.normal);
        ++uploads;
    }
    if (hasEmissive) {
        SetShaderValueTexture(shader, g.emissiveMap, m_material.emissive);
        ++uploads;
    }

    OZ_PROFILE_COUNT("mesh.bind");
    OZ_PROFILE_ADD("uniform.upload", (uint64_t)uploads);
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
