#pragma once
// ---------------------------------------------------------------------------
// World/SurfaceFlags.hpp — per-face surface properties for OZONE brushes
//
// Inspired by the UT99 Surface properties dialog (Flags / Alignment / Stats
// tabs, one entry per brush FACE rather than per brush). The editor edits these
// by right-clicking a face in the viewport; each face stores its own flags,
// texture slot, UV transform, U/V pan, alpha and glow.
//
// RAYLIB-FREE ON PURPOSE. Plain floats, no Color/Vector2/Vector3, so the
// server's `worldcheck` auditor, the OZONE parser and the headless test harness
// can all link this (same discipline as World/ZoneTypes.hpp and
// World/GameType.hpp). The renderer maps the float triples onto raylib types at
// the point of use.
//
// ---------------------------------------------------------------------------
// BIT ALLOCATION
//
// Bits 0-2 are unallocated. Bits 3 and 4 are the pre-existing, already-shipped
// meanings and MUST NOT be renumbered:
//
//   1<<3 SURF_FAKEBACKDROP      painted 2D backdrop (Ocarina of Time trick)
//   1<<4 SURF_COLLISION_PROXY   generated AutoConvex box, never drawn in-game
//
// Only `flags=8` appears in the shipped worlds (299 times), so nothing written
// today can collide with 1<<5 and above. Surface flags therefore start at bit 5
// and run to bit 29, which is exactly enough for the 25 UT99 checkboxes.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string>

