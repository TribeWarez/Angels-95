#pragma once
// ---------------------------------------------------------------------------
// Renderer/SkyPortal.hpp — the offscreen SKY VIEW that SURF_PORTAL faces sample.
//
// A portal face is drawn at its own world position and samples this texture in
// screen space (see GameData/Shaders/SkyPortal.fs). That is the whole mechanism:
// the face IS its own mask, so there is no stencil buffer, no portal recursion
// and no second world render. Depth testing against world geometry gives correct
// occlusion for free.
//
// The view is rendered from the SKY ZONE ANCHOR (`SkyZoneNode::position`) using
// the main camera's rotation and FOV. For a sky that is exactly right: it keeps
// the sky's orientation locked to the view (clouds must not swim when you
// strafe) while still letting a finite sky chamber be authored later.
//
// WHY THIS IS HEADER-ONLY: it must be includable from both the client and
// AngelEd without adding an object to the root Makefile, AngelEd/Makefile AND
// the inline g++ list in .github/workflows/ci.yml. Same reason Skybox.hpp,
// SkyMaterial.hpp and CullState.hpp are header-only.
//
// WHY IT IS HALF-RESOLUTION: the sky is smooth and is resampled 1:1 by screen
// position, so half resolution is visually indistinguishable and costs a
// quarter of the pixels. It is recreated only when the framebuffer size changes
// (the client recreates `Target` on resize / resolution-scale changes).
// ---------------------------------------------------------------------------

#include "raylib.h"
#include "Skybox.hpp"   // oz::Skybox, oz::SkyboxFog
#include <algorithm>

namespace oz {

class SkyPortal {
public:
    static SkyPortal& Instance() {
        static SkyPortal instance;
        return instance;
    }

    // Render the sky into the view. Call BEFORE BeginTextureMode(Target): raylib
    // cannot nest render textures (EndTextureMode rebinds the default
    // framebuffer, not the previous one), so this pass must own the target when
    // it runs.
    //
    // `vpW`/`vpH` are the dimensions of the framebuffer the faces will later be
    // drawn into. They set the view's aspect and its half-res size; the face
    // pass repeats them as the `uViewport` uniform.
    void Render(const Camera3D& mainCamera, Vector3 anchor,
                Texture2D capTex, Texture2D sideTex,
                const SkyboxFog* fog, int vpW, int vpH) {
        if (!IsWindowReady()) return;              // headless target: no GL
        if (capTex.id == 0 && sideTex.id == 0) return;

        EnsureSize(vpW, vpH);
        if (m_view.id == 0) return;

        // Same rotation/FOV, translated to the zone anchor. Skybox::Draw places
        // the cube itself (it takes a centre position), so the current 3D camera
        // only has to project it.
        Camera3D skyCam = mainCamera;
        skyCam.position = anchor;

        BeginTextureMode(m_view);
        ClearBackground(BLACK);
        BeginMode3D(skyCam);
        // cullBackfaces = false: the cube is drawn two-sided by design (see
        // Skybox::Draw - the faces are inward-facing and the history there
        // matters), and a portal must never lose a face to winding.
        Skybox::Instance().Draw(anchor, capTex, sideTex, /*cullBackfaces=*/false, fog);
        EndMode3D();
        EndTextureMode();
    }

    // The rendered view, for the face pass. Zero when nothing has been rendered.
    Texture2D View() const { return m_view.texture; }
    bool Ready() const { return m_view.id != 0; }

    // Release the GPU resources. Called once at shutdown while the GL context is
    // still alive; safe to call more than once.
    void Shutdown() {
        if (m_view.id > 0) {
            UnloadRenderTexture(m_view);
            m_view = RenderTexture2D{0};
        }
    }

private:
    SkyPortal() = default;

    void EnsureSize(int vpW, int vpH) {
        const int w = std::max(1, vpW / 2);
        const int h = std::max(1, vpH / 2);
        if (m_view.id > 0 &&
            m_view.texture.width == w && m_view.texture.height == h) return;
        if (m_view.id > 0) UnloadRenderTexture(m_view);
        m_view = LoadRenderTexture(w, h);
        if (m_view.id > 0) SetTextureFilter(m_view.texture, TEXTURE_FILTER_BILINEAR);
    }

    RenderTexture2D m_view{0};
};

} // namespace oz
