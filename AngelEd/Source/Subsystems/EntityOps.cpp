// =============================================================================
// Subsystems/EntityOps.cpp
//
// Whole-entity mutations: delete, duplicate, AutoConvex proxies, brush CSG commit.
//
// FRAGMENT - not a standalone translation unit. Included by Main.cpp, which is the
// single TU for AngelEd's core layer. See Wiki/Editor-Architecture-Refactor.md.
// =============================================================================

static void AppendAutoConvexForSelection() {
    // World-space vertex soup for the current selection.
    std::vector<float> soup;
    const char* what = "";

    if (g_sel.type == SelType::MESH) {
        MeshObjectNode* n = PawnSystem::Instance().GetMeshObject(g_sel.index);
        if (!n || !n->mesh || !n->mesh->Valid()) {
            EditorLog("AutoConvex: mesh has no loaded geometry");
            return;
        }
        what = n->meshPath.c_str();
        Model& mdl = n->mesh->GetModel();
        for (int i = 0; i < mdl.meshCount; i++) {
            Mesh& m = mdl.meshes[i];
            if (!m.vertices || m.vertexCount <= 0) continue;
            soup.reserve(soup.size() + (size_t)m.vertexCount * 3);
            for (int v = 0; v < m.vertexCount; v++) {
                // Bind pose + node transform. Skeletal meshes are voxelised in
                // their rest pose: the per-frame vertex upload happens at draw
                // time, so there is no posed geometry available here.
                float lx = m.vertices[v * 3 + 0] * n->scale;
                float ly = m.vertices[v * 3 + 1] * n->scale;
                float lz = m.vertices[v * 3 + 2] * n->scale;
                float rad = n->yaw * DEG2RAD;
                float cs = cosf(rad), sn = sinf(rad);
                soup.push_back(n->position.x + lx * cs + lz * sn);
                soup.push_back(n->position.y + ly);
                soup.push_back(n->position.z - lx * sn + lz * cs);
            }
        }
    } else if (g_sel.type == SelType::BRUSH) {
        OzoneRenderable* r = OzoneLoader::Instance().Get(g_sel.index);
        if (!r || !r->loaded || r->model.meshCount <= 0 || !r->model.meshes[0].vertices) {
            EditorLog("AutoConvex: brush has no generated mesh");
            return;
        }
        what = "brush";
        Mesh& m = r->model.meshes[0];
        BoundingBox mb = GetMeshBoundingBox(m);
        soup.reserve((size_t)m.vertexCount * 3);
        for (int v = 0; v < m.vertexCount; v++) {
            // Local vertex -> world, matching how Draw()/DrawWorldGeometry
            // place the model (position + scale, yaw about Y).
            float lx = m.vertices[v * 3 + 0];
            float ly = m.vertices[v * 3 + 1];
            float lz = m.vertices[v * 3 + 2];
            float rad = r->rotation;
            float cs = cosf(rad), sn = sinf(rad);
            soup.push_back(r->position.x + (lx * cs + lz * sn) * r->scale);
            soup.push_back(r->position.y + ly * r->scale);
            soup.push_back(r->position.z + (-lx * sn + lz * cs) * r->scale);
        }
    } else {
        return;
    }

    if (soup.size() < 9) {
        EditorLog("AutoConvex: no geometry to voxelise");
        return;
    }

    // Cell size is picked from the selection's own extent so a crate and a
    // castle wall both get a sensible budget instead of one of them exploding
    // into thousands of boxes.
    float cell = 0.5f;
    BoundingBox sel = {};
    if (g_sel.type == SelType::MESH) {
        MeshObjectNode* n = PawnSystem::Instance().GetMeshObject(g_sel.index);
        if (n && n->mesh && n->mesh->Valid()) {
            const BoundingBox& mb = n->mesh->Bounds();
            sel = {{n->position.x + mb.min.x * n->scale, n->position.y + mb.min.y * n->scale,
                    n->position.z + mb.min.z * n->scale},
                   {n->position.x + mb.max.x * n->scale, n->position.y + mb.max.y * n->scale,
                    n->position.z + mb.max.z * n->scale}};
        }
    } else {
        OzoneRenderable* r = OzoneLoader::Instance().Get(g_sel.index);
        if (r && r->loaded && r->model.meshCount > 0) {
            BoundingBox mb = GetMeshBoundingBox(r->model.meshes[0]);
            sel = {{r->position.x + mb.min.x * r->scale, r->position.y + mb.min.y * r->scale,
                    r->position.z + mb.min.z * r->scale},
                   {r->position.x + mb.max.x * r->scale, r->position.y + mb.max.y * r->scale,
                    r->position.z + mb.max.z * r->scale}};
        }
    }
    float extent = fmaxf(fmaxf(sel.max.x - sel.min.x, sel.max.y - sel.min.y),
                         sel.max.z - sel.min.z);
    if (extent > 0.0f) cell = fmaxf(0.25f, extent / 16.0f);

    HistoryPush();
    AutoConvexFail fail = AutoConvexFail::NONE;
    int added = OzoneLoader::Instance().AppendAutoConvexCollision(
        soup.data(), (int)soup.size(), cell, kAutoConvexMaxBoxes, &fail);

    if (added > 0) {
        // Turn the collision view on so the author sees the proxies land
        // somewhere sensible instead of having to go hunting for the toggle.
        g_editorPanels.showCollisionBounds = true;
        OzoneLoader::Instance().SetDrawCollisionProxies(true);
        EditorLog("AutoConvex: %d proxies appended to '%s' (cell %.2f)", added, what, cell);
    } else {
        // Report the ACTUAL cause. Every failure used to be logged as "over the
        // box budget", so a mesh with no usable geometry sent the author to
        // raise a budget they were nowhere near.
        EditorLog("AutoConvex: refused for '%s' (cell %.2f) - %s", what, cell,
                  AutoConvexFailName(fail));
        if (fail == AutoConvexFail::OVER_BUDGET) {
            // Tell the author what it WOULD cost, so the fix is obvious.
            AutoConvexParams probe;
            probe.cellSize = cell;
            probe.maxBoxes = kAutoConvexMaxBoxes;
            const int needed = AutoConvex_CountCells(soup.data(), (int)soup.size(), probe);
            EditorLog("AutoConvex: this mesh needs about %d proxies at cell %.2f "
                      "(budget %d) - use a coarser cell size", needed, cell, kAutoConvexMaxBoxes);
        }
    }
}

