// =============================================================================
// Subsystems/Selection.cpp
//
// Picking and selection: viewport raycast, surface-face pick, gizmo snap, hover, and adopting a selection.
//
// FRAGMENT - not a standalone translation unit. Included by Main.cpp, which is the
// single TU for AngelEd's core layer. See Wiki/Editor-Architecture-Refactor.md.
// =============================================================================

#include <algorithm>

#include "SnapMath.hpp"
#include "../Core/SelectionSet.hpp"

// Thirteen per-container raycasts, one per entity kind, all sharing the same shape:
// pick the nearest hit and fill `out`. They lived in Core/EditorShell.hpp until R6,
// which is how a 420-line state header came to contain 250 lines of picking logic.
// EditorRaycastAt below is their only caller, which is why they moved down to it.

// Read-only selection accessors for the UI TU. g_sel is an inline global in
// Core/EditorState.hpp, which UI/ cannot include because that header pulls
// Editor.hpp (a Main-TU header). Declared extern in UI/UiCommon.hpp - same
// pattern as the WorldGraph_GetModel* accessors.
int EdSel_CurrentType()  { return (int)g_sel.type; }
int EdSel_CurrentIndex() { return g_sel.index; }

// One rotation-aware brush box for the picker, the gizmo pivot/write-back and
// duplicate. Kept here (and declared in Selection.hpp) so those four sites cannot
// disagree about where a rotated brush's centre is - which they did: the picker
// used RotatedAabbY while the per-frame sync used the unrotated mesh centre.
BoundingBox BrushWorldBox(const OzoneRenderable& r) {
    if (r.model.meshCount <= 0) return BoundingBox{{0,0,0},{0,0,0}};
    const BoundingBox mb = GetMeshBoundingBox(r.model.meshes[0]);
    const AabbBox local{mb.min.x, mb.min.y, mb.min.z, mb.max.x, mb.max.y, mb.max.z};
    const AabbBox w = RotatedAabbY(local, r.rotation,
                                   r.position.x, r.position.y, r.position.z, r.scale);
    return BoundingBox{{w.minX, w.minY, w.minZ}, {w.maxX, w.maxY, w.maxZ}};
}

Vector3 BrushCenter(const OzoneRenderable& r) {
    const BoundingBox b = BrushWorldBox(r);
    return {(b.min.x + b.max.x) * 0.5f,
            (b.min.y + b.max.y) * 0.5f,
            (b.min.z + b.max.z) * 0.5f};
}

// Offset from a Mesh.* node's ORIGIN to its world CENTRE. The gizmo and the
// raycast address the CENTRE while `m->position` is the ORIGIN (often the feet),
// so every writer - the mover and the clipboard paster - has to agree on this one
// conversion. It used to be two inline copies.
static Vector3 MeshCentreOffset(const MeshObjectNode& m) {
    if (!m.mesh || !m.mesh->Valid()) return {0, 0, 0};
    const BoundingBox& mb = m.mesh->Bounds();
    const float lx = (mb.min.x + mb.max.x) * 0.5f * m.scale;
    const float ly = (mb.min.y + mb.max.y) * 0.5f * m.scale;
    const float lz = (mb.min.z + mb.max.z) * 0.5f * m.scale;
    const float c = cosf(m.yaw * DEG2RAD);
    const float s = sinf(m.yaw * DEG2RAD);
    return { lx * c + lz * s, ly, -lx * s + lz * c };
}

static RayCollision RaycastTestOzPrimitives(Ray ray, EditorSelection& out) {
    RayCollision best = { false, 1e9f, {0,0,0}, {0,0,0} };
    int count = OzoneLoader::Instance().Count();
    for (int i = 0; i < count; i++) {
        OzoneRenderable* r = OzoneLoader::Instance().Get(i);
        if (!r || !r->loaded || r->model.meshCount == 0) continue;
        // Never pick what cannot be seen: every drawing path skips collision proxies
        // (DrawWorldGeometry, DrawZoneGeometry, DrawGlowGeometry) and invisible faces,
        // so without this guard a plain click can select an invisible AutoConvex box
        // and then go on to move, resize or delete it. The predicate is shared with
        // the WorldGraph list and the surface picker - see IsEditorPickableSurface.
        if (!IsEditorPickableSurface(*r))
            continue;
        BoundingBox box = BrushWorldBox(*r);
        RayCollision hit = GetRayCollisionBox(ray, box);
        if (hit.hit && hit.distance < best.distance) {
            best = hit;
            // Store ACTUAL mesh center as selection position, not r->position
            Vector3 center = {(box.min.x + box.max.x) * 0.5f,
                              (box.min.y + box.max.y) * 0.5f,
                              (box.min.z + box.max.z) * 0.5f};
            out = { SelType::BRUSH, i, TextFormat("OzPrimitive %d", i),
                    center, r->scale, r->rotation * RAD2DEG, true }; // UI works in degrees
        }
    }
    return best;
}

static RayCollision RaycastTestModels(Ray ray, EditorSelection& out) {
    RayCollision best = { false, 1e9f, {0,0,0}, {0,0,0} };
    for (int i = 0; i < CachedModelCounter; i++) {
        int mid = CachedModels[i].ModelId;
        if (mid <= 0) continue;
        LoadedModel* lm = WDLModels.GetModelByWDLId(mid);
        if (!lm || !lm->loaded || lm->model.meshCount == 0) continue;
        BoundingBox box = GetMeshBoundingBox(lm->model.meshes[0]);
        float sx = CachedModels[i].S;
        Vector3 pos = {CachedModels[i].X, CachedModels[i].Y, CachedModels[i].Z};
        box.min = Vector3Add(Vector3Scale(box.min, sx), pos);
        box.max = Vector3Add(Vector3Scale(box.max, sx), pos);
        RayCollision hit = GetRayCollisionBox(ray, box);
        if (hit.hit && hit.distance < best.distance) {
            best = hit;
            out = { SelType::MODEL, i, TextFormat("Model%d", mid), pos, sx, CachedModels[i].R, true };
        }
    }
    return best;
}

static RayCollision RaycastTestPawns(Ray ray, EditorSelection& out) {
    RayCollision best = { false, 1e9f, {0,0,0}, {0,0,0} };
    auto& pawns = PawnSystem::Instance().GetPawns();
    for (auto& p : pawns) {
        if (!p.active) continue;
        // `p.position` is the FEET (the model is drawn with a ground offset so its
        // lowest point sits there), so the pick volume has to be the BODY, rising
        // from the feet. It used to be feet +/- 1, i.e. a 2-unit cube: the bottom
        // third of a pawn was clickable and the head and chest were not, which reads
        // as "enemies are hard to select". 2.8 is the engine's body height
        // (PlayerMovement::Height); the width matches the drawn billboard footprint.
        const float halfW = 0.8f;
        BoundingBox box = { {p.position.x - halfW, p.position.y,        p.position.z - halfW},
                            {p.position.x + halfW, p.position.y + 2.8f, p.position.z + halfW} };
        RayCollision hit = GetRayCollisionBox(ray, box);
        if (hit.hit && hit.distance < best.distance) {
            best = hit;
            // Selection position stays the FEET: that is what the mover writes back
            // and what SnapGizmoToSelection feeds the gizmo.
            out = { SelType::NPC, (int)p.id, p.defName, p.position };
        }
    }
    return best;
}

static RayCollision RaycastTestPickups(Ray ray, EditorSelection& out) {
    RayCollision best = { false, 1e9f, {0,0,0}, {0,0,0} };
    auto& pickups = PawnSystem::Instance().GetPickups();
    for (auto& pk : pickups) {
        if (!pk.active) continue;
        BoundingBox box = { {pk.position.x - 0.6f, pk.position.y - 0.3f, pk.position.z - 0.6f},
                            {pk.position.x + 0.6f, pk.position.y + 0.9f, pk.position.z + 0.6f} };
        RayCollision hit = GetRayCollisionBox(ray, box);
        if (hit.hit && hit.distance < best.distance) {
            best = hit;
            out = { SelType::PICKUP, (int)pk.id, pk.typeName, pk.position };
        }
    }
    return best;
}

static RayCollision RaycastTestZones(Ray ray, EditorSelection& out) {
    RayCollision best = { false, 1e9f, {0,0,0}, {0,0,0} };
    auto& zones = ZoneManager::Instance().GetZones();
    Vector3 camPos = OTEditor.MainCamera.position;
    for (auto& z : zones) {
        // Skip zones containing the camera â€” can't select the boundary you're inside
        if (camPos.x >= z.bounds.min.x && camPos.x <= z.bounds.max.x &&
            camPos.y >= z.bounds.min.y && camPos.y <= z.bounds.max.y &&
            camPos.z >= z.bounds.min.z && camPos.z <= z.bounds.max.z)
            continue;
        RayCollision hit = GetRayCollisionBox(ray, z.bounds);
        if (hit.hit && hit.distance < best.distance) {
            best = hit;
            out = { SelType::ZONE, (int)z.id, "ZoneVolume", {
                (z.bounds.min.x + z.bounds.max.x) * 0.5f,
                (z.bounds.min.y + z.bounds.max.y) * 0.5f,
                (z.bounds.min.z + z.bounds.max.z) * 0.5f
            }};
        }
    }
    return best;
}

static RayCollision RaycastTestLights(Ray ray, EditorSelection& out) {
    RayCollision best = { false, 1e9f, {0,0,0}, {0,0,0} };
    auto& lights = PawnSystem::Instance().GetLights();
    // A light is drawn as a billboard at position + 0.4 Y, so the pick volume
    // has to cover that sprite rather than sitting under it - otherwise the
    // visible marker and the clickable region disagree and aiming feels broken.
    for (auto& l : lights) {
        if (!l.active) continue;
        BoundingBox box = { {l.position.x - 0.6f, l.position.y - 0.1f, l.position.z - 0.6f},
                            {l.position.x + 0.6f, l.position.y + 1.0f, l.position.z + 0.6f} };
        RayCollision hit = GetRayCollisionBox(ray, box);
        if (hit.hit && hit.distance < best.distance) {
            best = hit;
            out = { SelType::LIGHT, (int)l.id,
                    l.name.empty() ? "Light" : l.name.c_str(), l.position };
        }
    }
    return best;
}

