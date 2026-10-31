#pragma once
#include "raylib.h"
#include "raymath.h"
#include "rlights/rlights.h"
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

// Light types matching rlights.h LightType enum
enum class LitLightType : int {
    DIRECTIONAL = 0,
    POINT = 1,
    SPOT = 2
};

// Light effect types matching rlights.h LightEffect enum
enum class LitLightEffect : int {
    NONE = 0,
    WATERY = 1,
    TORCH = 2,
    FIRE = 3,
    LAMP = 4
};

// LightNode — a first-class light entity managed by PawnSystem
struct LightNode {
    uint32_t id = 0;
    bool active = false;

    LitLightType type = LitLightType::POINT;
    Vector3 position{0, 0, 0};
    Vector3 target{0, 0, 0};
    Color color = WHITE;

    float intensity = 1.0f;
    float radius = 50.0f;

    // Spot light cone (cosine of half-angles)
    float innerCone = 0.95f;   // cos(~18 deg)
    float outerCone = 0.80f;   // cos(~37 deg)

    // Light effect animation
    LitLightEffect effect = LitLightEffect::NONE;
    float phase = 0.0f;
    float period = 1.0f;

    // Static = baked into lightmap, skipped per-frame update
    bool isStatic = false;
    bool castShadow = false;    // deferred to later shadow phase
    bool flare = false;         // draws a bright additive billboard at the light
    bool corona = false;        // draws a larger, dimmer halo billboard

    // -1 = affects all zones, 0+ = only affects matching zone.
    //
    // WRITE-ONLY TODAY. PawnSystem::AssignLightZones() writes this on every world
    // load / light apply, and NOTHING reads it -- there is no per-zone light
    // filtering yet. Kept (rather than deleted) as the intended hook for a future
    // per-zone lighting system; do not treat it as live. The per-apply cost is a
    // pure write.
    int zoneId = -1;
    // Remaining lifetime for transient (effect) lights, seconds. World lights
    // leave this at 0 and are never aged out by CombatFX.
    float timer = 0.0f;
    std::string name;           // editor label
};

// ---------------------------------------------------------------------------
// Core runtime functions
// ---------------------------------------------------------------------------

// Update all active lights: animate dynamics, sort by distance, submit to shader
// Submit the lights to one shader program. Does NOT animate - see
// LitLightning_Animate, which is called once per frame rather than once per
// program.
//
// Two caches make repeated submission within a frame cheap, and both are
// content-based rather than frame-based, so a caller that never goes through
// LitLightning_UpdateFrame (AngelEd calls this directly, once per pane) cannot
// be served stale state:
//   * the SORT is skipped when nothing the comparator reads has changed;
//   * a light whose 12 uniforms already hold the same values in this program is
//     not re-uploaded. See LitLightning.cpp's UploadedLight.
//
// Returns how many lights actually reached the GPU. That number is the only
// truthful answer to "is this scene lit", because a light can be active, sorted
// first and still be force-disabled when the budget is full.
int LitLightning_Update(std::vector<LightNode>& lights, Shader shader, Camera3D camera, float dt);

// Full per-frame lighting pass: uploads the camera view uniform, submits
// `lights` via LitLightning_Update, applies a directional camera fill light
// only when no world light is active, and advances the uTime uniform.
// `fallbackDirectional` is the headlight slot (engine GameLight[0]).
// Called once per frame from UpdateLightSources().
void LitLightning_UpdateFrame(std::vector<LightNode>& lights, Shader shader,
                              Camera3D camera, Light& fallbackDirectional, float dt);

// Animate a dynamic light based on its effect type and phase/period
void LitLightning_Animate(LightNode& node, float dt);

// Order lights for submission: most wanted first, then nearest.
//
// The sort DECIDES WHICH LIGHTS SURVIVE when a level has more than MAX_LIGHTS of
// them, so "most wanted" is a correctness question, not a quality one. A
// directional light has no radius and no attenuation, so ranking it by distance
// like a point light put it last in any busy level - measured: 40 point lights
// plus a sun rendered byte-identically to the same level with the sun deleted.
//
// Order: active, then tier (directional, in-range, out-of-range), then distance.
// Tiers are computed in LitLightning.cpp; see LightSubmitTier for why.
//
// Called from LitLightning_Update, which skips it while the comparator's inputs
// are unchanged; calling it directly always performs the sort.
void LitLightning_SortByDistance(std::vector<LightNode>& lights, Vector3 cameraPos);

// ---------------------------------------------------------------------------
// Transient light pool.
//
// Short-lived effect lights (muzzle flash, impact sparks, explosions). They are
// submitted to the shader BEFORE world lights so a busy world can never evict
// them: the frame buffer holds MAX_LIGHTS entries and everything past that is
// force-disabled, so a transient light pushed through the normal `lights`
// vector would flicker out the moment a level filled the budget.
// ---------------------------------------------------------------------------
static constexpr int MAX_TRANSIENT_LIGHTS = 8;

std::vector<LightNode>& LitLightning_TransientLights();
void LitLightning_ClearTransientLights();
