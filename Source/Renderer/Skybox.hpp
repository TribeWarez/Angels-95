#pragma once
#include "raylib.h"
#include "rlgl.h"
#include "CullState.hpp"
#include "SkyMaterial.hpp"

// ---------------------------------------------------------------------------
// oz::Skybox - the one skybox cube.
//
// WHY THIS EXISTS
// ---------------
// The client (Source/Core.hpp) and the editor (AngelEd/Source/Main.cpp) each
// built and drew their own skybox cube. They were near-identical and had drifted:
//
//   * face geometry: duplicated verbatim - GenMeshPlane(2000,2000,1,1) plus the
//     same texcoord flips for faces 0/2/5, LoadModelFromMesh, six times.
//   * distance: 700 in the client, a hardcoded 1000 in the editor. So the editor
//     viewport showed a sky at a different angular size than the game does.
//   * fog: the client went through SkyMaterial (as of the horizon-fog commit),
//     the editor drew through raylib's default material shader - so authoring a
//     view could not show the horizon fog the player would actually get.
//
// A skybox is not a place for two implementations to disagree. This is the one.
//
// NOTE ON Mesh / Model INSIDE THIS FILE
// ------------------------------------
// These functions are inside namespace oz, where oz::Mesh (the engine's
// animated/skeletal mesh) hides raylib's ::Mesh. So every raylib mesh type here
// is spelled ::Mesh / ::Model. Core.hpp does not have to care because it is not
// in the namespace; this file does.
//
// THE TABLE, NOT SIX BLOCKS
// -------------------------
// Six hand-copied blocks is not six readable lines, it is six places to forget:
// there was no single point at which to attach a shader, which is precisely why
// the sky could be drawn unfogged while the world around it fogged. Each row is
// (face index, offset from the camera, euler rotation, which texture, fallback
// tint) and there is one draw loop.
//
// The rotations are not arbitrary. The plane is generated in the XZ plane, so a
// vertical wall needs a rotation about X or Z; a rotation about Y only spins the
// quad within its own plane and leaves it horizontal, i.e. edge-on and invisible
// at the camera's own height - which is why outdoor levels once showed a black
// void wherever a side face should have been. Top is additionally flipped 180
// about X so its -Y normal faces down.
//
// WHY DISTANCE IS 700
// -------------------
// The cube must enclose the frustum or the player sees the void past its corner.
// The face is 2000 wide, so a corner sits sqrt(2)*1000 from the centre and the
// cube covers atan(1000/dist) either side of the view axis: at 700 that is 55
// degrees, comfortably past the corner angle for the default 45-degree FOV, and
// 700 is well inside RL_CULL_DISTANCE_FAR (4000). The editor's 1000 covered the
// same frustum but drew the sky at a different apparent size than the game.
//
// HEADER-ONLY ON PURPOSE
// ----------------------
// Same reasoning as Renderer/CullState.hpp: a .cpp here would mean editing the
// root Makefile, AngelEd/Makefile AND the inline g++ list in ci.yml, because CI
// builds AngelEd by hand rather than through AngelEd/Makefile. Nothing here is
// big enough to justify a third build-list edit.
//
// TEARDOWN ORDER MATTERS
// ---------------------
// Shutdown() calls UnloadModel, which is a GPU call. Source/Main.cpp must call it
// BEFORE CloseWindow - and skip it entirely in shot mode, since shot runs call
// CloseWindow early and have no live context to unload into.
// ---------------------------------------------------------------------------
namespace oz {

// The world's fog, as the sky shader needs it. Passed in rather than read from
// OzoneLoader here, so this header stays a renderer and does not reach into the
// world loader for state; both call sites already hold an OzoneLoader reference.
struct SkyboxFog {
    bool  active = false;
    float color[3] = { 0.7f, 0.7f, 0.8f };
    float start = 10.0f;
    float end = 100.0f;
    float density = 1.0f;
    float intensity = 0.0f;
};

// How high above the horizon the sky is fully clear again. The SINE of the
// elevation, so 0.10 is about 5.7 degrees.
//
// A constant rather than a per-level setting on purpose: it is a property of how
// the cube is built (side faces are vertical planes through the camera, so the
// horizon is always at dir.y == 0), not of any one level's art. A level that
// wants a different haze profile authors it in fog density and colour, which both
// feed the same blend.
constexpr float kSkyHorizonBlend = 0.10f;

// Cubes are centred on the camera, so this also sets the sky's angular coverage.
constexpr float kSkyboxDist = 700.0f;

class Skybox {
public:
    static Skybox& Instance() {
        static Skybox inst;
        return inst;
    }

