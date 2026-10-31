#include "SurfaceFlags.hpp"
#include <cmath>
#include <cstring>

namespace oz {
namespace surface {

// ---------------------------------------------------------------------------
// Faces
// ---------------------------------------------------------------------------
static const char* const kFaceNames[FACE_COUNT] = {
    "px", "nx", "py", "ny", "pz", "nz"
};

const char* FaceName(SurfaceFace f) {
    if (f < 0 || f >= FACE_COUNT) return "?";
    return kFaceNames[(int)f];
}

SurfaceFace FaceFromName(const char* name) {
    if (!name) return FACE_NONE;
    for (int i = 0; i < FACE_COUNT; i++) {
        // Accept the long spellings too, so hand-edited worlds are forgiving.
        if (strcmp(name, kFaceNames[i]) == 0) return (SurfaceFace)i;
    }
    if (strcmp(name, "+x") == 0 || strcmp(name, "+X") == 0) return FACE_PX;
    if (strcmp(name, "-x") == 0 || strcmp(name, "-X") == 0) return FACE_NX;
    if (strcmp(name, "+y") == 0 || strcmp(name, "+Y") == 0) return FACE_PY;
    if (strcmp(name, "-y") == 0 || strcmp(name, "-Y") == 0) return FACE_NY;
    if (strcmp(name, "+z") == 0 || strcmp(name, "+Z") == 0) return FACE_PZ;
    if (strcmp(name, "-z") == 0 || strcmp(name, "-Z") == 0) return FACE_NZ;
    return FACE_NONE;
}

SurfaceFace FaceFromNormal(float nx, float ny, float nz) {
    // Pick the dominant axis. Ties (a 45-degree face) resolve to the first
    // axis in X, Y, Z order, which keeps the mapping deterministic - an
    // unstable pick would make an edited face's flags "jump" between faces as
    // the camera moved.
    const float ax = std::fabs(nx), ay = std::fabs(ny), az = std::fabs(nz);
    if (ax >= ay && ax >= az) return (nx >= 0.0f) ? FACE_PX : FACE_NX;
    if (ay >= az)               return (ny >= 0.0f) ? FACE_PY : FACE_NY;
    return (nz >= 0.0f) ? FACE_PZ : FACE_NZ;
}

// ---------------------------------------------------------------------------
// SurfaceProps
// ---------------------------------------------------------------------------
bool SurfaceProps::IsNonDefault() const {
    if (flags != 0) return true;
    if (texSlot != 0 || !texPath.empty()) return true;
    if (!material.empty()) return true;
    if (uvScaleU != 1.0f || uvScaleV != 1.0f) return true;
    if (uvOffsetU != 0.0f || uvOffsetV != 0.0f) return true;
    if (panU != 0.0f || panV != 0.0f) return true;
    if (alpha != 1.0f || alphaCutoff != 0.0f) return true;
    if (glowR != 0.0f || glowG != 0.0f || glowB != 0.0f) return true;
    if (glowScale != 1.0f) return true;
    return false;
}

// ---------------------------------------------------------------------------
// BrushSurface
// ---------------------------------------------------------------------------
void BrushSurface::SetFace(SurfaceFace f, const SurfaceProps& p) {
    if (f < 0 || f >= FACE_COUNT) return;
    face[(int)f] = p;
    overridden[(int)f] = true;
    selectionMask |= (1u << (int)f);
}

void BrushSurface::ClearFace(SurfaceFace f) {
    if (f < 0 || f >= FACE_COUNT) return;
    overridden[(int)f] = false;
    face[(int)f] = SurfaceProps{};
}

void BrushSurface::ClearAllFaces() {
    for (int i = 0; i < FACE_COUNT; i++) {
        overridden[i] = false;
        face[i] = SurfaceProps{};
    }
}

void BrushSurface::ResetToDefault() {
    ClearAllFaces();
    selectionMask = 0;
}

// A brush-wide property that only exists as a SHADER UNIFORM. `texPath`,
// `texSlot` and the UV scale/offset are NOT in this set: they land in the
// model's material or are baked into the mesh's texcoords by
// ApplyRenderableUV, so all six faces sample identically and the single-draw
// fast path produces the same image.
//
// This distinction is the difference between 1 and 6 draw calls (and ~12× the
// uniform uploads) per brush. It used to be absent, so every world whose brushes
// authored `texPath=` - Sektor_01_SilentPines (266/266), World_endless_snow
// (251/251), KryoArchipel (91/91) - drew every face separately for no visual
// reason. Keep this list about uniforms only; do not add a texture/UV field.
static bool PropsNeedSurfaceShader(const oz::surface::SurfaceProps& p) {
    if (p.flags != 0) return true;
    if (!p.material.empty()) return true;
    if (p.panU != 0.0f || p.panV != 0.0f) return true;
    if (p.alpha != 1.0f || p.alphaCutoff != 0.0f) return true;
    if (p.glowR != 0.0f || p.glowG != 0.0f || p.glowB != 0.0f) return true;
    if (p.glowScale != 1.0f) return true;
    return false;
}

bool BrushSurface::NeedsPerFaceDraw() const {
    // Any face override may differ from its neighbours (texture, flags, UV), so
    // it genuinely needs its own draw.
    for (int i = 0; i < FACE_COUNT; i++)
        if (overridden[i]) return true;
    return PropsNeedSurfaceShader(def);
}

void BrushSurface::SelectOnly(SurfaceFace f) {
    selectionMask = 0;
    if (f >= 0 && f < FACE_COUNT) selectionMask |= (1u << (int)f);
}

void BrushSurface::AddToSelection(SurfaceFace f) {
    if (f >= 0 && f < FACE_COUNT) selectionMask |= (1u << (int)f);
}

void BrushSurface::ToggleSelection(SurfaceFace f) {
    if (f < 0 || f >= FACE_COUNT) return;
    selectionMask ^= (1u << (int)f);
}

int BrushSurface::SelectedCount() const {
    int n = 0;
    for (int i = 0; i < FACE_COUNT; i++)
        if (selectionMask & (1u << i)) n++;
    return n;
}

void ApplyToSelection(BrushSurface& s, const SurfaceProps& p) {
    if (s.selectionMask == 0) return;   // never write "all faces" by accident
    for (int i = 0; i < FACE_COUNT; i++) {
        if (s.selectionMask & (1u << i)) s.SetFace((SurfaceFace)i, p);
    }
}

} // namespace surface
} // namespace oz
