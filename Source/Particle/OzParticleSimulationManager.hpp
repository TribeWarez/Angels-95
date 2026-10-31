#pragma once
#include "raylib.h"
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace oz {
namespace particles {

// The ONE definition of an emission cone, shared by OZONE `ParticleEmitter`
// nodes and gameplay Burst() calls.
//
// These used to be two different meanings for the same authored field, and the
// mismatch was invisible because neither side could see the other:
//   * an emitter treated `spread` as a raw unit-vector offset added to each
//     component ((x,y,z) +- spread), so spread=12 -- which every shipped fire
//     emitter authors -- produced a near-isotropic spray rather than a cone;
//   * Burst() treated its `spread` as a half-angle in DEGREES.
// Emitters now use degrees too, which is the unit the rest of the OZONE
// vocabulary uses for angles (yaw, rotation). HOISTED into the header so the
// behaviour is unit-testable without a GL context: the particle pool lives in a
// .cpp that links raylib, and a cone that can only be checked by looking at a
// screenshot is how the two meanings drifted apart in the first place.
//
// `base` need not be normalised. `u1`/`u2` are caller-supplied uniform [0,1)
// randoms so both call sites share one distribution and a test can be
// deterministic. sqrt(u2) keeps the direction uniform over the cone's disc
// rather than bunched at its axis.
inline Vector3 ConeDirection(Vector3 base, float spreadDegrees, float u1, float u2) {
    constexpr float kPi = 3.14159265358979f;

    float bl = sqrtf(base.x * base.x + base.y * base.y + base.z * base.z);
    if (bl > 1e-4f) {
        base.x /= bl; base.y /= bl; base.z /= bl;
    } else {
        base = Vector3{0.0f, 1.0f, 0.0f};
    }

    if (spreadDegrees <= 0.0f) return base;
    if (spreadDegrees > 180.0f) spreadDegrees = 180.0f;   // a cone, never a full sphere
    const float spreadRad = spreadDegrees * (kPi / 180.0f);

    auto cross = [](Vector3 a, Vector3 b) {
        return Vector3{a.y * b.z - a.z * b.y,
                       a.z * b.x - a.x * b.z,
                       a.x * b.y - a.y * b.x};
    };
    auto normalize = [](Vector3 v) {
        const float l = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
        if (l < 1e-6f) return Vector3{0.0f, 1.0f, 0.0f};
        return Vector3{v.x / l, v.y / l, v.z / l};
    };

    // Orthonormal basis around base. The 0.99 test keeps the cross product
    // well-conditioned when the axis is (nearly) vertical.
    const Vector3 up = (fabsf(base.y) > 0.99f) ? Vector3{1.0f, 0.0f, 0.0f}
                                               : Vector3{0.0f, 1.0f, 0.0f};
    const Vector3 right = normalize(cross(up, base));
    const Vector3 realUp = cross(base, right);

    const float ang = u1 * 2.0f * kPi;
    const float rad = sqrtf(u2) * spreadRad;
    const float sa = sinf(rad);

    return normalize(Vector3{
        base.x * cosf(rad) + (right.x * cosf(ang) + realUp.x * sinf(ang)) * sa,
        base.y * cosf(rad) + (right.y * cosf(ang) + realUp.y * sinf(ang)) * sa,
        base.z * cosf(rad) + (right.z * cosf(ang) + realUp.z * sinf(ang)) * sa});
}

} // namespace particles
} // namespace oz

// ---------------------------------------------------------------------------
// OzParticleSimulationManager — GameEngine.ParticleEmitter simulation.
//
// Isolation contract: this manager is ticked once per frame at a fixed point
// *after* weapon/NPC/projectile simulation, and it only READS emitter
// definitions from PawnSystem. It never calls gameplay code, so particles can
// never re-enter or interrupt weapon mechanics or NPC ticks. Client-only and
// purely cosmetic (no networking).
//
// Emitter node definitions live in PawnSystem (m_particleEmitters); live
// particles live here.
// ---------------------------------------------------------------------------
class OzParticleSimulationManager {
public:
    static OzParticleSimulationManager& Instance();

