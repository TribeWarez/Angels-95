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

    // Measure the skin/skeleton fit once. raylib keeps only the FIRST skin of a
    // multi-skin glTF, so a mesh whose bone indices belong to another skin runs
    // off the end of model.boneMatrices during CPU skinning - garbage geometry
    // or a crash, with nothing in the log. Consumers that do not intend to force
    // playback go through MeshCache::GetSkeletalChecked and get a static mesh.
    m_skeletonFits = BoneIdsFitSkeleton(m_model);
    if (!m_skeletonFits) {
        OZ_WARN("SkeletalMesh: '%s' bone indices do not fit its %d-bone skeleton "
                "(multi-skin glTF? raylib loads one skin) - stays static unless forced",
                meshPath.c_str(), OZ_MODEL_BONE_COUNT(m_model));
    }

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

int SkeletalMesh::ResolveClip(const std::string& preferred) const {
    if (m_animCount <= 0) return -1;
    if (!preferred.empty()) {
        const int wanted = FindClip(preferred);
        if (wanted >= 0) return wanted;
    }
    const int idle = FindClip("Idle");
    if (idle >= 0) return idle;
    return 0;
}

float SkeletalMesh::ClipSeconds(int index, float fps) const {
    if (index < 0 || index >= m_animCount) return 0.0f;
    int frames = OZ_ANIM_KEYFRAME_COUNT(m_anims[index]);
    if (frames <= 0) return 0.0f;
    if (fps <= 0.0f) fps = kSkeletalClipFps;
    return (float)frames / fps;
}

int SkeletalMesh::ClipFrameCount(int index) const {
    if (index < 0 || index >= m_animCount) return -1;
    return OZ_ANIM_KEYFRAME_COUNT(m_anims[index]);
}

void SkeletalMesh::ApplyPose(int clipIndex, float timeSeconds, float fps) const {
    if (!m_valid || clipIndex < 0 || clipIndex >= m_animCount) return;
    const ModelAnimation& anim = m_anims[clipIndex];
    const int frames = OZ_ANIM_KEYFRAME_COUNT(anim);
    if (frames <= 0) return;
    // A mismatch (a skin dropped by raylib because the glTF declared several, or
    // a clip taken from a different rig) used to be a silent no-op: the model
    // just stood in its bind pose with nothing in the log to say why. Warn once
    // per asset - this runs once per instance per frame.
    if (!IsModelAnimationValid(m_model, anim)) {
        if (!m_invalidWarned) {
            m_invalidWarned = true;
            OZ_WARN("SkeletalMesh: clip '%s' (%d bones) does not match '%s' (%d bones) - animation cannot play",
                    anim.name, anim.boneCount, m_diskPath.c_str(), OZ_MODEL_BONE_COUNT(m_model));
        }
        return;
    }

    if (fps <= 0.0f) fps = kSkeletalClipFps;
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
