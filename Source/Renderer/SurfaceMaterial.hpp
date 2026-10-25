#pragma once
// ---------------------------------------------------------------------------
// Renderer/SurfaceMaterial — the CPU half of the UT99-style per-face surface
// system (see World/SurfaceFlags.hpp for the data model).
//
// Owns GameData/Shaders/Surface.{vs,fs} and turns one SurfaceProps into shader
// uniforms + GL state for a single draw. A brush with per-face differences is
// drawn as up to six DrawMesh calls (one per face) because the surface
// properties are uniforms, i.e. per draw call.
//
// Design note — why a second shader program rather than extra uniforms on
// LitFog: Lighting.vs/LitFog.fs drives every mesh, particle, pickup, projectile
// and the view-model. Adding branches to it would change the whole game's
// output and cost a uniform check per fragment everywhere. Only brushes that
// actually carry a surface flag reach this path; everything else keeps the
// original single DrawModel fast path.
// ---------------------------------------------------------------------------

#include "raylib.h"
#include "CullState.hpp"
#include "../World/SurfaceFlags.hpp"
#include <memory>
#include <optional>
#include <vector>

// Forward-declared: SurfaceMaterial only needs to know it is a light source.
struct LightNode;

namespace oz {

class SurfaceMaterial {
public:
    static SurfaceMaterial& Instance();

    // Load Surface.vs/Surface.fs through the package-aware loader. Safe to call
    // repeatedly; a second call while loaded is a no-op.
    void Init(const char* shaderDir = "GameData/Shaders/");
    void Shutdown();
    bool Ready() const { return m_shader.id > 0; }
    Shader Get() const { return m_shader; }

    // Re-submit the world lights, view position and ambient to the surface
    // program. Must run once per frame before any surface draw, otherwise the
    // lights[] array on this program holds whatever the last world set.
    //
    // This deliberately reuses LitLightning_Update rather than duplicating the
    // 32-light loop: two copies of that loop would drift, and a surface brush
    // that was lit by a different set of lights than the room around it is
    // exactly the kind of bug that is very hard to see.
    //
    // `ambient` is the caller's Vec4 (RGB premultiplied by intensity). It is a
    // parameter rather than something read back from OzoneLoader because raylib
    // has no GetShaderValue, so the owner of the LitFog uniform has to declare it
    // — and this program carries its OWN copy of every uniform it uses.
    //
    // Non-const because LitLightning_Update sorts the light vector in place.
    void UpdateFrame(std::vector<LightNode>& lights, Camera3D camera, float dt,
                     const float ambient[4]);

    // Mirror the world's fog onto this program. Surface.fs declares and uses
    // fogStart/fogEnd/fogDensity/fogColor/fogIntensity, so without this a
    // surface-flagged brush always fogged at the GLSL defaults regardless of the
    // level's own fog. Applied lazily inside UpdateFrame (one upload per change,
    // not five per frame).
    void SetFog(const float color[3], float start, float end,
                float density, float intensity);

    // Push one face's properties. Leaves GL blend/depth state alone; the caller
    // brackets the draw with BeginSurfaceState/EndSurfaceState.
    void ApplyUniforms(const surface::SurfaceProps& p);

    // Baked material maps for the face about to be drawn. Applied inside the
    // next ApplyUniforms call; pass zeroed maps to clear. Surface.fs declares the
    // same normal/emissive uniforms as LitFog.fs.
    void SetMaterialMaps(Texture2D diffuse, Texture2D normal, Texture2D emissive,
                         float normalStrength, float emissiveScale);

    // Gain for the full-bright branch (UNLIT / FAKE_LIT / FAKEBACKDROP).
    //
    // This is a PASS-wide setting, not a per-face one, because it exists only to
    // reproduce a per-pass decision: the backdrop pass runs at 0.55 so painted
    // backdrops keep the tonality the old DrawZoneGeometry ambient/10 hack gave
    // them, while the world pass runs at 1.0.
    //
    // Uploaded IMMEDIATELY by SetFullBright, not batched into UpdateFrame: the
    // caller sets this from inside the draw passes, which run after the frame's
    // single UpdateFrame, so a lazy flush here would never land.
    void SetFullBright(float gain);
    // Last value passed to SetFullBright. Exposed for the headless tests and for
    // asserting a pass set the gain it meant to.
    float GetFullBright() const { return m_fullBright; }