    // Advance the simulation. Safely callable with dt <= 0 (no-op).
    void Update(float dt);

    // --- Ambient weather (OZONE `particles` / LevelSettings) ---
    //
    // These fields were parsed, round-tripped, exposed over the server's HTTP
    // map API and logged on world load, but NOTHING ever spawned from them: a
    // world authored `particles SNOW 120 1.2 ...` rendered no weather at all.
    // ConfigureWeather() is the first consumer.
    //
    // Weather shares the pool with gameplay particles, so it is hard-capped well
    // below MAX_PARTICLES: a dense snow field that consumed the whole pool would
    // silently eat muzzle flashes and impact sparks.
    void ConfigureWeather(int type, float density, float speed,
                          int r, int g, int b, float windX, float windZ);
    void ClearWeather();

    // Spawn weather around the camera for one frame. Separate from Update() on
    // purpose: Update() is fed only emitter definitions and must stay free of any
    // camera/gameplay input, so the weather spawn cannot be folded into it.
    void UpdateWeather(float dt, Vector3 camPos);

    // Draw all live particles in the 3D pass. `transientLights` (optional) are
    // effect lights that should be sampled before world lights; pass
    // LitLightning_TransientLights().
    void Draw(Camera3D& camera, const std::vector<struct LightNode>* transientLights = nullptr);

    // Ambient-layer on/off, driven by the user's "Particles" setting.
    //
    // Gates OZONE emitters and weather ONLY. Gameplay bursts spawned by Burst()
    // (muzzle smoke, impact sparks, blood/gibs) still draw when this is off, so
    // the setting cannot silently remove combat feedback/gore.
    //
    // Turning it OFF also RETIRES the live ambient field. Hiding alone left a
    // 300-second snow emitter simulating invisibly, spending CPU and squatting
    // in the shared pool where it starved combat bursts (see Acquire).
    void SetEnabled(bool enabled);

    // Drop every live particle (emitter nodes are cleared separately).
    void Clear();

    // Release the instanced shader/VAO/VBOs. Called once at client shutdown,
    // BEFORE CloseWindow(), while the GL context is still alive - the manager is
    // an intentionally-leaked singleton, so without this its GL objects would
    // outlive the context. Safe to call more than once.
    void Shutdown();

    // One-shot burst, independent of any OZONE emitter. There was no way to
    // spawn a particle from gameplay code before - Update() only reads emitter
    // defs - so muzzle smoke and impact sparks had to be drawn as separate
    // immediate-mode primitives instead.
    //
    // velocity is a base speed along dir; spread (degrees) randomises around it.
    void Burst(Vector3 pos, Vector3 dir, int count, float speed, float spread,
               Color color, Color colorEnd, float sizeStart, float sizeEnd,
               float lifetime, float gravity = 0.0f, Texture2D tex = {0});

    // Cap the number of particles the DRAW path emits (0 = no cap). Used by
    // the period render profile: the simulation budget is unchanged, so combat
    // bursts still exist, but a 2,000-particle ambient field cannot dominate a
    // period-class fill rate.
    void SetDrawBudget(int maxDrawn) { m_drawBudget = maxDrawn > 0 ? maxDrawn : 0; }
    int DrawBudget() const { return m_drawBudget; }

    int LiveCount() const { return m_live; }

    // Per-source accounting. One pool is shared by three sources with THREE
    // different claims on it, so "how many particles" alone cannot answer
    // "is the ambient field starving combat feedback?".
    int EmitterLiveCount() const { return m_emitterLive; }
    int WeatherLiveCount() const { return m_weather.live; }
    int GameplayLiveCount() const {
        const int n = m_live - m_emitterLive - m_weather.live;
        return n > 0 ? n : 0;
    }