    // Idempotent. Build eagerly at startup if you want the cost off the first
    // frame; Draw() also builds on demand so a caller cannot forget.
    void Init(float faceSize = 2000.0f) {
        if (m_ready) return;
        for (int i = 0; i < 6; i++) {
            ::Mesh plane = GenMeshPlane(faceSize, faceSize, 1, 1);
            float* tc = (float*)plane.texcoords;
            const int vcount = plane.vertexCount;
            if (tc) {
                // Per-face UV orientation, so each authored texture is not
                // mirrored on the faces that need flipping.
                switch (i) {
                    case 0: for (int v = 0; v < vcount; v++) tc[v*2+1] = 1.0f - tc[v*2+1]; break;
                    case 2: for (int v = 0; v < vcount; v++) tc[v*2]   = 1.0f - tc[v*2];   break;
                    case 5: for (int v = 0; v < vcount; v++) tc[v*2]   = 1.0f - tc[v*2];   break;
                    default: break;
                }
            }
            m_faces[i] = LoadModelFromMesh(plane);
        }
        m_ready = true;
    }

    // GPU teardown. MUST run before CloseWindow.
    void Shutdown() {
        for (int i = 0; i < 6; i++) {
            if (m_faces[i].meshCount > 0) UnloadModel(m_faces[i]);
            m_faces[i] = ::Model{0};
        }
        m_ready = false;
    }

