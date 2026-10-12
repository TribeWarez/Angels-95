#ifndef ENGINE_BILLBOARD_HPP
#define ENGINE_BILLBOARD_HPP

#include "raylib.h"
#include "raymath.h"
#include "OzAssetMapper.hpp"
#include "../Package/PackageAssetLoader.hpp"
#include "../Script/LightningEntityRegistry.hpp"
#include <string>
#include <unordered_map>

// EngineBillboard - Helper class for drawing billboard sprites with optional shader support
// Used for displaying entity icons (Light, Sound, Music, NPCs, pickups, etc.)
// When a shader is provided, renders via a mesh plane model so the shader applies.

class EngineBillboard {
private:
    inline static Model s_BillboardModel{0};
    // Memoised def->icon textures. A class member, not a function-local static,
    // so InvalidateIconCache() can actually reach and free them - a local static
    // outlived every reload and handed out Texture2D handles for textures
    // AssetMapper had already unloaded.
    inline static std::unordered_map<std::string, Texture2D> s_defIconCache;

    // A 1x1 quad in the XY plane (normal +Z), centred on the origin.
    //
    // This deliberately does NOT use GenMeshPlane(): that lays the quad out on
    // the XZ plane with its normal along +Y, so a Y-axis yaw left it lying flat
    // on the ground. Every sprite drawn through DrawSprite with a lit shader
    // (which is what AngelEd always uses, and what the game uses in LIT mode)
    // was therefore edge-on to the camera and effectively invisible - the whole
    // gizmo/billboard layer silently rendered nothing. A vertical quad yawed to
    // face the camera is the only correct shape here.
    //
    // The NORMALS ARE NOT OPTIONAL. Lighting.vs declares `in vec3 vertexNormal`
    // and feeds it through matNormal; a mesh built without a normals array has
    // that attribute disabled, so the vertex shader reads (0,0,0),
    // `normalize(vec3(0))` is NaN, and LitFog.fs's NdotL degenerates. The sprite
    // then renders at just the `ambient / 10` term - roughly 2% brightness, i.e.
    // effectively invisible on a dark viewport. Every quad normal is +Z, which
    // after the camera-facing yaw points at the viewer.
    static Mesh MakeBillboardQuad() {
        // Separate arrays. The previous version packed position and UV into one
        // interleaved array and then handed that array to mesh.vertices as if it
        // were positions-only, with texcoords pointing at `v + 12`. Both reads
        // were therefore garbage: the four "vertices" were not the quad corners
        // at all, so every sprite/gizmo billboard drew as a thin degenerate
        // sliver (or nothing). One array per attribute is the only correct form.
        static float v[] = {
            // x     y     z
            -0.5f, -0.5f, 0.0f,
             0.5f, -0.5f, 0.0f,
             0.5f,  0.5f, 0.0f,
            -0.5f,  0.5f, 0.0f,
        };
        static float uv[] = {
            // u    v
            0.0f, 1.0f,
            1.0f, 1.0f,
            1.0f, 0.0f,
            0.0f, 0.0f,
        };
        static float n[] = {
             0.0f, 0.0f, 1.0f,
             0.0f, 0.0f, 1.0f,
             0.0f, 0.0f, 1.0f,
             0.0f, 0.0f, 1.0f,
        };
        static unsigned short idx[] = { 0, 1, 2, 0, 2, 3 };
        Mesh mesh = {0};
        mesh.vertexCount = 4;
        // triangleCount is the number of TRIANGLES (2 here), not the index count
        // (6). Raylib's DrawMesh draws `triangleCount * 3` indices, so the old
        // value of 6 made it read 18 indices from this 6-element array.
        mesh.triangleCount = 2;
        mesh.vertices = v;
        mesh.texcoords = uv;
        mesh.normals = n;
        mesh.indices = idx;
        return mesh;
    }

public:
    static void Init() {
        AssetMapper::Instance().PreloadCategory("engine");
        AssetMapper::Instance().PreloadCategory("items");
        s_BillboardModel = LoadModelFromMesh(MakeBillboardQuad());
        // raylib 6.0's LoadModelFromMesh does NOT upload the mesh to the GPU
        // (earlier versions did), so the model carried a CPU-side quad with
        // vaoId == 0 and every DrawModelEx of it rendered nothing. Upload it
        // explicitly; the false keeps the static vertex arrays alive, which is
        // fine because MakeBillboardQuad's arrays are static storage.
        if (s_BillboardModel.meshCount > 0 && s_BillboardModel.meshes[0].vaoId == 0)
            UploadMesh(&s_BillboardModel.meshes[0], false);
    }

