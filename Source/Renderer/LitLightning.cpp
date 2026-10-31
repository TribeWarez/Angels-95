#include "LitLightning.hpp"
#include "rlights/rlights.h"
#include "ShadingTuning.hpp"
#include "../GameplayFlags.hpp"
#include "../Log.hpp"
#include "../Diagnostics/FrameProfiler.hpp"
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
// Cached uniform locations, per shader PROGRAM.
//
// This used to call GetShaderLocation 12 times per light AND 12 times for every
// unused slot, every frame, for every program (LitFog, Surface.fs, Wind):
// ~2,300 string-formatted glGetUniformLocation calls per frame, each allocating
// through TextFormat. Uniform locations are fixed for the lifetime of a linked
// program, so they are resolved once per program id and reused.
//
// The shader-id key also handles a hot-reloaded/re-linked program: a new id is a
// new cache entry, and the oldest entry is recycled when the table fills.
// ---------------------------------------------------------------------------
namespace {
constexpr int kLightFieldCount = 12;
constexpr int kLightCachePrograms = 8;

// What was last uploaded into ONE slot of ONE program.
//
// `UpdateLightValues` writes 12 uniforms per light, and each `SetShaderValue` is
// a `glUseProgram` in raylib's rcore.c. A light's enable/type/colour/intensity/
// radius/cones/period only change when the light is EDITED, while the array was
// re-uploaded in full for every program every frame (LitFog, Surface.fs and the
// Wind program all share this submission path). Comparing against this record
// skips the unchanged majority; only `phase` (animated lights) and `position`
// (moving lights) change per frame in practice.
//
// Exact bit equality on purpose: a spurious upload costs 12 uniforms, a
// SUPPRESSED one leaves the program holding someone else's light, so only an
// exact match may skip. `hasFlare`/`hasCorona` are CPU-side (Core.hpp draws the
// billboards) and are deliberately absent - they are not uniforms.
struct UploadedLight {
    bool    valid = false;
    int     type = 0;
    bool    enabled = false;
    Vector3 position{0.0f, 0.0f, 0.0f};
    Vector3 target{0.0f, 0.0f, 0.0f};
    Color   color{0, 0, 0, 0};
    float   intensity = 0.0f;
    float   radius = 0.0f;
    float   innerCone = 0.0f;
    float   outerCone = 0.0f;
    float   phase = 0.0f;
    float   period = 0.0f;
    int     effect = 0;
};

// `ignorePhase` exists so the submission path can treat the animation clock
// separately: it changes every frame on every non-static light, and uploading 12
// uniforms for a one-float change defeats the cache entirely.
bool SameUploadedLight(const Light& l, const UploadedLight& u, bool ignorePhase = false) {
    if (!u.valid) return false;
    if (!(l.type == u.type && l.enabled == u.enabled &&
          l.position.x == u.position.x && l.position.y == u.position.y &&
          l.position.z == u.position.z &&
          l.target.x == u.target.x && l.target.y == u.target.y &&
          l.target.z == u.target.z &&
          l.color.r == u.color.r && l.color.g == u.color.g &&
          l.color.b == u.color.b && l.color.a == u.color.a &&
          l.intensity == u.intensity && l.radius == u.radius &&
          l.innerCone == u.innerCone && l.outerCone == u.outerCone &&
          l.period == u.period && l.effect == u.effect)) {
        return false;
    }
    return ignorePhase || l.phase == u.phase;
}

void RememberUploadedLight(const Light& l, UploadedLight& u) {
    u.valid = true;
    u.type = l.type;
    u.enabled = l.enabled;
    u.position = l.position;
    u.target = l.target;
    u.color = l.color;
    u.intensity = l.intensity;
    u.radius = l.radius;
    u.innerCone = l.innerCone;
    u.outerCone = l.outerCone;
    u.phase = l.phase;
    u.period = l.period;
    u.effect = l.effect;
}

struct ShaderLightLocs {
    unsigned int id = 0;
    int locs[MAX_LIGHTS][kLightFieldCount];
    // Number of slots the fragment loop must evaluate. When the shader has it,
    // the disabled tail is never read, so it no longer needs to be uploaded at
    // all. A legacy shader (loc < 0) falls back to the disable-the-tail path.
    int lightCountLoc = -1;
    // Legacy path: slots from `lastSubmitted` upward were already disabled last
    // frame, so only a DROP in the submitted count needs the tail re-uploaded.
    int lastSubmitted = MAX_LIGHTS;
    bool uploadedOnce = false;
    // Last values written into each slot of THIS program. Invalidated whole on
    // program relink and on cache-entry recycle (see LightLocsFor).
    UploadedLight uploaded[MAX_LIGHTS];
};
ShaderLightLocs g_lightLocs[kLightCachePrograms];
int g_lightLocPrograms = 0;

ShaderLightLocs* LightLocsFor(Shader shader) {
    if (shader.id == 0) return nullptr;

    for (int i = 0; i < g_lightLocPrograms; ++i)
        if (g_lightLocs[i].id == shader.id) return &g_lightLocs[i];

    int slot = (g_lightLocPrograms < kLightCachePrograms)
                   ? g_lightLocPrograms++
                   : 0;   // recycle; the engine has three programs, this is defensive
    ShaderLightLocs& entry = g_lightLocs[slot];
    // Reset the WHOLE entry when an id is reused. The recycle branch used to
    // overwrite only `id` and `locs`, leaving `lastSubmitted`/`uploadedOnce`
    // from the evicted program; with per-slot upload records that would also
    // leave the new program comparing against another program's uniforms, i.e.
    // skipping uploads whose values are not in this program at all.
    entry = ShaderLightLocs{};
    entry.id = shader.id;
    entry.lightCountLoc = GetShaderLocation(shader, "uLightCount");

    for (int i = 0; i < MAX_LIGHTS; ++i) {
        int* l = entry.locs[i];
        l[0]  = GetShaderLocation(shader, TextFormat("lights[%i].enabled", i));
        l[1]  = GetShaderLocation(shader, TextFormat("lights[%i].type", i));
        l[2]  = GetShaderLocation(shader, TextFormat("lights[%i].position", i));
        l[3]  = GetShaderLocation(shader, TextFormat("lights[%i].target", i));
        l[4]  = GetShaderLocation(shader, TextFormat("lights[%i].color", i));
        l[5]  = GetShaderLocation(shader, TextFormat("lights[%i].intensity", i));
        l[6]  = GetShaderLocation(shader, TextFormat("lights[%i].radius", i));
        l[7]  = GetShaderLocation(shader, TextFormat("lights[%i].innerCone", i));
        l[8]  = GetShaderLocation(shader, TextFormat("lights[%i].outerCone", i));
        l[9]  = GetShaderLocation(shader, TextFormat("lights[%i].phase", i));
        l[10] = GetShaderLocation(shader, TextFormat("lights[%i].period", i));
        l[11] = GetShaderLocation(shader, TextFormat("lights[%i].effect", i));
    }
    return &entry;
}

void SetShaderLightCount(Shader shader, int count) {
    ShaderLightLocs* entry = LightLocsFor(shader);
    if (!entry || entry->lightCountLoc < 0) return;
    if (count < 0) count = 0;
    if (count > MAX_LIGHTS) count = MAX_LIGHTS;
    SetShaderValue(shader, entry->lightCountLoc, &count, SHADER_UNIFORM_INT);
}
} // namespace