// ---------------------------------------------------------------------------
// CommitBrushRenderable
//
// The single place a brush is added to the document. Both the Enter-key ghost
// and the sidebar Solid/Add/Sub/Inter buttons used to open-code this sequence,
// which is how the two could drift; the collision rebuild and the no-effect
// warning below have to happen on both paths or a brush silently arrives with
// no collision.
//
// SUB / DE_RESC / INTERSECT are *modifiers*: CsgProcessor (Source/Physics/
// OzBsp.cpp) subtracts from, or intersects with, the solids already in the
// world. With nothing there to act on, the brush contributes no volume at all
// while its render mesh still draws - which is how a floor authored as `sub`
// looks like a floor in game and drops the player straight through it.
// ---------------------------------------------------------------------------
static int CommitBrushRenderable(int primType, const Vector3& center,
                                 const Vector3& size, float rot, float scale,
                                 int csgOp) {
    auto& loader = OzoneLoader::Instance();
    const int before = (int)loader.GetCollisionVolumes().size();

    int ridx = loader.AddBrushRenderable(primType, center, size, rot, scale, csgOp);

    // Rebuild collision volumes (includes the new brush).
    loader.RebuildCollisionVolumes();

    if (ridx >= 0) {
        const int after = (int)loader.GetCollisionVolumes().size();
        const bool modifier = (csgOp == (int)CsgOp::SUB ||
                               csgOp == (int)CsgOp::DE_RESC ||
                               csgOp == (int)CsgOp::INTERSECT);
        if (modifier && after <= before) {
            static const char* kOpName[] = {"solid", "add", "sub", "intersect", "de-resc"};
            const char* opName = (csgOp >= 0 && csgOp <= 4) ? kOpName[csgOp] : "?";
            // Loud on purpose: the viewport shows a solid-looking brush and the
            // collision count is the only place the failure is visible.
            g_editorPanels.collisionOpWarning = true;
            EditorLog("*** CSG '%s' added NO collision volume (world has %d) ***", opName, after);
            EditorLog("***    '%s' only modifies solids that already exist. "
                      "Add a Solid/Add brush first, or switch the op to Solid. ***", opName);
            #ifdef _WIN32
            MessageBoxA((HWND)GetWindowHandle(),
                        "This CSG operation produced NO collision.\n\n"
                        "Sub / Intersect only modify solids that already exist in the "
                        "world. The brush still renders, so it looks solid in game - "
                        "but the player will fall straight through it.\n\n"
                        "Add a Solid/Add brush first, or switch the operation to Solid.",
                        "AngelEd - no collision generated", MB_OK | MB_ICONWARNING);
            #endif
        } else {
            g_editorPanels.collisionOpWarning = false;
        }
    }
    return ridx;
}