namespace oz {
namespace surface {

// ---------------------------------------------------------------------------
// Flag bits
//
// These are constants, not an enum and not macros. They were `#define`s in
// OzOzoneLoader.hpp before, which meant the names could not also be declared in
// a scoped type, and every call site used them unqualified. Keeping the same
// unqualified spellings (via the using-declarations at the bottom of this
// header) means no existing call site has to change, while the values now have
// a real type and a single home.
//
// Bits 0-2 are unallocated. Bits 3 and 4 are the pre-existing, already-shipped
// meanings and MUST NOT be renumbered:
//
//   1<<3 SURF_FAKEBACKDROP      painted 2D backdrop (Ocarina of Time trick)
//   1<<4 SURF_COLLISION_PROXY   generated AutoConvex box, never drawn in-game
//
// Only `flags=8` appears in the shipped worlds (299 times), so nothing written
// today can collide with 1<<5 and above. Surface flags therefore start at bit 5
// and run to bit 31, which covers the 25 UT99 checkboxes.
// ---------------------------------------------------------------------------

// --- reserved / pre-existing -----------------------------------------------
inline constexpr uint32_t SURF_FAKEBACKDROP      = (1u << 3);
inline constexpr uint32_t SURF_COLLISION_PROXY   = (1u << 4);

// Gain the sky pass applies to a SURF_FAKEBACKDROP face.
//
// A painted backdrop must be seen at its authored value, so the pass runs the
// face through Surface.fs's full-bright branch at a fixed tone-curve gain rather
// than through lighting. 0.55 is NOT arbitrary and NOT 1.0: it reproduces the
// gain the old shared-uniform DrawZoneGeometry hack produced (it forced ambient
// to 5.5, and the shader divides ambient by 10 -> 0.55). At 1.0 the pale stone in
// the shipped backdrops blows out to flat white, which is why the 0.55 was chosen
// in the first place.
//
// Lives here, next to the flag, rather than as a local in DrawZoneGeometry so it
// is (a) raylib-free and therefore assertable from tests/Surface.test.cpp and
// (b) impossible to silently change in one of the two passes that read it. Only
// the backdrop pass should ever use it; OzoneLoader::DrawZoneGeometry sets
// SurfaceMaterial::SetFullBright to this and back to 1.0.
inline constexpr float kBackdropGain = 0.55f;
// The full-bright gain for every OTHER pass (the world pass, and the editor).
inline constexpr float kFullBrightGain = 1.0f;

// --- rendering behaviour ---------------------------------------------------
inline constexpr uint32_t SURF_INVISIBLE       = (1u << 5);   // not drawn (still collides)
inline constexpr uint32_t SURF_MASKED          = (1u << 6);   // alpha test vs cutoff
inline constexpr uint32_t SURF_TRANSLUCENT     = (1u << 7);   // alpha blend, no depth write
inline constexpr uint32_t SURF_ALPHABLEND      = (1u << 8);   // explicit blend, no fog
inline constexpr uint32_t SURF_MODULATED       = (1u << 9);   // multiply blend
inline constexpr uint32_t SURF_TWO_SIDED       = (1u << 10);  // draw back faces too
inline constexpr uint32_t SURF_UNLIT           = (1u << 11);  // flat albedo, no ambient
// Lit geometry, NO light contribution: takes the world's ambient term so the
// surface sits in the room's exposure, but no directional/point/spot light
// touches it. Distinct from SURF_UNLIT, which takes no ambient either - the two
// shared one shader branch until FAKE_LIT was given its own.
inline constexpr uint32_t SURF_FAKE_LIT        = (1u << 12);
inline constexpr uint32_t SURF_SPECIAL_LIT     = (1u << 13);  // reserved: custom model
inline constexpr uint32_t SURF_GLOW            = (1u << 14);  // self-illumination
inline constexpr uint32_t SURF_PORTAL          = (1u << 15);  // portal surface
inline constexpr uint32_t SURF_MIRROR          = (1u << 16);  // mirror surface
inline constexpr uint32_t SURF_ENVIRONMENT     = (1u << 17);  // environment-mapped

// --- texture animation -----------------------------------------------------
inline constexpr uint32_t SURF_PAN_U           = (1u << 18);  // scroll U
inline constexpr uint32_t SURF_PAN_V           = (1u << 19);  // scroll V
inline constexpr uint32_t SURF_SMALL_WAVY      = (1u << 20);
inline constexpr uint32_t SURF_BRIGHT_CORNERS  = (1u << 21);

// --- lighting / shadow hints -----------------------------------------------
inline constexpr uint32_t SURF_DIRTY_SHADOWS   = (1u << 22);
inline constexpr uint32_t SURF_SHADOW_HI       = (1u << 23);  // high shadow detail
inline constexpr uint32_t SURF_SHADOW_LO       = (1u << 24);  // low shadow detail
inline constexpr uint32_t SURF_NO_SMOOTH       = (1u << 25);  // flat shading

// --- fog / bounds / BSP hints ----------------------------------------------
inline constexpr uint32_t SURF_NO_FOG          = (1u << 26);
inline constexpr uint32_t SURF_NO_BOUNDS_REJECT = (1u << 27); // always draw
inline constexpr uint32_t SURF_NO_BSP_CUTS     = (1u << 28); // cosmetic only
inline constexpr uint32_t SURF_ZONE_HACK       = (1u << 29); // zone visual override
inline constexpr uint32_t SURF_INVISIBLE_OCCLUDER = (1u << 30); // occludes, not drawn
inline constexpr uint32_t SURF_FORCE_VIEW_ZONE   = (1u << 31); // forces sky-zone view

// ---------------------------------------------------------------------------
// Faces
//
// Six buckets, chosen by the dominant axis of a face's normal. Deriving the
// face geometrically (rather than assuming the primitive generator's vertex
// order) is what lets the same six faces work for a box, a cylinder, a sphere
// and a pyramid.
// ---------------------------------------------------------------------------
enum SurfaceFace : int {
    FACE_PX = 0,   // +X
    FACE_NX = 1,   // -X
    FACE_PY = 2,   // +Y  (floor in OZONE terms; engine Y-up)
    FACE_NY = 3,   // -Y
    FACE_PZ = 4,   // +Z
    FACE_NZ = 5,   // -Z
    FACE_COUNT = 6,
    FACE_NONE = -1
};

// Editor / export spelling of each face. Note this is ENGINE Y-up naming, not
// OZONE Z-up: the OZONE text swaps y/z on load and load, so a face authored as
// `face_pz` is the brush's +Z side in engine space.
const char* FaceName(SurfaceFace f);
SurfaceFace  FaceFromName(const char* name);      // FACE_NONE when unknown
SurfaceFace  FaceFromNormal(float nx, float ny, float nz);

// ---------------------------------------------------------------------------
// SurfaceProps — one face's worth of surface state. Plain data, no raylib.
// ---------------------------------------------------------------------------
struct SurfaceProps {
    uint32_t flags = 0;

