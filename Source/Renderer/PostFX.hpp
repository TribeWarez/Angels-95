#pragma once
// ---------------------------------------------------------------------------
// Renderer/PostFX.hpp — HDR scene target + bloom + display grade.
//
// The chain is deliberately small and every pass is a normal raylib 2D draw
// (BeginShaderMode + DrawTexturePro). Do NOT hand-drive the GL program here:
// raylib 6.0's batch flush ends with glUseProgram(0)/glBindVertexArray(0), so a
// custom draw has to bind after the last flush and set its sampler as an INT
// uniform. The 2D batch already does all of that correctly, and the particle
// instanced path is the only place in the engine that needs manual GL.
//
//   scene (RGBA16F) -> bright -> blur H -> blur V -> grade -> +bloom -> LDR
//
// Why a float target: bloom needs headroom above 1.0 to have anything to catch,
// and a future tonemap needs the scene unclamped. The world shaders still
// gamma-correct themselves, so the grade works in display space.
//
// Why a separate LDR result: the existing Pixel/Jitter post shaders draw a
// texture to the screen. Resolving into `ResolvedTexture()` keeps that contract
// (and the pause menu's frozen frame) working unchanged.
//
// Header-only for the same reason as Skybox.hpp / SkyPortal.hpp / CullState.hpp:
// adding an object would mean editing the root Makefile, AngelEd/Makefile AND the
// inline g++ list in CI.
// ---------------------------------------------------------------------------

#include "raylib.h"
#include "rlgl.h"
#include "raymath.h"
#include "../Package/PackageAssetLoader.hpp"
#include "../Log.hpp"
#include <algorithm>

namespace oz {

struct PostGrade {
    float exposure        = 1.00f;
    float contrast        = 1.06f;
    float saturation      = 1.10f;
    float vignette        = 0.22f;
    float tint[3]         = {1.0f, 1.0f, 1.0f};
    float bloomThreshold  = 0.78f;
    float bloomKnee       = 0.18f;
    float bloomStrength   = 0.85f;
};

class PostFX {
public:
    static PostFX& Instance() {
        static PostFX fx;
        return fx;
    }

    // -----------------------------------------------------------------------
    // HDR scene target
    // -----------------------------------------------------------------------
    // raylib's LoadRenderTexture is always RGBA8, so the target is built by hand:
    // a half-float colour attachment plus a depth renderbuffer. If the driver
    // cannot complete it (no float-texture support) it falls back to RGBA8, which
    // still renders correctly - the grade just cannot bloom above 1.0.
    static RenderTexture2D CreateTarget(int w, int h) {
        RenderTexture2D t{};
        if (!IsWindowReady()) return t;
        w = std::max(1, w);
        h = std::max(1, h);

        t.id = rlLoadFramebuffer();
        if (t.id == 0) return LoadRenderTexture(w, h);

        t.texture.id = rlLoadTexture(nullptr, w, h,
                                     RL_PIXELFORMAT_UNCOMPRESSED_R16G16B16A16, 1);
        t.texture.width = w;
        t.texture.height = h;
        t.texture.mipmaps = 1;
        t.texture.format = RL_PIXELFORMAT_UNCOMPRESSED_R16G16B16A16;

        t.depth.id = rlLoadTextureDepth(w, h, true);
        t.depth.width = w;
        t.depth.height = h;
        t.depth.mipmaps = 1;

        rlFramebufferAttach(t.id, t.texture.id, RL_ATTACHMENT_COLOR_CHANNEL0,
                            RL_ATTACHMENT_TEXTURE2D, 0);
        rlFramebufferAttach(t.id, t.depth.id, RL_ATTACHMENT_DEPTH,
                            RL_ATTACHMENT_RENDERBUFFER, 0);

        if (!rlFramebufferComplete(t.id)) {
            OZ_WARN("PostFX: RGBA16F target incomplete - falling back to RGBA8 (no HDR bloom headroom)");
            DestroyTarget(t);
            return LoadRenderTexture(w, h);
        }
        return t;
    }

    static void DestroyTarget(RenderTexture2D& t) {
        if (t.id > 0) rlUnloadFramebuffer(t.id);
        if (t.texture.id > 0) rlUnloadTexture(t.texture.id);
        if (t.depth.id > 0) rlUnloadTexture(t.depth.id);
        t = RenderTexture2D{0};
    }

