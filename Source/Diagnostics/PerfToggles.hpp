#pragma once
// ---------------------------------------------------------------------------
// PerfToggles - runtime ablation switches.
//
// This is the fastest way to attribute a slow frame without reading a profile:
// turn one system off and watch the frame time move. They are deliberately
// NON-PERSISTED (a) so a diagnostic mode can never leak into a player's next
// launch and (b) so "why does my game look wrong" is never answered by a stale
// toggle. Overlay visibility is the one thing that persists, and it lives in the
// client/editor INI, not here.
//
// Header-only for the usual build-wiring reason (root Makefile + AngelEd/Makefile
// + ci.yml inline g++ list).
// ---------------------------------------------------------------------------

#include <cstring>

namespace oz::diag {

struct PerfToggles {
    bool lights   = true;   // light submission + shader light loops
    bool particles = true;  // emitter simulation + weather + draw
    bool props    = true;   // Mesh.* props and pickups
    bool pawns    = true;   // NPC models, billboards, CPU skinning
    bool fx       = true;   // CombatFX, projectiles, decals, tracers
    bool skinning = true;   // skeletal/animated pose application
    bool sky      = true;   // skybox cube + sky portal pre-pass
    bool portals  = true;   // SURF_PORTAL faces
    bool glow     = true;   // additive SURF_GLOW pass
    bool postfx   = true;   // bloom/grade/CRT chain
    bool overlays = true;   // editor overlays, gizmos, authoring billboards
    bool grid     = true;   // editor grid
    bool hud      = true;   // in-game HUD / hotbar
    bool cull     = true;   // frustum culling (OFF = draw every brush/prop)
};

inline PerfToggles& Toggles() {
    static PerfToggles toggles;
    return toggles;
}

inline const char* PerfSystemList() {
    return "lights,particles,props,pawns,fx,skinning,sky,portals,glow,postfx,overlays,grid,hud,cull";
}

inline bool PerfOn(const char* system) {
    if (!system) return true;
    PerfToggles& t = Toggles();
    if (std::strcmp(system, "lights")    == 0) return t.lights;
    if (std::strcmp(system, "particles") == 0) return t.particles;
    if (std::strcmp(system, "props")     == 0) return t.props;
    if (std::strcmp(system, "pawns")     == 0) return t.pawns;
    if (std::strcmp(system, "fx")        == 0) return t.fx;
    if (std::strcmp(system, "skinning")  == 0) return t.skinning;
    if (std::strcmp(system, "sky")       == 0) return t.sky;
    if (std::strcmp(system, "portals")   == 0) return t.portals;
    if (std::strcmp(system, "glow")      == 0) return t.glow;
    if (std::strcmp(system, "postfx")    == 0) return t.postfx;
    if (std::strcmp(system, "overlays")  == 0) return t.overlays;
    if (std::strcmp(system, "grid")      == 0) return t.grid;
    if (std::strcmp(system, "hud")       == 0) return t.hud;
    if (std::strcmp(system, "cull")      == 0) return t.cull;
    return true;   // unknown name: never disable something by accident
}

inline bool PerfSet(const char* system, bool on) {
    if (!system) return false;
    PerfToggles& t = Toggles();
    if (std::strcmp(system, "lights")    == 0) { t.lights = on; return true; }
    if (std::strcmp(system, "particles") == 0) { t.particles = on; return true; }
    if (std::strcmp(system, "props")     == 0) { t.props = on; return true; }
    if (std::strcmp(system, "pawns")     == 0) { t.pawns = on; return true; }
    if (std::strcmp(system, "fx")        == 0) { t.fx = on; return true; }
    if (std::strcmp(system, "skinning")  == 0) { t.skinning = on; return true; }
    if (std::strcmp(system, "sky")       == 0) { t.sky = on; return true; }
    if (std::strcmp(system, "portals")   == 0) { t.portals = on; return true; }
    if (std::strcmp(system, "glow")      == 0) { t.glow = on; return true; }
    if (std::strcmp(system, "postfx")    == 0) { t.postfx = on; return true; }
    if (std::strcmp(system, "overlays")  == 0) { t.overlays = on; return true; }
    if (std::strcmp(system, "grid")      == 0) { t.grid = on; return true; }
    if (std::strcmp(system, "hud")       == 0) { t.hud = on; return true; }
    if (std::strcmp(system, "cull")      == 0) { t.cull = on; return true; }
    return false;
}

inline bool PerfToggle(const char* system) {
    const bool next = !PerfOn(system);
    PerfSet(system, next);
    return next;
}

} // namespace oz::diag