    // Published so tests and worldcheck can assert against the real numbers
    // instead of hard-coding 4096 and drifting.
    static constexpr int PoolCapacity()      { return MAX_PARTICLES; }
    static constexpr int GameplayReserve()   { return kGameplayReserve; }
    static constexpr int EmitterBudget()     { return kEmitterMax; }

private:
    OzParticleSimulationManager() = default;

    struct Particle {
        Vector3 position{0, 0, 0};
        Vector3 velocity{0, 0, 0};
        float age = 0.0f;
        float lifetime = 1.0f;
        float sizeStart = 0.4f;
        float sizeEnd = 0.0f;
        Color colorStart{255, 255, 255, 255};
        Color colorEnd{255, 255, 255, 0};
        float gravity = 0.0f;
        Texture2D texture{0};
        // Copied from the emitter (and true for one-shot bursts). false draws
        // the particle as a lit sphere instead of a camera-facing quad; the
        // emitter field was previously written by the loader and never read.
        bool billboard = true;
        bool active = false;
        // Claimed by the ambient-weather spawner rather than an OZONE emitter or a
        // Burst. Weather is retargeted (not merely stopped) when the world's
        // weather changes, and it has its own share of the pool cap - so the two
        // sources cannot be told apart after the fact without this.
        bool weather = false;
        // Spawned by Burst(): gameplay feedback (muzzle smoke, impact sparks,
        // blood/gibs) rather than an ambient emitter or weather. The "Particles"
        // master toggle gates the AMBIENT layer only; gameplay bursts still draw
        // when it is off, so turning particles off cannot silently remove gore
        // from every hit.
        bool gameplay = false;
    };

    static constexpr int MAX_PARTICLES = 4096;

    // Pool share-out. Three sources contend for ONE pool and they do not have
    // equal claim on it:
    //
    //   * gameplay bursts (muzzle smoke, impact sparks, blood/gibs) are the
    //     player's confirmation that a shot connected and must NEVER fail;
    //   * a weather field is ambient set-dressing and interchangeable flake
    //     for flake;
    //   * an authored emitter is a deliberate feature, but its demand is
    //     unbounded -- World_endless_snow authors rate 60 x life 300 twice,
    //     i.e. 59,400 steady particles against this pool.
    //
    // Before these caps, `Acquire` simply returned nullptr once the pool
    // filled, so a saturated ambient field silently killed every combat burst
    // for the rest of the session, with no log and no visible cause.
    //
    // The reserve is a hard floor for bursts and doubles as the ceiling for
    // weather (MAX_WEATHER_PARTICLES) plus everything else for emitters.
    static constexpr int kGameplayReserve = 512;
    static constexpr int MAX_WEATHER_PARTICLES = 1200;
    static constexpr int kEmitterMax = MAX_PARTICLES - kGameplayReserve - MAX_WEATHER_PARTICLES;

    // Which source is claiming a slot. The claim sets the particle's tags AND
    // charges the matching counter in exactly one place (Claim), because the
    // counters and the flags used to be maintained by hand at four call sites.
    enum class Source { Emitter, Weather, Gameplay };

    // Per-particle render state: the size/colour interpolation over the
    // particle's age, plus the CPU light modulation. size <= 0 means "skip".
    //
    // ONE function, shared by the instanced and immediate draw paths. Written as
    // two copies of the same lerp it is exactly the shape of bug where the fast
    // path quietly renders a different colour or size from the fallback - the
    // fallback only runs when the shader failed to load, so the drift would be
    // invisible until then. tests/Surface.test.cpp pins that both paths call this
    // and neither re-derives the lerp inline.
    struct RenderState {
        float size = 0.0f;
        Color color{255, 255, 255, 0};
    };
    static RenderState ComputeRenderState(const Particle& p,
                                          const std::vector<const LightNode*>& lights);

