#include "OzParticleSimulationManager.hpp"
#include "../Pawn/OzPawnSystem.hpp"
#include "../Package/PackageAssetLoader.hpp"
#include "../Renderer/CullState.hpp"
#include "raymath.h"
#include <rlgl.h>
#include <cmath>
#include <algorithm>

namespace {

// Small self-contained PRNG so the smoke look is stable and independent of
// raylib's global RNG state.
uint32_t g_rng = 0x9E3779B9u;
float FRand() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return (float)(g_rng & 0xFFFFFFu) / (float)0xFFFFFFu;
}
float FRandSym() { return FRand() * 2.0f - 1.0f; }

} // namespace

OzParticleSimulationManager& OzParticleSimulationManager::Instance() {
    // Intentionally leaked: destroying particle textures at static-destruction
    // time would run after CloseWindow() tore down the GL context.
    static OzParticleSimulationManager* instance = new OzParticleSimulationManager();
    return *instance;
}

Texture2D OzParticleSimulationManager::DefaultTexture() {
    if (m_defaultTex.id != 0) return m_defaultTex;

    const int S = 32;
    Image img = GenImageColor(S, S, BLANK);
    for (int y = 0; y < S; y++) {
        for (int x = 0; x < S; x++) {
            float dx = (x + 0.5f - S * 0.5f) / (S * 0.5f);
            float dy = (y + 0.5f - S * 0.5f) / (S * 0.5f);
            float d = sqrtf(dx * dx + dy * dy);
            float a = 1.0f - d;
            if (a < 0.0f) a = 0.0f;
            a *= a; // soft radial falloff
            ImageDrawPixel(&img, x, y, (Color){255, 255, 255, (unsigned char)(a * 255.0f)});
        }
    }
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    m_defaultTex = LoadTextureFromImage(img);
    UnloadImage(img);
    if (m_defaultTex.id > 0) SetTextureFilter(m_defaultTex, TEXTURE_FILTER_BILINEAR);
    return m_defaultTex;
}

Texture2D OzParticleSimulationManager::TextureFor(const std::string& path) {
    if (path.empty()) return DefaultTexture();
    auto it = m_texCache.find(path);
    if (it != m_texCache.end()) return it->second;
    Texture2D t = LoadTextureWithFallback(path.c_str());
    if (t.id > 0) SetTextureFilter(t, TEXTURE_FILTER_POINT); // PS1 look
    m_texCache[path] = t;
    if (t.id == 0) return DefaultTexture();
    return t;
}

void OzParticleSimulationManager::SpawnFromEmitter(const ParticleEmitterNode& e, int count) {
    if ((int)m_pool.size() != MAX_PARTICLES) m_pool.resize(MAX_PARTICLES);

    Texture2D tex = TextureFor(e.texturePath);
    float yawRad = e.yaw * DEG2RAD;
    float cy = cosf(yawRad), sy = sinf(yawRad);
    // Rotate the base direction around +Y.
    Vector3 base = {e.direction.x * cy + e.direction.z * sy,
                    e.direction.y,
                   -e.direction.x * sy + e.direction.z * cy};
    float bl = sqrtf(base.x * base.x + base.y * base.y + base.z * base.z);
    if (bl > 1e-4f) { base.x /= bl; base.y /= bl; base.z /= bl; }
    else base = {0.0f, 1.0f, 0.0f};

    for (int i = 0; i < count; i++) {
        Particle* p = nullptr;
        for (int tries = 0; tries < MAX_PARTICLES; tries++) {
            Particle& cand = m_pool[m_next];
            m_next = (m_next + 1) % MAX_PARTICLES;
            if (!cand.active) { p = &cand; break; }
        }
        if (!p) return; // pool exhausted

        Vector3 dir = {base.x + FRandSym() * e.spread,
                       base.y + FRandSym() * e.spread,
                       base.z + FRandSym() * e.spread};
        float dl = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
        if (dl < 1e-4f) dl = 1.0f;
        dir.x /= dl; dir.y /= dl; dir.z /= dl;
        float spd = e.speed * (0.7f + 0.6f * FRand());

        p->position = {e.position.x + FRandSym() * e.radius,
                       e.position.y + FRandSym() * e.radius,
                       e.position.z + FRandSym() * e.radius};
        p->velocity = {dir.x * spd, dir.y * spd, dir.z * spd};
        p->age = 0.0f;
        p->lifetime = e.lifetime * (0.8f + 0.4f * FRand());
        if (p->lifetime < 0.05f) p->lifetime = 0.05f;
        p->sizeStart = e.sizeStart;
        p->sizeEnd = e.sizeEnd;
        p->colorStart = e.colorStart;
        p->colorEnd = e.colorEnd;
        p->gravity = e.gravity;
        p->billboard = e.billboard;
        p->texture = tex;
        p->active = true;
        m_live++;
    }
}

