#pragma once
// ---------------------------------------------------------------------------
// Menu/CharacterPreview.hpp
//
// Renders REAL portraits from the shipped character meshes. There is no portrait
// art in GameData, so a static PNG would either be missing or a lie; this uses
// the same oz::MeshCache assets gameplay uses and renders a studio shot.
//
// Two outputs:
//   Portrait(def)   - cached 512x640 card art, rendered once per character.
//   LivePreview()   - a 3:4 render texture repainted while the Character page is
//                     visible, so the model can turn under the cursor.
//
// The render texture and any authored `portrait=` texture are owned here and
// unloaded in Shutdown(), while the GL context is alive. Meshes stay in
// MeshCache (intentionally process-lifetime, like every other renderer asset).
// ---------------------------------------------------------------------------
#include "raylib.h"
#include "raymath.h"
#include "../Renderer/Mesh/MeshCache.hpp"
#include "../Renderer/LitLightning.hpp"
#include "../Package/PackageAssetLoader.hpp"
#include "CharacterRoster.hpp"
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>

namespace angelsmenu {

class CharacterPreview {
public:
    ~CharacterPreview() { Shutdown(); }

    void SetShader(Shader shader) { m_shader = shader; }

    // A cached portrait card. `w`/`h` only choose the default render size; the
    // first request wins and later draws scale the texture.
    Texture2D Portrait(const CharacterDef& def, int w = 512, int h = 640) {
        if (!def.portrait.empty()) {
            auto it = m_overrideTex.find(def.id);
            if (it != m_overrideTex.end()) return it->second;
            Texture2D tex = LoadTextureWithFallback(def.portrait.c_str());
            if (tex.id != 0) {
                SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
                m_overrideTex[def.id] = tex;
                return tex;
            }
        }
        auto it = m_portraits.find(def.id);
        if (it != m_portraits.end()) return it->second.texture;
        RenderTexture2D rt = MakeTarget(w, h);
        RenderInto(rt, def, 18.0f, false);
        m_portraits[def.id] = rt;
        return rt.texture;
    }

    // Draw a portrait correctly. A rendered RenderTexture is stored flipped in
    // raylib (the source rect must use -height); an authored portrait PNG is
    // not. Keeping the decision here stops every call site from getting it
    // wrong (the Home card shipped upside down for exactly that reason).
    void DrawPortrait(const CharacterDef& def, Rectangle dst) {
        Texture2D tex = Portrait(def);
        if (!tex.id) return;
        bool renderTexture = true;
        if (!def.portrait.empty() && m_overrideTex.find(def.id) != m_overrideTex.end())
            renderTexture = false;
        const float h = renderTexture ? -(float)tex.height : (float)tex.height;
        DrawTexturePro(tex, {0, 0, (float)tex.width, h}, dst, {0, 0}, 0.0f, WHITE);
    }

    // Render (and return) a live 3:4 preview. The caller draws the texture.
    Texture2D LivePreview(const CharacterDef& def, Rectangle area, float yaw,
                          float dt) {
        int w = (int)std::max(240.0f, area.width);
        int h = (int)std::max(320.0f, area.height);
        // Cap the backing texture: a 4K card does not need 4K of MSAA-less GL.
        w = std::min(w, 1024);
        h = std::min(h, 1365);
        if (m_live.id == 0 || m_live.texture.width != w || m_live.texture.height != h) {
            if (m_live.id != 0) UnloadRenderTexture(m_live);
            m_live = MakeTarget(w, h);
        }
        RenderInto(m_live, def, yaw, true);
        (void)dt;
        return m_live.texture;
    }

    void Shutdown() {
        for (auto& [_, rt] : m_portraits)
            if (rt.id != 0) UnloadRenderTexture(rt);
        m_portraits.clear();
        for (auto& [_, tex] : m_overrideTex)
            if (tex.id != 0) UnloadTexture(tex);
        m_overrideTex.clear();
        if (m_live.id != 0) {
            UnloadRenderTexture(m_live);
            m_live = RenderTexture2D{0};
        }
    }

private:
    static RenderTexture2D MakeTarget(int w, int h) {
        RenderTexture2D rt = LoadRenderTexture(std::max(1, w), std::max(1, h));
        if (rt.id != 0) SetTextureFilter(rt.texture, TEXTURE_FILTER_BILINEAR);
        return rt;
    }

    void InitLights() {
        if (m_lightsInit || m_shader.id == 0) return;
        m_lightsInit = true;
        m_fallback = CreateLight(LIGHT_DIRECTIONAL, {0, 10, 10}, {0, 0, 0},
                                 WHITE, m_shader);

        LightNode key;
        key.active = true;
        key.type = LitLightType::POINT;
        key.position = {3.5f, 4.0f, 3.5f};
        key.target = {0, 1.2f, 0};
        key.color = {255, 228, 200, 255};
        key.intensity = 2.2f;
        key.radius = 18.0f;
        key.isStatic = true;

        LightNode fill;
        fill.active = true;
        fill.type = LitLightType::POINT;
        fill.position = {-3.5f, 2.2f, 2.5f};
        fill.target = {0, 1.0f, 0};
        fill.color = {150, 185, 255, 255};
        fill.intensity = 1.0f;
        fill.radius = 16.0f;
        fill.isStatic = true;

        LightNode rim;
        rim.active = true;
        rim.type = LitLightType::POINT;
        rim.position = {0.0f, 3.2f, -4.0f};
        rim.target = {0, 1.4f, 0};
        rim.color = {255, 190, 120, 255};
        rim.intensity = 2.6f;
        rim.radius = 14.0f;
        rim.isStatic = true;

        m_lights = {key, fill, rim};
    }