static RayCollision RaycastTestStarts(Ray ray, EditorSelection& out) {
    RayCollision best = { false, 1e9f, {0,0,0}, {0,0,0} };
    auto& starts = PawnSystem::Instance().GetPlayerStarts();
    for (auto& s : starts) {
        BoundingBox box = { {s.position.x - 0.6f, s.position.y - 0.3f, s.position.z - 0.6f},
                            {s.position.x + 0.6f, s.position.y + 1.0f, s.position.z + 0.6f} };
        RayCollision hit = GetRayCollisionBox(ray, box);
        if (hit.hit && hit.distance < best.distance) {
            best = hit;
            out = { SelType::SPAWN, (int)s.id, "PlayerStart", s.position, 1.0f, s.yaw, true };
        }
    }
    return best;
}

static RayCollision RaycastTestPortals(Ray ray, EditorSelection& out) {
    RayCollision best = { false, 1e9f, {0,0,0}, {0,0,0} };
    auto& portals = ZoneManager::Instance().GetPortals();
    for (size_t p = 0; p < portals.size(); p++) {
        auto& portal = portals[p];
        RayCollision hit = GetRayCollisionBox(ray, portal.bounds);
        if (hit.hit && hit.distance < best.distance) {
            best = hit;
            out = { SelType::PORTAL, (int)p, portal.targetWorld, {
                (portal.bounds.min.x + portal.bounds.max.x) * 0.5f,
                (portal.bounds.min.y + portal.bounds.max.y) * 0.5f,
                (portal.bounds.min.z + portal.bounds.max.z) * 0.5f
            }};
        }
    }
    return best;
}

static RayCollision RaycastTestMeshObjects(Ray ray, EditorSelection& out) {
    RayCollision best = { false, 1e9f, {0,0,0}, {0,0,0} };
    auto& objs = PawnSystem::Instance().GetMeshObjects();
    for (auto& n : objs) {
        BoundingBox box;
        Vector3 center = n.position;
        if (n.mesh && n.mesh->Valid()) {
            const BoundingBox& mb = n.mesh->Bounds();
            // Mesh::Draw composes scale * RotateY(yaw) * translate, so the pick
            // volume must too. It used to ignore yaw, which made a rotated prop
            // pick from its unrotated footprint.
            const AabbBox local{mb.min.x, mb.min.y, mb.min.z,
                                mb.max.x, mb.max.y, mb.max.z};
            const AabbBox w = RotatedAabbY(local, n.yaw * DEG2RAD,
                                           n.position.x, n.position.y,
                                           n.position.z, n.scale);
            box.min = {w.minX, w.minY, w.minZ};
            box.max = {w.maxX, w.maxY, w.maxZ};
            center = {(w.minX + w.maxX) * 0.5f,
                      (w.minY + w.maxY) * 0.5f,
                      (w.minZ + w.maxZ) * 0.5f};
        } else {
            box.min = {n.position.x - 1.0f, n.position.y - 1.0f, n.position.z - 1.0f};
            box.max = {n.position.x + 1.0f, n.position.y + 1.0f, n.position.z + 1.0f};
        }
        RayCollision hit = GetRayCollisionBox(ray, box);
        if (hit.hit && hit.distance < best.distance) {
            best = hit;
            out = { SelType::MESH, (int)n.id,
                    n.skeletal ? "Mesh.Skeletal" : "Mesh.Static",
                    center, n.scale, n.yaw, true };
        }
    }
    return best;
}

static RayCollision RaycastTestParticleEmitters(Ray ray, EditorSelection& out) {
    RayCollision best = { false, 1e9f, {0,0,0}, {0,0,0} };
    auto& list = PawnSystem::Instance().GetParticleEmitters();
    for (auto& e : list) {
        float r = e.radius + 0.5f;
        BoundingBox box = {{e.position.x - r, e.position.y - r, e.position.z - r},
                           {e.position.x + r, e.position.y + r, e.position.z + r}};
        RayCollision hit = GetRayCollisionBox(ray, box);
        if (hit.hit && hit.distance < best.distance) {
            best = hit;
            out = { SelType::PARTICLE, (int)e.id, "ParticleEmitter", e.position, 1.0f, e.yaw, true };
        }
    }
    return best;
}

static RayCollision RaycastTestPathNodes(Ray ray, EditorSelection& out) {
    RayCollision best = { false, 1e9f, {0,0,0}, {0,0,0} };
    auto& list = PawnSystem::Instance().GetPathNodes();
    for (auto& n : list) {
        float r = n.radius + 0.4f;
        BoundingBox box = {{n.position.x - r, n.position.y - r, n.position.z - r},
                           {n.position.x + r, n.position.y + r, n.position.z + r}};
        RayCollision hit = GetRayCollisionBox(ray, box);
        if (hit.hit && hit.distance < best.distance) {
            best = hit;
            out = { SelType::PATHNODE, (int)n.id, n.name, n.position, 1.0f, 0.0f };
        }
    }
    return best;
}

static RayCollision RaycastTestWindZones(Ray ray, EditorSelection& out) {
    RayCollision best = { false, 1e9f, {0,0,0}, {0,0,0} };
    auto& list = PawnSystem::Instance().GetWindZones();
    for (auto& z : list) {
        RayCollision hit = GetRayCollisionBox(ray, z.bounds);
        if (hit.hit && hit.distance < best.distance) {
            best = hit;
            Vector3 center = {(z.bounds.min.x + z.bounds.max.x) * 0.5f,
                              (z.bounds.min.y + z.bounds.max.y) * 0.5f,
                              (z.bounds.min.z + z.bounds.max.z) * 0.5f};
            out = { SelType::WINDZONE, (int)z.id, "WindZone", center, 1.0f, 0.0f };
        }
    }
    return best;
}
// Fill the rotation/scale fields the Properties panel seeds itself from.
// Defined below (it needs FindPlayerStartById); declared here for the raycast.
static void FillSelectionTransform(EditorSelection& s);

static bool RaycastAllAt(const Ray& ray, EditorSelection& out, float& outDistance) {
    out = { SelType::NONE, -1, "", {0,0,0} };
    RayCollision best = { false, 1e9f, {0,0,0}, {0,0,0} };
    EditorSelection bestSel = { SelType::NONE, -1, "", {0,0,0} };

    auto testWithSel = [&](RayCollision hit, const EditorSelection& sel, float distMul = 1.0f) {
        if (!hit.hit) return;
        // Zones get a distance penalty so solid entities inside them are preferred.
        // The PENALISED distance must be what `best` stores: the old code assigned
        // `best = hit`, i.e. the raw distance, so the penalty vanished as soon as a
        // zone became the current best and the very next solid candidate was
        // compared against the un-penalised number.
        const float d = hit.distance * distMul;
        if (d < best.distance) {
            best = hit;
            best.distance = d;
#ifdef DEBUG_EDITOR_TRACE
            EditorLog("bestNormalHit: x=%f y=%f z=%f", (float)best.normal.x, (float)best.normal.y, (float)best.normal.z);
            EditorLog("bestPointHit: x=%f y=%f z=%f", (float)best.point.x, (float)best.point.y, (float)best.point.z);
#endif   
            
            bestSel = sel;
#ifdef DEBUG_EDITOR_TRACE
            EditorLog("bestSelHit: name=%s idx=%d", bestSel.name.c_str(), (int)bestSel.index);
            EditorLog("bestSelHit: x=%f y=%f z=%f", (float)bestSel.pos.x, (float)bestSel.pos.y, (float)bestSel.pos.z);
#endif
        }
    };

    EditorSelection tmp;
    // RaycastTestBrushes is GONE. It picked from GetCollisionVolumes() - the CSG
    // OUTPUT, whose entries are merged AABBs - and returned that index under the same
    // SelType::BRUSH that RaycastTestOzPrimitives used for a RENDERABLE index. Two
    // index spaces, one selection type, and a brush's volume sits in nearly the same
    // space as its own mesh AABB, so which one you got was decided by a sub-pixel
    // distance difference. Evidence from System/AngelEd.log: four clicks on one spot
    // produced Brush idx=102, then 15, 14, 13, 11, 10, 9, 7.
    //
    // Everything downstream then read that index as a renderable index, which is how
    // Delete could remove a different brush than the one selected.
    //
    // Collision volumes are derived state, regenerated by every RebuildCollisionVolumes,
    // so they were never a sound thing to select. RaycastTestOzPrimitives is now the
    // only producer of SelType::BRUSH and its index space is unambiguously
    // m_renderables.
    testWithSel(RaycastTestOzPrimitives(ray, tmp), tmp);
    testWithSel(RaycastTestModels(ray, tmp), tmp);
    testWithSel(RaycastTestPawns(ray, tmp), tmp);
    testWithSel(RaycastTestPickups(ray, tmp), tmp);
    testWithSel(RaycastTestLights(ray, tmp), tmp);
    testWithSel(RaycastTestZones(ray, tmp), tmp, 1.2f);
    testWithSel(RaycastTestStarts(ray, tmp), tmp);
    testWithSel(RaycastTestPortals(ray, tmp), tmp);
    testWithSel(RaycastTestMeshObjects(ray, tmp), tmp);
    testWithSel(RaycastTestParticleEmitters(ray, tmp), tmp);
    testWithSel(RaycastTestPathNodes(ray, tmp), tmp);
    testWithSel(RaycastTestWindZones(ray, tmp), tmp, 1.2f);

    out = bestSel;
    FillSelectionTransform(out);
    outDistance = best.distance;
    return best.hit;
}