// ---------------------------------------------------------------------------
// Build an rlights Light struct from a LightNode
// ---------------------------------------------------------------------------
static Light BuildRLight(const LightNode& node, const int* locs, int index) {
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

    const int* l = locs + index * kLightFieldCount;
    light.enabledLoc   = l[0];
    light.typeLoc      = l[1];
    light.positionLoc  = l[2];
    light.targetLoc    = l[3];
    light.colorLoc     = l[4];
    light.intensityLoc = l[5];
    light.radiusLoc    = l[6];
    light.innerConeLoc = l[7];
    light.outerConeLoc = l[8];
    light.phaseLoc     = l[9];
    light.periodLoc    = l[10];
    light.effectLoc    = l[11];

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
// Mutates `lights` in place (callers rely on the vector being reordered; nothing
// depends on the previous order).
//
// DECORATE-SORT. The comparator used to call Vector3Distance(a, cam) and
// Vector3Distance(b, cam) on every comparison, and LightSubmitTier recomputed it
// a third time - O(n log n) square roots for an ordering that needs O(n). The
// keys are computed once per light here; the ordering is unchanged.
//
// The index tie-break is deliberate: std::sort is not stable, so two lights that
// compare equal were ordered arbitrarily. Equal keys mean the comparator cannot
// distinguish them, so pinning the original order cannot change which lights
// survive the budget - it only makes a frame reproducible.
void LitLightning_SortByDistance(std::vector<LightNode>& lights, Vector3 cameraPos) {
    struct Keyed {
        size_t index = 0;
        bool active = false;
        int tier = 0;
        float dist = 0.0f;
    };
    std::vector<Keyed> keys;
    keys.reserve(lights.size());
    for (size_t i = 0; i < lights.size(); ++i) {
        const LightNode& n = lights[i];
        keys.push_back(Keyed{i, n.active, LightSubmitTier(n, cameraPos),
                             Vector3Distance(n.position, cameraPos)});
    }
    std::sort(keys.begin(), keys.end(), [](const Keyed& a, const Keyed& b) {
        if (a.active != b.active) return a.active;
        if (a.tier != b.tier) return a.tier < b.tier;
        if (a.dist != b.dist) return a.dist < b.dist;
        return a.index < b.index;
    });

    std::vector<LightNode> ordered;
    ordered.reserve(lights.size());
    for (const Keyed& k : keys) ordered.push_back(std::move(lights[k.index]));
    lights = std::move(ordered);
}

// ---------------------------------------------------------------------------
// "Do the lights still sort the same?" signature.
//
// LitLightning_Update runs once per shader PROGRAM, so the same vector was
// re-sorted three times a frame (LitFog, Surface.fs, Wind.vs) with identical
// inputs. The sort only has to run when something the comparator reads changes:
// active, type, radius, position - plus the camera.
//
// This is CONTENT-based rather than a frame counter on purpose. A frame token
// stored by LitLightning_UpdateFrame would not advance when the profiler is
// disabled (BeginFrame returns early), and AngelEd calls LitLightning_Update
// directly once per pane (no UpdateFrame at all) - both would serve a stale
// order, and the order decides WHICH LIGHTS SURVIVE the budget. A hash of the
// inputs is always correct: equal inputs => the existing order is already a
// valid sort of them.
//
// Any new input to the comparator must be added here.
// ---------------------------------------------------------------------------
namespace {
uint64_t LightSortSignature(const std::vector<LightNode>& lights, Vector3 cameraPos) {
    uint64_t h = 1469598103934665603ull;   // FNV-1a 64 offset basis
    auto mix = [&h](const void* data, size_t bytes) {
        const unsigned char* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < bytes; ++i) {
            h ^= (uint64_t)p[i];
            h *= 1099511628211ull;
        }
    };
    mix(&cameraPos, sizeof(cameraPos));
    for (const LightNode& n : lights) {
        const int type = (int)n.type;
        mix(&n.active, sizeof(n.active));
        mix(&type, sizeof(type));
        mix(&n.radius, sizeof(n.radius));
        mix(&n.position, sizeof(n.position));
    }
    return h;
}

uint64_t g_lastLightSortSig = 0;
bool g_hasLightSortSig = false;
} // namespace

