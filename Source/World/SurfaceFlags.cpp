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

bool BrushSurface::NeedsPerFaceDraw() const {
    if (def.IsNonDefault()) return true;
    for (int i = 0; i < FACE_COUNT; i++)
        if (overridden[i]) return true;
    return false;
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