// Nearest pick within the 5x5 pixel click window.
//
// CONTRACT: `out` is ALWAYS written - NONE when nothing was hit. It used to be
// left untouched on a miss, which made "keep the selection" and "I forgot to
// clear the probe" indistinguishable at every call site. Callers that want to
// preserve the selection on a miss must use a local (EditorPickEntity does), and
// the backdrop deselect is explicit in the click handler.
static bool EditorRaycastAt(Vector2 mousePos, EditorSelection& out) {
    out = { SelType::NONE, -1, "", {0,0,0} };
    // UE1 resolves a click against a 5x5 PIXEL window, not an infinitesimally thin
    // ray: UEditorEngine::Click (UnEdClick.cpp) hit-tests the box
    //   HitX = floor(MouseX)-2 .. +3,  HitY = floor(MouseY)-2 .. +3
    // against the renderer's hit buffer, and dispatches to the front-most proxy in
    // it. A single ray makes thin targets - a path node, a light billboard, a fence
    // prop, an NPC's arm - almost unclickable, because being one pixel off means
    // missing entirely.
    //
    // Same window here: five samples (centre + the four 2px cardinals), and the
    // NEAREST hit wins, which is what UE1's depth-sorted hit list dispatches to.
    // Picking is not per-frame work (hover is throttled, picks happen on release),
    // so five AABB sweeps are cheap.
    static const float kOffsets[5][2] = {{0,0}, {-2,0}, {2,0}, {0,-2}, {0,2}};

    float bestDist = 1e9f;
    bool any = false;
    for (const auto& o : kOffsets) {
        const Vector2 p = {mousePos.x + o[0], mousePos.y + o[1]};
        // Pane-aware: raylib's GetMouseRay maps the mouse over the whole window, so
        // in a split it aims the ray at the wrong place. g_activeViewportRect is the
        // pane that owns input (set each frame in Main.cpp).
        const Ray ray = RayFromPane(p, OTEditor.MainCamera, g_activeViewportRect);
        EditorSelection sel;
        float d = 0.0f;
        if (!RaycastAllAt(ray, sel, d)) continue;
        if (!any || d < bestDist) {
            bestDist = d;
            out = sel;
            any = true;
        }
    }
    return any;
}

// ---------------------------------------------------------------------------
// Per-face surface picking (UT99-style: right-click a FACE, not a brush)
//
// The brush raycast above only returns the renderable, so Surface Properties
// had no idea which of the six faces was clicked. This intersects the actual
// triangles of the renderable's mesh and derives the face from the dominant
// axis of the hit triangle's normal - the same rule SurfaceMaterial's per-face
// mesh split uses, so the face the user clicks is provably the face whose
// properties the dialog will edit.
//
// Self-contained Moeller-Trumbore rather than raylib's triangle helper, which
// is not part of the public API in every supported raylib version.
// ---------------------------------------------------------------------------
struct SurfaceFacePick {
    bool hit = false;
    int  renderable = -1;
    oz::surface::SurfaceFace face = oz::surface::FACE_NONE;
    Vector3 point{0, 0, 0};
    Vector3 normal{0, 1, 0};
    float  distance = 0.0f;
};

static bool RayTri(const Ray& ray, const Vector3& v0, const Vector3& v1, const Vector3& v2,
                   float& tOut, Vector3& nOut) {
    const Vector3 e1 = Vector3Subtract(v1, v0);
    const Vector3 e2 = Vector3Subtract(v2, v0);
    const Vector3 p = Vector3CrossProduct(ray.direction, e2);
    const float det = Vector3DotProduct(e1, p);
    if (fabsf(det) < 1e-8f) return false;          // parallel
    const float invDet = 1.0f / det;
    const Vector3 tv = Vector3Subtract(ray.position, v0);
    const float u = Vector3DotProduct(tv, p) * invDet;
    if (u < 0.0f || u > 1.0f) return false;
    const Vector3 q = Vector3CrossProduct(tv, e1);
    const float v = Vector3DotProduct(ray.direction, q) * invDet;
    if (v < 0.0f || u + v > 1.0f) return false;
    const float t = Vector3DotProduct(e2, q) * invDet;
    if (t <= 1e-4f) return false;                  // behind the camera
    tOut = t;
    nOut = Vector3Normalize(Vector3CrossProduct(e1, e2));
    return true;
}

static SurfaceFacePick PickSurfaceFace(Vector2 mousePos) {
    SurfaceFacePick best;
    Ray ray = RayFromPane(mousePos, OTEditor.MainCamera, g_activeViewportRect);
    int count = OzoneLoader::Instance().Count();
    for (int i = 0; i < count; i++) {
        OzoneRenderable* r = OzoneLoader::Instance().Get(i);
        if (!r || !r->loaded || r->model.meshCount == 0) continue;
        Mesh& m = r->model.meshes[0];
        if (!m.vertices || !m.indices) continue;
        // Same visibility rule as the brush raycast, via the same predicate. This
        // guard was MISSING here, so right-clicking an invisible AutoConvex box
        // reported a hit and the context menu offered "Surface Properties" for
        // geometry that could not otherwise be selected - and whose Apply went on
        // to write a face override and rebuild its collision volumes.
        if (!IsEditorPickableSurface(*r)) continue;
        for (int t = 0; t < m.triangleCount; t++) {
            unsigned short i0 = m.indices[t * 3 + 0];
            unsigned short i1 = m.indices[t * 3 + 1];
            unsigned short i2 = m.indices[t * 3 + 2];
            if ((int)i0 >= m.vertexCount || (int)i1 >= m.vertexCount ||
                (int)i2 >= m.vertexCount) continue;
            Vector3 a, b, c;
            // Local -> world, matching Draw()/DrawSurface(): position + scale,
            // yaw about Y. Picking in local space instead would report the
            // wrong face on any rotated brush.
            float rad = r->rotation;
            float cs = cosf(rad), sn = sinf(rad);
            const float* vs[3] = { m.vertices + (size_t)i0 * 3,
                                   m.vertices + (size_t)i1 * 3,
                                   m.vertices + (size_t)i2 * 3 };
            // Bucket the face from the LOCAL triangle normal, not the world one.
            // BuildFaceMeshes splits a brush's mesh by the dominant axis of each
            // triangle's LOCAL normal, while this picker used to map the world
            // normal. On a yawed brush those disagree, so right-clicking the
            // visible +X face applied the edit to a different face's bucket.
            const float e1x = vs[1][0] - vs[0][0], e1y = vs[1][1] - vs[0][1], e1z = vs[1][2] - vs[0][2];
            const float e2x = vs[2][0] - vs[0][0], e2y = vs[2][1] - vs[0][1], e2z = vs[2][2] - vs[0][2];
            const float lnx = e1y * e2z - e1z * e2y;
            const float lny = e1z * e2x - e1x * e2z;
            const float lnz = e1x * e2y - e1y * e2x;
            Vector3 out[3];
            for (int k = 0; k < 3; k++) {
                float lx = vs[k][0] * r->scale;
                float ly = vs[k][1] * r->scale;
                float lz = vs[k][2] * r->scale;
                out[k] = { r->position.x + (lx * cs + lz * sn),
                           r->position.y + ly,
                           r->position.z + (-lx * sn + lz * cs) };
            }
            a = out[0]; b = out[1]; c = out[2];
            float dist = 0.0f; Vector3 n{0, 1, 0};
            if (!RayTri(ray, a, b, c, dist, n)) continue;
            if (best.hit && dist >= best.distance) continue;
            best.hit = true;
            best.renderable = i;
            best.distance = dist;
            best.normal = n;
            best.point = Vector3Add(ray.position, Vector3Scale(ray.direction, dist));
            // Faces are named in the mesh's own (engine Y-up) space, matching
            // BuildFaceMeshes. The LOCAL cross product is what the renderer uses.
            best.face = oz::surface::FaceFromNormal(lnx, lny, lnz);
        }
    }
    return best;
}

// Enter selection adds to the current set instead of replacing it, which is how
// the "(N Selected)" count in the context menu is built up. Plain left-click
// still replaces, so ordinary picking is unchanged.
static std::vector<SurfaceFacePick> g_selectedSurfaces;

static void ToggleSurfacePick(const SurfaceFacePick& sp) {
    for (size_t i = 0; i < g_selectedSurfaces.size(); i++) {
        if (g_selectedSurfaces[i].renderable == sp.renderable &&
            g_selectedSurfaces[i].face == sp.face) {
            g_selectedSurfaces.erase(g_selectedSurfaces.begin() + i);
            return;
        }
    }
    g_selectedSurfaces.push_back(sp);
}

static void ClearSurfacePicks() { g_selectedSurfaces.clear(); }

