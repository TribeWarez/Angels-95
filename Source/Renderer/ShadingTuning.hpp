#pragma once
// ============================================================================
// Renderer/ShadingTuning.hpp
//
// Uploads the three player-facing shading knobs from Advanced Options >
// Display > Rendering > Lighting to a shader program:
//
//     uAmbientBoost   multiplies the ambient term        (1.0 = authored)
//     uSpecularScale  multiplies the Blinn-Phong specular (1.0 = authored)
//     uLightSoftness  widens the attenuation curve        (0.0 = authored)
//
// ONE definition, called once per program per frame:
//
//     LitFog  (world geometry)          Core.hpp UpdateLightSources
//     Surface (decorated brushes)       SurfaceMaterial::UpdateFrame
//     Wind    (Wind.vs + LitFog.fs)     Core.hpp UpdateLightSources
//
// A fourth copy of this would drift, and the drift is invisible: a shader that
// declares a uniform nobody uploads renders on the GLSL default, which is
// exactly how `period` shipped frozen for as long as it did (see AGENTS.md).
// tests/Surface.test.cpp checks that every uniform named here still exists in
// both fragment shaders.
//
// Header-only for the usual build-wiring reason (Makefile + AngelEd/Makefile +
// the inline g++ list in ci.yml), the same as CullState.hpp and Skybox.hpp.
// ============================================================================
#include "raylib.h"
#include "../GameplayFlags.hpp"

namespace oz {

inline void ApplyShadingTuning(Shader shader) {
    if (shader.id == 0) return;

    // Locations are stable per program; cache them (a GetShaderLocation is a
    // string lookup, and this runs for up to three programs every frame).
    static unsigned int s_lastId = 0;
    static int s_boostLoc = -1, s_specLoc = -1, s_softLoc = -1;
    if (shader.id != s_lastId) {
        s_lastId = shader.id;
        s_boostLoc = GetShaderLocation(shader, "uAmbientBoost");
        s_specLoc  = GetShaderLocation(shader, "uSpecularScale");
        s_softLoc  = GetShaderLocation(shader, "uLightSoftness");
    }

    if (s_boostLoc >= 0)
        SetShaderValue(shader, s_boostLoc, &g_lightAmbientBoost, SHADER_UNIFORM_FLOAT);
    if (s_specLoc >= 0)
        SetShaderValue(shader, s_specLoc, &g_lightSpecularScale, SHADER_UNIFORM_FLOAT);
    if (s_softLoc >= 0)
        SetShaderValue(shader, s_softLoc, &g_lightSoftness, SHADER_UNIFORM_FLOAT);
}

} // namespace oz
