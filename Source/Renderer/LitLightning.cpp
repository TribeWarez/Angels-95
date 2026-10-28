#include "LitLightning.hpp"
#include "rlights/rlights.h"
#include "ShadingTuning.hpp"
#include "../GameplayFlags.hpp"
#include "../Log.hpp"
#include <cmath>

// Transient effect lights. Static storage: effect lights must outlive any single
// call site and must not be reallocated per frame.
static std::vector<LightNode> g_transientLights;

// ---------------------------------------------------------------------------
// Advance a dynamic light's animation clock
// ---------------------------------------------------------------------------
// This now does ONE thing: advance the phase.
//
// It used to also OVERWRITE node.intensity and node.radius with per-effect
// constants, which meant a torch authored `intensity 0.3 radius 12` silently
// became `0.85 +/- 0.15` and `45 +/- 10` every frame - the authored values were
// unreachable. The per-effect modulation now lives in the shader's
// applyLightEffect(), where the colour TINTS already were and where it can use the
// light's own phase instead of its slot index.
//
// Split from submission on purpose: LitLightning_Update is called once per shader
// PROGRAM (LitFog and Surface.fs each get one), so animating inside it advanced
// phase twice per frame and ran every flicker at double speed.
void LitLightning_Animate(LightNode& node, float dt) {
    if (node.isStatic) return;
    // A PLAIN ELAPSED-TIME CLOCK. It used to advance by `dt * (1.0f / period)`,
    // and then the shader divided by `period` AGAIN to build its animation clock:
    //
    //     w = phase / period = (t / period) / period = t / period^2
    //
    // so an authored period of 0.25 ran the flicker at 16x the intended rate and a
    // period of 2 ran it at a quarter. That was invisible only because the one
    // shipped `.ozls` default is `period = 1`, where 1/period^2 == 1/period == 1
    // and the two forms coincide. `period` was ALSO never uploaded (see the Light
    // struct in rlights.h), so the shader saw 0 and never flickered at all - which
    // masked the CPU-side error rather than causing it.
    //
    // The shader owns the conversion. Scale `period` in one place, not two.
    node.phase += dt;
}

// ---------------------------------------------------------------------------
// Build an rlights Light struct from a LightNode
// ---------------------------------------------------------------------------
static Light BuildRLight(const LightNode& node, Shader shader, int index) {
    Light light = {0};
    light.type = (int)node.type;
    light.enabled = node.active;
    light.position = node.position;
    light.target = node.target;
    // Colour is uploaded UNMODULATED. It used to be pre-multiplied by intensity
    // here AND multiplied by intensity again in the shader's applyLightEffect(),
    // so the rendered contribution was colour * intensity^2: an authored 0.5 came
    // out at 0.25, and anything above 1.0 clamped its colour channel before the
    // second multiply. intensity is a single scalar and the shader owns it.
    light.color = node.color;
    light.intensity = fmaxf(node.intensity, 0.0f);
    light.radius = node.radius;

    // Cosines, matching LightNode and spotCone(). These two were never uploaded at
    // all, so the shader read 0/0 and smoothstep(0,0,x) == 1 - every spot light
    // was omnidirectional no matter what the cone said.
    light.innerCone = node.innerCone;
    light.outerCone = node.outerCone;

    // The light's own clock, so a flicker belongs to the light rather than to the
    // slot it happens to occupy this frame (the distance sort reorders).
    light.phase = node.phase;

    // Never assigned before, and never uploaded before. Both shaders read
    // `lights[i].period` and use `<= 0` to mean "do not animate", so the missing
    // member left every torch / fire / water / lamp rendering as a steady light
    // with a correct colour tint and no flicker whatsoever.
    //
    // Advanced Options > Lighting > "Dynamic Lights" is the player switch for
    // that animation, and this is the only place a light's period reaches the
    // wire: uploading 0 is what the shader already treats as "static", so the
    // toggle needs no new uniform and no branch in either fragment shader.
    light.period = g_dynamicLights ? node.period : 0.0f;

    // Never assigned before. `Light light = {0}` left this 0 == LIGHT_EFFECT_NONE,
    // so applyLightEffect()'s four tint branches were unreachable: a torch was
    // white, fire was not orange.
    light.effect = (int)node.effect;
    light.hasFlare = node.flare ? 1 : 0;
    light.hasCorona = node.corona ? 1 : 0;

    light.enabledLoc = GetShaderLocation(shader, TextFormat("lights[%i].enabled", index));
    light.typeLoc = GetShaderLocation(shader, TextFormat("lights[%i].type", index));
    light.positionLoc = GetShaderLocation(shader, TextFormat("lights[%i].position", index));
    light.targetLoc = GetShaderLocation(shader, TextFormat("lights[%i].target", index));
    light.colorLoc = GetShaderLocation(shader, TextFormat("lights[%i].color", index));
    light.intensityLoc = GetShaderLocation(shader, TextFormat("lights[%i].intensity", index));
    light.radiusLoc = GetShaderLocation(shader, TextFormat("lights[%i].radius", index));
    light.innerConeLoc = GetShaderLocation(shader, TextFormat("lights[%i].innerCone", index));
    light.outerConeLoc = GetShaderLocation(shader, TextFormat("lights[%i].outerCone", index));
    light.phaseLoc = GetShaderLocation(shader, TextFormat("lights[%i].phase", index));
    light.periodLoc = GetShaderLocation(shader, TextFormat("lights[%i].period", index));
    light.effectLoc = GetShaderLocation(shader, TextFormat("lights[%i].effect", index));

    return light;
}