int ClampPropInt(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// PlayerStart lookups are by node id, not vector index. Both the raycast and
// the WorldGraph hand out `s.id`, and ids are assigned from a counter shared
// with every other entity type, so they are neither 0-based nor contiguous per
// type. Indexing the vector by id (as an earlier revision of the properties
// apply handler did) therefore addressed the wrong node - or none.
PlayerStartNode* FindPlayerStartById(int id) {
    for (auto& s : PawnSystem::Instance().GetPlayerStarts())
        if ((int)s.id == id) return &s;
    return nullptr;
}

// Publish the selected entity's rotation/scale onto the selection.
//
// `EditorSelection::hasRotation` was NEVER set to true anywhere, so
// `propsTargetHasRotation` (seeded from it) was always false and PropsApply's
// rotation writes - the one gate that protects an authored yaw from being zeroed
// by an unrelated Apply - were dead. The Rot row therefore showed 0 for every
// selection and an edit could never be applied. Both pick paths (the viewport
// raycast and the World Hierarchy) go through here, so a rotated brush selected
// from either shows its real angle, and a type with no yaw keeps hasRotation
// false (so Apply never writes 0 over it).
static void FillSelectionTransform(EditorSelection& s) {
    s.rotation = 0.0f;
    s.scale = 1.0f;
    s.hasRotation = false;
    switch (s.type) {
        case SelType::BRUSH: {
            // Renderable rotation is radians; the panel edits degrees.
            OzoneRenderable* r = OzoneLoader::Instance().Get(s.index);
            if (r) {
                s.rotation = r->rotation * RAD2DEG;
                s.scale    = r->scale;
                s.hasRotation = true;
            }
            break;
        }
        case SelType::MESH: {
            MeshObjectNode* m = PawnSystem::Instance().GetMeshObject(s.index);
            if (m) {
                s.rotation = m->yaw;          // already degrees
                s.scale    = m->scale;
                s.hasRotation = true;
            }
            break;
        }
        case SelType::PARTICLE: {
            ParticleEmitterNode* e = PawnSystem::Instance().GetParticleEmitter(s.index);
            if (e) { s.rotation = e->yaw; s.hasRotation = true; }
            break;
        }
        case SelType::SPAWN: {
            PlayerStartNode* p = FindPlayerStartById(s.index);
            if (p) { s.rotation = p->yaw; s.hasRotation = true; }
            break;
        }
        default: break;
    }
}

// Centre the camera on the current selection. Distance and direction are kept,
// so this reads as "look here" rather than as a view reset. The World Hierarchy
// exposes it as Frame Selection on a row.
void FocusSelectionInViewport() {
    if (g_sel.type == SelType::NONE) return;
    Vector3 p = g_sel.pos;
    Vector3 delta = Vector3Subtract(OTEditor.MainCamera.position, OTEditor.MainCamera.target);
    if (Vector3Length(delta) < 0.001f) delta = {8.0f, 8.0f, 8.0f};
    OTEditor.MainCamera.target = p;
    OTEditor.MainCamera.position = Vector3Add(p, delta);
}

void SnapGizmoToSelection(const EditorSelection& sel) {
    AngelTechEditor.X = sel.pos.x;
    AngelTechEditor.Y = sel.pos.y;
    AngelTechEditor.Z = sel.pos.z;
    AngelTechEditor.S = sel.scale > 0.01f ? sel.scale : 1.0f;
    AngelTechEditor.R = sel.rotation;
    if (sel.type == SelType::BRUSH) {
        // Size comes from the RENDERABLE, not the collision volume. sel.index is a
        // renderable index (see the note in EditorRaycastAt), so indexing
        // GetCollisionVolumes() with it read an unrelated CSG volume and reported ITS
        // extent - a merged 180x10x180 slab for a small brush. The gizmo then drew
        // that box, which read as a brush being re-added on every click.
        OzoneRenderable* r = OzoneLoader::Instance().Get(sel.index);
        if (r && r->loaded && r->model.meshCount > 0) {
            // The drawn gizmo is axis-aligned, so report the rotated brush's
            // enclosing extents (the one shared rotation-aware box).
            const BoundingBox w = BrushWorldBox(*r);
            AngelTechEditor.W = w.max.x - w.min.x;
            AngelTechEditor.H = w.max.y - w.min.y;
            AngelTechEditor.L = w.max.z - w.min.z;
        }
    } else if (sel.type == SelType::WINDZONE) {
        auto& zones = PawnSystem::Instance().GetWindZones();
        for (auto& z : zones) {
            if ((int)z.id == sel.index) {
                AngelTechEditor.W = z.bounds.max.x - z.bounds.min.x;
                AngelTechEditor.H = z.bounds.max.y - z.bounds.min.y;
                AngelTechEditor.L = z.bounds.max.z - z.bounds.min.z;
                break;
            }
        }
    } else if (sel.type == SelType::ZONE) {
        auto& zones = ZoneManager::Instance().GetZones();
        for (auto& z : zones) {
            if ((int)z.id == sel.index) {
                AngelTechEditor.W = z.bounds.max.x - z.bounds.min.x;
                AngelTechEditor.H = z.bounds.max.y - z.bounds.min.y;
                AngelTechEditor.L = z.bounds.max.z - z.bounds.min.z;
                break;
            }
        }
    } else if (sel.type == SelType::PORTAL) {
        auto& portals = ZoneManager::Instance().GetPortals();
        if (sel.index >= 0 && sel.index < (int)portals.size()) {
            auto& p = portals[sel.index];
            AngelTechEditor.W = p.bounds.max.x - p.bounds.min.x;
            AngelTechEditor.H = p.bounds.max.y - p.bounds.min.y;
            AngelTechEditor.L = p.bounds.max.z - p.bounds.min.z;
        }
    }
    EditorLog("Gizmo snapped to %s idx=%d", sel.name.c_str(), sel.index);
}

// Put `sel`'s world centre at `centre`. Lifted verbatim out of the frame loop's
// per-frame sync so a MULTI-selection can use the same code: UnrealEd's
// `MoveActors` walks every `bSelected` actor with the same Delta, and a
// Shift-selected set here used to move only its primary.
//
// Every branch writes the CENTRE of the thing, never its origin, because that is
// what the gizmo and the raycast store (see SnapGizmoToSelection): a brush's
// `position` is its top/base and a Mesh.* node's is its origin, so each has to
// translate by the difference between the two.
void MoveSelectionCentre(const EditorSelection& sel, Vector3 centre, bool applyTool) {
    const int idx = sel.index;
    if (sel.type == SelType::NPC) {
        if (Pawn* p = PawnSystem::Instance().Get(idx)) p->position = centre;
    } else if (sel.type == SelType::PICKUP) {
        for (auto& pk : PawnSystem::Instance().GetPickups()) {
            if ((int)pk.id == idx) { pk.position = centre; break; }
        }
    } else if (sel.type == SelType::BRUSH) {
        // idx is a renderable index (see EditorRaycastAt), so there is no
        // collision-volume fallback to try. The old code guessed with
        // `idx < Count()` and otherwise went looking for a matching collision
        // volume, which is how dragging a brush could move a different one.
        OzoneRenderable* r = OzoneLoader::Instance().Get(idx);
        if (r) {
            // Rotation-aware centre (the same box the picker and
            // SnapGizmoToSelection use). The old code used the UNROTATED mesh
            // centre here, so a rotated brush's pivot and its write-back disagreed
            // and the brush slid/jittered as this ran every frame.
            const Vector3 center = BrushCenter(*r);
            const Vector3 oldPos = r->position;
            const float oldScale = r->scale;
            const float oldRot = r->rotation;
            r->position = {centre.x + (oldPos.x - center.x),
                           centre.y + (oldPos.y - center.y),
                           centre.z + (oldPos.z - center.z)};
            // Scale/Rotate tool modes write back to the renderable, primary only.
            if (applyTool && g_editorPanels.currentToolMode == 2) r->scale = AngelTechEditor.S;
            if (applyTool && g_editorPanels.currentToolMode == 3) r->rotation = AngelTechEditor.R * DEG2RAD;
            if (r->position.x != oldPos.x || r->position.y != oldPos.y ||
                r->position.z != oldPos.z || r->scale != oldScale ||
                r->rotation != oldRot) {
                g_editorPanels.collisionDirty = true;
                // Culling must see the NEW transform immediately; otherwise the
                // brush is culled against its old AABB and vanishes as soon as you
                // drag it.
                OzoneLoader::Instance().RefreshRenderableBounds(idx);
            }
        }
    } else if (sel.type == SelType::LIGHT) {
        if (LightNode* l = PawnSystem::Instance().GetLight(idx)) l->position = centre;
    } else if (sel.type == SelType::SPAWN) {
        if (PlayerStartNode* s = FindPlayerStartById(idx)) s->position = centre;
    } else if (sel.type == SelType::PORTAL) {
        auto& portals = ZoneManager::Instance().GetPortals();
        if (idx >= 0 && idx < (int)portals.size()) {
            auto& p = portals[idx];
            Vector3 c = {(p.bounds.min.x + p.bounds.max.x) * 0.5f,
                         (p.bounds.min.y + p.bounds.max.y) * 0.5f,
                         (p.bounds.min.z + p.bounds.max.z) * 0.5f};
            Vector3 d = {centre.x - c.x, centre.y - c.y, centre.z - c.z};
            p.bounds.min.x += d.x; p.bounds.min.y += d.y; p.bounds.min.z += d.z;
            p.bounds.max.x += d.x; p.bounds.max.y += d.y; p.bounds.max.z += d.z;
        }
    } else if (sel.type == SelType::ZONE) {
        // Bounds-centred, like PORTAL. This branch was missing once, so the Move
        // tool changed the gizmo and then wrote nothing.
        for (auto& z : ZoneManager::Instance().GetZones()) {
            if ((int)z.id != idx) continue;
            Vector3 c = {(z.bounds.min.x + z.bounds.max.x) * 0.5f,
                         (z.bounds.min.y + z.bounds.max.y) * 0.5f,
                         (z.bounds.min.z + z.bounds.max.z) * 0.5f};
            Vector3 d = {centre.x - c.x, centre.y - c.y, centre.z - c.z};
            z.bounds.min.x += d.x; z.bounds.min.y += d.y; z.bounds.min.z += d.z;
            z.bounds.max.x += d.x; z.bounds.max.y += d.y; z.bounds.max.z += d.z;
            break;
        }
    } else if (sel.type == SelType::MESH) {
        if (MeshObjectNode* m = PawnSystem::Instance().GetMeshObject(idx)) {
            if (applyTool) m->yaw = AngelTechEditor.R;   // UI works in degrees
            if (applyTool && g_editorPanels.currentToolMode == 2 && AngelTechEditor.S > 0.01f)
                m->scale = AngelTechEditor.S;
            // The gizmo / raycast store the model's world CENTRE, but m->position
            // is the model's ORIGIN (often the feet). MeshCentreOffset is the one
            // definition of that conversion; it reads THIS node's own scale and
            // yaw, so moving a multi-selected prop does not rotate it to the
            // primary's yaw.
            if (m->mesh && m->mesh->Valid()) {
                const Vector3 o = MeshCentreOffset(*m);
                m->position = {centre.x - o.x, centre.y - o.y, centre.z - o.z};
            } else if (g_gizmoDrag) {
                // Unknown local offset (mesh still resolving): only an explicit
                // drag may write the centre. Doing it on every frame made a
                // texture reload shift the prop.
                m->position = centre;
            }
        }
    } else if (sel.type == SelType::PARTICLE) {
        if (ParticleEmitterNode* e = PawnSystem::Instance().GetParticleEmitter(idx)) {
            e->position = centre;
            if (applyTool) e->yaw = AngelTechEditor.R;
        }
    } else if (sel.type == SelType::PATHNODE) {
        if (PathNode* pn = PawnSystem::Instance().GetPathNode(idx)) pn->position = centre;
    } else if (sel.type == SelType::WINDZONE) {
        if (WindZoneNode* z = PawnSystem::Instance().GetWindZone(idx)) {
            Vector3 c = {(z->bounds.min.x + z->bounds.max.x) * 0.5f,
                         (z->bounds.min.y + z->bounds.max.y) * 0.5f,
                         (z->bounds.min.z + z->bounds.max.z) * 0.5f};
            Vector3 d = {centre.x - c.x, centre.y - c.y, centre.z - c.z};
            z->bounds.min.x += d.x; z->bounds.min.y += d.y; z->bounds.min.z += d.z;
            z->bounds.max.x += d.x; z->bounds.max.y += d.y; z->bounds.max.z += d.z;
        }
    } else if (sel.type == SelType::MODEL) {
        if (idx >= 0 && idx < CachedModelCounter) {
            CachedModels[idx].X = centre.x;
            CachedModels[idx].Y = centre.y;
            CachedModels[idx].Z = centre.z;
            if (applyTool) CachedModels[idx].R = AngelTechEditor.R;
            if (applyTool && g_editorPanels.currentToolMode == 2 && AngelTechEditor.S > 0.01f)
                CachedModels[idx].S = AngelTechEditor.S;
        }
    }
}

// UE1's selection modifier is CTRL, not Shift: Click(HActor) does
// `Hit.Actor->bSelected ^= 1` when `Buttons & MOUSE_Ctrl`. We accept BOTH:
//
//   * Shift always toggles membership (the modifier this editor documented first);
//   * Ctrl toggles too, but ONLY while Unreal-style brush editing is OFF, because
//     in that mode Ctrl+click is the transform gesture and must not select.
//
// Returns true when the click should EXTEND/toggle the set instead of replacing it.
bool SelectionModifierHeld() {
    if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) return true;
    const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    return ctrl && !g_editorPanels.unrealBrushMove;
}

