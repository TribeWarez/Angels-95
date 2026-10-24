// ============================================================================
// Subsystems/SurfaceOps.cpp
//
// Per-face surface-property edits: ApplySurface and ResetSurface. Extracted from the
// surface dispatch pass in main() in R6, and named per the original plan
// (Subsystems/SurfaceOps) - which the plan wrongly claimed already existed folded into
// EntityOps.cpp. It did not: EntityOps.cpp holds only AutoConvex and the CSG commit.
//
// REAL translation unit as of Phase F.
// ============================================================================
#include "../Core/EditorShell.hpp"
#include "../Core/EditorState.hpp"
#include "SurfaceOps.hpp"

#include "../../../Source/World/OzOzoneLoader.hpp"
#include "../../../Source/World/SurfaceFlags.hpp"

void ApplySurfaceEdits(const std::vector<ed::Event>& events) {
    for (const ed::Event& sev : events) {
        const ed::SurfaceEdit& se = sev.surface();
        if (sev.kind == ed::Ev::ApplySurface) {
            OzoneRenderable* r = OzoneLoader::Instance().Get(se.renderable);
            if (!r) {
                EditorLog("Surface: apply ignored - renderable %d no longer exists",
                          se.renderable);
            } else if (!se.isTargeted()) {
                // Guarded on purpose: an empty mask must never be treated as
                // "apply to all six faces", NOR as "apply brush-wide".
                EditorLog("Surface: apply ignored - no face selected");
            } else {
                HistoryPush();
                oz::surface::SurfaceProps p = g_editorPanels.surfaceEdit;
                p.flags      = se.flags;
                p.glowR      = se.glowR;  p.glowG = se.glowG;  p.glowB = se.glowB;
                p.glowScale  = se.glowScale;
                p.alpha      = se.alpha;  p.alphaCutoff = se.alphaCutoff;
                p.uvScaleU   = se.uvScaleU; p.uvScaleV = se.uvScaleV;
                p.uvOffsetU  = se.uvOffsetU; p.uvOffsetV = se.uvOffsetV;
                p.panU       = se.panU;   p.panV = se.panV;
                p.texSlot    = se.texSlot;
                // Every value now travels ON THE EVENT. texPath used to be picked
                // up from g_editorPanels.surfaceEdit by the copy above, which meant
                // the one field the payload did not carry was the one field that
                // could be applied to faces other than the ones on screen.
                p.texPath    = se.texPath;
                p.material   = se.material;

                // A flag with no companion value is inert: Masked without a cutoff,
                // Glow without a colour, a pan flag with zero speed. Surface.fs
                // gates on the flag (so a zero speed is a hard no-op, not a divide),
                // which means a freshly ticked checkbox would otherwise commit and
                // appear to do nothing. Seed it and say so.
                const char* inert = nullptr;
                oz::surface::SurfaceProps probed = p;
                if (oz::surface::SurfaceHasInertFlags(probed, &inert))
                    EditorLog("Surface: %s - seeding a working value so the flag does something",
                              inert);
                const int seeded = oz::surface::SeedInertSurfaceValues(p);

                if (se.brushWide) {
                    // FACE_NONE is how OzoneLoader spells "write def". Going
                    // through the same entry point as the face path is what gets
                    // the legacy mirrors re-derived - and the exporter reads the
                    // brush-wide UV fields from those mirrors, so skipping this is
                    // how a brush-wide texture or UV edit gets silently dropped on
                    // save.
                    OzoneLoader::Instance().SetRenderableFace(
                        se.renderable, oz::surface::FACE_NONE, p);
                    EditorLog("Surface: applied BRUSH-WIDE to renderable %d (flags=0x%X, %d seeded)",
                              se.renderable, se.flags, seeded);
                } else {
                    for (int f = 0; f < oz::surface::FACE_COUNT; f++) {
                        if (!(se.faceMask & (1u << f))) continue;
                        OzoneLoader::Instance().SetRenderableFace(
                            se.renderable, (oz::surface::SurfaceFace)f, p);
                    }
                    EditorLog("Surface: applied to %d face(s) of renderable %d (flags=0x%X, %d seeded)",
                              __builtin_popcount(se.faceMask), se.renderable, se.flags, seeded);
                }
                // The Surface Properties dialog is refreshed by the CALLER now, not
                // from here - see SurfaceOps.hpp.
            }
        } else if (sev.kind == ed::Ev::ResetSurface) {
            const int rIdx = se.renderable;
            OzoneRenderable* r = OzoneLoader::Instance().Get(rIdx);
            if (r && se.isTargeted()) {
                HistoryPush();
                if (se.brushWide) {
                    // NOT ResetRenderableSurface: BrushSurface::ResetToDefault
                    // deliberately KEEPS def (it means "push the default onto every
                    // face"), so reusing it would reset nothing at all here.
                    OzoneLoader::Instance().ResetRenderableSurfaceDefault(rIdx);
                    EditorLog("Surface: reset BRUSH-WIDE default of renderable %d", rIdx);
                } else {
                    for (int f = 0; f < oz::surface::FACE_COUNT; f++) {
                        if (!(se.faceMask & (1u << f))) continue;
                        r->surface.ClearFace((oz::surface::SurfaceFace)f);
                    }
                    // Face meshes only exist while a brush is decorated, so
                    // dropping the last override has to release them.
                    OzoneLoader::Instance().RebuildSurfaceMeshes(rIdx);
                    EditorLog("Surface: reset %d face(s) of renderable %d",
                              __builtin_popcount(se.faceMask), rIdx);
                }
            }
        }
    }
}