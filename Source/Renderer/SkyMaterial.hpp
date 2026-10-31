#pragma once
#include "raylib.h"
#include "../Package/PackageAssetLoader.hpp"   // LoadShaderWithFallback
#include <string>

// ---------------------------------------------------------------------------
// oz::SkyMaterial - horizon fog for the skybox cube.
//
// WHY THIS EXISTS
// ---------------
// World geometry fogs; the skybox did not. The sky cube is drawn at kSkyboxDist
// (700 units), where a level's own fog curve is typically ~97% saturated, so an
// unfogged sky was reading as clear air immediately behind fully hazed geometry.
//
// Be clear about how little this is worth on its own, because it is easy to
// oversell: in all six shipped worlds the painted SURF_FAKEBACKDROP ring stands
// ~13 degrees ABOVE the horizon, so it occludes the entire band where horizon fog
// would be visible. Rendering a shipped world with and without this shader
// changes zero pixels at the sky/backdrop seam. It matters where the true horizon
// IS visible (a ledge, an unclipped gap, a world with no backdrop ring), and it
// closes a real correctness gap; it does not fix the seam people actually mean.
//
// HEADER-ONLY ON PURPOSE
// ----------------------
// Same reasoning as Renderer/CullState.hpp: a .cpp here would mean editing the
// root Makefile, AngelEd/Makefile AND the inline g++ list in ci.yml, because CI
// builds AngelEd by hand rather than through AngelEd/Makefile. Nothing here is
// big enough to justify a third build-list edit.
//
// ONE OWNER, PUBLISHED LIKE THE OTHERS
// ------------------------------------
// OzoneLoader owns the world's fog because raylib has no GetShaderValue, and
// UpdateLightSources() is the single mirror point into SurfaceMaterial. The sky is
// a third consumer of the SAME state, so it takes the same treatment: Core.hpp
// reads the values back from OzoneLoader and pushes them here. Reading fog from a
// local would be a fourth copy to drift.
//
// THE THIRD fogFactorAt
// ---------------------
// Sky.fs now carries the third hand-maintained copy of fogFactorAt(), alongside
// LitFog.fs and Surface.fs. That is a known hazard - the two existing copies have
// already diverged in four places - so tests/Surface.test.cpp compares all three
// bodies for byte equality and fails the build when they differ. Extend that test,
// do not work around it.
// ---------------------------------------------------------------------------
namespace oz {

class SkyMaterial {
public:
    static SkyMaterial& Instance() {
        static SkyMaterial inst;
        return inst;
    }

    // Safe to call repeatedly; a second call while loaded is a no-op.
    void Init(const char* shaderDir = "GameData/Shaders/") {
        if (m_loaded) return;
        std::string dir = shaderDir ? shaderDir : "GameData/Shaders/";
        if (dir.back() != '/' && dir.back() != '\\') dir += '/';
        // LoadShaderWithFallback, NOT raylib's LoadShader: this was the ONLY
        // shader loader in the tree that could not read from a package, and
        // release builds ship System/ with the shaders in shaders.ozpak (no
        // GameData/ tree at all). So in every packaged install this failed and
        // the skybox lost the horizon fog the shader exists for - invisible in a
        // dev checkout, which is why it survived.
        m_shader = LoadShaderWithFallback((dir + "Sky.vs").c_str(),
                                          (dir + "Sky.fs").c_str());
        if (m_shader.id == 0) {
            // Not fatal. The skybox then draws through raylib's default material
            // shader exactly as it always has - i.e. unfogged. Say so, because the
            // symptom otherwise reads as "fog is broken" rather than "the sky
            // shader is missing".
            TraceLog(LOG_WARNING, "SkyMaterial: Sky.vs/fs failed to load from '%s' - "
                                  "the skybox will not fog", dir.c_str());
            return;
        }
        CacheLocations();
        m_loaded = true;
    }

    void Shutdown() {
        if (m_loaded && m_shader.id > 0) UnloadShader(m_shader);
        m_shader = Shader{0};
        m_loaded = false;
    }

    bool Ready() const { return m_shader.id > 0; }
    Shader Get() const { return m_shader; }