    static void Shutdown() {
        InvalidateIconCache();
        AssetMapper::Instance().UnloadCategory("engine");
        AssetMapper::Instance().UnloadCategory("items");
        if (s_BillboardModel.meshCount > 0)
            UnloadModel(s_BillboardModel);
        s_BillboardModel = Model{0};
    }

    // Drop the memoised def->icon textures. MUST be called whenever the entity
    // registry is reloaded (a world load / undo-restore re-scans .ozls): the
    // cached Texture2D handles belong to textures AssetMapper may have already
    // unloaded, and handing a stale id to DrawBillboard draws a freed texture.
    static void InvalidateIconCache() {
        for (auto& kv : s_defIconCache) {
            // Def icons are loaded by us (not owned by an AssetMapEntry), so
            // they are ours to free.
            if (kv.second.id > 0) UnloadTexture(kv.second);
        }
        s_defIconCache.clear();
    }

    // Draw a shader-lit billboard using the cached model.
    // For use when you already have a Texture2D handle.
    static void DrawSprite(Camera3D camera, Texture2D tex, Vector3 position,
                           float size, Color tint, Shader shader) {
        if (s_BillboardModel.meshCount == 0) return;
        s_BillboardModel.materials[0].shader = shader;
        s_BillboardModel.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = tex;
        Vector3 cp = camera.position;
        float yaw = atan2f(cp.x - position.x, cp.z - position.z) * RAD2DEG;
        DrawModelEx(s_BillboardModel, position, {0, 1, 0}, yaw, {size, size, size}, tint);
    }

    // Draw a billboard at position, optionally with a lit shader.
    // If litShader.id != 0 uses the cached mesh model so lighting/fog applies.
    static void Draw(Camera3D camera, const char* entityName, Vector3 position,
                     float size = 1.0f, Shader litShader = {0}) {
        Texture2D tex = AssetMapper::Instance().GetTexture(entityName);
        if (tex.id == 0) return;

        if (litShader.id > 0) {
            DrawSprite(camera, tex, position, size, WHITE, litShader);
        } else {
            DrawBillboard(camera, tex, position, size, WHITE);
        }
    }

    // Draw a billboard with a tint color, optionally with a lit shader.
    static void DrawTinted(Camera3D camera, const char* entityName, Vector3 position,
                           float size, Color tint, Shader litShader = {0}) {
        Texture2D tex = AssetMapper::Instance().GetTexture(entityName);
        if (tex.id == 0) return;

        if (litShader.id > 0) {
            DrawSprite(camera, tex, position, size, tint, litShader);
        } else {
            DrawBillboard(camera, tex, position, size, tint);
        }
    }

    // Draw a pickup billboard (floating, bobbing), optionally with a lit shader.
// Resolve a pickup's icon. AssetMapper is name-based, so it only finds items
// that happen to have a file named after them (HealthVial.png, Coin.png, ...).
// Items whose .ozls aliases a different texture - Medkit reuses
// HealthVial.png, PistonPart reuses key.png - fall through to the magenta
// missing-icon grid. Consult the entity def's own `icon` first and cache it.
inline static Texture2D ResolvePickupIcon(const char* itemName) {
    auto cached = s_defIconCache.find(itemName ? itemName : "");
    if (cached != s_defIconCache.end())
        return cached->second;

    Texture2D tex{0};
    const EntityDef* def = itemName ? LightningEntityRegistry::Instance().Find(itemName) : nullptr;
    if (def && !def->icon.empty())
        tex = LoadTextureWithFallback(def->icon.c_str());
    s_defIconCache[itemName ? itemName : ""] = tex;
    return tex;
}

static void DrawPickup(Camera3D camera, const char* itemName, Vector3 position,
  float size = 1.2f, Shader litShader = {0}) {
  Texture2D tex = ResolvePickupIcon(itemName);
  if (tex.id == 0)
    tex = AssetMapper::Instance().GetTexture(itemName);
  if (tex.id == 0) return;

        float bob = sinf((float)GetTime() * 3.0f) * 0.15f;
        Vector3 bobPos = {position.x, position.y + 0.9f + bob, position.z};

        if (litShader.id > 0) {
            DrawSprite(camera, tex, bobPos, size, WHITE, litShader);
        } else {
            DrawBillboardPro(camera, tex,
                (Rectangle){0, 0, (float)tex.width, (float)tex.height},
                bobPos, {0, 1, 0}, {size, size}, {0.5f, 0.5f}, 0, WHITE);
        }
    }
};

#endif