    // Texture. texSlot is 1-based into the world's <world>/oztex/tileset/ list;
    // 0 means "whatever the brush mesh already uses". texPath, when non-empty,
    // wins over texSlot (a free-placement asset rather than a tileset entry).
    int         texSlot  = 0;
    std::string texPath;

    // UV transform, applied on top of the mesh's generated UVs. Same units as
    // the existing texScaleU/V + texOffsetU/V brush kwargs.
    float uvScaleU  = 1.0f;
    float uvScaleV  = 1.0f;
    float uvOffsetU = 0.0f;
    float uvOffsetV = 0.0f;

    // U/V pan, in texture units per second. Only animated when the matching
    // SURF_PAN_U / SURF_PAN_V bit is set, so a zero speed is a hard no-op
    // rather than a per-frame division.
    float panU = 0.0f;
    float panV = 0.0f;

    // Transparency. `alpha` multiplies the sampled colour; `alphaCutoff > 0`
    // switches the surface to masked (alpha-test) instead of blended.
    float alpha      = 1.0f;
    float alphaCutoff = 0.0f;

    // Self-illumination. `glowR/G/B` in 0..1, scaled by glowScale.
    float glowR = 0.0f, glowG = 0.0f, glowB = 0.0f;
    float glowScale = 1.0f;

    bool Has(uint32_t bit) const { return (flags & bit) != 0; }
    void Set(uint32_t bit, bool on) {
        if (on) flags |= bit; else flags &= ~bit;
    }
    // True when this face needs anything the default DrawModel path cannot do.
    // Used to keep undecorated brushes on the original single-draw fast path.
    bool IsNonDefault() const;
};

// ---------------------------------------------------------------------------
// Which texture a face binds, and how its UVs are transformed.
//
// These live here, raylib-free, because they are the ONE definition used by the
// renderer (BuildFaceMeshes / DrawSurface) and by the tests. Written separately
// in each place they would drift, and a face that sampled a different texture
// from the one the dialog named is exactly the bug this replaces: texSlot and
// texPath were stored, exported and round-tripped for as long as they have
// existed, and no code ever bound them.
// ---------------------------------------------------------------------------
struct FaceTextureRef {
    std::string path;   // non-empty: load this asset (wins over slot)
    int slot = 0;       // >0: 1-based index into the world's oztex/tileset list

    // Nothing authored: keep whatever the brush's material already carries (the
    // auto-selected or positional texSlot texture).
    bool Keep() const { return path.empty() && slot <= 0; }
};

// `p` is a face's RESOLVED props (face override or brush default - the caller
// uses BrushSurface::Resolve). Precedence: path beats slot, and neither beats
// the brush material.
inline FaceTextureRef ResolveFaceTexture(const SurfaceProps& p) {
    FaceTextureRef r;
    if (!p.texPath.empty())  r.path = p.texPath;
    else if (p.texSlot > 0)  r.slot = p.texSlot;
    return r;
}

// Write `base` (pristine generated UVs, 2 floats per vertex) transformed by a
// face's scale/offset into `out`. `out` must hold vertexCount*2 floats.
inline void ApplyUVTransform(const float* base, int vertexCount,
                             float su, float sv, float ou, float ov, float* out) {
    for (int i = 0; i < vertexCount; i++) {
        out[i * 2 + 0] = base[i * 2 + 0] * su + ou;
        out[i * 2 + 1] = base[i * 2 + 1] * sv + ov;
    }
}

// ---------------------------------------------------------------------------
// BrushSurface — a brush's six faces plus the brush-wide default they inherit.
//
// Precedence when resolving face f:
//     face[f] if overridden  >  def  >  engine defaults
//
// This mirrors the rule GameData/Pawn/Lights/Light.ozls documents: a value
// the author set explicitly always wins over the shared default.
// ---------------------------------------------------------------------------
struct BrushSurface {
    SurfaceProps def;                 // brush-wide default
    SurfaceProps face[FACE_COUNT];    // per-face, valid only where overridden
    bool         overridden[FACE_COUNT] = {false, false, false, false, false, false};