    // --- Instanced GPU path (Particles.vs / Particles.fs) ---
    //
    // The immediate-mode path issued one DrawBillboard per particle - a texture
    // bind plus a quad per particle - which does not scale past a few hundred.
    // The instanced path expands one 6-vertex quad into N camera-facing quads in
    // a single draw, one draw per distinct source texture (the atlas is a
    // sampler, not a vertex attribute).
    //
    // Colour is still lit on the CPU (ApplyLighting) and uploaded per instance,
    // so this needs no light uniforms and cannot drift from the immediate path.
    //
    // ALL GL goes through rlgl's public API: rlLoadVertexBuffer allocates the
    // VBOs (it calls glGenBuffers/glBufferData internally), rlUpdateVertexBuffer
    // streams the per-frame instances, rlDrawVertexArrayInstanced issues the
    // draw, rlUnloadVertexBuffer frees them.
    //
    // Falls back to the immediate path if the shader or the GL objects fail to
    // initialise, so a missing shader - or a headless test target with no GL
    // context - never takes particles off screen.
    bool EnsureInstancedResources();
    void DrawInstanced(Camera3D& camera, const std::vector<const LightNode*>& lights,
                       bool ambient);

    // Modulate a particle colour by the light reaching it. Immediate-mode
    // DrawBillboard cannot carry a custom shader, so lighting is sampled on the
    // CPU from the same LightNode data the lit mesh shader uses.
    static Color ApplyLighting(Color c, Vector3 pos,
                               const std::vector<const struct LightNode*>& lights);

    Texture2D TextureFor(const std::string& path);
    Texture2D DefaultTexture();
    void SpawnFromEmitter(const struct ParticleEmitterNode& e, int count);

    // Claim a free pool slot for `src`, or nullptr if that source is at its
    // budget. Gameplay claims may evict the oldest ambient particle when the
    // pool is genuinely full -- see the implementation for why that is a
    // guarantee rather than a nicety.
    Particle* Acquire(Source src);
    // Attach a claimed slot to a source: the ONE place the tags and the
    // per-source counters are updated, so they cannot disagree.
    Particle* Claim(Particle& p, Source src);
    // Retire a live particle: the ONE place the counters are released.
    void Retire(Particle& p);

    // Ambient weather state. `type` matches ParticleTypeNameToIndex()
    // (OzoneParser.cpp): 0 none, 1 snow, 2 rain, 3 void, 4 psychic.
    //
    // The cap is a fraction of MAX_PARTICLES on purpose - see the share-out
    // comment next to MAX_PARTICLES.
    struct Weather {
        int type = 0;
        float density = 0.0f;      // spawns per second
        float speed = 1.0f;
        Color color{255, 255, 255, 255};
        float windX = 0.0f, windZ = 0.0f;
        float accum = 0.0f;
        int live = 0;              // live particles attributed to weather
    };
    Weather m_weather;

    std::vector<Particle> m_pool;
    int m_next = 0;
    int m_live = 0;
    int m_emitterLive = 0;           // authored-emitter particles only (not weather, not gameplay)
    Texture2D m_defaultTex{0};
    std::unordered_map<std::string, Texture2D> m_texCache;
    std::unordered_map<uint32_t, float> m_spawnAccum;
    bool m_enabled = true;
    int  m_drawBudget = 0;   // 0 = unlimited; period render profile sets a cap

    // Instanced renderer state. Zero ids mean "not initialised".
    unsigned int m_quadVao = 0;      // VAO combining the quad + instance attributes
    unsigned int m_quadVbo = 0;      // static 6-vertex unit quad
    unsigned int m_instVbo = 0;      // dynamic per-instance stream
    Shader m_instShader{0};
    int m_locMVP = -1, m_locCamRight = -1, m_locCamUp = -1;
    int m_locTexUnit = -1;           // `texture0` sampler: must be the INT 0 (see EnsureInstancedResources)
    bool m_instTried = false;        // one attempt, so a missing shader does not re-log per frame

    // Per-frame instance scratch, reused to avoid allocating in the draw path.
    std::vector<float> m_instData;   // centre(3) + size(1) + colour(4) = 8 floats
};