    // Publish the world's fog. ALL FIVE values, not just the colour: the fragment
    // stage evaluates the same fogFactorAt() the world shaders do, so dropping
    // fogStart/fogEnd/fogDensity would leave the sky on the GLSL defaults
    // (10/100, rate 1.0) and disagree with the room it sits in - which is exactly
    // the bug SurfaceMaterial had before it grew its own fog uniforms.
    void SetFog(const float color[3], float start, float end, float density, float intensity) {
        m_fogColor[0] = color[0];
        m_fogColor[1] = color[1];
        m_fogColor[2] = color[2];
        m_fogStart = start;
        m_fogEnd = end;
        m_fogDensity = density;
        m_fogIntensity = intensity;
    }

    // Sine of the elevation at which the sky is clear again. ~0.10 is about six
    // degrees, which is roughly how high a haze layer actually sits.
    void SetHorizonBlend(float sinElev) { m_horizonBlend = sinElev; }

    // Applied immediately, not batched: the sky is drawn from the client and from
    // the editor, and both push fog in the same breath as the draw, so there is
    // nothing to defer and one fewer flush ordering to get wrong. This is the same
    // mistake class SurfaceMaterial's uFullBright had - it batched into
    // UpdateFrame, which runs BEFORE the draw passes, so the value never landed.
    void Apply(const Vector3& cameraPos, const Color& tint) {
        if (m_shader.id == 0) return;

        if (m_tintLoc >= 0) {
            float c[4] = { (float)tint.r / 255.0f, (float)tint.g / 255.0f,
                           (float)tint.b / 255.0f, (float)tint.a / 255.0f };
            SetShaderValue(m_shader, m_tintLoc, c, SHADER_UNIFORM_VEC4);
        }
        float p[3] = { cameraPos.x, cameraPos.y, cameraPos.z };
        if (m_camLoc >= 0) SetShaderValue(m_shader, m_camLoc, p, SHADER_UNIFORM_VEC3);

        if (m_fogColorLoc >= 0)
            SetShaderValue(m_shader, m_fogColorLoc, m_fogColor, SHADER_UNIFORM_VEC3);
        if (m_fogStartLoc >= 0)
            SetShaderValue(m_shader, m_fogStartLoc, &m_fogStart, SHADER_UNIFORM_FLOAT);
        if (m_fogEndLoc >= 0)
            SetShaderValue(m_shader, m_fogEndLoc, &m_fogEnd, SHADER_UNIFORM_FLOAT);
        if (m_fogDensityLoc >= 0)
            SetShaderValue(m_shader, m_fogDensityLoc, &m_fogDensity, SHADER_UNIFORM_FLOAT);
        if (m_fogIntensityLoc >= 0)
            SetShaderValue(m_shader, m_fogIntensityLoc, &m_fogIntensity, SHADER_UNIFORM_FLOAT);
        if (m_horizonLoc >= 0)
            SetShaderValue(m_shader, m_horizonLoc, &m_horizonBlend, SHADER_UNIFORM_FLOAT);
    }

private:
    SkyMaterial() = default;

    void CacheLocations() {
        m_tintLoc         = GetShaderLocation(m_shader, "colDiffuse");
        m_camLoc          = GetShaderLocation(m_shader, "uCamPos");
        m_fogColorLoc     = GetShaderLocation(m_shader, "fogColor");
        m_fogStartLoc     = GetShaderLocation(m_shader, "fogStart");
        m_fogEndLoc       = GetShaderLocation(m_shader, "fogEnd");
        m_fogDensityLoc   = GetShaderLocation(m_shader, "fogDensity");
        m_fogIntensityLoc = GetShaderLocation(m_shader, "fogIntensity");
        m_horizonLoc      = GetShaderLocation(m_shader, "uSkyHorizonBlend");
    }

    Shader m_shader{0};
    bool   m_loaded = false;

    int m_tintLoc = -1;
    int m_camLoc = -1;
    int m_fogColorLoc = -1;
    int m_fogStartLoc = -1;
    int m_fogEndLoc = -1;
    int m_fogDensityLoc = -1;
    int m_fogIntensityLoc = -1;
    int m_horizonLoc = -1;

    float m_fogColor[3] = {0.7f, 0.7f, 0.8f};
    float m_fogStart = 10.0f;
    float m_fogEnd = 100.0f;
    float m_fogDensity = 1.0f;
    float m_fogIntensity = 0.0f;   // fog off until a level publishes some
    float m_horizonBlend = 0.10f;
};

} // namespace oz