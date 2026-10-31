#include "SurfaceMaterial.hpp"
#include "LitLightning.hpp"
#include "ShadingTuning.hpp"
#include "rlgl.h"
#include "../Package/PackageAssetLoader.hpp"
#include "../Log.hpp"
#include <cstring>

namespace oz {

SurfaceMaterial& SurfaceMaterial::Instance() {
    static SurfaceMaterial inst;
    return inst;
}

void SurfaceMaterial::Init(const char* shaderDir) {
    if (m_loaded) return;
    std::string dir = shaderDir ? shaderDir : "GameData/Shaders/";
    if (dir.back() != '/' && dir.back() != '\\') dir += '/';
    m_shader = LoadShaderWithFallback((dir + "Surface.vs").c_str(),
                                      (dir + "Surface.fs").c_str());
    if (m_shader.id == 0) {
        // Not fatal: brushes keep the original DrawModel path, so a world still
        // loads and renders, just without the surface effects. Say so loudly -
        // a silently-missing Surface.fs would look like "the flags do nothing".
        OZ_WARN("SurfaceMaterial: Surface.vs/fs failed to load from '%s' - "
                "surface flags will be ignored", dir.c_str());
        return;
    }
    CacheLocations();
    m_loaded = true;
    OZ_INFO("SurfaceMaterial: shader loaded (id=%d)", m_shader.id);
}

void SurfaceMaterial::Shutdown() {
    if (m_loaded && m_shader.id > 0) UnloadShader(m_shader);
    m_shader = Shader{0};
    m_loaded = false;
    m_blendOpen = m_depthMaskOff = false;
    m_cullScope.reset();
}

void SurfaceMaterial::CacheLocations() {
    m_lightsLoc    = GetShaderLocation(m_shader, "lights[0].position");
    m_ambientLoc   = GetShaderLocation(m_shader, "ambient");
    m_viewPosLoc   = GetShaderLocation(m_shader, "viewPos");
    m_timeLoc      = GetShaderLocation(m_shader, "uTime");
    m_flagsLoc     = GetShaderLocation(m_shader, "uSurfaceFlags");
    m_panLoc       = GetShaderLocation(m_shader, "uPan");
    m_alphaLoc     = GetShaderLocation(m_shader, "uAlpha");
    m_cutoffLoc    = GetShaderLocation(m_shader, "uAlphaCutoff");
    m_glowLoc      = GetShaderLocation(m_shader, "uGlow");
    m_glowScaleLoc = GetShaderLocation(m_shader, "uGlowScale");
    m_fullBrightLoc = GetShaderLocation(m_shader, "uFullBright");
    m_haloPassLoc = GetShaderLocation(m_shader, "uHaloPass");
    // Surface.fs declares and uses all five fog uniforms (see the SF_NO_FOG block
    // in its fragment stage) but none were cached here, so surface-flagged
    // brushes always fogged at the GLSL defaults — 10/100 with density 1 —
    // regardless of what the world's own fog was set to.
    m_fogStartLoc     = GetShaderLocation(m_shader, "fogStart");
    m_fogEndLoc       = GetShaderLocation(m_shader, "fogEnd");
    m_fogDensityLoc   = GetShaderLocation(m_shader, "fogDensity");
    m_fogColorLoc     = GetShaderLocation(m_shader, "fogColor");
    m_fogIntensityLoc = GetShaderLocation(m_shader, "fogIntensity");
    // Material maps (baked .ozmat), same four uniforms Surface.fs shares with
    // LitFog.fs. Cached like every other uniform on this program.
    m_hasNormalLoc      = GetShaderLocation(m_shader, "uHasNormalMap");
    m_normalStrengthLoc = GetShaderLocation(m_shader, "uNormalStrength");
    m_normalMapLoc      = GetShaderLocation(m_shader, "normalMap");
    m_hasEmissiveLoc    = GetShaderLocation(m_shader, "uHasEmissive");
    m_emissiveScaleLoc  = GetShaderLocation(m_shader, "uEmissiveScale");
    m_emissiveMapLoc    = GetShaderLocation(m_shader, "emissiveMap");
}

void SurfaceMaterial::SetMaterialMaps(Texture2D diffuse, Texture2D normal, Texture2D emissive,
                                      float normalStrength, float emissiveScale) {
    m_faceDiffuse = diffuse;
    m_faceNormal = normal;
    m_faceEmissive = emissive;
    m_faceNormalStrength = normalStrength;
    m_faceEmissiveScale = emissiveScale;
}

void SurfaceMaterial::SetFog(const float color[3], float start, float end,
                             float density, float intensity) {
    m_fogColor[0] = color[0];
    m_fogColor[1] = color[1];
    m_fogColor[2] = color[2];
    m_fogStart   = start;
    m_fogEnd     = end;
    m_fogDensity = density;
    m_fogIntensity = intensity;
    m_fogDirty   = true;
}

// Uploads IMMEDIATELY, and that is the whole point.
//
// This used to set a dirty flag and flush inside UpdateFrame. That is wrong by
// construction: UpdateFrame runs once per frame from UpdateLightSources(), which
// is called BEFORE DrawWorld(), whereas SetFullBright is called from INSIDE the
// draw passes (DrawZoneGeometry). So the flush had already happened by the time
// the value changed, and uFullBright stayed at 1.0 forever - every painted
// backdrop rendered at full value instead of the 0.55 it was tuned for, and the
// gain nobody could see was not the gain in effect.
//
// A per-pass scalar cannot be batched the way per-frame uniforms can: it changes
// at most twice per frame (down into the sky pass, back out), so paying two
// uploads is free and correctness is not negotiable. Do NOT reintroduce a lazy
// flush for this value.
void SurfaceMaterial::SetFullBright(float gain) {
    if (m_fullBright == gain) return;
    m_fullBright = gain;
    if (m_shader.id > 0 && m_fullBrightLoc >= 0)
        SetShaderValue(m_shader, m_fullBrightLoc, &m_fullBright, SHADER_UNIFORM_FLOAT);
}

// SetHaloPass - the same immediate-upload rule as SetFullBright, and for the
// same structural reason: DrawGlowGeometry flips it from inside the draw pass,
// long after the frame's single UpdateFrame flush.
//
// The halo pass exists to bloom a glow, not to re-draw the face. Adding the
// whole face a second time meant `lit + lit` - a 0.6-textured surface reached
// 1.2 and clipped - so every glowing wall, window strip and shard fissure was
// a white card regardless of `surfGlowScale`. The shader now replaces `lit`
// with the emissive term while this is on.
void SurfaceMaterial::SetHaloPass(int on) {
    on = on ? 1 : 0;
    if (m_haloPass == on) return;
    m_haloPass = on;
    if (m_shader.id > 0 && m_haloPassLoc >= 0)
        SetShaderValue(m_shader, m_haloPassLoc, &m_haloPass, SHADER_UNIFORM_INT);
}

void SurfaceMaterial::UpdateFrame(std::vector<LightNode>& lights,
                                  Camera3D camera, float dt,
                                  const float ambient[4]) {
    if (m_shader.id == 0) return;

    // Same 32-light submission the world uses, pointed at this program. Reusing
    // LitLightning_Update is deliberate: a second copy of that loop would drift
    // and a surface-lit brush would disagree with the room around it.
    LitLightning_Update(lights, m_shader, camera, dt);

    // Player shading knobs, the same three the world program gets. Without this
    // a decorated brush would use the GLSL defaults (1/1/0) while the room around
    // it used the player's settings - the exact per-program drift the parity test
    // exists to prevent.
    oz::ApplyShadingTuning(m_shader);

    const float viewPos[3] = { camera.position.x, camera.position.y, camera.position.z };
    if (m_viewPosLoc >= 0)
        SetShaderValue(m_shader, m_viewPosLoc, viewPos, SHADER_UNIFORM_VEC3);

    // The ambient the caller owns, i.e. the same value the world pushes to LitFog.
    // This used to be a hardcoded {0.1,0.1,0.1,1} with a comment claiming the
    // value lived on OzoneLoader — there was no accessor and it never read one, so
    // a surface-flagged brush was lit by a flat 0.1 no matter how bright or dark
    // the room actually was. Unlit surfaces ignore the uniform entirely, which is
    // the common case for surface-flagged brushes.
    if (m_ambientLoc >= 0 && ambient)
        SetShaderValue(m_shader, m_ambientLoc, ambient, SHADER_UNIFORM_VEC4);

    // Push fog only when it changed: UpdateFrame runs every frame, and five
    // redundant uniform uploads per frame per material is pure overhead for a
    // value that only changes when the user edits it.
    if (m_fogDirty) {
        if (m_fogColorLoc     >= 0) SetShaderValue(m_shader, m_fogColorLoc,     m_fogColor,     SHADER_UNIFORM_VEC3);
        if (m_fogStartLoc     >= 0) SetShaderValue(m_shader, m_fogStartLoc,     &m_fogStart,     SHADER_UNIFORM_FLOAT);
        if (m_fogEndLoc       >= 0) SetShaderValue(m_shader, m_fogEndLoc,       &m_fogEnd,       SHADER_UNIFORM_FLOAT);
        if (m_fogDensityLoc   >= 0) SetShaderValue(m_shader, m_fogDensityLoc,   &m_fogDensity,   SHADER_UNIFORM_FLOAT);
        if (m_fogIntensityLoc >= 0) SetShaderValue(m_shader, m_fogIntensityLoc, &m_fogIntensity, SHADER_UNIFORM_FLOAT);
        m_fogDirty = false;
    }

    if (m_timeLoc >= 0) {
        const float t = (float)GetTime();
        SetShaderValue(m_shader, m_timeLoc, &t, SHADER_UNIFORM_FLOAT);
    }
}

void SurfaceMaterial::ApplyUniforms(const surface::SurfaceProps& p) {
    if (m_shader.id == 0) return;

    const int flags = (int)p.flags;
    if (m_flagsLoc >= 0)
        SetShaderValue(m_shader, m_flagsLoc, &flags, SHADER_UNIFORM_INT);

    const float pan[2] = { p.panU, p.panV };
    if (m_panLoc >= 0)
        SetShaderValue(m_shader, m_panLoc, pan, SHADER_UNIFORM_VEC2);

    if (m_alphaLoc >= 0)
        SetShaderValue(m_shader, m_alphaLoc, &p.alpha, SHADER_UNIFORM_FLOAT);
    if (m_cutoffLoc >= 0)
        SetShaderValue(m_shader, m_cutoffLoc, &p.alphaCutoff, SHADER_UNIFORM_FLOAT);
    if (m_glowLoc >= 0) {
        const float g[3] = { p.glowR, p.glowG, p.glowB };
        SetShaderValue(m_shader, m_glowLoc, g, SHADER_UNIFORM_VEC3);
    }
    if (m_glowScaleLoc >= 0)
        SetShaderValue(m_shader, m_glowScaleLoc, &p.glowScale, SHADER_UNIFORM_FLOAT);

    // Material maps. Uploaded on EVERY face (including zeros) so a material can
    // never leak onto the next face, exactly like oz::Mesh::BindMaterial.
    const int hasNormal = (m_faceNormal.id > 0) ? 1 : 0;
    const int hasEmissive = (m_faceEmissive.id > 0) ? 1 : 0;
    if (m_hasNormalLoc >= 0)
        SetShaderValue(m_shader, m_hasNormalLoc, &hasNormal, SHADER_UNIFORM_INT);
    if (m_normalStrengthLoc >= 0)
        SetShaderValue(m_shader, m_normalStrengthLoc, &m_faceNormalStrength, SHADER_UNIFORM_FLOAT);
    if (m_hasEmissiveLoc >= 0)
        SetShaderValue(m_shader, m_hasEmissiveLoc, &hasEmissive, SHADER_UNIFORM_INT);
    if (m_emissiveScaleLoc >= 0)
        SetShaderValue(m_shader, m_emissiveScaleLoc, &m_faceEmissiveScale, SHADER_UNIFORM_FLOAT);
    if (hasNormal && m_normalMapLoc >= 0)
        SetShaderValueTexture(m_shader, m_normalMapLoc, m_faceNormal);
    if (hasEmissive && m_emissiveMapLoc >= 0)
        SetShaderValueTexture(m_shader, m_emissiveMapLoc, m_faceEmissive);
}

void SurfaceMaterial::BeginSurfaceState(const surface::SurfaceProps& p) {
    using namespace oz::surface;
    if (m_blendOpen || m_cullScope || m_depthMaskOff) return;  // never nest

    if (p.Has(SURF_MODULATED)) {
        // HIDDEN in the dialog: the shader's only "modulate" is
        // `lit *= colDiffuse.rgb`, and a brush's diffuse-map colour is always
        // WHITE, so it is a x1.0 multiply and looks identical to an opaque face.
        // Kept for hand-authored files so they do not silently change blend
        // state; depth write stays on, as before. The old comment claimed this
        // multiplied against the framebuffer, which is not what BLEND_ALPHA does.
        BeginBlendMode(BLEND_ALPHA);
        m_blendOpen = true;
    } else if (p.Has(SURF_TRANSLUCENT) || p.Has(SURF_ALPHABLEND)) {
        BeginBlendMode(BLEND_ALPHA);
        m_blendOpen = true;
        // A blended surface must not occlude what is behind it, or the second
        // translucent brush in the room disappears behind the first.
        rlDisableDepthMask();
        m_depthMaskOff = true;
    }
    if (p.Has(SURF_TWO_SIDED)) {
        // Scoped, not a raw call plus a private bool: the tracked flag in
        // Renderer/CullState.hpp is the single authority, so a two-sided face
        // restores whatever the caller had set rather than assuming "on".
        m_cullScope.emplace();
    }
}

void SurfaceMaterial::EndSurfaceState() {
    if (m_cullScope) m_cullScope.reset();   // restores the pre-draw state
    if (m_depthMaskOff) { rlEnableDepthMask();         m_depthMaskOff = false; }
    if (m_blendOpen)    { EndBlendMode();              m_blendOpen = false; }
}

} // namespace oz
