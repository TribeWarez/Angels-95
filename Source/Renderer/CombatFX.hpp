#pragma once

// ---------------------------------------------------------------------------
// CombatFX — transient combat visuals: muzzle flashes, impact bursts,
// lingering tracers and surface decals (bullet holes).
//
// Header-only singleton, updated + drawn inside DrawWorld()'s BeginMode3D
// pass. Decals are REAL surface decals: a quad built in the hit surface's own
// plane (not a camera-facing billboard) so it lies flat on the brush, drawn
// alpha-blended with the depth mask off so overlapping splats merge instead of
// painting over one another. A procedural scorch texture is generated on Init
// so no GameData asset is required.
//
// Usage:
//   CombatFX::Instance().ArmMuzzleFlash(origin);          // on weapon fire
//   CombatFX::Instance().SpawnImpact(hitPos, normal, ..); // on hit
//   CombatFX::Instance().AddDecal(hitPos, normal);        // wall hit
//   // per frame inside BeginMode3D:
//   CombatFX::Instance().Update(dt);
//   CombatFX::Instance().Draw3D(camera);
// ---------------------------------------------------------------------------

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"      // rlBegin/rlSetTexture/rlDisableDepthMask for surface decals
#include "LitLightning.hpp"
#include "../Particle/OzParticleSimulationManager.hpp"
#include "../Package/PackageAssetLoader.hpp"
#include "../Log.hpp"
#include <vector>