void OzParticleSimulationManager::Update(float dt) {
    if (dt <= 0.0f) return;
    if ((int)m_pool.size() != MAX_PARTICLES) m_pool.resize(MAX_PARTICLES);

    // Spawn (read-only snapshot of the emitter definitions).
    for (auto& e : PawnSystem::Instance().GetParticleEmitters()) {
        if (!e.active || e.rate <= 0.0f) continue;
        float& acc = m_spawnAccum[e.id];
        acc += e.rate * dt;
        int count = (int)acc;
        if (count > 128) count = 128;
        acc -= (float)count;
        if (count > 0) SpawnFromEmitter(e, count);
    }

    // Integrate / retire.
    for (auto& p : m_pool) {
        if (!p.active) continue;
        p.age += dt;
        if (p.age >= p.lifetime) {
            p.active = false;
            m_live--;
            continue;
        }
        p.velocity.y += p.gravity * dt;
        p.position.x += p.velocity.x * dt;
        p.position.y += p.velocity.y * dt;
        p.position.z += p.velocity.z * dt;
    }
    if (m_live < 0) m_live = 0;
}

// The single source of truth for how a particle looks this frame. Both the
// instanced path and the immediate-mode fallback call this, so they cannot
// disagree about a particle's size or lit colour - a fast path that renders
// differently from its own fallback is invisible until the fallback triggers.
OzParticleSimulationManager::RenderState
OzParticleSimulationManager::ComputeRenderState(const Particle& p,
                                               const std::vector<const LightNode*>& lights) {
    RenderState r;
    float t = (p.lifetime > 0.0f) ? (p.age / p.lifetime) : 1.0f;
    if (t > 1.0f) t = 1.0f;
    if (t < 0.0f) t = 0.0f;
    r.size = p.sizeStart + (p.sizeEnd - p.sizeStart) * t;
    if (r.size <= 0.001f) return r;
    Color c = {
        (unsigned char)(p.colorStart.r + (p.colorEnd.r - (int)p.colorStart.r) * t),
        (unsigned char)(p.colorStart.g + (p.colorEnd.g - (int)p.colorStart.g) * t),
        (unsigned char)(p.colorStart.b + (p.colorEnd.b - (int)p.colorStart.b) * t),
        (unsigned char)(p.colorStart.a + (p.colorEnd.a - (int)p.colorStart.a) * t)
    };
    r.color = ApplyLighting(c, p.position, lights);
    return r;
}