// Drop the whole selection (HBackdrop::Click). Kept in one place because the
// fields below must move together: a leftover DrawModel re-arms the gizmo at the
// old position on the next frame, and a leftover placement ghost would place the
// next time the viewport is clicked.
void ClearSelection() {
    g_sel = { SelType::NONE, -1, "", {0,0,0} };
    g_hoverSel = { SelType::NONE, -1, "", {0,0,0} };
    g_selSet.clear();
    AngelTechEditor.DrawModel = false;
    g_placementArmed = false;
    AngelTechEditor.ActiveModelPath.clear();
}

// Apply an already-resolved pick. Split out of EditorPickEntity so the click
// handler can decide double-click / "was it already selected" from ONE raycast.
//
// `allowToggle`: left-click paths pass true, the context-menu path false. It
// gates the MODIFIER toggle only - a plain click is IDEMPOTENT.
//
// A plain left click on the entity that is already selected must KEEP it. This
// used to deselect ("click the selected object again to drop it"), which became
// a bug the moment picking moved to PRESS (UnEdClick.cpp parity): the hit is
// resolved on button-down and the drag then moves whatever is selected, so
// deselecting on that same press meant a Move/Scale/Rotate gesture started with
// nothing selected - reported as "I try to move the brush and it deselects
// itself". UE1 toggles membership with MOUSE_Ctrl (`bSelected ^= 1`), never on a
// plain click; empty space is what clears the selection (HBackdrop::Click, which
// the caller applies).
//
// Returns true only when `hit` is true. On a miss NOTHING is touched - the caller
// decides whether that means "clear the selection" (HBackdrop) or "keep it" (the
// context-menu path must not target an entity the user did not click).
static bool EditorPickEntityAt(const EditorSelection& probe, bool hit,
                               bool allowToggle) {
    if (!hit) return false;

    const EditorSelection prevSel = g_sel;
    g_sel = probe;

    // UE1 keeps one bSelected flag per actor and every operation (delete,
    // duplicate, MoveActors) walks the whole set, so multi-select is the base
    // model there. g_sel stays the PRIMARY (the gizmo/Properties target); g_selSet
    // is the full set every bulk operation acts on.
    //
    // The membership rule is a pure function so it can be pinned headlessly
    // (tests/SelectionSet.test.cpp): a plain click on an already-selected entity
    // KEEPS the set, because the same press starts the transform gesture.
    bool alreadyInSet = false;
    for (const auto& s : g_selSet) {
        if (s.type == g_sel.type && s.index == g_sel.index) {
            alreadyInSet = true;
            break;
        }
    }
    const edsel::PickAction action =
        edsel::ResolvePickAction(allowToggle, SelectionModifierHeld(), alreadyInSet);
    const bool extend = (action == edsel::PickAction::Toggle);

    // A ZONE pick while something else is selected clears instead of switching:
    // zones are huge volumes (one usually contains the camera), so a click that
    // merely lands inside one must not steal the selection from the object the
    // user is about to transform.
    //
    // Gated on PickAction::Replace - i.e. "this would otherwise replace the
    // set". It must NOT fire for Keep (the zone is already selected, and this
    // press is about to drag it) or Toggle, which would make a selected zone
    // undeselectable by clicking it.
    if (action == edsel::PickAction::Replace && allowToggle &&
        prevSel.type != SelType::NONE && g_sel.type == SelType::ZONE) {
        ClearSelection();
        return true;
    }
    if (extend) {
        bool removed = false;
        for (auto it = g_selSet.begin(); it != g_selSet.end(); ++it) {
            if (it->type == g_sel.type && it->index == g_sel.index) {
                g_selSet.erase(it);
                removed = true;
                break;
            }
        }
        if (!removed) g_selSet.push_back(g_sel);
        if (g_selSet.empty()) {
            ClearSelection();
            return true;
        }
        if (removed) g_sel = g_selSet.back();   // promote another member to primary
    } else if (action == edsel::PickAction::Replace) {
        g_selSet.clear();
        g_selSet.push_back(g_sel);
    }
    // PickAction::Keep: the set is untouched and g_sel is already the primary.
    EditorLog("Selected: %s (type=%d idx=%d)  [%zu selected]",
              g_sel.name.c_str(), (int)g_sel.type, g_sel.index, g_selSet.size());
    AngelTechEditor.DrawModel = true;
    SnapGizmoToSelection(g_sel);
    if (g_sel.type == SelType::PORTAL) {
        // Portals have no list window any more; the Entity Properties panel
        // edits them, so picking one in the viewport opens it there.
        ShowPropertiesPanel(true);
        g_editorPanels.portalTargetWorld = g_sel.name;
    }
    return true;
}

// Raycast at the CURRENT mouse position and apply the pick. Callers that already
// resolved it (the press handler, which also needs the double-click test) use
// EditorPickEntityAt directly so one click costs one raycast sweep.
static bool EditorPickEntity(bool allowToggle = true) {
    EditorSelection probe;
    const bool hit = EditorRaycastAt(GetMousePosition(), probe);
    return EditorPickEntityAt(probe, hit, allowToggle);
}

static void EditorHoverEntity() {
    Vector2 mousePos = GetMousePosition();
    EditorRaycastAt(mousePos, g_hoverSel);   // writes NONE on a miss
}