// ---------------------------------------------------------------------------
// Submit lights to one shader program. Does NOT animate - see LitLightning_Animate.
// ---------------------------------------------------------------------------
int LitLightning_Update(std::vector<LightNode>& lights, Shader shader, Camera3D camera, float dt) {
    (void)dt;   // animation is LitLightning_Animate's job, called once per frame
    OZ_PROFILE_SCOPE("lighting.submit");

    // One lookup table per program, resolved on first use. A missing table means
    // the program has no lights[] uniform at all, which is not an error for a
    // program that draws unlit geometry.
    ShaderLightLocs* locEntry = LightLocsFor(shader);
    if (!locEntry) return 0;
    const int* lightLocs = &locEntry->locs[0][0];

    // Sort active lights by distance to camera - but only when the inputs the
    // comparator reads have actually changed. See LightSortSignature: this call
    // site runs once per shader program.
    const uint64_t sortSig = LightSortSignature(lights, camera.position);
    if (!g_hasLightSortSig || sortSig != g_lastLightSortSig) {
        LitLightning_SortByDistance(lights, camera.position);
        g_lastLightSortSig = sortSig;
        g_hasLightSortSig = true;
    }

    // Submit lights to shader.
    //
    // Transient lights (muzzle flash, impact sparks) are submitted first so they
    // always win a slot. The frame buffer only has MAX_LIGHTS entries and the
    // tail is force-disabled, so a transient light pushed through `lights` would
    // be evicted the moment a world filled the budget - which is presumably why
    // the muzzle flash was implemented as an unlit DrawSphere in the first place.
    int submitted = 0;
    int uploadedLights = 0;
    int phaseUploads = 0;
    int uploads = 0;

    // One submission path for both pools, so the upload cache cannot apply to
    // one of them and not the other.
    auto submitOne = [&](const LightNode& node) {
        Light rlight = BuildRLight(node, lightLocs, submitted);
        UploadedLight& record = locEntry->uploaded[submitted];
        // Everything except `phase`: the animation clock changes every frame on
        // every non-static light, so comparing it here would make the whole cache
        // dead (measured: Dust_Ravine uploaded 96 lights/frame unchanged).
        if (!SameUploadedLight(rlight, record, /*ignorePhase=*/true)) {
            UpdateLightValues(shader, rlight);
            RememberUploadedLight(rlight, record);
            ++uploadedLights;
            uploads += 12;
        } else if (rlight.period > 0.0f && rlight.phase != record.phase) {
            // Only the flicker clock moved. Both shaders read `phase` solely when
            // `period > 0` (applyLightEffect gates on it), so one uniform is the
            // whole update - and a period <= 0 light uploads nothing at all.
            // record.phase is deliberately NOT advanced when period <= 0, so if a
            // light is later animated it re-uploads its clock instead of
            // inheriting a stale one.
            if (rlight.phaseLoc >= 0)
                SetShaderValue(shader, rlight.phaseLoc, &rlight.phase, SHADER_UNIFORM_FLOAT);
            record.phase = rlight.phase;
            ++phaseUploads;
            ++uploads;
        }
        ++submitted;
    };

    for (auto& node : g_transientLights) {
        if (!node.active) continue;
        if (submitted >= MAX_TRANSIENT_LIGHTS) break;
        if (submitted >= MAX_LIGHTS) break;
        submitOne(node);
    }

    for (auto& node : lights) {
        if (!node.active) continue;
        if (submitted >= MAX_LIGHTS) break;
        submitOne(node);
    }

    if (locEntry->lightCountLoc >= 0) {
        // The fragment loop stops at the submitted count, so the tail is never
        // read and does not need disabling at all.
        SetShaderLightCount(shader, submitted);
        ++uploads;
    } else {
        // Legacy shader without uLightCount: the loop still runs to MAX_LIGHTS,
        // so disabled slots must stay disabled. Only a DROP in the submitted
        // count needs the tail re-uploaded - re-uploading all 64 slots every
        // frame was 12 SetShaderValue calls x (64 - submitted) per program.
        // `type` is DIRECTIONAL and radius 0 deliberately: a zero-radius
        // directional contributes no attenuation beyond one branch per fragment.
        const int previouslyLiveTo = locEntry->uploadedOnce
                                         ? locEntry->lastSubmitted
                                         : MAX_LIGHTS;
        if (submitted < previouslyLiveTo) {
            for (int i = submitted; i < previouslyLiveTo; i++) {
                const int* l = lightLocs + i * kLightFieldCount;
                Light dummy = {0};
                dummy.type = LIGHT_DIRECTIONAL;
                dummy.enabledLoc   = l[0];
                dummy.typeLoc      = l[1];
                dummy.positionLoc  = l[2];
                dummy.targetLoc    = l[3];
                dummy.colorLoc     = l[4];
                dummy.intensityLoc = l[5];
                dummy.radiusLoc    = l[6];
                dummy.innerConeLoc = l[7];
                dummy.outerConeLoc = l[8];
                dummy.phaseLoc     = l[9];
                dummy.periodLoc    = l[10];
                dummy.effectLoc    = l[11];
                dummy.enabled = false;
                // Same cache rule: a slot already holding a disabled dummy is
                // not re-uploaded. This is what keeps the tail free on every
                // frame after the first drop.
                UploadedLight& record = locEntry->uploaded[i];
                if (!SameUploadedLight(dummy, record)) {
                    UpdateLightValues(shader, dummy);
                    RememberUploadedLight(dummy, record);
                    uploads += 12;
                }
            }
        }
        locEntry->lastSubmitted = submitted;
        locEntry->uploadedOnce = true;
    }

    OZ_PROFILE_ADD("light.submitted", (uint64_t)submitted);
    // Full struct uploads plus phase-only ones; both are "this light's state was
    // written this program this frame", and `uniform.upload` above is the real
    // cost (12 per full upload, 1 per phase-only).
    OZ_PROFILE_ADD("light.uploads", (uint64_t)(uploadedLights + phaseUploads));
    OZ_PROFILE_ADD("uniform.upload", (uint64_t)uploads);

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
        // Slot 0 is now live for this frame; without this the loop bound would
        // stay at the submitted count (0) and the fill light would be ignored.
        SetShaderLightCount(shader, 1);
        // Record it: this light is NOT in `lights`, so without the record a
        // later frame's world light whose fields matched the PREVIOUS record
        // would skip its upload while the uniform actually held the fill
        // light's values.
        if (ShaderLightLocs* entry = LightLocsFor(shader))
            RememberUploadedLight(fallbackDirectional, entry->uploaded[0]);
        OZ_PROFILE_ADD("uniform.upload", 13);   // 12 light fields + uLightCount
        OZ_PROFILE_ADD("light.uploads", 1);
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
