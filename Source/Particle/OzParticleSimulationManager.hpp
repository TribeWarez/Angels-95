#pragma once
#include "raylib.h"
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

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

    // Draw all live particles in the 3D pass. `transientLights` (optional) are
    // effect lights that should be sampled before world lights; pass
    // LitLightning_TransientLights().
    void Draw(Camera3D& camera, const std::vector<struct LightNode>* transientLights = nullptr);

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

    int LiveCount() const { return m_live; }

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
    };

    static constexpr int MAX_PARTICLES = 4096;

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
    void DrawInstanced(Camera3D& camera, const std::vector<const LightNode*>& lights);

    // Modulate a particle colour by the light reaching it. Immediate-mode
    // DrawBillboard cannot carry a custom shader, so lighting is sampled on the
    // CPU from the same LightNode data the lit mesh shader uses.
    static Color ApplyLighting(Color c, Vector3 pos,
                               const std::vector<const struct LightNode*>& lights);

    Texture2D TextureFor(const std::string& path);
    Texture2D DefaultTexture();
    void SpawnFromEmitter(const struct ParticleEmitterNode& e, int count);

    std::vector<Particle> m_pool;
    int m_next = 0;
    int m_live = 0;
    Texture2D m_defaultTex{0};
    std::unordered_map<std::string, Texture2D> m_texCache;
    std::unordered_map<uint32_t, float> m_spawnAccum;

    // Instanced renderer state. Zero ids mean "not initialised".
    unsigned int m_quadVao = 0;      // VAO combining the quad + instance attributes
    unsigned int m_quadVbo = 0;      // static 6-vertex unit quad
    unsigned int m_instVbo = 0;      // dynamic per-instance stream
    Shader m_instShader{0};
    int m_locMVP = -1, m_locCamRight = -1, m_locCamUp = -1;
    bool m_instTried = false;        // one attempt, so a missing shader does not re-log per frame

    // Per-frame instance scratch, reused to avoid allocating in the draw path.
    std::vector<float> m_instData;   // centre(3) + size(1) + colour(4) = 8 floats
};
