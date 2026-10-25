#include "AnimatedMesh.hpp"
#include "../../Package/PackageAssetLoader.hpp"
#include <cctype>

namespace oz {

bool AnimatedMesh::LoadAnimated(const std::string& meshPath, const std::string& texPath,
                                const std::string& animFile, const std::string& baseDir,
                                bool applyPointFilter) {
    if (!Mesh::Load(meshPath, texPath, baseDir, applyPointFilter)) return false;

    std::string animResolved = ResolveMeshAsset(baseDir, animFile);
    std::string text = LoadFileTextWithFallback(animResolved.c_str());
    if (text.empty()) {
        OZ_WARN("AnimatedMesh: anim file '%s' not found (fallback to static)", animResolved.c_str());
        return false;
    }

    m_anim = ozanim::Parse(text);
    if (m_anim.clips.empty()) {
        OZ_WARN("AnimatedMesh: no clips in '%s' (fallback to static)", animResolved.c_str());
        return false;
    }

    // Base positions + per-mesh slices (global vertex index space).
    m_vertexCount = 0;
    m_slices.clear();
    for (int i = 0; i < m_model.meshCount; i++) {
        MeshSlice s;
        s.meshIndex = i;
        s.vertexOffset = m_vertexCount;
        s.vertexCount = m_model.meshes[i].vertexCount;
        m_slices.push_back(s);
        m_vertexCount += s.vertexCount;
    }

    m_base.assign((size_t)m_vertexCount * 3, 0.0f);
    int g = 0;
    for (int i = 0; i < m_model.meshCount; i++) {
        ::Mesh& m = m_model.meshes[i];
        int n = m.vertexCount * 3;
        if (m.vertices) {
            for (int k = 0; k < n; k++)
                m_base[(size_t)g * 3 + k] = m.vertices[k];
        }
        g += m.vertexCount;
    }
    m_scratch.assign((size_t)m_vertexCount * 3, 0.0f);

    OZ_INFO("AnimatedMesh: '%s' + '%s' (%d clips, %d vertices)",
            meshPath.c_str(), animResolved.c_str(), (int)m_anim.clips.size(), m_vertexCount);

    // Apply the first clip at t=0 so the object shows a valid pose immediately.
    if (m_vertexCount > 0) ApplyVertexPose(0, 0.0f);
    return true;
}

const char* AnimatedMesh::ClipName(int index) const {
    if (index < 0 || index >= (int)m_anim.clips.size()) return "";
    return m_anim.clips[index].name.c_str();
}

int AnimatedMesh::FindClip(const std::string& name) const {
    if (name.empty()) return -1;
    auto lower = [](std::string s) {
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };
    std::string want = lower(name);
    for (int i = 0; i < (int)m_anim.clips.size(); i++)
        if (lower(m_anim.clips[i].name) == want) return i;
    for (int i = 0; i < (int)m_anim.clips.size(); i++)
        if (lower(m_anim.clips[i].name).find(want) != std::string::npos) return i;
    return -1;
}

int AnimatedMesh::ResolveClip(const std::string& preferred) const {
    if (m_anim.clips.empty()) return -1;
    if (!preferred.empty()) {
        const int wanted = FindClip(preferred);
        if (wanted >= 0) return wanted;
    }
    const int idle = FindClip("Idle");
    return (idle >= 0) ? idle : 0;
}

void AnimatedMesh::UploadOffsets(const std::vector<float>& denseOffsets) {
    if (!m_valid || m_vertexCount <= 0) return;

    for (const auto& s : m_slices) {
        if (s.meshIndex < 0 || s.meshIndex >= m_model.meshCount) continue;
        m_upload.assign((size_t)s.vertexCount * 3, 0.0f);
        for (int v = 0; v < s.vertexCount; v++) {
            int gv = s.vertexOffset + v;
            for (int c = 0; c < 3; c++) {
                float base = m_base[(size_t)gv * 3 + c];
                size_t oi = (size_t)gv * 3 + c;
                float off = (oi < denseOffsets.size()) ? denseOffsets[oi] : 0.0f;
                m_upload[(size_t)v * 3 + c] = base + off;
            }
        }
        ::Mesh& m = m_model.meshes[s.meshIndex];
        if (m.vboId != nullptr) {
            UpdateMeshBuffer(m, 0, m_upload.data(),
                             s.vertexCount * 3 * (int)sizeof(float), 0);
        }
    }
}

void AnimatedMesh::ApplyVertexPose(int clipIndex, float timeSeconds) {
    if (!m_valid || clipIndex < 0 || clipIndex >= (int)m_anim.clips.size()) return;
    m_anim.clips[clipIndex].SampleOffsets(timeSeconds, m_vertexCount, m_scratch);
    UploadOffsets(m_scratch);
}

} // namespace oz
