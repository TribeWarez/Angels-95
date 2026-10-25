#pragma once
// ============================================================================
// Renderer/CombatFeedback.hpp
//
// The 2D feedback layer for combat, deliberately separate from CombatFX (which
// owns the 3D effects: impacts, blood, decals, tracers, gibs).
//
// Three questions the HUD could not answer before this existed:
//   * did my attack connect?            -> hit marker on the crosshair
//   * where did that damage come from?  -> arc on the screen edge
//   * am I about to die?                -> low-health edge pulse
//
// Header-only + inline for the same reason as Crosshair.hpp / CullState.hpp:
// a new .cpp means editing the root Makefile, AngelEd/Makefile AND ci.yml's
// inline g++ list, and this owns no assets or GL state.
//
// The timers are plain framebuffer-time fades, so every value is a pure
// function of the event + elapsed time: a capture at the same frame is
// reproducible, and nothing here allocates.
// ============================================================================
#include "raylib.h"
#include "raymath.h"
#include "../GameplayFlags.hpp"

namespace oz {

class CombatFeedback {
public:
    static CombatFeedback& Instance() {
        static CombatFeedback fx;
        return fx;
    }

    // The local player's attack damaged an NPC. `killed` tints the marker and
    // holds it slightly longer - the only "kill confirm" signal the client has
    // in single player (MP learns deaths from the server on a later packet).
    void ConfirmHit(bool killed = false) {
        m_hit = 1.0f;
        m_hitIsKill = killed;
        if (killed) m_kill = 1.0f;
    }

    // The local player took damage. `sourcePos` is the best-effort attacker
    // position (the PLAYER_HURT packet carries no attacker, so the caller passes
    // the nearest hostile). The arc is drawn relative to the camera's horizontal
    // forward, so it stays correct while the camera turns: the angle is captured
    // once, at the hit.
    void DamageFrom(Vector3 sourcePos, Vector3 camPos, Vector3 camForward) {
        Vector3 to = {sourcePos.x - camPos.x, 0.0f, sourcePos.z - camPos.z};
        Vector3 fwd = {camForward.x, 0.0f, camForward.z};
        if (Vector3LengthSqr(to) < 1e-4f || Vector3LengthSqr(fwd) < 1e-4f) return;
        to = Vector3Normalize(to);
        fwd = Vector3Normalize(fwd);
        // The camera's horizontal right is cross(forward, up) for a Y-up world:
        // with forward = -Z that is +X, so a source on the player's right gives
        // +PI/2 and the arc is drawn to the right of the crosshair.
        const Vector3 right = {-fwd.z, 0.0f, fwd.x};
        m_damageAngle = atan2f(Vector3DotProduct(to, right), Vector3DotProduct(to, fwd));
        m_damage = 1.0f;
    }

    void Update(float dt) {
        if (dt < 0.0f) dt = 0.0f;
        m_hit = fmaxf(0.0f, m_hit - dt / kHitSeconds);
        m_kill = fmaxf(0.0f, m_kill - dt / kKillSeconds);
        m_damage = fmaxf(0.0f, m_damage - dt / kDamageSeconds);
        m_lowPulse += dt;
        if (m_lowPulse > 100.0f) m_lowPulse = 0.0f;   // keep the phase bounded
    }

    // World/session reset: drop every transient so a level change cannot show
    // last level's damage arc.
    void Reset() {
        m_hit = m_kill = m_damage = 0.0f;
        m_hitIsKill = false;
        m_damageAngle = 0.0f;
        m_lowPulse = 0.0f;
    }

    // For tests/callers: are the transient cues live?
    bool HitActive() const { return m_hit > 0.0f; }
    bool DamageActive() const { return m_damage > 0.0f; }
    float DamageAngle() const { return m_damageAngle; }

    // `healthFrac` is remaining/max (1.0 when unknown). Call once per frame,
    // after the world pass and before/with the rest of the HUD.
    void Draw(float healthFrac) {
        DrawLowHealth(healthFrac);
        DrawDamageArc();
        DrawHitMarker();
    }

private:
    static constexpr float kHitSeconds = 0.22f;
    static constexpr float kKillSeconds = 0.40f;
    static constexpr float kDamageSeconds = 0.90f;
    static constexpr float kLowHealth = 0.30f;   // fraction that starts the pulse

    float m_hit = 0.0f;
    float m_kill = 0.0f;
    float m_damage = 0.0f;
    float m_damageAngle = 0.0f;   // radians, 0 = ahead, + = to the right
    float m_lowPulse = 0.0f;
    bool  m_hitIsKill = false;