    // -----------------------------------------------------------------------
    // Resolve
    // -----------------------------------------------------------------------
    // `w`/`h` are the SCENE dimensions. Bloom runs at half resolution; the grade
    // resolves to a full-resolution LDR texture.
    //
    // Returns the resolved texture, or {0} when the chain is unavailable - the
    // caller must then draw the scene directly, so a missing shader can never
    // black out the game.
    Texture2D Resolve(const RenderTexture2D& scene, int w, int h) {
        if (!IsWindowReady() || scene.id == 0 || w <= 0 || h <= 0) return Texture2D{0};
        if (!EnsureShaders()) return Texture2D{0};
        EnsureSize(w, h);
        if (m_ldr.id == 0) return Texture2D{0};

        const float bw = (float)m_bright.texture.width;
        const float bh = (float)m_bright.texture.height;

        // 1. Bright pass: scene -> half-res bloom source.
        {
            const Rectangle src = {0, 0, (float)scene.texture.width, -(float)scene.texture.height};
            const Rectangle dst = {0, 0, bw, bh};
            BeginTextureMode(m_bright);
            ClearBackground(BLACK);
            BeginShaderMode(m_brightShader);
            SetShaderValue(m_brightShader, m_locThreshold, &m_grade.bloomThreshold, SHADER_UNIFORM_FLOAT);
            SetShaderValue(m_brightShader, m_locKnee, &m_grade.bloomKnee, SHADER_UNIFORM_FLOAT);
            DrawTexturePro(scene.texture, src, dst, Vector2{0, 0}, 0.0f, WHITE);
            EndShaderMode();
            EndTextureMode();
        }

        // 2. Separable blur, H then V. `uDirection` is in UV units so the radius
        //    is resolution-independent.
        BlurPass(m_blurA, m_bright.texture, 1.0f / bw, 0.0f, bw, bh);
        BlurPass(m_blurB, m_blurA.texture, 0.0f, 1.0f / bh, bw, bh);

        // 3. Grade the scene into the LDR result.
        {
            const Rectangle src = {0, 0, (float)scene.texture.width, -(float)scene.texture.height};
            const Rectangle dst = {0, 0, (float)w, (float)h};
            BeginTextureMode(m_ldr);
            ClearBackground(BLACK);
            BeginShaderMode(m_gradeShader);
            SetShaderValue(m_gradeShader, m_locExposure, &m_grade.exposure, SHADER_UNIFORM_FLOAT);
            SetShaderValue(m_gradeShader, m_locContrast, &m_grade.contrast, SHADER_UNIFORM_FLOAT);
            SetShaderValue(m_gradeShader, m_locSaturation, &m_grade.saturation, SHADER_UNIFORM_FLOAT);
            SetShaderValue(m_gradeShader, m_locVignette, &m_grade.vignette, SHADER_UNIFORM_FLOAT);
            SetShaderValue(m_gradeShader, m_locTint, m_grade.tint, SHADER_UNIFORM_VEC3);
            DrawTexturePro(scene.texture, src, dst, Vector2{0, 0}, 0.0f, WHITE);
            EndShaderMode();
            EndTextureMode();
        }

        // 4. Additive bloom over the graded frame. Additive blend means this pass
        //    needs one sampler and no material binding.
        {
            const Rectangle src = {0, 0, bw, -bh};
            const Rectangle dst = {0, 0, (float)w, (float)h};
            BeginTextureMode(m_ldr);
            BeginBlendMode(BLEND_ADDITIVE);
            BeginShaderMode(m_addShader);
            SetShaderValue(m_addShader, m_locStrength, &m_grade.bloomStrength, SHADER_UNIFORM_FLOAT);
            DrawTexturePro(m_blurB.texture, src, dst, Vector2{0, 0}, 0.0f, WHITE);
            EndShaderMode();
            EndBlendMode();
            EndTextureMode();
        }

        return m_ldr.texture;
    }

    Texture2D ResolvedTexture() const { return m_ldr.texture; }
    bool Ready() const { return m_ldr.id != 0; }

    PostGrade& Grade() { return m_grade; }

    void Shutdown() {
        DestroyTarget(m_bright);
        DestroyTarget(m_blurA);
        DestroyTarget(m_blurB);
        DestroyTarget(m_ldr);
        if (m_brightShader.id > 0) { UnloadShader(m_brightShader); m_brightShader = Shader{0}; }
        if (m_blurShader.id > 0)   { UnloadShader(m_blurShader);   m_blurShader = Shader{0}; }
        if (m_gradeShader.id > 0)  { UnloadShader(m_gradeShader);  m_gradeShader = Shader{0}; }
        if (m_addShader.id > 0)    { UnloadShader(m_addShader);    m_addShader = Shader{0}; }
        m_shadersTried = false;
    }

private:
    PostFX() = default;