    // Bitmask of faces with at least one override. Lets the editor track a
    // multi-face selection ("Surface Properties (3 Selected)") as a single int.
    uint32_t selectionMask = 0;

    bool IsFaceOverridden(SurfaceFace f) const {
        return f >= 0 && f < FACE_COUNT && overridden[(int)f];
    }
    void SetFace(SurfaceFace f, const SurfaceProps& p);
    void ClearFace(SurfaceFace f);
    void ClearAllFaces();
    // Push the brush-wide default onto every face and drop all overrides.
    void ResetToDefault();

    // Effective properties for a face (override or default).
    const SurfaceProps& Resolve(SurfaceFace f) const {
        return IsFaceOverridden(f) ? face[(int)f] : def;
    }

    // True when the brush needs the per-face draw path.
    bool NeedsPerFaceDraw() const;
    // True when the brush-wide default alone already forces the surface shader.
    bool DefIsNonDefault() const { return def.IsNonDefault(); }

    // Selection helpers (editor).
    void SelectOnly(SurfaceFace f);
    void AddToSelection(SurfaceFace f);
    void ToggleSelection(SurfaceFace f);
    void ClearSelection() { selectionMask = 0; }
    int  SelectedCount() const;
    bool IsSelected(SurfaceFace f) const {
        return f >= 0 && (selectionMask & (1u << (int)f)) != 0;
    }
};

// Apply one face's props to every face in the selection mask (no-op when the
// mask is empty). Used by the Surface Properties dialog's Apply.
void ApplyToSelection(BrushSurface& s, const SurfaceProps& p);

// The subset of flags the *legacy* render path branches on. OzoneRenderable::
// surfaceFlags and OzonePrimitive::surfaceFlags are DERIVED VIEWS holding exactly
// this mask of surface.def.flags, so an arbitrary decorative flag can never make
// a brush invisible or carve it out of CSG by accident.
//
// This constant is the single definition of that mask. It used to be copy-pasted
// at four sites (DeriveLegacySurfaceFields, SetRenderableFace,
// ResetRenderableSurface, and AddBrushRenderable), which is how the two fields
// came to disagree in the first place — see DeriveLegacyFlags.
inline constexpr uint32_t kLegacyPipelineFlags =
    (SURF_FAKEBACKDROP | SURF_COLLISION_PROXY | SURF_INVISIBLE);

// Re-derive `legacyFlags` from the surface block, which is the single owner.
// Every writer of surface flags must call this afterwards.
inline int DeriveLegacyFlags(const SurfaceProps& def) {
    return (int)(def.flags & kLegacyPipelineFlags);
}

// Re-derive every DERIVED field on an OzoneRenderable from its surface block.
//
// OzoneRenderable::texScaleU/V, texOffsetU/V and texPath are VIEWS onto
// surface.def, not independent state - the parser's twin for this is
// OzoneParser.cpp's DeriveLegacySurfaceFields(OzonePrimitive&). They exist
// because DrawWorldGeometry, DrawZoneGeometry, ApplyRenderableUV and
// ExportToOzone still read them, and because the shipped worlds rely on the
// legacy `texScale=`/`texPath=` spelling.
//
// Every writer of surface.def MUST call this. SetRenderableFace used to
// re-derive surfaceFlags alone, which meant a brush-wide edit of U/V scale or
// texture left the mirrors holding the pre-edit values - and ExportToOzone emits
// the brush-wide UV fields FROM THE MIRRORS and suppresses `surfTex=` whenever
// the mirror's texPath is non-empty. So brush-wide texture and UV edits were
// silently discarded on save. The bug was latent only because nothing could write
// surface.def at all; the editor's brush-wide scope is what made it live.
//
// Takes a small struct rather than OzoneRenderable so it stays raylib-free and
// testable in the headless harness; the caller copies the results in. See
// DeriveLegacyRenderable in OzOzoneLoader.hpp for the renderable-shaped wrapper.
struct LegacySurfaceMirror {
    int         surfaceFlags = 0;
    float       texScaleU = 1.0f, texScaleV = 1.0f;
    float       texOffsetU = 0.0f, texOffsetV = 0.0f;
    std::string texPath;
};

inline LegacySurfaceMirror DeriveLegacySurfaceMirror(const SurfaceProps& def) {
    LegacySurfaceMirror m;
    m.surfaceFlags = DeriveLegacyFlags(def);
    m.texScaleU  = def.uvScaleU;
    m.texScaleV  = def.uvScaleV;
    m.texOffsetU = def.uvOffsetU;
    m.texOffsetV = def.uvOffsetV;
    // Only adopt the surface's own texture path when it actually has one, so
    // clearing the surface texture falls back to the tileset instead of latching
    // the previous value.
    if (!def.texPath.empty()) m.texPath = def.texPath;
    return m;
}

// Whether a brush line needs an explicit `flags=` kwarg.
//
// This used to be written inline in the exporter as
//     if (def.flags != (uint32_t)legacyFlags) output << " flags=" << def.flags;
// i.e. "emit the owner's value only when it disagrees with the owner's own
// derived mirror". After a load those are equal BY CONSTRUCTION, so the condition
// was never true and `flags=` was never written — every `flags=8` backdrop and
// every `flags=16` collision proxy was silently dropped on re-export. A freshly
// appended AutoConvex proxy went the other way (mirror set, owner left at 0) and
// exported a literal `flags=0`.
//
// The honest condition is simply "the owner is non-zero". Kept here, next to the
// mask, so the exporter's decision is a named, testable predicate rather than an
// inline comparison someone can invert again.
inline constexpr bool NeedsFlagsKwarg(uint32_t defFlags) { return defFlags != 0; }

// ---------------------------------------------------------------------------
// Flag/value conjunctions
//
// Four flags do NOTHING unless a second value is also set, which made a ticked
// checkbox silently inert:
//
//   SURF_MASKED       needs alphaCutoff  > 0   (Surface.fs's discard predicate)
//   SURF_GLOW         needs glowR|G|B != 0     (the added emissive term)
//   SURF_PAN_U / _V   needs panU|panV  != 0
//
// The shader gates on the flag because a zero speed must be a hard no-op rather
// than a per-frame divide; the cost is that the flag alone is inert. These
// predicates are the one place that knows it, so the editor can warn or seed a
// sane value instead of letting the user wonder why nothing happened, and the
// tests can pin the rule instead of it being folklore in a GLSL comment.
//
// `Resolve`ing a face is the caller's job: pass the RESOLVED props (brush
// default unless the face overrides), since an inherited value counts.
// ---------------------------------------------------------------------------

// Does SURF_MASKED actually do anything on this face?
inline constexpr bool MaskedIsActive(const SurfaceProps& p) {
    return p.Has(SURF_MASKED) && p.alphaCutoff > 0.0f;
}
// Does SURF_GLOW actually do anything on this face?
inline constexpr bool GlowIsActive(const SurfaceProps& p) {
    return p.Has(SURF_GLOW) && (p.glowR != 0.0f || p.glowG != 0.0f || p.glowB != 0.0f);
}
// Does either pan flag actually do anything on this face?
inline constexpr bool PanIsActive(const SurfaceProps& p) {
    return (p.Has(SURF_PAN_U) && p.panU != 0.0f) || (p.Has(SURF_PAN_V) && p.panV != 0.0f);
}

// True when the face carries at least one flag whose companion value is missing,
// i.e. the editor has something to tell the author about. `which` (optional)
// receives a short label for the log line.
//
// Written off the *IsActive predicates above rather than re-deriving the rules,
// so the warning and the tests cannot disagree with the shader's own gate.
inline bool SurfaceHasInertFlags(const SurfaceProps& p, const char** which = nullptr) {
    if (p.Has(SURF_MASKED) && !MaskedIsActive(p)) {
        if (which) *which = "Masked needs Alpha Cutoff > 0";
        return true;
    }
    if (p.Has(SURF_GLOW) && !GlowIsActive(p)) {
        if (which) *which = "Glow needs a non-zero colour";
        return true;
    }
    if (p.Has(SURF_PAN_U) && p.panU == 0.0f) {
        if (which) *which = "U-Pan needs a non-zero speed";
        return true;
    }
    if (p.Has(SURF_PAN_V) && p.panV == 0.0f) {
        if (which) *which = "V-Pan needs a non-zero speed";
        return true;
    }
    return false;
}

// Fill in a usable companion value for any flag whose companion is missing, so
// Apply never commits an inert combination. Returns the number of values seeded.
//
// Chosen defaults: a cutoff of 0.5 (the conventional binary alpha cut), white
// glow at scale 1, and 0.1 texels/sec (slowly enough to read as movement rather
// than as a broken texture lookup). Seeding beats rejecting the edit - the user
// asked for the flag, and the value is one keystroke away in the dialog.
inline int SeedInertSurfaceValues(SurfaceProps& p) {
    int n = 0;
    if (p.Has(SURF_MASKED) && p.alphaCutoff <= 0.0f) { p.alphaCutoff = 0.5f; n++; }
    if (p.Has(SURF_GLOW) && !GlowIsActive(p)) {
        p.glowR = p.glowG = p.glowB = 1.0f;
        if (p.glowScale <= 0.0f) p.glowScale = 1.0f;
        n++;
    }
    if (p.Has(SURF_PAN_U) && p.panU == 0.0f) { p.panU = 0.1f; n++; }
    if (p.Has(SURF_PAN_V) && p.panV == 0.0f) { p.panV = 0.1f; n++; }
    return n;
}

} // namespace surface
} // namespace oz

