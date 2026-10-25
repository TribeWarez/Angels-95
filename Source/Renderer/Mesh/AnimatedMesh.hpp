#pragma once
#include "Mesh.hpp"
#include "../../Package/Anim/OzAnimFormat.hpp"
#include <vector>
#include <string>

namespace oz {

// GameEngine.Mesh.Skeletal with an external vertex-keyframe clip (`.ozanim`).
//
// The clip stores sparse per-vertex offsets that are ADDED to the model's base
// vertex positions. Playback CPU-updates the position VBO (index 0) via
// UpdateMeshBuffer — apply-pose-then-draw so multiple instances can share the
// cached Model without permanently corrupting it.
//
// Vertex indices in the clip are GLOBAL across the model (all meshes
// concatenated in load order).
class AnimatedMesh : public Mesh {
public:
    AnimatedMesh() = default;
    ~AnimatedMesh() override = default;

    bool LoadAnimated(const std::string& meshPath, const std::string& texPath,
                      const std::string& animFile, const std::string& baseDir,
                      bool applyPointFilter);

    int ClipCount() const { return (int)m_anim.clips.size(); }
    const char* ClipName(int index) const;
    int FindClip(const std::string& name) const;

    // Same contract as oz::SkeletalMesh::ResolveClip: `preferred`, else "Idle",
    // else clip 0 (-1 only when the file declares no clips).
    int ResolveClip(const std::string& preferred) const;

    // Apply a clip pose at timeSeconds (samples + uploads). No-op if invalid.
    void ApplyVertexPose(int clipIndex, float timeSeconds);

    // Upload an explicit dense offset buffer (size vertexCount*3) added to base.
    void UploadOffsets(const std::vector<float>& denseOffsets);

    int TotalVertexCount() const { return m_vertexCount; }
    const std::vector<float>& BasePositions() const { return m_base; }
    const ozanim::Animation& GetAnimation() const { return m_anim; }
    ozanim::Animation& MutableAnimation() { return m_anim; }
    void SetAnimation(const ozanim::Animation& a) { m_anim = a; }

private:
    struct MeshSlice {
        int meshIndex = 0;
        int vertexOffset = 0; // global index of this mesh's first vertex
        int vertexCount = 0;
    };

    std::vector<MeshSlice> m_slices;
    std::vector<float> m_base;    // concatenated base positions (vertexCount*3)
    std::vector<float> m_scratch; // dense offset buffer (vertexCount*3)
    std::vector<float> m_upload;  // per-slice upload buffer
    ozanim::Animation m_anim;
    int m_vertexCount = 0;
};

} // namespace oz