// ---------------------------------------------------------------------------
// Entity action functions (called from native context menu + WorldGraph)
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Multi-select copy/paste (Ctrl+C / Ctrl+V). A brush clipboard: everything that
// AddBrushRenderable + the surface block need to recreate the brush, so a paste
// does not depend on the source still existing (copy, delete, paste works).
// ---------------------------------------------------------------------------
namespace {
struct BrushClip {
    int typeId = 0;
    Vector3 pos{0,0,0};
    Vector3 size{1,1,1};
    float rotDeg = 0.0f;
    float scale = 1.0f;
    int csgOp = 0;
    int surfaceFlags = 0;
    float cylRTop = 0.0f, cylRBot = 0.0f;
    int cylSlices = 0;
    oz::surface::BrushSurface surface;
    int texSlot = 0;
    float texScaleU = 1.0f, texScaleV = 1.0f, texOffsetU = 0.0f, texOffsetV = 0.0f;
};
// ONE clipboard entry: the TYPE plus a COPY of that node. A copy (not a pointer
// into the live container) is what makes copy -> delete -> paste work, and a
// tagged struct (rather than a serialised OZONE line) avoids a second parser that
// would have to stay in step with OzoneParser.
//
// UnrealEd's clipboard is text - `edactCopySelected` exports the actors and
// `edactPasteSelected` imports them at +32 and SELECTS them - but the property
// that matters is the same: the clipboard is independent of the live objects, and
// a paste creates NEW objects. **NEW IDs are the load-bearing half of that.** A
// cloned light used to keep its source's id, and every Remove*(id) is a
// remove_if, so deleting either copy destroyed BOTH - which is exactly the
// "copying a light deletes the selected light" report. AddLight/AddPickup/
// AddPlayerStart/AddZone only allocate when `id == 0`, so every clone must zero
// it (AddMeshObject/AddParticleEmitter/AddPathNode/AddWindZone already did).
struct ClipEntry {
    SelType type = SelType::NONE;
    std::string name;                // carried so the pasted set is addressable
    BrushClip brush;                 // BRUSH
    LightNode light;                 // LIGHT
    PickupNode pickup;               // PICKUP
    PlayerStartNode spawn;           // SPAWN
    ZoneVolumeNode zone;             // ZONE
    ZonePortal portal;               // PORTAL
    MeshObjectNode mesh;             // MESH
    ParticleEmitterNode particle;    // PARTICLE
    PathNode path;                   // PATHNODE
    WindZoneNode wind;               // WINDZONE
    std::string npcDef;              // NPC: the def name + yaw is the payload
    float npcYaw = 0.0f;
    Vector3 npcPos{0, 0, 0};
};

std::vector<ClipEntry> g_clip;
int g_pasteCount = 0;

bool ClipOneBrush(int idx, BrushClip& c) {
    OzoneRenderable* r = OzoneLoader::Instance().Get(idx);
    if (!r || !r->loaded || r->model.meshCount <= 0) return false;
    c.typeId = r->typeId;
    c.pos = BrushCenter(*r);
    const BoundingBox mb = GetMeshBoundingBox(r->model.meshes[0]);
    c.size = {mb.max.x - mb.min.x, mb.max.y - mb.min.y, mb.max.z - mb.min.z};
    c.rotDeg = r->rotation * RAD2DEG;
    c.scale = r->scale;
    c.csgOp = r->csgOp;
    c.surfaceFlags = r->surfaceFlags;
    c.cylRTop = r->cylRTop;
    c.cylRBot = r->cylRBot;
    c.cylSlices = r->cylSlices;
    c.surface = r->surface;
    c.texSlot = r->texSlot;
    c.texScaleU = r->texScaleU;
    c.texScaleV = r->texScaleV;
    c.texOffsetU = r->texOffsetU;
    c.texOffsetV = r->texOffsetV;
    return true;
}

// Harvest one selection into a clipboard record. False for the level itself, an
// unsupported type, or a node that has since gone - so a stale set entry cannot
// silently paste an empty object.
bool ClipOne(const EditorSelection& s, ClipEntry& e) {
    e.type = s.type;
    e.name = s.name;
    switch (s.type) {
        case SelType::BRUSH:
            return ClipOneBrush(s.index, e.brush);
        case SelType::LIGHT:
            if (LightNode* l = PawnSystem::Instance().GetLight(s.index)) { e.light = *l; return true; }
            return false;
        case SelType::PICKUP:
            for (const auto& pk : PawnSystem::Instance().GetPickups())
                if ((int)pk.id == s.index) { e.pickup = pk; return true; }
            return false;
        case SelType::SPAWN:
            if (PlayerStartNode* p = FindPlayerStartById(s.index)) { e.spawn = *p; return true; }
            return false;
        case SelType::ZONE:
            for (const auto& z : ZoneManager::Instance().GetZones())
                if ((int)z.id == s.index) { e.zone = z; return true; }
            return false;
        case SelType::PORTAL: {
            auto& ps = ZoneManager::Instance().GetPortals();
            if (s.index < 0 || s.index >= (int)ps.size()) return false;
            e.portal = ps[s.index];
            return true;
        }
        case SelType::MESH:
            if (MeshObjectNode* m = PawnSystem::Instance().GetMeshObject(s.index)) { e.mesh = *m; return true; }
            return false;
        case SelType::PARTICLE:
            if (ParticleEmitterNode* p = PawnSystem::Instance().GetParticleEmitter(s.index)) { e.particle = *p; return true; }
            return false;
        case SelType::PATHNODE:
            if (PathNode* p = PawnSystem::Instance().GetPathNode(s.index)) { e.path = *p; return true; }
            return false;
        case SelType::WINDZONE:
            if (WindZoneNode* w = PawnSystem::Instance().GetWindZone(s.index)) { e.wind = *w; return true; }
            return false;
        case SelType::NPC: {
            Pawn* p = PawnSystem::Instance().Get(s.index);
            if (!p) return false;
            e.npcDef = p->defName;
            e.npcYaw = p->yaw;
            e.npcPos = p->position;
            return true;
        }
        default:
            return false;
    }
}

// Re-create one clipboard record at +off on X/Z. Every id is zeroed, so the
// result is a genuinely NEW object rather than a second name for the source.
EditorSelection PasteOne(const ClipEntry& e, float off) {
    EditorSelection out = { SelType::NONE, -1, "", {0,0,0} };
    switch (e.type) {
        case SelType::BRUSH: {
            BrushClip c = e.brush;
            c.pos.x += off; c.pos.z += off;
            const int idx = OzoneLoader::Instance().AddBrushRenderable(
                c.typeId, c.pos, c.size, c.rotDeg, c.scale, c.csgOp,
                c.surfaceFlags, c.cylRTop, c.cylRBot, c.cylSlices);
            if (idx < 0) break;
            if (OzoneRenderable* d = OzoneLoader::Instance().Get(idx)) {
                d->texSlot = c.texSlot;
                d->texScaleU = c.texScaleU;
                d->texScaleV = c.texScaleV;
                d->texOffsetU = c.texOffsetU;
                d->texOffsetV = c.texOffsetV;
                d->surface = c.surface;
                // surface.def owns the derived mirrors; re-derive after the copy.
                DeriveLegacyRenderable(*d);
                OzoneLoader::Instance().RebuildSurfaceMeshes(idx);
            }
            out = { SelType::BRUSH, idx, e.name, c.pos };
            break;
        }
        case SelType::LIGHT: {
            LightNode n = e.light;
            n.id = 0;                       // NEW id - see ClipEntry's note
            n.position.x += off; n.position.z += off;
            out = { SelType::LIGHT, PawnSystem::Instance().AddLight(n), e.name, n.position };
            break;
        }
        case SelType::PICKUP: {
            PickupNode n = e.pickup;
            n.id = 0;
            n.active = true;    // a pasted pickup is a fresh, collectable one
            n.position.x += off; n.position.z += off;
            out = { SelType::PICKUP, PawnSystem::Instance().AddPickup(n), e.name, n.position };
            break;
        }
        case SelType::SPAWN: {
            PlayerStartNode n = e.spawn;
            n.id = 0;
            n.position.x += off; n.position.z += off;
            PawnSystem::Instance().AddPlayerStart(n);
            auto& starts = PawnSystem::Instance().GetPlayerStarts();
            if (starts.empty()) break;
            out = { SelType::SPAWN, (int)starts.back().id, e.name, starts.back().position };
            break;
        }
        case SelType::ZONE: {
            ZoneVolumeNode n = e.zone;
            n.id = 0;
            n.bounds.min.x += off; n.bounds.min.z += off;
            n.bounds.max.x += off; n.bounds.max.z += off;
            Vector3 c = {(n.bounds.min.x + n.bounds.max.x) * 0.5f,
                         (n.bounds.min.y + n.bounds.max.y) * 0.5f,
                         (n.bounds.min.z + n.bounds.max.z) * 0.5f};
            out = { SelType::ZONE, ZoneManager::Instance().AddZone(n), e.name, c };
            break;
        }
        case SelType::PORTAL: {
            ZonePortal p = e.portal;
            p.bounds.min.x += off; p.bounds.min.z += off;
            p.bounds.max.x += off; p.bounds.max.z += off;
            Vector3 c = {(p.bounds.min.x + p.bounds.max.x) * 0.5f,
                         (p.bounds.min.y + p.bounds.max.y) * 0.5f,
                         (p.bounds.min.z + p.bounds.max.z) * 0.5f};
            const int idx = ZoneManager::Instance().AddPortal(p);
            out = { SelType::PORTAL, idx, e.name, c };
            break;
        }
        case SelType::MESH: {
            // The centre offset comes from the SOURCE node: the clone's mesh is
            // deliberately reset so MeshCache resolves it fresh.
            const Vector3 localCentre = MeshCentreOffset(e.mesh);
            MeshObjectNode m = e.mesh;
            m.id = 0;
            m.mesh.reset();
            m.position.x += off; m.position.z += off;
            const int id = PawnSystem::Instance().AddMeshObject(m);
            out = { SelType::MESH, id, e.name,
                    {m.position.x + localCentre.x, m.position.y + localCentre.y,
                     m.position.z + localCentre.z} };
            break;
        }
        case SelType::PARTICLE: {
            ParticleEmitterNode n = e.particle;
            n.id = 0;
            n.position.x += off; n.position.z += off;
            out = { SelType::PARTICLE, PawnSystem::Instance().AddParticleEmitter(n), e.name, n.position };
            break;
        }
        case SelType::PATHNODE: {
            static int s_pathDupCounter = 1000;
            PathNode n = e.path;
            n.id = 0;
            n.name = "path_" + std::to_string(s_pathDupCounter++);   // names stay unique
            n.next.clear();                                          // copies start unlinked
            n.position.x += off; n.position.z += off;
            out = { SelType::PATHNODE, PawnSystem::Instance().AddPathNode(n), n.name, n.position };
            break;
        }
        case SelType::WINDZONE: {
            WindZoneNode n = e.wind;
            n.id = 0;
            n.bounds.min.x += off; n.bounds.min.z += off;
            n.bounds.max.x += off; n.bounds.max.z += off;
            Vector3 c = {(n.bounds.min.x + n.bounds.max.x) * 0.5f,
                         (n.bounds.min.y + n.bounds.max.y) * 0.5f,
                         (n.bounds.min.z + n.bounds.max.z) * 0.5f};
            out = { SelType::WINDZONE, PawnSystem::Instance().AddWindZone(n), e.name, c };
            break;
        }
        case SelType::NPC: {
            Vector3 p = {e.npcPos.x + off, e.npcPos.y, e.npcPos.z + off};
            const int id = PawnSystem::Instance().Spawn(p, e.npcDef.c_str());
            if (id < 0) break;      // unknown def: nothing was spawned
            if (Pawn* np = PawnSystem::Instance().Get(id)) np->yaw = e.npcYaw;
            out = { SelType::NPC, id, e.name, p };
            break;
        }
        default:
            break;
    }
    return out;
}
} // namespace

// Harvest the live selection into `out`. The whole set, exactly like UnrealEd,
// which copies every bSelected actor.
static void HarvestSelection(std::vector<ClipEntry>& out) {
    out.clear();
    auto grab = [&out](const EditorSelection& s) {
        ClipEntry e;
        if (ClipOne(s, e)) out.push_back(std::move(e));
    };
    if (g_selSet.size() > 1) {
        for (const auto& s : g_selSet) grab(s);
    } else if (g_sel.type != SelType::NONE) {
        grab(g_sel);
    }
}

