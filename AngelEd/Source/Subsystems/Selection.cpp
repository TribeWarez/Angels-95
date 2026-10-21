// =============================================================================
// Subsystems/Selection.cpp
//
// Picking and selection: viewport raycast, surface-face pick, gizmo snap, hover, and adopting a selection.
//
// FRAGMENT - not a standalone translation unit. Included by Main.cpp, which is the
// single TU for AngelEd's core layer. See Wiki/Editor-Architecture-Refactor.md.
// =============================================================================

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

// RaycastTestBrushes REMOVED (R8). It picked from GetCollisionVolumes() - the CSG
// output, whose entries are merged AABBs - and returned that index as
// SelType::BRUSH, the same type RaycastTestOzPrimitives used for a RENDERABLE index.
// See the note at the call site in EditorRaycastAt for the full account.
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
        BoundingBox mb = GetMeshBoundingBox(r->model.meshes[0]);
        // Rotation-aware pick box, using the SAME composition the renderer and
        // collision use (scale, then yaw about the brush origin, then translate).
        // The old code applied position + scale only, so a rotated brush was
        // picked from its unrotated footprint: clicks in empty space selected
        // it, and clicks on its visible ends missed.
        const AabbBox local{mb.min.x, mb.min.y, mb.min.z,
                            mb.max.x, mb.max.y, mb.max.z};
        const AabbBox w = RotatedAabbY(local, r->rotation,
                                       r->position.x, r->position.y,
                                       r->position.z, r->scale);
        BoundingBox box = {{w.minX, w.minY, w.minZ}, {w.maxX, w.maxY, w.maxZ}};
        RayCollision hit = GetRayCollisionBox(ray, box);
        if (hit.hit && hit.distance < best.distance) {
            best = hit;
            // Store ACTUAL mesh center as selection position, not r->position
            Vector3 center = {(w.minX + w.maxX) * 0.5f, (w.minY + w.maxY) * 0.5f,
                              (w.minZ + w.maxZ) * 0.5f};
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
        BoundingBox box = { {p.position.x - 1, p.position.y - 1, p.position.z - 1},
                            {p.position.x + 1, p.position.y + 1, p.position.z + 1} };
        RayCollision hit = GetRayCollisionBox(ray, box);
        if (hit.hit && hit.distance < best.distance) {
            best = hit;
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
static bool EditorRaycastAt(Vector2 mousePos, EditorSelection& out) {
    Ray ray = GetMouseRay(mousePos, OTEditor.MainCamera);
#ifdef DEBUG_EDITOR_TRACE
    EditorLog("MouseAt: x=%.0f y=%.0f)", mousePos.x, mousePos.y);
#endif   
    out = { SelType::NONE, -1, "", {0,0,0} };
    RayCollision best = { false, 1e9f, {0,0,0}, {0,0,0} };
    EditorSelection bestSel = { SelType::NONE, -1, "", {0,0,0} };

    auto testWithSel = [&](RayCollision hit, const EditorSelection& sel, float distMul = 1.0f) {
        // Zones get a distance penalty so solid entities inside them are preferred
        float d = hit.distance * distMul;
        if (hit.hit && d < best.distance) {
            best = hit;
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
    return best.hit;
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
    Ray ray = GetMouseRay(mousePos, OTEditor.MainCamera);
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

void SnapGizmoToSelection(const EditorSelection& sel) {    OmegaTechEditor.X = sel.pos.x;
    OmegaTechEditor.Y = sel.pos.y;
    OmegaTechEditor.Z = sel.pos.z;
    OmegaTechEditor.S = sel.scale > 0.01f ? sel.scale : 1.0f;
    OmegaTechEditor.R = sel.rotation;
    if (sel.type == SelType::BRUSH) {
        // Size comes from the RENDERABLE, not the collision volume. sel.index is a
        // renderable index (see the note in EditorRaycastAt), so indexing
        // GetCollisionVolumes() with it read an unrelated CSG volume and reported ITS
        // extent - a merged 180x10x180 slab for a small brush. The gizmo then drew
        // that box, which read as a brush being re-added on every click.
        OzoneRenderable* r = OzoneLoader::Instance().Get(sel.index);
        if (r && r->loaded && r->model.meshCount > 0) {
            BoundingBox mb = GetMeshBoundingBox(r->model.meshes[0]);
            const AabbBox local{mb.min.x, mb.min.y, mb.min.z,
                                mb.max.x, mb.max.y, mb.max.z};
            // The drawn gizmo is axis-aligned, so report the rotated brush's
            // enclosing extents (same rotation-aware box the picker/collision use).
            const AabbBox w = RotatedAabbY(local, r->rotation,
                                           r->position.x, r->position.y,
                                           r->position.z, r->scale);
            OmegaTechEditor.W = w.maxX - w.minX;
            OmegaTechEditor.H = w.maxY - w.minY;
            OmegaTechEditor.L = w.maxZ - w.minZ;
        }
    } else if (sel.type == SelType::WINDZONE) {
        auto& zones = PawnSystem::Instance().GetWindZones();
        for (auto& z : zones) {
            if ((int)z.id == sel.index) {
                OmegaTechEditor.W = z.bounds.max.x - z.bounds.min.x;
                OmegaTechEditor.H = z.bounds.max.y - z.bounds.min.y;
                OmegaTechEditor.L = z.bounds.max.z - z.bounds.min.z;
                break;
            }
        }
    } else if (sel.type == SelType::ZONE) {
        auto& zones = ZoneManager::Instance().GetZones();
        for (auto& z : zones) {
            if ((int)z.id == sel.index) {
                OmegaTechEditor.W = z.bounds.max.x - z.bounds.min.x;
                OmegaTechEditor.H = z.bounds.max.y - z.bounds.min.y;
                OmegaTechEditor.L = z.bounds.max.z - z.bounds.min.z;
                break;
            }
        }
    } else if (sel.type == SelType::PORTAL) {
        auto& portals = ZoneManager::Instance().GetPortals();
        if (sel.index >= 0 && sel.index < (int)portals.size()) {
            auto& p = portals[sel.index];
            OmegaTechEditor.W = p.bounds.max.x - p.bounds.min.x;
            OmegaTechEditor.H = p.bounds.max.y - p.bounds.min.y;
            OmegaTechEditor.L = p.bounds.max.z - p.bounds.min.z;
        }
    }
    EditorLog("Gizmo snapped to %s idx=%d", sel.name.c_str(), sel.index);
}

// toggleOffSame: left-click toggles a repeat pick off (deselect); right-click
// must NOT toggle, otherwise right-clicking the already-selected entity would
// deselect it instead of opening its context menu.
// Returns true only when the raycast actually HIT an entity. Callers that open a
// context menu must gate on this: on a miss the selection is intentionally left
// untouched, so g_sel still names the previous entity and would otherwise target
// a menu at an entity the user did not right-click.
static bool EditorPickEntity(bool toggleOffSame = true) {
    Vector2 mousePos = GetMousePosition();
    EditorSelection prevSel = g_sel;

    if (EditorRaycastAt(mousePos, g_sel)) {
        // Toggle: clicking the same entity deselects
        if (toggleOffSame && g_sel.type == prevSel.type && g_sel.index == prevSel.index) {
            g_sel = { SelType::NONE, -1, "", {0,0,0} };
            g_hoverSel = { SelType::NONE, -1, "", {0,0,0} };
            OmegaTechEditor.DrawModel = false;
            return true;
        }
        // Clicking a different zone while one is selected = deselect
        if (toggleOffSame && prevSel.type != SelType::NONE && g_sel.type == SelType::ZONE) {
            g_sel = { SelType::NONE, -1, "", {0,0,0} };
            g_hoverSel = { SelType::NONE, -1, "", {0,0,0} };
            OmegaTechEditor.DrawModel = false;
            return true;
        }
        EditorLog("Selected: %s (type=%d idx=%d x=%f y=%f z=%f)",
                  g_sel.name.c_str(), (int)g_sel.type, g_sel.index, (float)g_sel.pos.x, (float)g_sel.pos.y, (float)g_sel.pos.z);
        OmegaTechEditor.DrawModel = true;
        SnapGizmoToSelection(g_sel);
        if (g_sel.type == SelType::PORTAL) {
            // Portals have no list window any more; the Entity Properties panel
            // edits them, so picking one in the viewport opens it there.
            ShowPropertiesPanel(true);
            g_editorPanels.portalTargetWorld = g_sel.name;
        }
    } else {
        OmegaTechEditor.DrawModel = false;
        return false;
    }
    return true;
}

static void EditorHoverEntity() {
    Vector2 mousePos = GetMousePosition();
    EditorRaycastAt(mousePos, g_hoverSel);
}


// ---------------------------------------------------------------------------
// Entity action functions (called from native context menu + WorldGraph)
// ---------------------------------------------------------------------------
static void DeleteSelectedEntity() {
    if (g_sel.type == SelType::NONE) return;
    HistoryPush();
    EditorLog("Deleted %s idx=%d", g_sel.name.c_str(), g_sel.index);
    if (g_sel.type == SelType::NPC)
        PawnSystem::Instance().Despawn(g_sel.index);
    else if (g_sel.type == SelType::PICKUP)
        PawnSystem::Instance().RemovePickup(g_sel.index);
else if (g_sel.type == SelType::BRUSH) {
        // g_sel.index is unambiguously a renderable index now, so there is nothing to
        // guess and no fallback. The old code tested `index < Count()` and otherwise
        // went looking for a matching collision volume - which meant a BRUSH selected
        // with a COLLISION index (see EditorRaycastAt) deleted a different brush
        // entirely whenever that index happened to fall below the renderable count.
        // Get() is bounds-checked and RemoveRenderable() no-ops on a null, so an
        // out-of-range index is now harmless instead of destructive.
        OzoneLoader::Instance().RemoveRenderable(g_sel.index);
        OzoneLoader::Instance().RebuildCollisionVolumes();
    } else if (g_sel.type == SelType::LIGHT) {
        PawnSystem::Instance().RemoveLight(g_sel.index);
    } else if (g_sel.type == SelType::ZONE) {
        ZoneManager::Instance().RemoveZone(g_sel.index);
    } else if (g_sel.type == SelType::SPAWN) {
        PawnSystem::Instance().RemovePlayerStart(g_sel.index);
    } else if (g_sel.type == SelType::PORTAL) {
        ZoneManager::Instance().RemovePortal(g_sel.index);
        RefreshPortalList();
        RefreshLevelList();
    } else if (g_sel.type == SelType::MESH) {
        PawnSystem::Instance().RemoveMeshObject(g_sel.index);
    } else if (g_sel.type == SelType::PARTICLE) {
        PawnSystem::Instance().RemoveParticleEmitter(g_sel.index);
    } else if (g_sel.type == SelType::PATHNODE) {
        PawnSystem::Instance().RemovePathNode(g_sel.index);
    } else if (g_sel.type == SelType::WINDZONE) {
        PawnSystem::Instance().RemoveWindZone(g_sel.index);
    }
    g_sel = { SelType::NONE, -1, "", {0,0,0} };
    g_hoverSel = { SelType::NONE, -1, "", {0,0,0} };
    OmegaTechEditor.DrawModel = false; // drop the gizmo so picking isn't gated
}

static void DuplicateSelectedEntity() {
    if (g_sel.type == SelType::NONE) return;
    HistoryPush();
    Vector3 offset = {2.0f, 0, 2.0f};
    EditorLog("Duplicating %s idx=%d", g_sel.name.c_str(), g_sel.index);
    if (g_sel.type == SelType::NPC) {
        Pawn* p = PawnSystem::Instance().Get(g_sel.index);
        if (p) PawnSystem::Instance().Spawn({p->position.x+offset.x, p->position.y+offset.y, p->position.z+offset.z}, p->defName.c_str());
    } else if (g_sel.type == SelType::PICKUP) {
        auto& pickups = PawnSystem::Instance().GetPickups();
        for (auto& pk : pickups) {
            if ((int)pk.id == g_sel.index) {
                PickupNode clone = pk;
                clone.position.x += offset.x; clone.position.z += offset.z;
                PawnSystem::Instance().AddPickup(clone);
                break;
            }
        }
    } else if (g_sel.type == SelType::ZONE) {
        auto& zones = ZoneManager::Instance().GetZones();
        for (auto& z : zones) {
            if ((int)z.id == g_sel.index) {
                ZoneVolumeNode clone = z;
                clone.bounds.min.x += offset.x; clone.bounds.min.z += offset.z;
                clone.bounds.max.x += offset.x; clone.bounds.max.z += offset.z;
                ZoneManager::Instance().AddZone(clone);
                break;
            }
        }
    } else if (g_sel.type == SelType::BRUSH) {
        // The old code cloned a COLLISION volume, push_back'd it, and then called
        // RebuildCollisionVolumes() - which begins with m_collisionVolumes.clear().
        // The clone was discarded, so Ctrl+D silently did nothing for brushes.
        // Duplicate the renderable instead.
        OzoneRenderable* r = OzoneLoader::Instance().Get(g_sel.index);
        if (!r || !r->loaded || r->model.meshCount == 0) return;
        BoundingBox mb = GetMeshBoundingBox(r->model.meshes[0]);
        // AddBrushRenderable takes UNSCALED size plus the scale; multiplying the
        // bbox by r->scale here as well applied the scale twice.
        Vector3 size = { mb.max.x - mb.min.x,
                         mb.max.y - mb.min.y,
                         mb.max.z - mb.min.z };
        // ...and `pos` is the world CENTRE, not the stored renderable position
        // (cylinder/pyramid store their TOP).
        Vector3 pos = {
            r->position.x + (mb.min.x + mb.max.x) * 0.5f * r->scale + offset.x,
            r->position.y + (mb.min.y + mb.max.y) * 0.5f * r->scale,
            r->position.z + (mb.min.z + mb.max.z) * 0.5f * r->scale + offset.z };
        // AddBrushRenderable takes rotation in DEGREES (its other caller is the
        // editor ghost, whose field is degrees); r->rotation is radians.
        int idx = OzoneLoader::Instance().AddBrushRenderable(
            r->typeId, pos, size, r->rotation * RAD2DEG, r->scale, r->csgOp,
            r->surfaceFlags, r->cylRTop, r->cylRBot, r->cylSlices);
        if (idx >= 0) {
            // AddBrushRenderable regenerates a default surface, so carry the source
            // brush's texture slot and UV transform across by hand.
            if (OzoneRenderable* dup = OzoneLoader::Instance().Get(idx)) {
                dup->texSlot    = r->texSlot;
                dup->texScaleU  = r->texScaleU;
                dup->texScaleV  = r->texScaleV;
                dup->texOffsetU = r->texOffsetU;
                dup->texOffsetV = r->texOffsetV;
                dup->surface    = r->surface;
                // surface.def is the owner of the flags/UV/texture mirrors; the
                // copy above changed it without re-deriving them.
                DeriveLegacyRenderable(*dup);
                OzoneLoader::Instance().RebuildSurfaceMeshes(idx);
            }
            OzoneLoader::Instance().RebuildCollisionVolumes();
            EditorLog("Duplicated Brush idx=%d -> idx=%d", g_sel.index, idx);
        }
    } else if (g_sel.type == SelType::LIGHT) {
        LightNode* l = PawnSystem::Instance().GetLight(g_sel.index);
        if (l) {
            LightNode clone = *l;
            clone.position.x += offset.x; clone.position.z += offset.z;
            PawnSystem::Instance().AddLight(clone);
        }
    } else if (g_sel.type == SelType::SPAWN) {
        if (PlayerStartNode* s = FindPlayerStartById(g_sel.index)) {
            PlayerStartNode clone = *s;
            clone.position.x += offset.x; clone.position.z += offset.z;
            PawnSystem::Instance().AddPlayerStart(clone);
        }
    } else if (g_sel.type == SelType::PORTAL) {
        auto& portals = ZoneManager::Instance().GetPortals();
        if (g_sel.index >= 0 && g_sel.index < (int)portals.size()) {
            ZonePortal clone = portals[g_sel.index];
            clone.bounds.min.x += offset.x; clone.bounds.min.z += offset.z;
            clone.bounds.max.x += offset.x; clone.bounds.max.z += offset.z;
            ZoneManager::Instance().AddPortal(clone);
            RefreshLevelList();
        }
    } else if (g_sel.type == SelType::MESH) {
        MeshObjectNode* src = PawnSystem::Instance().GetMeshObject(g_sel.index);
        if (src) {
            MeshObjectNode clone = *src;
            clone.id = 0;
            clone.mesh.reset(); // force re-resolve through MeshCache
            clone.position.x += offset.x;
            clone.position.z += offset.z;
            PawnSystem::Instance().AddMeshObject(clone);
        }
    } else if (g_sel.type == SelType::PARTICLE) {
        ParticleEmitterNode* src = PawnSystem::Instance().GetParticleEmitter(g_sel.index);
        if (src) {
            ParticleEmitterNode clone = *src;
            clone.id = 0;
            clone.position.x += offset.x;
            clone.position.z += offset.z;
            PawnSystem::Instance().AddParticleEmitter(clone);
        }
    } else if (g_sel.type == SelType::PATHNODE) {
        PathNode* src = PawnSystem::Instance().GetPathNode(g_sel.index);
        if (src) {
            static int s_pathDupCounter = 1000;
            PathNode clone = *src;
            clone.id = 0;
            clone.name = "path_" + std::to_string(s_pathDupCounter++); // names must stay unique
            clone.position.x += offset.x;
            clone.position.z += offset.z;
            clone.next.clear(); // duplicates start unlinked
            PawnSystem::Instance().AddPathNode(clone);
        }
    } else if (g_sel.type == SelType::WINDZONE) {
        WindZoneNode* src = PawnSystem::Instance().GetWindZone(g_sel.index);
        if (src) {
            WindZoneNode clone = *src;
            clone.id = 0;
            clone.bounds.min.x += offset.x; clone.bounds.min.z += offset.z;
            clone.bounds.max.x += offset.x; clone.bounds.max.z += offset.z;
            PawnSystem::Instance().AddWindZone(clone);
        }
    }
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
    g_sel.type = (SelType)pick.ref.kind;
    g_sel.index = pick.ref.index;
    g_sel.name = pick.name;
    g_sel.pos = {pick.x, pick.y, pick.z};
    EditorLog("Selected from WorldGraph: %s (type=%d idx=%d)",
              g_sel.name.c_str(), (int)g_sel.type, g_sel.index);
    // SelType::MAP is the level, not a placed object: it has no transform, so
    // arming the gizmo would put a draggable handle at {0,0,0} that the Move tool
    // then writes into g_sel.pos every frame. Only an instance selection gets one.
    if (g_sel.type != SelType::MAP) {
        OmegaTechEditor.DrawModel = true;
        SnapGizmoToSelection(g_sel);
    } else {
        OmegaTechEditor.DrawModel = false;
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
