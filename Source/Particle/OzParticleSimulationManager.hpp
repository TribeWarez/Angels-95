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
};