// ---------------------------------------------------------------------------
// How badly does this light want a slot? Lower is better.
//
// THIS IS A CORRECTNESS THING, NOT A QUALITY ONE. The sort's job is to decide
// which MAX_LIGHTS lights survive; anything it ranks badly can silently vanish.
//
// A directional light has no radius and no attenuation - it contributes NdotL
// everywhere in the level - so "distance from the camera" says nothing about how
// much it is worth. Point and spot lights DO have a finite radius, so for those,
// distance is a real proxy. The old comparator applied one distance rule to both,
// which meant a sun authored 400 units away sorted as though it were the least
// important light in the world, and it was therefore the first thing dropped.
//
// Measured: a level with 40 point lights plus one directional sun rendered
// BYTE-IDENTICALLY to the same level with the sun line deleted. Same SHA-256.
// ---------------------------------------------------------------------------
static int LightSubmitTier(const LightNode& n, Vector3 cameraPos) {
    if (n.type == LitLightType::DIRECTIONAL) return 0;
    // `radius <= 0` means unlimited range, so such a light is never tier 2.
    if (n.radius <= 0.0f) return 1;
    // Beyond this the shader's lightAttenuation hard-cuts to 0.0, so the light
    // contributes EXACTLY NOTHING to any fragment from here. Ranking it last means
    // it is the correct light to lose when the budget runs out.
    //
    // Note the threshold is 1x radius, not the old 2x: a light between 1x and 2x
    // occupied a precious slot while already being invisible.
    return (Vector3Distance(n.position, cameraPos) > n.radius) ? 2 : 1;
}

// Order lights for submission: most wanted first, then nearest.
// Mutates `lights` in place (callers and ExportToOzone rely on the vector being
// reordered; nothing depends on the previous order).
void LitLightning_SortByDistance(std::vector<LightNode>& lights, Vector3 cameraPos) {
    std::sort(lights.begin(), lights.end(),
        [cameraPos](const LightNode& a, const LightNode& b) {
            if (a.active != b.active) return a.active;

            const int ta = LightSubmitTier(a, cameraPos);
            const int tb = LightSubmitTier(b, cameraPos);
            if (ta != tb) return ta < tb;

            return Vector3Distance(a.position, cameraPos) <
                   Vector3Distance(b.position, cameraPos);
        });
}

// ---------------------------------------------------------------------------
// Submit lights to one shader program. Does NOT animate - see LitLightning_Animate.
// ---------------------------------------------------------------------------
int LitLightning_Update(std::vector<LightNode>& lights, Shader shader, Camera3D camera, float dt) {
    (void)dt;   // animation is LitLightning_Animate's job, called once per frame

    // Sort active lights by distance to camera
    LitLightning_SortByDistance(lights, camera.position);

    // Submit lights to shader.
    //
    // Transient lights (muzzle flash, impact sparks) are submitted first so they
    // always win a slot. The frame buffer only has MAX_LIGHTS entries and the
    // tail is force-disabled, so a transient light pushed through `lights` would
    // be evicted the moment a world filled the budget - which is presumably why
    // the muzzle flash was implemented as an unlit DrawSphere in the first place.
    int submitted = 0;
    for (auto& node : g_transientLights) {
        if (!node.active) continue;
        if (submitted >= MAX_TRANSIENT_LIGHTS) break;
        if (submitted >= MAX_LIGHTS) break;
        Light rlight = BuildRLight(node, shader, submitted);
        UpdateLightValues(shader, rlight);
        submitted++;
    }

    for (auto& node : lights) {
        if (!node.active) continue;
        if (submitted >= MAX_LIGHTS) break;
        Light rlight = BuildRLight(node, shader, submitted);
        UpdateLightValues(shader, rlight);
        submitted++;
    }

    // Disable remaining light slots. `type` is set to DIRECTIONAL and the radius
    // left at 0 deliberately: a zero-radius DIRECTIONAL contributes no
    // attenuation term at all, so a disabled slot costs one branch rather than a
    // full attenuation evaluation for every fragment.
    for (int i = submitted; i < MAX_LIGHTS; i++) {
        Light dummy = {0};
        dummy.type = LIGHT_DIRECTIONAL;
        dummy.enabledLoc = GetShaderLocation(shader, TextFormat("lights[%i].enabled", i));
        dummy.typeLoc = GetShaderLocation(shader, TextFormat("lights[%i].type", i));
        dummy.positionLoc = GetShaderLocation(shader, TextFormat("lights[%i].position", i));
        dummy.targetLoc = GetShaderLocation(shader, TextFormat("lights[%i].target", i));
        dummy.colorLoc = GetShaderLocation(shader, TextFormat("lights[%i].color", i));
        dummy.intensityLoc = GetShaderLocation(shader, TextFormat("lights[%i].intensity", i));
        dummy.radiusLoc = GetShaderLocation(shader, TextFormat("lights[%i].radius", i));
        dummy.innerConeLoc = GetShaderLocation(shader, TextFormat("lights[%i].innerCone", i));
        dummy.outerConeLoc = GetShaderLocation(shader, TextFormat("lights[%i].outerCone", i));
        dummy.phaseLoc = GetShaderLocation(shader, TextFormat("lights[%i].phase", i));
        dummy.periodLoc = GetShaderLocation(shader, TextFormat("lights[%i].period", i));
        dummy.effectLoc = GetShaderLocation(shader, TextFormat("lights[%i].effect", i));
        dummy.enabled = false;
        UpdateLightValues(shader, dummy);
    }

    return submitted;
}