    void BlurPass(RenderTexture2D& dst, Texture2D src, float dx, float dy, float bw, float bh) {
        const Rectangle s = {0, 0, (float)src.width, -(float)src.height};
        const Rectangle d = {0, 0, bw, bh};
        BeginTextureMode(dst);
        ClearBackground(BLACK);
        BeginShaderMode(m_blurShader);
        const float dir[2] = {dx, dy};
        SetShaderValue(m_blurShader, m_locDirection, dir, SHADER_UNIFORM_VEC2);
        DrawTexturePro(src, s, d, Vector2{0, 0}, 0.0f, WHITE);
        EndShaderMode();
        EndTextureMode();
    }

    bool EnsureShaders() {
        if (m_brightShader.id > 0) return true;
        if (m_shadersTried) return false;
        m_shadersTried = true;

        m_brightShader = LoadShaderWithFallback(nullptr, "GameData/Shaders/BloomBright.fs");
        m_blurShader   = LoadShaderWithFallback(nullptr, "GameData/Shaders/BloomBlur.fs");
        m_gradeShader  = LoadShaderWithFallback(nullptr, "GameData/Shaders/Grade.fs");
        m_addShader    = LoadShaderWithFallback(nullptr, "GameData/Shaders/BloomAdd.fs");
        if (m_brightShader.id == 0 || m_blurShader.id == 0 ||
            m_gradeShader.id == 0 || m_addShader.id == 0) {
            OZ_WARN("PostFX: bloom/grade shader failed to load - drawing the scene ungraded");
            return false;
        }

        m_locThreshold = GetShaderLocation(m_brightShader, "uThreshold");
        m_locKnee      = GetShaderLocation(m_brightShader, "uKnee");
        m_locDirection = GetShaderLocation(m_blurShader, "uDirection");
        m_locExposure  = GetShaderLocation(m_gradeShader, "uExposure");
        m_locContrast  = GetShaderLocation(m_gradeShader, "uContrast");
        m_locSaturation= GetShaderLocation(m_gradeShader, "uSaturation");
        m_locVignette  = GetShaderLocation(m_gradeShader, "uVignette");
        m_locTint      = GetShaderLocation(m_gradeShader, "uTint");
        m_locStrength  = GetShaderLocation(m_addShader, "uStrength");
        return true;
    }

    void EnsureSize(int w, int h) {
        if (m_ldr.id > 0 && m_ldr.texture.width == w && m_ldr.texture.height == h) return;

        DestroyTarget(m_bright);
        DestroyTarget(m_blurA);
        DestroyTarget(m_blurB);
        DestroyTarget(m_ldr);

        const int bw = std::max(1, w / 2);
        const int bh = std::max(1, h / 2);

        // Bloom chain stays half-float even if the scene target had to fall back.
        auto makeHalf = [](int tw, int th) {
            RenderTexture2D t{};
            t.id = rlLoadFramebuffer();
            t.texture.id = rlLoadTexture(nullptr, tw, th,
                                         RL_PIXELFORMAT_UNCOMPRESSED_R16G16B16A16, 1);
            t.texture.width = tw; t.texture.height = th; t.texture.mipmaps = 1;
            t.texture.format = RL_PIXELFORMAT_UNCOMPRESSED_R16G16B16A16;
            rlFramebufferAttach(t.id, t.texture.id, RL_ATTACHMENT_COLOR_CHANNEL0,
                                RL_ATTACHMENT_TEXTURE2D, 0);
            if (!rlFramebufferComplete(t.id)) {
                DestroyTarget(t);
                return LoadRenderTexture(tw, th);
            }
            return t;
        };
        m_bright = makeHalf(bw, bh);
        m_blurA  = makeHalf(bw, bh);
        m_blurB  = makeHalf(bw, bh);
        m_ldr    = LoadRenderTexture(w, h);
        if (m_ldr.id > 0) SetTextureFilter(m_ldr.texture, TEXTURE_FILTER_BILINEAR);
    }

    RenderTexture2D m_bright{0};
    RenderTexture2D m_blurA{0};
    RenderTexture2D m_blurB{0};
    RenderTexture2D m_ldr{0};

    Shader m_brightShader{0};
    Shader m_blurShader{0};
    Shader m_gradeShader{0};
    Shader m_addShader{0};
    bool m_shadersTried = false;

    int m_locThreshold = -1, m_locKnee = -1, m_locDirection = -1;
    int m_locExposure = -1, m_locContrast = -1, m_locSaturation = -1;
    int m_locVignette = -1, m_locTint = -1, m_locStrength = -1;

    PostGrade m_grade;
};

} // namespace oz