// ---------------------------------------------------------------------------
// Instanced GPU path (Particles.vs / Particles.fs)
// ---------------------------------------------------------------------------
bool OzParticleSimulationManager::EnsureInstancedResources() {
    if (m_quadVao != 0) return true;
    if (m_instTried) return false;   // already failed once; do not retry per frame
    m_instTried = true;

    // Instancing is CORE from OpenGL 3.0, so a 3.3/4.3/ES3 build always has
    // glDrawArraysInstanced bound by GLAD.
    //
    // This gate is not paranoia. rlDrawVertexArrayInstanced() does NOT check
    // RLGL.ExtSupported.instancing before calling through the function pointer
    // (rlgl.h:4095), so on a legacy GL2.1/ES2 build the pointer is NULL and the
    // call is a crash, not a no-op. rlgl only fills those pointers from the
    // extension list in rlLoadExtensions(), which is the GL 2.1 path.
    //
    // We deliberately do NOT consult RLGL.ExtSupported.instancing to decide this:
    // rlgl.h:1147 declares `static rlglData RLGL`, so every translation unit
    // that includes rlgl.h without RLGL_IMPLEMENTATION gets its OWN zeroed copy.
    // Reading it from here would always report "unsupported" and silently
    // disable instancing forever. rlGetVersion() is a real exported function
    // that returns a build-time constant, which is exactly the decision we want.
    if (rlGetVersion() < RL_OPENGL_33) {
        OZ_WARN("Particles: instancing needs OpenGL 3.3+ - using immediate-mode path");
        return false;
    }

    m_instShader = LoadShaderWithFallback("GameData/Shaders/Particles.vs",
                                          "GameData/Shaders/Particles.fs");
    if (m_instShader.id == 0) {
        OZ_WARN("Particles: instanced shader failed to load - using immediate-mode path");
        return false;
    }

    m_locMVP      = GetShaderLocation(m_instShader, "mvp");   // raylib's default MVP name
    m_locCamRight = GetShaderLocation(m_instShader, "camRight");
    m_locCamUp    = GetShaderLocation(m_instShader, "camUp");

    // Static unit quad: two triangles, positions only (the UV is derived in the
    // vertex shader from the corner offset). Six vertices, so no index buffer.
    const float quad[] = {
        -0.5f, -0.5f, 0.0f,   0.5f, -0.5f, 0.0f,   0.5f, 0.5f, 0.0f,
        -0.5f, -0.5f, 0.0f,   0.5f,  0.5f, 0.0f,  -0.5f, 0.5f, 0.0f,
    };
    // Per-instance layout: centre (3) + size (1) + colour (4) = 8 floats.
    const int kInstStride = 8 * (int)sizeof(float);

    m_quadVbo = rlLoadVertexBuffer(quad, (int)sizeof(quad), false);
    // Sized for a full pool so the draw path never reallocates mid-frame.
    m_instVbo = rlLoadVertexBuffer(nullptr, kInstStride * MAX_PARTICLES, true);
    if (m_quadVbo == 0 || m_instVbo == 0) {
        OZ_WARN("Particles: instanced VBO alloc failed - using immediate-mode path");
        return false;
    }

    m_quadVao = rlLoadVertexArray();
    if (m_quadVao == 0 || !rlEnableVertexArray(m_quadVao)) {
        OZ_WARN("Particles: VAO alloc failed - using immediate-mode path");
        return false;
    }

    // Per-vertex corner, divisor 0.
    rlEnableVertexBuffer(m_quadVbo);
    rlEnableVertexAttribute(0);
    rlSetVertexAttribute(0, 3, RL_FLOAT, false, 3 * sizeof(float), 0);
    rlSetVertexAttributeDivisor(0, 0);

    // Per-instance: centre (3), size (1), colour (4) - divisor 1 on each.
    rlEnableVertexBuffer(m_instVbo);
    rlEnableVertexAttribute(11);
    rlSetVertexAttribute(11, 3, RL_FLOAT, false, kInstStride, 0);
    rlSetVertexAttributeDivisor(11, 1);
    rlEnableVertexAttribute(12);
    rlSetVertexAttribute(12, 1, RL_FLOAT, false, kInstStride, 3 * (int)sizeof(float));
    rlSetVertexAttributeDivisor(12, 1);
    rlEnableVertexAttribute(13);
    rlSetVertexAttribute(13, 4, RL_FLOAT, false, kInstStride, 4 * (int)sizeof(float));
    rlSetVertexAttributeDivisor(13, 1);

    rlDisableVertexArray();
    m_instData.reserve((size_t)kInstStride * MAX_PARTICLES / sizeof(float));
    return true;
}