// ---------------------------------------------------------------------------
// Unqualified re-exports.
//
// SURF_FAKEBACKDROP and SURF_COLLISION_PROXY were `#define`s, and every call
// site (DrawWorldGeometry, Core.hpp, the editor, the AutoConvex pass) uses the
// bare name. These using-declarations keep those spellings working without
// reintroducing macros - a macro of the same name would break any scoped
// declaration that mentions it.
// ---------------------------------------------------------------------------
using oz::surface::SURF_FAKEBACKDROP;
using oz::surface::SURF_COLLISION_PROXY;
using oz::surface::kBackdropGain;
using oz::surface::kFullBrightGain;
using oz::surface::SURF_INVISIBLE;
using oz::surface::SURF_MASKED;
using oz::surface::SURF_TRANSLUCENT;
using oz::surface::SURF_ALPHABLEND;
using oz::surface::SURF_MODULATED;
using oz::surface::SURF_TWO_SIDED;
using oz::surface::SURF_UNLIT;
using oz::surface::SURF_FAKE_LIT;
using oz::surface::SURF_SPECIAL_LIT;
using oz::surface::SURF_GLOW;
using oz::surface::SURF_PORTAL;
using oz::surface::SURF_MIRROR;
using oz::surface::SURF_ENVIRONMENT;
using oz::surface::SURF_PAN_U;
using oz::surface::SURF_PAN_V;
using oz::surface::SURF_SMALL_WAVY;
using oz::surface::SURF_BRIGHT_CORNERS;
using oz::surface::SURF_DIRTY_SHADOWS;
using oz::surface::SURF_SHADOW_HI;
using oz::surface::SURF_SHADOW_LO;
using oz::surface::SURF_NO_SMOOTH;
using oz::surface::SURF_NO_FOG;
using oz::surface::SURF_NO_BOUNDS_REJECT;
using oz::surface::SURF_NO_BSP_CUTS;
using oz::surface::SURF_ZONE_HACK;
using oz::surface::SURF_INVISIBLE_OCCLUDER;
using oz::surface::SURF_FORCE_VIEW_ZONE;