// Swept segment vs AABB slab test.
// Returns true if the segment p0->p1 intersects box; outputs hit distance as
// a parametric t in [0,1] along the segment and the entry face normal.
// Segments starting fully inside report no hit (axis < 0).
inline bool SegmentVsAABB(Vector3 p0, Vector3 p1, const BoundingBox& box,
                          float& outT, Vector3& outNormal) {
    float d[3] = {p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
    float p[3] = {p0.x, p0.y, p0.z};
    float bmin[3] = {box.min.x, box.min.y, box.min.z};
    float bmax[3] = {box.max.x, box.max.y, box.max.z};

    float tmin = 0.0f, tmax = 1.0f;
    int hitAxis = -1;
    float hitSign = 0.0f;

    for (int i = 0; i < 3; i++) {
        if (fabsf(d[i]) < 1e-8f) {
            if (p[i] < bmin[i] || p[i] > bmax[i]) return false;
        } else {
            float inv = 1.0f / d[i];
            float t1 = (bmin[i] - p[i]) * inv;
            float t2 = (bmax[i] - p[i]) * inv;
            float sign = -1.0f;
            if (t1 > t2) {
                float tt = t1; t1 = t2; t2 = tt;
                sign = 1.0f;
            }
            if (t1 > tmin) { tmin = t1; hitAxis = i; hitSign = sign; }
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return false;
        }
    }

    if (hitAxis < 0) return false;
    outT = tmin;
    outNormal = {0.0f, 0.0f, 0.0f};
    if (hitAxis == 0) outNormal.x = hitSign;
    else if (hitAxis == 1) outNormal.y = hitSign;
    else outNormal.z = hitSign;
    return true;
}

class CombatFX {
public:
    static CombatFX& Instance() {
        static CombatFX fx;
        return fx;
    }

    void Init() {
        if (m_decalTex.id != 0) return;

        Image img = GenImageColor(64, 64, BLANK);
        Color* px = (Color*)img.data;
        for (int y = 0; y < 64; y++) {
            for (int x = 0; x < 64; x++) {
                float dx = (x - 31.5f) / 31.5f;
                float dy = (y - 31.5f) / 31.5f;
                float r = sqrtf(dx * dx + dy * dy);
                float alpha = 0.0f, bright = 30.0f;
                if (r < 0.55f) {
                    alpha = 225.0f;
                    bright = 25.0f + 45.0f * (r / 0.55f);
                } else if (r < 0.82f) {
                    float f = 1.0f - (r - 0.55f) / 0.27f;
                    alpha = 225.0f * f;
                    bright = 70.0f + 40.0f * f;
                }
                unsigned char b = (unsigned char)bright;
                px[y * 64 + x] = {b, b, b, (unsigned char)alpha};
            }
        }
        m_decalTex = LoadTextureFromImage(img);
        UnloadImage(img);

        // A separate soft glow for the muzzle flash. The decal texture has a
        // dark core (it is a scorch mark); a flash wants a bright centre.
        Image glow = GenImageColor(64, 64, BLANK);
        Color* gp = (Color*)glow.data;
        for (int y = 0; y < 64; y++) {
            for (int x = 0; x < 64; x++) {
                float dx = (x - 31.5f) / 31.5f;
                float dy = (y - 31.5f) / 31.5f;
                float r = sqrtf(dx * dx + dy * dy);
                if (r > 1.0f) r = 1.0f;
                float a = 1.0f - r;
                a = a * a;   // tighter falloff, brighter core
                gp[y * 64 + x] = {255, 255, 255, (unsigned char)(255.0f * a)};
            }
        }
        m_glowTex = LoadTextureFromImage(glow);
        UnloadImage(glow);

        // Blood splat decals. The authored art is JPG (no alpha) painted on a
        // black background; raylib's decoder rejects the shipped JPGs, so they
        // are converted to PNG (tools/convert_gibs.ps1) and loaded here.
        // LoadBloodTexture derives alpha from brightness either way, so a decal
        // does not draw as an opaque black square.
        static const char* kBlood[] = {
            "GameData/Tex/Global/Gibs/blood_1.png",
            "GameData/Tex/Global/Gibs/blood_2.png",
            "GameData/Tex/Global/Gibs/blood_3.png",
        };
        for (const char* p : kBlood) {
            Texture2D t = LoadBloodTexture(p);
            if (t.id > 0) m_bloodTex[m_bloodCount++] = t;
        }
        if (m_bloodCount == 0)
            OZ_WARN("CombatFX: no blood decal textures loaded from GameData/Tex/Global/Gibs");

        m_flashes.reserve(8);
        m_decals.resize(kMaxDecals);
    }

    void Shutdown() {
        if (m_decalTex.id != 0) { UnloadTexture(m_decalTex); m_decalTex = {0}; }
        if (m_glowTex.id != 0)  { UnloadTexture(m_glowTex);  m_glowTex = {0}; }
        for (int i = 0; i < m_bloodCount; i++) {
            if (m_bloodTex[i].id != 0) UnloadTexture(m_bloodTex[i]);
            m_bloodTex[i] = Texture2D{0};
        }
        m_bloodCount = 0;
        m_flashes.clear();
        m_tracers.clear();
        m_decals.clear();
    }

    void ClearAll() {
        m_flashes.clear();
        m_tracers.clear();
        m_nextDecal = 0;
        for (auto& d : m_decals) d.age = -1.0f;
        // Transient lights belong to the world that fired them; carrying one
        // across a map change would leave a glow in empty space.
        LitLightning_ClearTransientLights();
    }

    void ArmMuzzleFlash(Vector3 pos, float duration = 0.12f,
                        Color color = {255, 200, 120, 255}) {
        // Reject a non-positive duration at ARM time, not at draw time. Draw3D
        // computes `f.timer / f.duration`; a zero duration there is 0/0 -> NaN,
        // which feeds a NaN radius into DrawSphere and a NaN into the colour
        // channels. ArmTransientLight, two functions down, already guarded this;
        // ArmMuzzleFlash did not.
        if (duration <= 0.0f) return;
        EnsureInit();
        Flash f{pos, duration, duration, color};
        if (m_flashes.size() >= 8) m_flashes.erase(m_flashes.begin());
        m_flashes.push_back(f);
        // Also cast real light. The visible flash was an unlit DrawSphere, so it
        // ignored the room entirely: a shot in an unlit corridor lit nothing.
        // Transient lights take priority over world lights in the shader budget,
        // so this cannot be evicted by a busy level.
        ArmTransientLight(pos, color, duration * 1.4f, 7.0f);
    }

    // A soft additive glow drawn AT a world position right now, for something
    // that moves (a spell projectile's core). The flash list is deliberately not
    // used: a flash is a one-shot at a fixed point, so a moving round would trail
    // a string of them behind it. Must be called inside the 3D pass; a missing
    // glow texture (headless / Init failed) is a no-op rather than a crash.
    void DrawGlow(const Camera3D& camera, Vector3 pos, Color color, float size) {
        if (m_glowTex.id == 0 || size <= 0.0f) return;
        BeginBlendMode(BLEND_ADDITIVE);
        DrawBillboard(camera, m_glowTex, pos, size, color);
        EndBlendMode();
    }

    // Add a short-lived effect light to the transient pool. Reuses an inactive
    // slot when one is available, otherwise drops the request (MAX_TRANSIENT_LIGHTS).
    void ArmTransientLight(Vector3 pos, Color color, float duration, float radius) {
        if (duration <= 0.0f) return;
        auto& pool = LitLightning_TransientLights();
        for (auto& n : pool) {
            if (n.active) continue;
            n.active = true;
            n.type = LitLightType::POINT;
            n.position = pos;
            n.target = pos;
            n.color = color;
            n.intensity = 1.0f;
            n.radius = radius;
            n.isStatic = false;
            n.effect = LitLightEffect::NONE;
            n.timer = duration;
            return;
        }
        OZ_WARN("CombatFX: transient light pool full, dropping light at %.2f,%.2f,%.2f",
                pos.x, pos.y, pos.z);
    }

    // A tracer streak. Deliberately does not call EnsureInit(): tracers are drawn
    // with DrawLine3D and need no texture, and every projectile step emits one,
    // so this must stay allocation-and-GL-free. Decals are what need Init().
    //
    // Capped because Update() is only reached from the draw path: a frame that
    // ticks projectiles without drawing (tests, a paused world) would otherwise
    // grow this vector without bound.
    void AddTracer(Vector3 start, Vector3 end, Color color, float life = 0.05f) {
        if (life <= 0.0f) return;
        if (m_tracers.size() >= kMaxTracers) m_tracers.erase(m_tracers.begin());
        Tracer t{start, end, color, life, life};
        m_tracers.push_back(t);
    }

    void SpawnImpact(Vector3 pos, Vector3 normal, Color color,
                     int count = 10, float speed = 6.0f) {
        // Sparks go through the shared particle pool instead of a private list.
        // They used to be drawn here with DrawBillboard, which is unlit, so a
        // spark burst lit by a muzzle flash or a torch just glowed at full
        // brightness. Pool particles are lit (see
        // OzParticleSimulationManager::ApplyLighting) and fade out on their own.
        Color end = Fade(color, 0.0f);
        end.r = (unsigned char)(end.r / 2);
        end.g = (unsigned char)(end.g / 3);
        end.b = (unsigned char)(end.b / 3);
        OzParticleSimulationManager::Instance().Burst(
            pos, normal, count, speed, /*spread*/ 55.0f,
            color, end,
            /*sizeStart*/ 0.07f, /*sizeEnd*/ 0.015f,
            /*lifetime*/ 0.30f, /*gravity*/ -9.0f);
    }

    // tex {0} means the procedural scorch decal (bullet holes); pass a blood
    // texture for gore. Deliberately does NOT call EnsureInit: AddDecal runs on
    // gameplay/update paths (projectile hit, death) that a headless test also
    // drives, and uploading textures there needs a GL context. The decal array
    // is CPU-only, so it is sized here and the textures are loaded in Draw3D.
    void AddDecal(Vector3 pos, Vector3 normal, float size = 0.35f,
                  Color color = WHITE, Texture2D tex = {0}) {
        if (m_decals.empty()) m_decals.resize(kMaxDecals);
        Decal& d = m_decals[m_nextDecal];
        m_nextDecal = (m_nextDecal + 1) % m_decals.size();
        d.pos = Vector3Add(pos, Vector3Scale(normal, 0.03f));
        d.normal = normal;
        d.size = size;
        d.color = color;
        d.tex = tex;
        d.age = 0.0f;
    }

    // --- Gore ---------------------------------------------------------------
    // Blood on a non-fatal hit and gibs on death. Both go through the shared
    // particle pool (lit + faded) so they sit in the same lighting as the room,
    // plus a blood-splat decal so a hit leaves a mark. Gated by the caller on
    // the "Gore" setting (GameplayFlags.hpp) - this only draws when asked.
    void SpawnBlood(Vector3 pos, Vector3 dir, int count = 16, float scale = 1.0f) {
        // The particles use the authored blood art directly (white tint so the
        // texture's own colour shows) - a soft grey disc tinted dark red was
        // nearly invisible against a dark scene.
        Texture2D tex = RandomBloodTex();
        Color start = {255, 255, 255, 255};
        Color end = {170, 0, 0, 0};
        OzParticleSimulationManager::Instance().Burst(
            pos, dir, count, 6.0f * scale, /*spread*/ 80.0f,
            start, end,
            /*sizeStart*/ 0.35f * scale, /*sizeEnd*/ 0.12f * scale,
            /*lifetime*/ 0.7f, /*gravity*/ -10.0f, tex);
        // A splat stuck to the surface that was hit, facing the shooter.
        AddDecal(pos, dir, 0.55f * scale, WHITE, tex);
    }

    void SpawnGibs(Vector3 pos) {
        // A bigger, faster, longer-lived burst plus a splat on the ground under
        // the kill so the death reads even after the particles fade.
        Texture2D tex = RandomBloodTex();
        Color start = {255, 255, 255, 255};
        Color end = {150, 0, 0, 0};
        OzParticleSimulationManager::Instance().Burst(
            pos, {0.0f, 1.0f, 0.0f}, 30, 9.0f, /*spread*/ 90.0f,
            start, end, /*sizeStart*/ 0.5f, /*sizeEnd*/ 0.15f,
            /*lifetime*/ 1.0f, /*gravity*/ -12.0f, tex);
        // A ground splat below the body.
        AddDecal({pos.x, pos.y - 0.9f, pos.z}, {0.0f, 1.0f, 0.0f}, 1.3f,
                 WHITE, RandomBloodTex());
    }

    void Update(float dt) {
        // Age out transient effect lights; intensity ramps down with the timer so
        // a muzzle flash fades rather than popping off.
        for (auto& n : LitLightning_TransientLights()) {
            if (!n.active) continue;
            n.timer -= dt;
            if (n.timer <= 0.0f) {
                n.active = false;
            } else if (n.timer < 0.05f) {
                n.intensity = n.timer / 0.05f;
            }
        }
        for (size_t i = 0; i < m_flashes.size();) {
            m_flashes[i].timer -= dt;
            if (m_flashes[i].timer <= 0.0f)
                m_flashes.erase(m_flashes.begin() + i);
            else
                i++;
        }
        for (size_t i = 0; i < m_tracers.size();) {
            m_tracers[i].life -= dt;
            if (m_tracers[i].life <= 0.0f)
                m_tracers.erase(m_tracers.begin() + i);
            else
                i++;
        }
        for (auto& d : m_decals)
            if (d.age >= 0.0f) d.age += dt;
    }

    void Draw3D(Camera3D& camera) {
        EnsureInit();

        // Muzzle flash: an additive camera-facing glow, not a solid sphere. The
        // old DrawSphere read as a debug ball sitting on the muzzle every shot.
        if (!m_flashes.empty()) {
            BeginBlendMode(BLEND_ADDITIVE);
            for (const auto& f : m_flashes) {
                float t = f.timer / f.duration;   // 1 -> 0 over the flash
                Color c = {(unsigned char)(f.color.r > 0 ? f.color.r : 255),
                           (unsigned char)(f.color.g * (0.55f + 0.45f * t)),
                           (unsigned char)(f.color.b * t),
                           (unsigned char)(f.color.a * t)};
                DrawBillboard(camera, m_glowTex, f.pos, 0.55f + 0.35f * t, c);
            }
            EndBlendMode();
        }

        for (const auto& tr : m_tracers) {
            float a = tr.life / tr.maxLife;
            DrawLine3D(tr.a, tr.b, Fade(tr.color, a));
        }

        // Surface decals. These used to be DrawBillboardPro, which makes the
        // quad FACE THE CAMERA - so a splat on a wall turned to face the player
        // and its whole opaque square painted over every decal behind it instead
        // of lying on the brush. Build the quad in the decal's own plane (the
        // hit normal) and draw it through rlgl, alpha-blended with the depth
        // mask OFF: decals then merge on the surface (a second splat darkens the
        // first rather than replacing it) while the depth TEST still hides a
        // decal whose surface is behind other geometry.
        BeginBlendMode(BLEND_ALPHA);
        rlDisableDepthMask();
        for (const auto& d : m_decals) {
            if (d.age < 0.0f) continue;
            Texture2D tex = (d.tex.id > 0) ? d.tex : m_decalTex;
            if (tex.id == 0) continue;

            const Vector3 n = Vector3Normalize(d.normal);
            const Vector3 ref = (fabsf(n.y) > 0.9f) ? Vector3{0, 0, 1} : Vector3{0, 1, 0};
            const Vector3 t1 = Vector3Normalize(Vector3CrossProduct(ref, n));
            const Vector3 t2 = Vector3Normalize(Vector3CrossProduct(n, t1));
            const float h = d.size * 0.5f;
            const Vector3 p0 = Vector3Add(d.pos, Vector3Add(Vector3Scale(t1, -h), Vector3Scale(t2, -h)));
            const Vector3 p1 = Vector3Add(d.pos, Vector3Add(Vector3Scale(t1,  h), Vector3Scale(t2, -h)));
            const Vector3 p2 = Vector3Add(d.pos, Vector3Add(Vector3Scale(t1,  h), Vector3Scale(t2,  h)));
            const Vector3 p3 = Vector3Add(d.pos, Vector3Add(Vector3Scale(t1, -h), Vector3Scale(t2,  h)));

            rlSetTexture(tex.id);
            rlBegin(RL_QUADS);
                rlColor4ub(d.color.r, d.color.g, d.color.b, d.color.a);
                rlNormal3f(n.x, n.y, n.z);
                rlTexCoord2f(0.0f, 0.0f); rlVertex3f(p0.x, p0.y, p0.z);
                rlTexCoord2f(1.0f, 0.0f); rlVertex3f(p1.x, p1.y, p1.z);
                rlTexCoord2f(1.0f, 1.0f); rlVertex3f(p2.x, p2.y, p2.z);
                rlTexCoord2f(0.0f, 1.0f); rlVertex3f(p3.x, p3.y, p3.z);
            rlEnd();
            rlSetTexture(0);
        }
        rlEnableDepthMask();
        EndBlendMode();
    }

// Live tracer count. Exposed so the projectile tick's tracer emission can be
    // asserted without reaching into the private vector.
    size_t TracerCount() const { return m_tracers.size(); }

    // Live flash count. Same purpose as TracerCount: lets a headless test assert
    // that a zero-duration ArmMuzzleFlash is rejected and that Update() reaps.
    size_t FlashCount() const { return m_flashes.size(); }

private:
    struct Flash { Vector3 pos; float timer; float duration; Color color; };
    struct Decal { Vector3 pos{0, 0, 0}; Vector3 normal{0, 1, 0}; float size = 0.35f;
                   Color color{255, 255, 255, 255}; Texture2D tex{0}; float age = -1.0f; };
    struct Tracer { Vector3 a, b; Color color; float life, maxLife; };

    // Load a blood splat JPG and derive alpha from brightness (the art has a
    // black background and no alpha channel). Package-aware.
    static Texture2D LoadBloodTexture(const char* path) {
        Image img = LoadImageWithFallback(path);
        if (img.data == nullptr || img.width <= 0 || img.height <= 0)
            return Texture2D{0};
        ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        Color* px = (Color*)img.data;
        const int n = img.width * img.height;
        for (int i = 0; i < n; i++) {
            unsigned char a = px[i].r;
            if (px[i].g > a) a = px[i].g;
            if (px[i].b > a) a = px[i].b;
            // The dense centre of the splat is dark red (low brightness), so a
            // raw brightness alpha leaves it half-transparent. Boost so the
            // splat is solid where the art is dense.
            int aa = (int)a * 3 / 2;
            if (aa > 255) aa = 255;
            px[i].a = (unsigned char)aa;
        }
        Texture2D t = LoadTextureFromImage(img);
        UnloadImage(img);
        return t;
    }

    Texture2D RandomBloodTex() const {
        if (m_bloodCount <= 0) return Texture2D{0};
        return m_bloodTex[GetRandomValue(0, m_bloodCount - 1)];
    }

    bool EnsureInit() {
        if (m_decalTex.id == 0) Init();
        return m_decalTex.id != 0;
    }

    static constexpr size_t kMaxDecals = 64;
    static constexpr size_t kMaxTracers = 256;
    static constexpr int kMaxBloodTex = 4;

    std::vector<Flash> m_flashes;
    std::vector<Tracer> m_tracers;
    std::vector<Decal> m_decals;
    size_t m_nextDecal = 0;

    Texture2D m_decalTex{0};
    Texture2D m_glowTex{0};
    Texture2D m_bloodTex[kMaxBloodTex]{};
    int m_bloodCount = 0;
};