void OzParticleSimulationManager::DrawInstanced(Camera3D& camera,
                                                const std::vector<const LightNode*>& lights) {
    if (!EnsureInstancedResources()) return;

    // Camera basis for billboarding. Camera3D carries position/target/up but no
    // right vector, so derive it: right = normalize(cross(up, forward)).
    const Vector3 fwd = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
    Vector3 right = Vector3CrossProduct(camera.up, fwd);
    if (Vector3Length(right) < 1e-4f) right = Vector3CrossProduct({0, 1, 0}, fwd);
    right = Vector3Normalize(right);
    const Vector3 up = Vector3Normalize(Vector3CrossProduct(fwd, right));

    // Visible textured billboards, sorted far-to-near so ALPHA blending
    // composites correctly. The immediate-mode path drew in pool order, which is
    // arbitrary, so ordering them is a straight correctness improvement too.
    std::vector<const Particle*> visible;
    visible.reserve((size_t)m_live);
    for (const auto& p : m_pool)
        if (p.active && p.billboard && p.texture.id > 0) visible.push_back(&p);
    if (visible.empty()) return;
    std::sort(visible.begin(), visible.end(),
              [&](const Particle* a, const Particle* b) {
                  return Vector3DistanceSqr(a->position, camera.position) >
                         Vector3DistanceSqr(b->position, camera.position);
              });

    // Walk the sorted list in per-texture runs. Each run is uploaded and drawn as
    // its OWN instanced draw starting at instance 0: rlDrawVertexArrayInstanced
    // has no instance-offset parameter (its first argument is a VERTEX offset),
    // so one concatenated buffer could not express "start this group at instance
    // N" without drawing the wrong particles. One upload per distinct texture,
    // and a scene normally has exactly one (the generated default sprite).
    BeginShaderMode(m_instShader);
    // raylib 5.5 has no GetModelViewProjectionMatrix(); GetCameraMatrix (raymath)
    // is the same view-projection the immediate path used, so both paths project
    // identically.
    SetShaderValueMatrix(m_instShader, m_locMVP, GetCameraMatrix(camera));
    if (m_locCamRight >= 0) SetShaderValue(m_instShader, m_locCamRight, &right, SHADER_UNIFORM_VEC3);
    if (m_locCamUp >= 0)    SetShaderValue(m_instShader, m_locCamUp, &up, SHADER_UNIFORM_VEC3);

    BeginBlendMode(BLEND_ALPHA);
    {
        // Camera-facing quads: backface culling is meaningless here and would drop
        // half of them on winding. Routed through the tracked setter so the
        // caller's state is restored (see Renderer/CullState.hpp).
        oz::ScopedCullOff cullOff;

        if (rlEnableVertexArray(m_quadVao)) {
            size_t i = 0;
            while (i < visible.size()) {
                const int texId = visible[i]->texture.id;
                m_instData.clear();
                size_t j = i;
                while (j < visible.size() && visible[j]->texture.id == texId) {
                    const RenderState rs = ComputeRenderState(*visible[j], lights);
                    if (rs.size > 0.001f) {
                        m_instData.push_back(visible[j]->position.x);
                        m_instData.push_back(visible[j]->position.y);
                        m_instData.push_back(visible[j]->position.z);
                        m_instData.push_back(rs.size);
                        m_instData.push_back(rs.color.r / 255.0f);
                        m_instData.push_back(rs.color.g / 255.0f);
                        m_instData.push_back(rs.color.b / 255.0f);
                        m_instData.push_back(rs.color.a / 255.0f);
                    }
                    j++;
                }
                const int count = (int)(m_instData.size() / 8);
                if (count > 0) {
                    rlSetTexture(texId);
                    rlUpdateVertexBuffer(m_instVbo, m_instData.data(),
                                         (int)(m_instData.size() * sizeof(float)), 0);
                    rlDrawVertexArrayInstanced(0, 6, count);
                }
                i = j;
            }
            // Unbind, restoring VAO 0. This is safe for every subsequent raylib
            // draw because each raylib path re-binds its own VAO before drawing
            // rather than relying on whatever was last bound: the 2D batch binds
            // batch->vertexBuffer[].vaoId in rlDrawRenderBatch (rlgl.h:2992),
            // and 3D meshes bind their own mesh.vaoId. rlgl itself unbinds to 0
            // the same way wherever it hands back to the driver.
            rlDisableVertexArray();
        }
    }
    EndBlendMode();
    EndShaderMode();
}