// Create one copy of every record at +X/+Z `off`, then SELECT the result.
// UnrealEd selects what it pasted (edactPasteSelected) and what it duplicated
// (edactDuplicateSelected deselects the sources and selects the copies), so the
// new objects are immediately draggable as a group - and it is obvious the
// originals are still there, which is the half that used to be broken for lights.
static size_t PasteEntries(const std::vector<ClipEntry>& entries, float off) {
    std::vector<EditorSelection> made;
    bool brushes = false, portals = false, lights = false;
    for (const auto& e : entries) {
        EditorSelection sel = PasteOne(e, off);
        if (sel.type == SelType::NONE) continue;
        made.push_back(sel);
        brushes |= (sel.type == SelType::BRUSH);
        portals |= (sel.type == SelType::PORTAL);
        lights  |= (sel.type == SelType::LIGHT);
    }
    if (brushes) OzoneLoader::Instance().RebuildCollisionVolumes();
    if (portals) { RefreshPortalList(); RefreshLevelList(); }
    if (lights)  PawnSystem::Instance().AssignLightZones();

    if (!made.empty()) {
        g_sel = made.back();
        g_selSet = made;
        AngelTechEditor.DrawModel = true;
        SnapGizmoToSelection(g_sel);
    }
    MarkWorldGraphDirty();
    return made.size();
}

void CopySelectedEntities() {
    HarvestSelection(g_clip);
    g_pasteCount = 0;
    EditorLog("Copied %zu entit%s", g_clip.size(), g_clip.size() == 1 ? "y" : "ies");
}

void PasteEntities() {
    if (g_clip.empty()) return;
    // UTransBuffer::Begin equivalent: snapshot BEFORE the mutation, so one undo
    // removes the whole pasted set rather than one object of it.
    HistoryPush();
    const size_t n = PasteEntries(g_clip, 2.0f * (float)(++g_pasteCount));
    EditorLog("Pasted %zu entit%s", n, n == 1 ? "y" : "ies");
}

// Apply the Texture Manager's active texture to the live selection. ONE caller-side
// implementation shared by the viewport's right-click menu and the WorldGraph menu:
// it posts ed::Ev::ApplyTextureToSelection (the dispatcher's single apply path) once
// per selected entity, carrying the faces that were picked for a brush (0 = the
// brush-wide default).
//
// The old context-menu path set a one-shot flag whose drain had its own copy of the
// apply logic, and that copy never learned about Mesh.* - so "apply texture" did
// nothing on a placed prop. Both menus now route here.
void ApplyActiveTextureToSelection() {
    const std::string& tex = g_editorPanels.activeTexturePath;
    if (tex.empty()) {
        EditorLog("Apply texture: no active texture selected");
        return;
    }
    std::vector<EditorSelection> targets;
    if (g_selSet.size() > 1) targets = g_selSet;
    else targets.push_back(g_sel);

    int posted = 0;
    for (const auto& t : targets) {
        if (t.type == SelType::NONE) continue;
        ed::TextureSelectionApply apply;
        apply.path = tex;
        apply.target = ed::SelRef{ ToBusKind(t.type), t.index };
        if (t.type == SelType::BRUSH) {
            for (const auto& sp : g_selectedSurfaces) {
                if (sp.renderable != t.index) continue;
                if (sp.face < 0 || sp.face >= oz::surface::FACE_COUNT) continue;
                apply.faceMask |= (1u << (int)sp.face);
            }
        }
        ed::EventBus::instance().post(ed::Ev::ApplyTextureToSelection, apply);
        posted++;
    }
    EditorLog("Apply texture '%s' -> %d entit%s", tex.c_str(), posted,
              posted == 1 ? "y" : "ies");
}

// ---------------------------------------------------------------------------
// Snap to Ground / Side
// ---------------------------------------------------------------------------

// World AABB of a selection, in the same "centre" terms the gizmo and the picker
// use. Anything without real extent gets a small box around its centre, so the
// queries still have something to work with instead of skipping the entity.
static bool SelectionWorldBounds(const EditorSelection& sel, BoundingBox& out) {
    switch (sel.type) {
        case SelType::BRUSH: {
            OzoneRenderable* r = OzoneLoader::Instance().Get(sel.index);
            if (!r || !r->loaded) return false;
            out = BrushWorldBox(*r);
            return true;
        }
        case SelType::MESH: {
            MeshObjectNode* m = PawnSystem::Instance().GetMeshObject(sel.index);
            if (!m) return false;
            float hx = 0.5f, hy = 0.5f, hz = 0.5f;
            if (m->mesh && m->mesh->Valid()) {
                const BoundingBox& b = m->mesh->Bounds();
                hx = fabsf(b.max.x - b.min.x) * 0.5f * m->scale;
                hy = fabsf(b.max.y - b.min.y) * 0.5f * m->scale;
                hz = fabsf(b.max.z - b.min.z) * 0.5f * m->scale;
            }
            // Axis-aligned: yaw is deliberately ignored, exactly like the frustum
            // cull, so the box can only over-cover.
            out = {{sel.pos.x - hx, sel.pos.y - hy, sel.pos.z - hz},
                   {sel.pos.x + hx, sel.pos.y + hy, sel.pos.z + hz}};
            return true;
        }
        case SelType::ZONE:
        case SelType::WINDZONE: {
            // Stored as bounds, already centred on sel.pos for zones; read the live
            // node so a stale sel.pos cannot offset the snap.
            if (sel.type == SelType::ZONE) {
                for (const auto& z : ZoneManager::Instance().GetZones())
                    if ((int)z.id == sel.index) { out = z.bounds; return true; }
            } else if (WindZoneNode* z = PawnSystem::Instance().GetWindZone(sel.index)) {
                out = z->bounds; return true;
            }
            return false;
        }
        case SelType::PORTAL: {
            auto& ps = ZoneManager::Instance().GetPortals();
            if (sel.index < 0 || sel.index >= (int)ps.size()) return false;
            out = ps[sel.index].bounds;
            return true;
        }
        case SelType::PARTICLE: {
            ParticleEmitterNode* e = PawnSystem::Instance().GetParticleEmitter(sel.index);
            if (!e) return false;
            const float r = e->radius > 0.01f ? e->radius : 0.5f;
            out = {{e->position.x - r, e->position.y - 0.5f, e->position.z - r},
                   {e->position.x + r, e->position.y + 0.5f, e->position.z + r}};
            return true;
        }
        case SelType::NPC: {
            Pawn* p = PawnSystem::Instance().Get(sel.index);
            if (!p) return false;
            out = {{p->position.x - 1.0f, p->position.y, p->position.z - 1.0f},
                   {p->position.x + 1.0f, p->position.y + 2.8f, p->position.z + 1.0f}};
            return true;
        }
        case SelType::NONE:
        case SelType::MAP:
        case SelType::MODEL:      // legacy WDL cache: no world box we can trust
            return false;
        default:
            out = {{sel.pos.x - 0.5f, sel.pos.y - 0.5f, sel.pos.z - 0.5f},
                   {sel.pos.x + 0.5f, sel.pos.y + 0.5f, sel.pos.z + 0.5f}};
            return true;
    }
}

// The collision AABBs the game will actually use, as SnapMath boxes.
static void CollectSnapSolids(std::vector<ozsnap::Box>& out) {
    out.clear();
    for (const auto& v : OzoneLoader::Instance().GetCollisionVolumes()) {
        // A heightmap volume is a terrain stand-in handled by the ground clamp, not
        // a wall or a platform, so it must not become a side target.
        if (v.isHeightmap) continue;
        out.push_back({v.aabb.min.x, v.aabb.min.y, v.aabb.min.z,
                       v.aabb.max.x, v.aabb.max.y, v.aabb.max.z});
    }
}

static std::vector<EditorSelection> SelectionMembers() {
    std::vector<EditorSelection> m;
    if (g_selSet.size() > 1) m = g_selSet;
    else m.push_back(g_sel);
    return m;
}

void SnapSelectionToGround() {
    if (g_sel.type == SelType::NONE || g_sel.type == SelType::MAP) return;
    std::vector<ozsnap::Box> solids;
    CollectSnapSolids(solids);

    std::vector<EditorSelection> members = SelectionMembers();
    struct Move { int i; float dy; };
    std::vector<Move> moves;
    for (int i = 0; i < (int)members.size(); i++) {
        BoundingBox b;
        if (!SelectionWorldBounds(members[i], b)) continue;
        const ozsnap::Box box = {b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z};
        // The heightmap is the floor where no solid is under the entity.
        float fallback = SampleHeightmapGroundY(members[i].pos.x, members[i].pos.z);
        if (fallback <= -50000.0f) fallback = box.minY;   // no terrain: stay put
        const float groundY = ozsnap::GroundBelow(box, solids.data(), (int)solids.size(), fallback);
        const float dy = groundY - box.minY;
        if (fabsf(dy) > 1e-4f) moves.push_back({i, dy});
    }
    if (moves.empty()) {
        EditorLog("Snap to ground: already on the floor");
        return;
    }

    HistoryPush();
    // Each member finds ITS OWN floor (UnrealEd's ACTOR SNAPTOFLOOR), so a crate on
    // a ledge and a barrel on the ground both land correctly instead of the whole
    // set moving by one delta.
    for (const auto& mv : moves) {
        const Vector3 p = {members[mv.i].pos.x, members[mv.i].pos.y + mv.dy,
                           members[mv.i].pos.z};
        MoveSelectionCentre(members[mv.i], p, false);
        for (auto& s : g_selSet)
            if (s.type == members[mv.i].type && s.index == members[mv.i].index) s.pos = p;
        if (members[mv.i].type == g_sel.type && members[mv.i].index == g_sel.index)
            g_sel.pos = p;
    }
    if (g_sel.type != SelType::NONE) SnapGizmoToSelection(g_sel);
    MarkWorldGraphDirty();
    EditorLog("Snap to ground: %zu entit%s", moves.size(), moves.size() == 1 ? "y" : "ies");
}