    // Halo pass switch, set by DrawGlowGeometry around its additive re-draw.
    //
    // Without it the halo adds the WHOLE FACE a second time, so any textured
    // glow surface saturates: base 0.6 + base 0.6 clips to white, which is why
    // a glowing window strip or a shard fissure read as a white card no matter
    // how small `surfGlowScale` was (the scale only ever moved the emissive
    // term, while the duplicated base did the damage). With the switch on,
    // Surface.fs outputs only `uGlow * uGlowScale` - what a halo is supposed to
    // be - and keeps the fog/gamma/alpha path.
    //
    // Uploaded immediately, for the same reason SetFullBright is: the caller
    // sets it from inside the draw passes, after the frame's UpdateFrame.
    void SetHaloPass(int on);
    int GetHaloPass() const { return m_haloPass; }

    // Bracket a translucent / masked / modulated / two-sided draw. No-op for
    // flags that do not change GL state.
    void BeginSurfaceState(const surface::SurfaceProps& p);
    void EndSurfaceState();

private:
    SurfaceMaterial() = default;
    void CacheLocations();

    Shader m_shader{0};
    bool   m_loaded = false;

    int m_lightsLoc = -1;
    int m_ambientLoc = -1;
    int m_viewPosLoc = -1;
    int m_timeLoc = -1;
    int m_flagsLoc = -1;
    int m_panLoc = -1;
    int m_alphaLoc = -1;
    int m_cutoffLoc = -1;
    int m_glowLoc = -1;
    int m_glowScaleLoc = -1;
    int m_fullBrightLoc = -1;
    int m_haloPassLoc = -1;

    // Fog uniforms — Surface.fs declares and uses all five, but none were
    // cached, so surface-flagged brushes fogged at the shader defaults (10/100,
    // density 1) no matter what the world was set to.
    int m_fogStartLoc = -1;
    int m_fogEndLoc = -1;
    int m_fogDensityLoc = -1;
    int m_fogColorLoc = -1;
    int m_fogIntensityLoc = -1;

    // Material maps (baked .ozmat). Cached here because this program carries its
    // own copy of every uniform it reads.
    int m_hasNormalLoc = -1;
    int m_normalStrengthLoc = -1;
    int m_normalMapLoc = -1;
    int m_hasEmissiveLoc = -1;
    int m_emissiveScaleLoc = -1;
    int m_emissiveMapLoc = -1;
    Texture2D m_faceDiffuse{0};
    Texture2D m_faceNormal{0};
    Texture2D m_faceEmissive{0};
    float m_faceNormalStrength = 1.0f;
    float m_faceEmissiveScale = 1.0f;

    float m_fogColor[3] = {0.7f, 0.7f, 0.8f};   // matches Surface.fs's defaults
    float m_fogStart = 10.0f;
    float m_fogEnd = 100.0f;
    float m_fogDensity = 1.0f;
    float m_fogIntensity = 1.0f;
    bool  m_fogDirty = true;   // first UpdateFrame must push the defaults

    // Full-bright gain. Default 1.0 so a shader that somehow misses the upload
    // still renders a full-value unlit surface rather than a black one.
    float m_fullBright = 1.0f;
    int   m_haloPass = 0;

    // Blend/depth/cull state actually changed by the currently open bracket, so
    // EndSurfaceState restores exactly what it found instead of guessing.
    //
    // Culling is NOT in that list: it uses the shared ScopedCullOff guard from
    // Renderer/CullState.hpp. Two flags for one toggle is how they drift — the
    // world pass, the editor and imported meshes all flip this state, and a
    // local bool here only knew about this one call site.
    bool m_blendOpen = false;
    bool m_depthMaskOff = false;
    std::optional<ScopedCullOff> m_cullScope;
};

} // namespace oz