    // cullBackfaces is the state to RESTORE after the six faces, not a request:
    // the cube itself is always drawn two-sided. It mirrors OzoneLoader::Draw's
    // parameter of the same name and meaning, so a caller states one intent
    // ("generated geometry wants culling; wireframe does not") in both places.
    void Draw(const Vector3& camPos, Texture2D capTex, Texture2D sideTex,
              bool cullBackfaces, const SkyboxFog* fog = nullptr) {
        Init();  // on demand, so no caller can render an empty cube

        // Sides fall back to the cap so the sky renders all around; without this
        // the four side faces drew a flat colour and appeared black. A caller that
        // wants the fallback to be visible simply passes sideTex = {0}.
        if (sideTex.id == 0) sideTex = capTex;
        if (capTex.id == 0 && sideTex.id == 0) return;

        const bool skyReady = SkyMaterial::Instance().Ready();
        if (skyReady) {
            auto& mat = SkyMaterial::Instance();
            if (fog && fog->active) {
                mat.SetFog(fog->color, fog->start, fog->end, fog->density, fog->intensity);
            } else {
                // No fog published: intensity 0 makes Sky.fs return the texture
                // untouched, so the sky renders exactly as it did before the
                // horizon-fog shader existed. That is the fallback, not a
                // special case - a level that never sets fog lands here.
                const float none[3] = { 0.0f, 0.0f, 0.0f };
                mat.SetFog(none, 10.0f, 100.0f, 1.0f, 0.0f);
            }
            mat.SetHorizonBlend(kSkyHorizonBlend);
            mat.Apply(camPos, WHITE);
        }

        struct Face {
            int index;
            Vector3 offset;
            float rx, ry, rz;
            bool useCap;
            Color fallback;
        };
        const float D = kSkyboxDist;
        const Face faces[6] = {
            // 0 top     at y=+D, normal -Y (faces down)
            { 0, {  0.0f,  D, 0.0f }, 180.0f, 0.0f,   0.0f, true,  { 80, 120, 200, 255 } },
            // 1 bottom  at y=-D, normal +Y (faces up)
            { 1, {  0.0f, -D, 0.0f },   0.0f, 0.0f,   0.0f, true,  { 80, 120, 200, 255 } },
            // 2 +X       3 -X
            //
            // Every side must put the panorama's V axis on WORLD +Y, or the sky
            // hangs sideways on that wall. raylib applies the listed rotations
            // rz then ry then rx, so the plain `rz = +/-90` the X faces used to
            // carry left V on Z: a vertical wall whose texture is rotated 90
            // degrees. The Z faces (4/5) always used rx = -90, which is the
            // construction that puts V on +Y, so the X faces are the same
            // rotation with the wall turned onto X by rz.
            //
            // Face 5's rx was +90, which points V DOWN: the south wall's
            // panorama was upside down, horizon at the top. The normal direction
            // is irrelevant - the cube is drawn two-sided and Sky.fs derives its
            // horizon from fragPosition, not the normal - so rx = -90 is right
            // on every side.
            { 2, {  D, 0.0f,  0.0f }, -90.0f, 0.0f,  90.0f, false, { 120, 180, 240, 255 } },
            { 3, { -D, 0.0f,  0.0f }, -90.0f, 0.0f, -90.0f, false, { 120, 180, 240, 255 } },
            // 4 +Z       5 -Z
            { 4, { 0.0f, 0.0f,  D }, -90.0f, 0.0f,   0.0f, false, { 120, 180, 240, 255 } },
            { 5, { 0.0f, 0.0f, -D }, -90.0f, 0.0f,   0.0f, false, { 120, 180, 240, 255 } },
        };

        rlDisableDepthMask();
        oz::SetBackfaceCulling(false);

        // id 0 means "no shader on this material", which is how raylib is told to
        // use the default material program. Needed because the SAME six models are
        // reused across frames and across both callers: a face that drew textured
        // on one frame must not keep last frame's program when it falls back.
        const Shader noShader{0};

        for (const Face& f : faces) {
            const Texture2D tex = f.useCap ? capTex : sideTex;
            ::Model& model = m_faces[f.index];

            rlPushMatrix();
            rlTranslatef(camPos.x + f.offset.x, camPos.y + f.offset.y, camPos.z + f.offset.z);
            rlRotatef(f.rx, 1.0f, 0.0f, 0.0f);
            rlRotatef(f.ry, 0.0f, 1.0f, 0.0f);
            rlRotatef(f.rz, 0.0f, 0.0f, 1.0f);

            if (tex.id > 0) {
                model.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = tex;
                // Bind the horizon-fog program. raylib looks `mvp` and `matModel`
                // up by name for whatever program is on the material, so the
                // vertex stage is fed without any manual plumbing.
                if (skyReady) model.materials[0].shader = SkyMaterial::Instance().Get();
                rlDrawRenderBatchActive();
                DrawModel(model, {0, 0, 0}, 1.0f, WHITE);
            } else {
                // No texture for this row: fall back to a flat tint. Deliberately
                // NOT given the sky shader - a solid colour has no horizon to
                // blend, and running the tint through the fog mix would only
                // darken the fallback.
                if (skyReady) model.materials[0].shader = noShader;
                rlDrawRenderBatchActive();
                DrawModel(model, {0, 0, 0}, 1.0f, f.fallback);
            }
            rlPopMatrix();
        }

        oz::SetBackfaceCulling(cullBackfaces);
        rlEnableDepthMask();
    }

    float Distance() const { return kSkyboxDist; }
    bool Ready() const { return m_ready; }

private:
    Skybox() = default;

    ::Model m_faces[6];
    bool   m_ready = false;
};

} // namespace oz