void OzParticleSimulationManager::Shutdown() {
    if (m_quadVao) { rlUnloadVertexArray(m_quadVao); m_quadVao = 0; }
    if (m_quadVbo) { rlUnloadVertexBuffer(m_quadVbo); m_quadVbo = 0; }
    if (m_instVbo) { rlUnloadVertexBuffer(m_instVbo); m_instVbo = 0; }
    if (m_instShader.id != 0) { UnloadShader(m_instShader); m_instShader = {0}; }
    m_instTried = false;
    m_instData.clear();
    m_instData.shrink_to_fit();
}

void OzParticleSimulationManager::Draw(Camera3D& camera,
                                       const std::vector<LightNode>* transientLights) {
    if (m_live <= 0) return;

    // Gather lights once per frame, transient first (matching the priority the
    // shader uses) so a muzzle flash visibly lights the smoke sitting in it.
    // Passed in by the caller rather than reaching for the global pool: the
    // transient pool lives in LitLightning, and depending on it here would drag
    // the whole lighting chain into every test target that links this file.
    std::vector<const LightNode*> lit;
    if (transientLights)
        for (const auto& n : *transientLights)
            if (n.active) lit.push_back(&n);
    for (const auto& n : PawnSystem::Instance().GetLights())
        if (n.active) lit.push_back(&n);

    // Instanced GPU path first: one draw call per texture instead of one quad per
    // particle. The immediate-mode loop below is the fallback, kept because
    // EnsureInstancedResources() can legitimately fail (a headless test target
    // has no GL context, and a missing shader must not take particles off
    // screen).
    //
    // Only textured billboards go through the instanced shader. Sphere particles
    // (billboard == false, or no texture) always take the immediate path, and
    // `instancedDrew` makes that loop skip the billboards the shader already
    // covered so nothing is drawn twice.
    bool instancedDrew = false;
    if (EnsureInstancedResources()) {
        bool anyBillboard = false;
        for (const auto& p : m_pool)
            if (p.active && p.billboard && p.texture.id > 0) { anyBillboard = true; break; }
        if (anyBillboard) {
            DrawInstanced(camera, lit);
            instancedDrew = true;
        }
    }

    BeginBlendMode(BLEND_ALPHA);
    for (auto& p : m_pool) {
        if (!p.active) continue;
        // Skip textured billboards when the instanced shader already drew them.
        if (instancedDrew && p.billboard && p.texture.id > 0) continue;
        const RenderState rs = ComputeRenderState(p, lit);
        if (rs.size <= 0.001f) continue;

        // billboard is now meaningful: a sphere regardless of texture. The
        // texture==0 fallback below is unchanged (that is how a Burst without a
        // texture renders), so this only adds a way to author solid particles
        // from an OZONE emitter.
        if (p.billboard && p.texture.id > 0)
            DrawBillboard(camera, p.texture, p.position, rs.size, rs.color);
        else
            DrawSphere(p.position, rs.size * 0.5f, rs.color);
    }
    EndBlendMode();
}

// Multiply a particle's colour by the light reaching it.
//
// Particles used to draw through raylib's immediate-mode DrawBillboard, which
// never touches the Lights uniform block, so they were completely unlit and
// floated at full brightness in dark rooms. Sampling the same LightNode data the
// lit mesh shader uses keeps particles consistent with the scene without
// routing them through the batcher (immediate mode cannot carry a custom shader).
//
// Alpha is preserved: lighting is a modulation of colour, not of coverage.
Color OzParticleSimulationManager::ApplyLighting(
        Color c, Vector3 pos, const std::vector<const LightNode*>& lights) {
    if (lights.empty()) return c;

    // Ambient floor so an unlit particle is still visible, matching the shader's
    // default ambient of 1.0.
    float lr = 1.0f, lg = 1.0f, lb = 1.0f;
    for (const LightNode* n : lights) {
        float dist = 0.0f;
        if (n->type == LitLightType::DIRECTIONAL) {
            // No positional falloff for the sun.
        } else {
            dist = Vector3Distance(pos, n->position);
            if (n->radius > 0.0f && dist > n->radius) continue;
        }
        float atten = (n->radius > 0.0f)
            ? std::max(0.0f, 1.0f - dist / n->radius)   // linear, cheap
            : 1.0f;
        if (atten <= 0.0f) continue;
        float k = atten * n->intensity;
        lr += (n->color.r / 255.0f) * k;
        lg += (n->color.g / 255.0f) * k;
        lb += (n->color.b / 255.0f) * k;
    }
    // Clamp so a bright light cannot blow the particle to flat white.
    c.r = (unsigned char)std::min(255.0f, c.r * lr);
    c.g = (unsigned char)std::min(255.0f, c.g * lg);
    c.b = (unsigned char)std::min(255.0f, c.b * lb);
    return c;
}

