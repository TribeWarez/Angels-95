#include "SkeletalMesh.hpp"
#include "../../Log.hpp"
#include <cctype>
#include <cmath>

// raylib renamed ModelAnimation.frameCount -> keyframeCount in 6.0. Support both
// so the engine builds against the pinned 5.5 (CI) and newer local toolchains.
#if defined(RAYLIB_VERSION_MAJOR) && RAYLIB_VERSION_MAJOR >= 6
    #define OZ_ANIM_KEYFRAME_COUNT(a) ((a).keyframeCount)
#else
    #define OZ_ANIM_KEYFRAME_COUNT(a) ((a).frameCount)
#endif

namespace oz {

SkeletalMesh::~SkeletalMesh() {
    if (m_anims && m_animCount > 0) {
        UnloadModelAnimations(m_anims, m_animCount);
    }
    m_anims = nullptr;
    m_animCount = 0;
}

bool SkeletalMesh::LoadSkeletal(const std::string& meshPath, const std::string& texPath,
                                const std::string& baseDir, bool applyPointFilter) {
    if (!Mesh::Load(meshPath, texPath, baseDir, applyPointFilter)) return false;

    int count = 0;
    m_anims = LoadModelAnimations(m_diskPath.c_str(), &count);
    m_animCount = (m_anims && count > 0) ? count : 0;
    if (m_animCount <= 0) {
        OZ_WARN("SkeletalMesh: no animation clips in '%s' (static fallback)", m_diskPath.c_str());
    } else {
        OZ_INFO("SkeletalMesh: loaded %d clip(s) from '%s'", m_animCount, m_diskPath.c_str());
    }
    return true;
}

const char* SkeletalMesh::ClipName(int index) const {
    if (index < 0 || index >= m_animCount) return "";
    return m_anims[index].name;
}

int SkeletalMesh::FindClip(const std::string& name) const {
    if (name.empty() || m_animCount <= 0) return -1;
    auto lower = [](std::string s) {
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };
    std::string want = lower(name);
    for (int i = 0; i < m_animCount; i++) {
        if (lower(m_anims[i].name) == want) return i;
    }
    for (int i = 0; i < m_animCount; i++) {
        if (lower(m_anims[i].name).find(want) != std::string::npos) return i;
    }
    return -1;
}

float SkeletalMesh::ClipSeconds(int index, float fps) const {
    if (index < 0 || index >= m_animCount) return 0.0f;
    int frames = OZ_ANIM_KEYFRAME_COUNT(m_anims[index]);
    if (frames <= 0) return 0.0f;
    if (fps <= 0.0f) fps = 30.0f;
    return (float)frames / fps;
}

void SkeletalMesh::ApplyPose(int clipIndex, float timeSeconds, float fps) const {
    if (!m_valid || clipIndex < 0 || clipIndex >= m_animCount) return;
    const ModelAnimation& anim = m_anims[clipIndex];
    const int frames = OZ_ANIM_KEYFRAME_COUNT(anim);
    if (frames <= 0) return;
    if (!IsModelAnimationValid(m_model, anim)) return;

    if (fps <= 0.0f) fps = 30.0f;
    float frame = std::fmod(timeSeconds * fps, (float)frames);
    if (frame < 0.0f) frame += (float)frames;
    UpdateModelAnimation(m_model, anim, frame);
}

bool SkeletalMesh::NeedsShaderSkinning() const {
    if (!m_valid || m_model.meshCount <= 0) return false;
    if (OZ_MODEL_BONE_COUNT(m_model) <= 0) return false;
    // CPU-skinning builds allocate animVertices and upload the posed positions,
    // so the shader must leave them alone. GPU builds leave it null.
    return m_model.meshes[0].animVertices == nullptr;
}

bool BoneIdsFitSkeleton(const ::Model& model) {
    // No skeleton loaded: no vertex may legally reference a bone, so any
    // bone indices at all (the multi-skin case where the skin was dropped)
    // fails.
    if (model.meshCount <= 0 || model.meshes == nullptr) return false;
    if (OZ_MODEL_BONE_COUNT(model) <= 0) {
        for (int i = 0; i < model.meshCount; i++)
            if (OZ_MESH_BONE_INDICES(model.meshes[i]) != nullptr) return false;
        return true;   // fully unskinned model: nothing to animate, nothing to crash
    }
    for (int i = 0; i < model.meshCount; i++) {
        const ::Mesh& rm = model.meshes[i];
        const unsigned char* indices = OZ_MESH_BONE_INDICES(rm);
        if (indices == nullptr) continue;   // unskinned submesh: nothing to check
        const int n = rm.vertexCount * 4;   // four bone influences per vertex
        for (int j = 0; j < n; j++)
            if ((int)indices[j] >= OZ_MODEL_BONE_COUNT(model)) return false;
    }
    return true;
}

} // namespace oz