// ---------------------------------------------------------------------------
// Transient light pool - short-lived effect lights that must never be evicted by
// world lighting. Kept separate from PawnSystem::m_lights for exactly that
// reason.
// ---------------------------------------------------------------------------
std::vector<LightNode>& LitLightning_TransientLights() {
    return g_transientLights;
}

void LitLightning_ClearTransientLights() {
    for (auto& n : g_transientLights) n.active = false;
}

// ---------------------------------------------------------------------------
// Full per-frame lighting pass (previously Core.hpp UpdateLightSources)
// ---------------------------------------------------------------------------
void LitLightning_UpdateFrame(std::vector<LightNode>& lights, Shader shader,
                              Camera3D camera, Light& fallbackDirectional, float dt) {
    float cameraPos[3] = { camera.position.x, camera.position.y, camera.position.z };

    SetShaderValue(shader, shader.locs[SHADER_LOC_VECTOR_VIEW], cameraPos, SHADER_UNIFORM_VEC3);

    // Advance every dynamic light's clock ONCE, here.
    //
    // This has to happen at the frame level rather than inside LitLightning_Update,
    // because LitLightning_Update runs once per shader PROGRAM - the client's
    // SurfaceMaterial::UpdateFrame calls it too - so animating inside it advanced
    // phase twice per frame and ran every flicker at double speed.
    for (auto& node : lights) {
        if (node.active && !node.isStatic)
            LitLightning_Animate(node, dt);
    }

    // Submit world/pawn lights
    const int submittedCount = LitLightning_Update(lights, shader, camera, dt);

    // One-shot diagnostic: confirm the world lights made it into the shader.
    static size_t s_lastLightCount = (size_t)-1;
    if (lights.size() != s_lastLightCount) {
        s_lastLightCount = lights.size();
        OZ_INFO("Lighting: worldLights=%zu shader=%d", lights.size(), shader.id);
    }

    // Directional camera fill, used ONLY when the world declares no active
    // lights of its own. A full-white headlight otherwise washed the lit scene
    // out and hid the world lights' colours.
    // Key this off what actually reached the GPU, NOT off `lights.size()`.
    //
    // The old scan asked "does any LightNode have active set", which is true even
    // when every one of them was culled or evicted - a level whose lights are all
    // beyond their radius renders with a real `active` count and therefore SUPPRESSES
    // this fill light, leaving a scene lit only by ambient (0.1/10 == 0.01, i.e.
    // near-black). The fill exists precisely to prevent that, so it must be asked
    // whether anything was submitted.
    const bool anyActiveLight = (submittedCount > 0);
    if (!anyActiveLight) {
        fallbackDirectional.position = camera.position;
        fallbackDirectional.target = { camera.target.x, camera.target.y - 5, camera.target.z };
        fallbackDirectional.type = LIGHT_DIRECTIONAL;
        fallbackDirectional.enabled = true;
        fallbackDirectional.intensity = 0.4f;
        UpdateLightValues(shader, fallbackDirectional);
    }

    // Update uTime for GPU light animation
    static int uTimeLoc = GetShaderLocation(shader, "uTime");
    float timeVal = (float)GetTime();
    SetShaderValue(shader, uTimeLoc, &timeVal, SHADER_UNIFORM_FLOAT);

    // Player shading knobs (Advanced Options > Lighting). Uploaded with the rest
    // of the frame-level lighting state so every program that shares this model
    // gets the same curve; SurfaceMaterial::UpdateFrame does the same for its
    // own program.
    oz::ApplyShadingTuning(shader);
}