void OzParticleSimulationManager::Burst(Vector3 pos, Vector3 dir, int count,
                                       float speed, float spread,
                                       Color color, Color colorEnd,
                                       float sizeStart, float sizeEnd,
                                       float lifetime, float gravity,
                                       Texture2D tex) {
    if (count <= 0) return;
    if ((int)m_pool.size() != MAX_PARTICLES) m_pool.resize(MAX_PARTICLES);

    Vector3 base = dir;
    const float bl = sqrtf(base.x*base.x + base.y*base.y + base.z*base.z);
    if (bl > 1e-4f) { base.x /= bl; base.y /= bl; base.z /= bl; }
    else base = {0.0f, 1.0f, 0.0f};

    // Build an orthonormal basis around base so the spread cone is isotropic.
    Vector3 up = (std::fabs(base.y) > 0.99f) ? Vector3{1,0,0} : Vector3{0,1,0};
    Vector3 right = Vector3Normalize(Vector3CrossProduct(up, base));
    Vector3 realUp = Vector3CrossProduct(base, right);

    const float spreadRad = spread * DEG2RAD;
    for (int i = 0; i < count; i++) {
        Particle* p = nullptr;
        for (int tries = 0; tries < MAX_PARTICLES; tries++) {
            Particle& cand = m_pool[m_next];
            m_next = (m_next + 1) % MAX_PARTICLES;
            if (!cand.active) { p = &cand; break; }
        }
        if (!p) break;   // pool exhausted

        // Random point in the cone: random angle, random radial falloff
        // (sqrt keeps the distribution uniform over the disc). FRand (the
        // file-local PRNG) rather than rand(): Burst used to call rand(), so a
        // gameplay srand() reshuffled every muzzle flash and the two emitters
        // shared the global sequence with the rest of the process.
        const float ang = FRand() * 2.0f * PI;
        const float rad = spreadRad > 0.0f ? sqrtf(FRand()) * spreadRad : 0.0f;
        Vector3 v = base * cosf(rad)
                  + (right * cosf(ang) + realUp * sinf(ang)) * sinf(rad);
        v = Vector3Normalize(v);

        // Per-particle speed jitter so a burst does not read as one rigid cone.
        const float s = speed * (0.7f + 0.6f * FRand());

        p->active = true;
        p->position = pos;
        p->velocity = Vector3Scale(v, s);
        p->age = 0.0f;
        p->lifetime = lifetime * (0.7f + 0.6f * FRand());
        p->sizeStart = sizeStart;
        p->sizeEnd = sizeEnd;
        p->colorStart = color;
        p->colorEnd = colorEnd;
        p->gravity = gravity;
        // One-shot bursts are billboarded by default; a caller wanting solid
        // spheres passes tex == 0 and Draw already falls back to a sphere.
        p->billboard = true;
        // Left as 0 when no texture is given: Draw falls back to a sphere, so a
        // one-shot burst does not need a GL upload. Resolving the soft sprite
        // here would touch the GPU even on paths that never draw (tests).
        p->texture = tex;
        m_live++;
    }
}

void OzParticleSimulationManager::Clear() {
    for (auto& p : m_pool) p.active = false;
    m_live = 0;
    m_next = 0;
    m_spawnAccum.clear();
}
