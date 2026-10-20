// ============================================================================
// Core/EditorDispatcher.cpp
//
// The single point where posted editor events are handled: a switch over ed::Ev for
// the 18 kinds on the frame pass. Extracted from main() in R6, where it was 240 lines
// of switch sitting in the middle of the render loop.
//
// The surface, animation and placement passes are NOT cases here. Each is drained and
// applied by its own subsystem (SurfaceOps, AnimEditing, Placement), because those are
// real work rather than a one-line call - and keeping them out of this switch is what
// let them move out of main() without reordering each other.
//
// FRAGMENT - not a standalone translation unit. Included by Main.cpp, which is the
// single TU for the AngelEd core layer. See Wiki/Editor-Architecture-Refactor.md.
// ============================================================================

static void DispatchFrameEvents(const std::vector<ed::Event>& events) {
    for (const ed::Event& ev : events) {
      switch (ev.kind) {
            case ed::Ev::SelectEntity:
                AdoptSelection(ev.selection());
                break;
            case ed::Ev::OpenProperties:
                // Open the Properties panel on the row that was right-clicked.
                // (ApplyProperties is a DIFFERENT kind - it writes the panel's
                // edited values and is owned by the props pass, not this one.)
                AdoptSelection(ev.selection());
                OpenPropertiesForSelection();
                break;
            case ed::Ev::DeleteEntity:
                if (!AdoptSelection(ev.selection())) break;
                DeleteSelectedEntity();
                break;
            case ed::Ev::DuplicateEntity:
                if (!AdoptSelection(ev.selection())) break;
                DuplicateSelectedEntity();
                break;
            case ed::Ev::DeletePortal: {
                // Portal deletion mutates ZoneManager::GetPortals(), so it does
                // NOT go through DeleteSelectedEntity (which knows the entity
                // containers). Its own event kind keeps the two index spaces
                // from ever being confused.
                int pidx = ev.sel().index;
                if (pidx < 0) break;
                HistoryPush();
                ZoneManager::Instance().RemovePortal(pidx);
                EditorLog("Portal %d deleted", pidx);
                RefreshPortalList();
                RefreshLevelList();
                // Drop any selection that pointed at the portal that just went.
                // RemovePortal shifts every later portal down by one, so a
                // retained propsTargetIndex means the next Apply silently
                // edits a DIFFERENT portal.
                if (g_sel.type == SelType::PORTAL && g_sel.index == pidx)
                    g_sel = { SelType::NONE, -1, "", {0,0,0} };
                if (g_editorPanels.propsTargetType == sel::PORTAL &&
                    g_editorPanels.propsTargetIndex == pidx) {
                    ShowPropertiesPanel(false);
                }
                break;
            }
            case ed::Ev::CsgPlace: {
                // Start placing a new primitive: reset the ghost to a default
                // box at the camera. Distinct from CsgCommit, which reads
                // whatever the ghost currently is.
                const ed::CsgIntent& ci = ev.csg();
                if (!ci.isPrimitive()) break;
                g_placeMode = PlaceMode::MODEL;
                g_sel = { SelType::NONE, -1, "", {0,0,0} };
                g_hoverSel = { SelType::NONE, -1, "", {0,0,0} };
                OmegaTechEditor.DrawModel = true;
                EMID = 200 + ci.primitive;
                OmegaTechEditor.X = OTEditor.MainCamera.target.x;
                OmegaTechEditor.Y = OTEditor.MainCamera.target.y;
                OmegaTechEditor.Z = OTEditor.MainCamera.target.z;
                OmegaTechEditor.W = 4.0f;
                OmegaTechEditor.H = 4.0f;
                OmegaTechEditor.L = 4.0f;
                OmegaTechEditor.S = 1.0f;
                OmegaTechEditor.R = 0.0f;
                break;
            }
            case ed::Ev::CsgCommit: {
                // Commit the current ghost immediately (Solid/Add/Sub/Inter).
                const ed::CsgIntent& ci = ev.csg();
                if (!ci.isCommit()) break;
                if (g_placeMode == PlaceMode::MODEL) {
                    HistoryPush();
                    // Default to a box at the camera if no primitive is armed.
                    if (EMID < 200) {
                        EMID = 200;
                        OmegaTechEditor.X = OTEditor.MainCamera.target.x;
                        OmegaTechEditor.Y = OTEditor.MainCamera.target.y;
                        OmegaTechEditor.Z = OTEditor.MainCamera.target.z;
                        OmegaTechEditor.W = 4.0f;
                        OmegaTechEditor.H = 4.0f;
                        OmegaTechEditor.L = 4.0f;
                    }
                    // Same CSG placement logic as the ENTER key (collision is
                    // rebuilt by RebuildCollisionVolumes inside
                    // CommitBrushRenderable).
                    int primType = EMID - 200;
                    Vector3 center = {OmegaTechEditor.X, OmegaTechEditor.Y, OmegaTechEditor.Z};
                    Vector3 size = {OmegaTechEditor.W, OmegaTechEditor.H, OmegaTechEditor.L};
                    int ridx = CommitBrushRenderable(primType, center, size,
                                                     OmegaTechEditor.R, OmegaTechEditor.S,
                                                     ci.operation);
                    if (ridx >= 0) {
                        EditorLog("CSG commit: op=%d prim=%d at (%.1f,%.1f,%.1f) size=(%.1f,%.1f,%.1f)",
                                  ci.operation, primType, center.x, center.y, center.z,
                                  size.x, size.y, size.z);
                    }
                }
                // Placing via the toolbox is done - drop the ghost so the
                // viewport returns to normal selection (Enter does the same).
                if (g_sel.type == SelType::NONE) OmegaTechEditor.DrawModel = false;
                break;
            }
            case ed::Ev::ConvertToAnimated:
                // Deferred to the conversion site further down this frame,
                // which owns EnsureMeshNodeLoaded + the Anim panel handover.
                // Posting the index is the point: the old field was read back
                // out of g_sel, so pressing Convert while the selection moved
                // converted the wrong mesh.
                evConvertMeshIndex = ev.sel().index;
                break;
            case ed::Ev::PlaySound: {
                // The PATH travelled with the event; volume/loop stay live
                // fields because they are re-applied every frame while the
                // preview plays (see the poll above).
                const std::string& path = ev.str();
                if (path.empty()) break;
                StopSoundPreview();
                g_previewSound = LoadSound(path.c_str());
                g_previewSoundLoaded = g_previewSound.frameCount > 0;
                if (g_previewSoundLoaded) {
                    SetSoundVolume(g_previewSound,
                                   g_editorPanels.previewSoundVolume / 100.0f);
                    PlaySound(g_previewSound);
                }
                break;
            }
            case ed::Ev::StopSoundPreview:
                StopSoundPreview();
                break;
            case ed::Ev::ApplyTextureToSelection: {
                // From the Texture Manager's Apply button and grid double-click.
                // The selection was CAPTURED into the event at post time; the old
                // path (Ev::ApplyTextureToModel + a "Model 1..20" dropdown index)
                // resolved through WDLModels and was a guaranteed silent no-op
                // for every CSG brush.
                const ed::TextureSelectionApply& apply = ev.textureSelectionApply();
                if (apply.path.empty() || !apply.target.valid()) break;
                HistoryPush();
                if (apply.target.kind == ed::SelKind::Brush) {
                    const bool applied = OzoneLoader::Instance().ApplyRenderableTexture(
                        apply.target.index, apply.path.c_str());
                    EditorLog("Applied texture to brush renderable idx=%d: %s",
                              apply.target.index, applied ? "ok" : "FAILED");
                    if (applied) g_editorPanels.textureProbeRenderable = apply.target.index;
                } else if (apply.target.kind == ed::SelKind::Model &&
                           apply.target.index >= 0 && apply.target.index < CachedModelCounter) {
                    const int modelId = CachedModels[apply.target.index].ModelId;
                    if (ApplyTextureToModel(modelId, apply.path.c_str()))
                        EditorLog("Applied texture to model %d: %s", modelId,
                                  apply.path.c_str());
                } else if (apply.target.kind == ed::SelKind::Mesh) {
                    // A placed GameEngine.Mesh.* prop is NOT a legacy WDL model.
                    // Without this branch the Texture Manager's "selected brush
                    // or model" Apply silently did nothing for every Mesh entity.
                    if (MeshObjectNode* m =
                            PawnSystem::Instance().GetMeshObject(apply.target.index)) {
                        m->texturePath = apply.path;
                        m->mesh.reset();   // re-resolve with the new texture
                        EditorLog("Applied texture to mesh '%s' idx=%d: %s",
                                  m->meshPath.c_str(), apply.target.index,
                                  apply.path.c_str());
                    }
                }
                break;
            }
            case ed::Ev::RefreshModelBrowser:
                ScanModelBrowserFiles();
                g_lastPreviewSel = -1;   // force a fresh preview after the rebuild
                g_previewNeedsUpdate = false;
                break;
            case ed::Ev::GenerateHeightmap: {
                const ed::HeightmapDesc& hd = ev.heightmap();
                if (hd.imagePath.empty()) break;
                HistoryPush();
                std::vector<float> args = {
                    hd.x, hd.y, hd.z, hd.scale,
                    hd.sizeX, hd.sizeY, hd.sizeZ
                };
                OzoneLoader::Instance().BuildHeightmap(hd.imagePath,
                                                       hd.texturePath, args);
                EditorLog("Heightmap generated from %s", hd.imagePath.c_str());
                break;
            }
            case ed::Ev::BeginPlacement: {
                const ed::PlacementRequest& pr = ev.placement();
                // Every placement mode clears the selection first: a ghost and
                // a selection must not coexist, or the drag gate cannot tell
                // which the user means to move.
                g_sel = { SelType::NONE, -1, "", {0,0,0} };
                g_hoverSel = { SelType::NONE, -1, "", {0,0,0} };
                switch (pr.kind) {
                    case ed::PlacementRequest::Kind::Pickup:
                        g_placeMode = PlaceMode::PICKUP;
                        // The def NAME came from the panel, already resolved from
                        // the legacy index or the registry. Resolving it here from
                        // an index meant a panel rebuild between the click and the
                        // frame placed a different pickup.
                        OmegaTechEditor.ActivePickupName = pr.key;
                        break;
                    case ed::PlacementRequest::Kind::Node:
                        g_placeMode = PlaceMode::NODE;
                        OmegaTechEditor.ActiveNodeType =
                            (EditorNodeType)atoi(pr.key.c_str());
                        break;
                    case ed::PlacementRequest::Kind::Model:
                        g_placeMode = PlaceMode::MODEL;
                        EMID = 0;   // 0 = user-selected obj
                        break;
                }
                OmegaTechEditor.DrawModel = true;
                OmegaTechEditor.X = OTEditor.MainCamera.position.x;
                OmegaTechEditor.Y = OTEditor.MainCamera.position.y;
                OmegaTechEditor.Z = OTEditor.MainCamera.position.z;
                OmegaTechEditor.R = 1;
                break;
            }
            case ed::Ev::OpenWorld: {
                const std::string& worldName = ev.str();
                if (worldName.empty()) break;
                fs::path ozone = fs::path("GameData/Worlds") / worldName / "World.ozone";
                if (!fs::exists(ozone))
                    ozone = fs::path("../GameData/Worlds") / worldName / "World.ozone";
                if (fs::exists(ozone)) {
                    g_pendingOpenPath = ozone;
                    EditorLog("LevelList: opening world '%s'", worldName.c_str());
                } else {
                    EditorLog("LevelList: world '%s' not found", worldName.c_str());
                }
                break;
            }
            case ed::Ev::LinkWorld: {
                // Create a portal in front of the camera linking to the target.
                const std::string& target = ev.str();
                if (target.empty()) break;
                Vector3 pos = OTEditor.MainCamera.target;
                HistoryPush();
                ZonePortal portal;
                portal.bounds = {{pos.x - 2, pos.y - 2, pos.z - 2},
                                 {pos.x + 2, pos.y + 2, pos.z + 2}};
                portal.targetWorld = target;
                portal.targetSpawn = {0, 20, 0};
                portal.bidirectional = true;
                ZoneManager::Instance().AddPortal(portal);
                g_editorPanels.portalTargetWorld = target;
                RefreshPortalList();
                RefreshLevelList();
                EditorLog("Created portal link to '%s' at camera target",
                          target.c_str());
                break;
            }
            case ed::Ev::ReloadMesh: {
                const ed::SelRef& t = ev.sel();
                if (!t.valid() || t.kind != ed::SelKind::Mesh) break;
                // reset() drops the cached mesh so the next draw reloads it
                // from disk. Guarded on the kind as well as the index: the old
                // handler read g_sel.index, so a Reload pressed while something
                // else was selected reset an unrelated object's cache.
                if (MeshObjectNode* m = PawnSystem::Instance().GetMeshObject(t.index)) {
                    m->mesh.reset();
                    EditorLog("Reset mesh cache for '%s'", m->meshPath.c_str());
                }
                break;
            }
            default:
                // B3..B5 add the remaining kinds. An unhandled event is counted
                // rather than ignored, so a batch that forgets a kind shows up as
                // a counter instead of a click that silently does nothing.
                g_editorUnhandledEvents++;
                break;
        }
    }

    // Deferred by one frame on purpose, and safe because every event carries its own
    // target captured at post time. Draining BEFORE dispatch (in Main.cpp) is also
    // deliberate: a handler that opens a panel can post further events, and draining
    // while iterating the live queue would dispatch those in the same frame.
    //
    // Panels are mid-layout while they post, so dispatching inline from WM_COMMAND
    // would mutate the world under a live dialog. What makes deferral safe is the
    // payload being complete, not the timing.
}