    void RenderInto(RenderTexture2D& rt, const CharacterDef& def, float yaw,
                    bool live) {
        if (rt.id == 0) return;

        BeginTextureMode(rt);
        ClearBackground(BLANK);

        // Studio backdrop. Two vertical gradients read as a soft vignette
        // without needing another texture.
        DrawRectangleGradientV(0, 0, rt.texture.width, rt.texture.height,
                               (Color){18, 28, 48, 255},
                               (Color){4, 8, 18, 255});
        DrawRectangleGradientV(0, rt.texture.height / 3, rt.texture.width,
                               rt.texture.height / 3,
                               (Color){40, 70, 110, 28},
                               (Color){0, 0, 0, 0});

        std::shared_ptr<oz::Mesh> mesh;
        oz::SkeletalMesh* skel = nullptr;
        std::string meshPath = def.mesh;
        std::string texPath = def.texture;
        bool skeletal = (def.meshType == "skeletal");
        if (meshPath.empty()) {
            // No roster entry: use the same guaranteed default PlayerModel uses,
            // rather than showing a silhouette on a fresh install with no cfg.
            meshPath = "GameData/Pawn/Player/Plague_Arcanist.glb";
            texPath.clear();
            skeletal = false;
        }
        if (skeletal)
            mesh = oz::MeshCache::Instance().GetSkeletal(meshPath, texPath,
                                                         "GameData/", true);
        else
            mesh = oz::MeshCache::Instance().GetStatic(meshPath, texPath,
                                                       "GameData/", true);
        if (mesh) skel = dynamic_cast<oz::SkeletalMesh*>(mesh.get());

        if (!mesh || !mesh->Valid()) {
            DrawSilhouette(rt.texture.width, rt.texture.height);
            EndTextureMode();
            return;
        }

        const BoundingBox b = mesh->Bounds();
        const float localH = std::max(0.001f, b.max.y - b.min.y);
        const float targetH = 2.6f;
        const float s = targetH / localH;
        const Vector3 c = {(b.min.x + b.max.x) * 0.5f,
                           (b.min.y + b.max.y) * 0.5f,
                           (b.min.z + b.max.z) * 0.5f};
        const float spanX = (b.max.x - b.min.x) * s;
        const float spanZ = (b.max.z - b.min.z) * s;
        const float need = std::max(spanX, spanZ);

        Camera3D cam{};
        cam.position = {0.0f, targetH * 0.55f, std::max(5.2f, need * 2.2f)};
        cam.target = {0.0f, targetH * 0.48f, 0.0f};
        cam.up = {0.0f, 1.0f, 0.0f};
        cam.fovy = live ? 32.0f : 30.0f;
        cam.projection = CAMERA_PERSPECTIVE;

        InitLights();
        if (m_lightsInit)
            LitLightning_UpdateFrame(m_lights, m_shader, cam, m_fallback, 0.0f);

        BeginMode3D(cam);
        oz::MeshTransform mt;
        mt.position = {-c.x * s, -b.min.y * s, -c.z * s};
        mt.scale = {s, s, s};
        // FBX2glTF bakes a Z-up -> Y-up rotation; the engine's yaw 0 faces +Z,
        // so 180 faces the camera. Same correction PlayerModel applies.
        mt.yaw = yaw;
        if (skel && skel->ClipCount() > 0) {
            int clip = -1;
            if (!def.animIdle.empty()) clip = skel->FindClip(def.animIdle);
            if (clip < 0) clip = 0;
            skel->ApplyPose(clip, GetTime());
        }
        mesh->Draw(mt, m_shader);
        EndMode3D();

        // Nameplate shadow at the feet, drawn in 2D over the render.
        const float feetY = rt.texture.height * 0.88f;
        DrawEllipse(rt.texture.width / 2, (int)feetY,
                    (int)(rt.texture.width * 0.20f),
                    (int)(rt.texture.height * 0.035f),
                    (Color){0, 0, 0, 90});

        EndTextureMode();
    }

    static void DrawSilhouette(int w, int h) {
        const Color c = {80, 100, 130, 255};
        DrawCircle(w / 2, (int)(h * 0.28f), (int)(w * 0.10f), c);
        DrawRectangleRounded({(float)w * 0.36f, (float)h * 0.38f,
                              (float)w * 0.28f, (float)h * 0.34f},
                             0.25f, 8, c);
        DrawRectangleRounded({(float)w * 0.40f, (float)h * 0.68f,
                              (float)w * 0.08f, (float)h * 0.20f},
                             0.20f, 6, c);
        DrawRectangleRounded({(float)w * 0.52f, (float)h * 0.68f,
                              (float)w * 0.08f, (float)h * 0.20f},
                             0.20f, 6, c);
    }

    Shader m_shader{0};
    Light m_fallback{};
    std::vector<LightNode> m_lights;
    bool m_lightsInit = false;
    std::unordered_map<std::string, RenderTexture2D> m_portraits;
    std::unordered_map<std::string, Texture2D> m_overrideTex;
    RenderTexture2D m_live{0};
};

} // namespace angelsmenu
