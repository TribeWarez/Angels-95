#pragma once
#include "raylib.h"
#include <string>
#include <vector>
#include <memory>

// ---------------------------------------------------------------------------
// oz::Mesh — base render primitive for the GameEngine.Mesh taxonomy.
//
//   GameEngine.Mesh
//     GameEngine.Mesh.Static    -> oz::StaticMesh   (props / map objects)
//     GameEngine.Mesh.Skeletal  -> oz::SkeletalMesh (animated NPC/player/item)
//
// The asset (Model + animation clips) is shared through oz::MeshCache; per
// instance playback state lives on the owning node (Pawn / mesh object), never
// inside the shared Mesh.
//
// NOTE: raylib already defines global ::Mesh, ::Model and ::Transform, hence the
// `oz` namespace and the oz::MeshTransform spelling.
// ---------------------------------------------------------------------------

namespace oz {

// Transform used for drawing a mesh instance (engine is Y-up, yaw = degrees).
struct MeshTransform {
    Vector3 position{0.0f, 0.0f, 0.0f};
    float yaw = 0.0f;
    Vector3 scale{1.0f, 1.0f, 1.0f};
};

enum class MeshRenderMode : unsigned char {
    LIT = 0,
    UNLIT = 1,
    WIRE = 2
};

// Baked material maps (see Material/OzMaterial.hpp). Owned by MaterialRegistry;
// a Mesh only borrows the handles for the duration of a draw.
struct MeshMaterial {
    Texture2D diffuse{0};   // baked diffuse gate; overrides the model's own map
    Texture2D normal{0};
    Texture2D emissive{0};
    float normalStrength = 1.0f;
    float emissiveScale = 1.0f;
    bool Valid() const { return diffuse.id > 0 || normal.id > 0 || emissive.id > 0; }
};

// Resolve a def-relative asset path: absolute and GameData-rooted paths are used
// verbatim, relative paths are first tried beside the declaring file, otherwise
// handed to the filesystem/package loader.
std::string ResolveMeshAsset(const std::string& baseDir, const std::string& rel);

class Mesh {
public:
    Mesh() = default;
    virtual ~Mesh();

    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;

    // Load a model and optional diffuse texture. `baseDir` resolves relative
    // paths (must exist for path-relative assets). `applyPointFilter` applies
    // TEXTURE_FILTER_POINT for the PS1 look.
    bool Load(const std::string& meshPath, const std::string& texPath,
              const std::string& baseDir, bool applyPointFilter);

    virtual void Update(float dt) { (void)dt; }

    // Bind the shader to every material slot and draw at the transform.
    virtual void Draw(const MeshTransform& t, Shader litShader);

    // Draw a single submesh (by model mesh index) with a diffuse tint. Used for
    // picking one projectile out of a multi-mesh model.
    void DrawSubmesh(int index, const MeshTransform& t, Shader litShader, Color tint = WHITE);

    // Draw one submesh with an explicit world matrix. Used by the view-model's
    // rigid attachment parts (the detachable magazine), which need to move with
    // the weapon but apply their own local transform.
    void DrawSubmeshMatrix(int index, const Matrix& transform, Shader litShader,
                           Color tint = WHITE);

    // Draw every submesh with an explicit world matrix. Used by the view-model
    // so the weapon inherits the camera's full orientation (pitch + yaw + roll).
    void DrawMatrix(const Matrix& transform, Shader litShader);

    int MeshCount() const { return m_valid ? m_model.meshCount : 0; }

    bool Valid() const { return m_valid; }
    Model& GetModel() { return m_model; }
    const Model& GetModel() const { return m_model; }
    const BoundingBox& Bounds() const { return m_bounds; }
    const std::string& DiskPath() const { return m_diskPath; }

    MeshRenderMode RenderMode = MeshRenderMode::LIT;
    bool WindAffected = false;

    // True when the shader must do the skinning (a GPU-skinning raylib build).
    // CPU-skinning builds upload posed vertices via animVertices themselves, so
    // applying bones again in the shader would double-transform the mesh.
    virtual bool NeedsShaderSkinning() const { return false; }

    // Optional baked material. Set per draw by the caller (a placed entity owns
    // the name; the registry owns the textures). Cleared again with
    // ClearMaterial so a material can never leak onto the next mesh.
    void SetMaterial(const MeshMaterial& mat) { m_material = mat; }
    void ClearMaterial() { m_material = MeshMaterial{}; }

protected:
    void Unload();

    // Upload the material uniforms to `shader`; ALWAYS called, including for a
    // mesh with no material, because the shader retains the previous draw's
    // normal/emissive binding otherwise.
    void BindMaterial(Shader shader) const;
    // Point each model material at the baked diffuse when one is bound, else at
    // the slot's original map (the Mesh is shared, so this must be reversible).
    void ApplyMaterialMaps();

    MeshMaterial m_material;
    // Pristine diffuse handle per material slot, restored when no material is
    // bound (a Mesh is cached and shared, so a bake must not stick to it).
    std::vector<Texture2D> m_baseDiffuse;

    Model m_model{0};
    std::vector<Texture2D> m_ownedTextures;
    std::vector<Shader> m_baseShaders;   // original shader per material slot
    BoundingBox m_bounds{};
    std::string m_diskPath;
    bool m_valid = false;
};

} // namespace oz