    // Edge bands, additive, breathing. Deliberately weaker than the single-hit
    // DamageFlash so the two do not read as the same event: the flash is "just
    // took a hit", this is "you are nearly dead".
    void DrawLowHealth(float healthFrac) const {
        if (healthFrac <= 0.0f || healthFrac >= kLowHealth) return;
        const float severity = 1.0f - (healthFrac / kLowHealth);   // 0..1
        // 1 Hz breathing, or a steady band with motion comfort on. The pulse is
        // the only periodic element in this whole layer; the hit marker and the
        // damage arc are one-shot fades with no shake or scaling.
        const float pulse = g_reducedMotion ? 0.5f
                            : (0.5f + 0.5f * sinf(m_lowPulse * 6.2f));
        // Tuned against a 640x360 capture: the old 18..60 range was invisible on
        // a dark scene at the severity a player actually reaches (0.3-0.5).
        const unsigned char a = (unsigned char)((30.0f + 70.0f * pulse) * severity);
        if (a == 0) return;
        const int sw = GetScreenWidth(), sh = GetScreenHeight();
        const int band = (int)fmaxf(28.0f, sh * 0.11f);
        BeginBlendMode(BLEND_ADDITIVE);
        DrawRectangle(0, 0, sw, band, {110, 0, 0, a});
        DrawRectangle(0, sh - band, sw, band, {110, 0, 0, a});
        DrawRectangle(0, band, band, sh - 2 * band, {110, 0, 0, a});
        DrawRectangle(sw - band, band, band, sh - 2 * band, {110, 0, 0, a});
        EndBlendMode();
    }

    // A thick arc on a ring around the crosshair, at the bearing of the damage
    // source. Screen-up is camera-forward, so 0 rad points up.
    void DrawDamageArc() const {
        if (m_damage <= 0.0f) return;
        const int sw = GetScreenWidth(), sh = GetScreenHeight();
        const float cx = sw * 0.5f, cy = sh * 0.5f;
        const float radius = fminf(sw, sh) * 0.19f;
        const float halfSpan = 0.38f;
        const float thick = fmaxf(3.0f, sh * 0.009f);
        const unsigned char a = (unsigned char)(220.0f * m_damage);
        const Color col = {255, 70, 60, a};
        const float inner = radius - thick * 1.6f;
        const int segs = 10;
        BeginBlendMode(BLEND_ADDITIVE);
        for (int i = 0; i < segs; i++) {
            const float a0 = m_damageAngle - halfSpan + (2.0f * halfSpan) * ((float)i / segs);
            const float a1 = m_damageAngle - halfSpan + (2.0f * halfSpan) * ((float)(i + 1) / segs);
            DrawLineEx({cx + sinf(a0) * radius, cy - cosf(a0) * radius},
                       {cx + sinf(a1) * radius, cy - cosf(a1) * radius},
                       thick, col);
            DrawLineEx({cx + sinf(a0) * inner, cy - cosf(a0) * inner},
                       {cx + sinf(a1) * inner, cy - cosf(a1) * inner},
                       thick * 0.6f, col);
        }
        EndBlendMode();
    }

    // Four diagonal ticks around the crosshair centre. The arms sit at 45
    // degrees so they never collide with a cross/circle crosshair's own ticks.
    void DrawHitMarker() const {
        if (m_hit <= 0.0f) return;
        const int sw = GetScreenWidth(), sh = GetScreenHeight();
        const float cx = sw * 0.5f, cy = sh * 0.5f;
        // Kill confirm holds a slightly wider marker.
        const float grow = m_hitIsKill ? (1.0f + 0.55f * (1.0f - m_hit)) : 1.0f;
        const float r0 = fmaxf(4.0f, sh * 0.008f) * grow;
        const float r1 = fmaxf(11.0f, sh * 0.020f) * grow;
        const float thick = fmaxf(1.5f, sh * 0.0028f);
        const unsigned char a = (unsigned char)(235.0f * m_hit);
        const Color col = m_hitIsKill ? Color{255, 190, 60, a} : Color{255, 255, 255, a};
        BeginBlendMode(BLEND_ADDITIVE);
        for (int sx = -1; sx <= 1; sx += 2) {
            for (int sy = -1; sy <= 1; sy += 2) {
                const float inv = 0.7071f;
                DrawLineEx({cx + sx * r0 * inv, cy + sy * r0 * inv},
                           {cx + sx * r1 * inv, cy + sy * r1 * inv}, thick, col);
            }
        }
        EndBlendMode();
    }
};

} // namespace oz