void SnapSelectionToSide() {
    if (g_sel.type == SelType::NONE || g_sel.type == SelType::MAP) return;
    std::vector<ozsnap::Box> solids;
    CollectSnapSolids(solids);

    // Push along the view's right axis, snapped to the nearest cardinal: the
    // collision world is AABB, so an arbitrary diagonal push cannot be honoured
    // exactly, and a diagonal would read as the object sliding sideways.
    const Vector3 fwd = Vector3Normalize(
        Vector3Subtract(OTEditor.MainCamera.target, OTEditor.MainCamera.position));
    Vector3 right = Vector3CrossProduct(fwd, {0.0f, 1.0f, 0.0f});
    if (Vector3Length(right) < 1e-4f) right = {1.0f, 0.0f, 0.0f};   // top-down view
    float dx = 0.0f, dz = 0.0f;
    ozsnap::CardinalFrom(right.x, right.z, dx, dz);

    std::vector<EditorSelection> members = SelectionMembers();
    // Never drag an object across the level to reach a distant wall.
    const float kMaxReach = 200.0f;

    struct Move { int i; float d; };
    std::vector<Move> moves;
    for (int i = 0; i < (int)members.size(); i++) {
        BoundingBox b;
        if (!SelectionWorldBounds(members[i], b)) continue;
        const ozsnap::Box box = {b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z};
        float d = 0.0f;
        if (!ozsnap::PushAgainstSideLimited(box, solids.data(), (int)solids.size(),
                                            dx, dz, kMaxReach, d))
            continue;
        if (fabsf(d) > 1e-4f) moves.push_back({i, d});
    }
    if (moves.empty()) {
        EditorLog("Snap to side: nothing within %.0f units along (%+.0f,%+.0f)",
                  kMaxReach, dx, dz);
        return;
    }

    HistoryPush();
    for (const auto& mv : moves) {
        const Vector3 p = {members[mv.i].pos.x + dx * mv.d,
                           members[mv.i].pos.y,
                           members[mv.i].pos.z + dz * mv.d};
        MoveSelectionCentre(members[mv.i], p, false);
        for (auto& s : g_selSet)
            if (s.type == members[mv.i].type && s.index == members[mv.i].index) s.pos = p;
        if (members[mv.i].type == g_sel.type && members[mv.i].index == g_sel.index)
            g_sel.pos = p;
    }
    if (g_sel.type != SelType::NONE) SnapGizmoToSelection(g_sel);
    MarkWorldGraphDirty();
    EditorLog("Snap to side: %zu entit%s along (%+.0f,%+.0f)",
              moves.size(), moves.size() == 1 ? "y" : "ies", dx, dz);
}

static void DeleteSelectedEntity() {
    if (g_sel.type == SelType::NONE) return;

    // The WHOLE selection, every type - UnrealEd's edactDeleteSelected walks the
    // level destroying each bSelected actor and nothing else. This used to delete
    // only brushes from a multi-selection and leave the rest selected-but-alive,
    // and a cloned light shared its source's id so RemoveLight(id) removed BOTH.
    //
    // HistoryPush is the analogue of UTransBuffer::Begin (UnEdTran.cpp): the
    // snapshot is taken BEFORE the mutation so one undo restores the whole set,
    // not one object of it.
    std::vector<EditorSelection> victims;
    if (g_selSet.size() > 1) victims = g_selSet;
    else victims.push_back(g_sel);

    HistoryPush();

    // Containers addressed BY POSITION must be emptied high-to-low, or each erase
    // shifts the indices still to be removed. Brushes are renderable indices and
    // portals are vector positions; every other type is addressed by an id that
    // Remove* looks up, so order does not matter there.
    std::vector<int> brushIdx, portalIdx;
    for (const auto& s : victims) {
        if (s.type == SelType::BRUSH && s.index >= 0) brushIdx.push_back(s.index);
        else if (s.type == SelType::PORTAL && s.index >= 0) portalIdx.push_back(s.index);
    }
    std::sort(brushIdx.begin(), brushIdx.end(), [](int a, int b) { return a > b; });
    std::sort(portalIdx.begin(), portalIdx.end(), [](int a, int b) { return a > b; });
    for (int i : brushIdx) OzoneLoader::Instance().RemoveRenderable(i);
    for (int i : portalIdx) ZoneManager::Instance().RemovePortal(i);

    size_t deleted = brushIdx.size() + portalIdx.size();
    auto& pawns = PawnSystem::Instance();
    for (const auto& s : victims) {
        switch (s.type) {
            case SelType::NPC:        pawns.Despawn(s.index);                    ++deleted; break;
            case SelType::PICKUP:     pawns.RemovePickup(s.index);               ++deleted; break;
            case SelType::LIGHT:      pawns.RemoveLight(s.index);                ++deleted; break;
            case SelType::SPAWN:      pawns.RemovePlayerStart(s.index);          ++deleted; break;
            case SelType::ZONE:       ZoneManager::Instance().RemoveZone(s.index);       ++deleted; break;
            case SelType::MESH:       pawns.RemoveMeshObject(s.index);           ++deleted; break;
            case SelType::PARTICLE:   pawns.RemoveParticleEmitter(s.index);      ++deleted; break;
            case SelType::PATHNODE:   pawns.RemovePathNode(s.index);             ++deleted; break;
            case SelType::WINDZONE:   pawns.RemoveWindZone(s.index);             ++deleted; break;
            default: break;   // BRUSH/PORTAL done above; MODEL (legacy WDL) has no delete path
        }
    }

    if (!brushIdx.empty()) OzoneLoader::Instance().RebuildCollisionVolumes();
    if (!portalIdx.empty()) { RefreshPortalList(); RefreshLevelList(); }
    if (std::any_of(victims.begin(), victims.end(),
                    [](const EditorSelection& s) { return s.type == SelType::LIGHT; }))
        pawns.AssignLightZones();

    g_sel = { SelType::NONE, -1, "", {0,0,0} };
    g_hoverSel = { SelType::NONE, -1, "", {0,0,0} };
    g_selSet.clear();
    AngelTechEditor.DrawModel = false; // drop the gizmo so picking isn't gated
    MarkWorldGraphDirty();
    EditorLog("Deleted %zu entit%s", deleted, deleted == 1 ? "y" : "ies");
}

static void DuplicateSelectedEntity() {
    if (g_sel.type == SelType::NONE) return;
    // UnrealEd's edactDuplicateSelected spawns a copy of every selected actor and
    // SELECTS the copies (the sources are deselected). That is exactly
    // copy-then-paste, so there is one implementation of "clone these objects" -
    // and therefore one place that has to remember a clone needs a NEW id.
    //
    // It harvests into a LOCAL record set rather than g_clip: Ctrl+D must not
    // overwrite what the user put on the clipboard with Ctrl+C.
    std::vector<ClipEntry> tmp;
    HarvestSelection(tmp);
    if (tmp.empty()) return;
    HistoryPush();
    const size_t n = PasteEntries(tmp, 2.0f);
    EditorLog("Duplicated %zu entit%s", n, n == 1 ? "y" : "ies");
}


static void OpenPropertiesForSelection() {
    if (g_sel.type == SelType::NONE) return;
    // Forward selection to the native Properties panel
    g_editorPanels.propsTargetType = (int)g_sel.type;
    g_editorPanels.propsTargetIndex = g_sel.index;
    g_editorPanels.propsTargetName = g_sel.name;
    g_editorPanels.propsTargetPos[0] = g_sel.pos.x;
    g_editorPanels.propsTargetPos[1] = g_sel.pos.y;
    g_editorPanels.propsTargetPos[2] = g_sel.pos.z;
    g_editorPanels.propsTargetScale = g_sel.scale;
    g_editorPanels.propsTargetRotation = g_sel.rotation;
    g_editorPanels.propsTargetHasRotation = g_sel.hasRotation;
    ShowPropertiesPanel(true);
    EditorLog("Properties for %s idx=%d", g_sel.name.c_str(), g_sel.index);
}

// Adopt a bus event's captured selection as the live one.
//
// Returns false when the event named nothing selectable, so a caller can skip the
// destructive action rather than applying it to whatever happens to be selected —
// the exact failure the loose `action*` fields made possible, where
// actionWorldGraphDelete stored an entity index that the handler then ignored.
//
// SelKind mirrors SelType one-for-one (see ToBusKind in SelType.hpp), so the cast
// is safe; the switch there is what guarantees they cannot drift.
static bool AdoptSelection(const ed::Selection& pick) {
    if (!pick.valid()) return false;
    // Adopting a hierarchy selection cancels a pending placement ghost: the ghost
    // and the selection gizmo share DrawModel, and leaving the placement armed
    // would make the next viewport click place a copy instead of selecting.
    g_placementArmed = false;
    g_sel.type = (SelType)pick.ref.kind;
    g_sel.index = pick.ref.index;
    g_sel.name = pick.name;
    g_sel.pos = {pick.x, pick.y, pick.z};
    // Resolve rotation/scale from the LIVE entity, exactly as the viewport pick
    // path does. ed::Selection carries only the transform it needs for the
    // gizmo, so without this the Rot row showed - and Apply wrote - the previous
    // selection's angle.
    FillSelectionTransform(g_sel);
    // One pick replaces the SET, exactly like clicking in the viewport. Without
    // this a stale g_selSet from an earlier selection survived, and since Delete /
    // Copy / Move now act on the whole set, a WorldGraph double-click could delete
    // entities the user had stopped looking at.
    g_selSet.clear();
    g_selSet.push_back(g_sel);
    EditorLog("Selected from WorldGraph: %s (type=%d idx=%d)",
              g_sel.name.c_str(), (int)g_sel.type, g_sel.index);
    // SelType::MAP is the level, not a placed object: it has no transform, so
    // arming the gizmo would put a draggable handle at {0,0,0} that the Move tool
    // then writes into g_sel.pos every frame. Only an instance selection gets one.
    if (g_sel.type != SelType::MAP) {
        AngelTechEditor.DrawModel = true;
        SnapGizmoToSelection(g_sel);
    } else {
        AngelTechEditor.DrawModel = false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// AppendAutoConvexForSelection
//
// The collision world is AABB-only, so a placed Mesh.Static prop has no
// collision at all and the player walks straight through it. This voxelises the
// selected Mesh or brush into convex boxes and appends them as
// SURF_COLLISION_PROXY brushes (invisible unless the "Collision Bounds" toggle
// is on, but exported to the .ozone so they survive a reload).
//
// The vertex soup is gathered first and handed to AutoConvex as one array, so
// a multi-mesh model is voxelised as a single union rather than producing one
// overlapping proxy set per submesh.
// ---------------------------------------------------------------------------
