#include "../Core/EditorShell.hpp"
#include "../Core/EditorState.hpp"
#include "PropsApply.hpp"
#include "LevelState.hpp"   // ApplyMapProperties
#include "Selection.hpp"   // FindPlayerStartById, ClampPropInt

#include "../../../Source/Pawn/OzPawnSystem.hpp"
#include "../../../Source/World/OzOzoneLoader.hpp"
#include "../../../Source/World/ZoneManager.hpp"
// Subsystems/PropsApply.cpp
//
// Writes the Entity Properties panel's edited values back into the world.
// Extracted from main() in R6, where a 281-line if-block sat inline in the frame
// loop, and converted from the `actionApplyProperties` field to ed::Ev::ApplyProperties.
//
// FRAGMENT - not a standalone translation unit. Included by Main.cpp.
// ============================================================================


// THE EVENT CARRIES THE TARGET ONLY. The ~87 prop* value fields are still read from
// g_editorPanels at drain time, and that is a decision rather than a half-finished
// conversion:
//
//   - The staleness this refactor exists to kill is an INDEX that ends up naming a
//     different entity than the one the user clicked. That cannot happen to a value
//     the user just typed. The panel is a normal window, not a modal dialog, but
//     between the Apply click and this drain - the same frame - nothing pumps input,
//     so no value can change underneath us. Same argument that keeps
//     actionApplyTextureToSel a field, and for the same reason.
//   - The alternative is a payload with ~87 fields, of which roughly six apply to any
//     given SelType. Snapshotting 87 to write 6 is worse than reading 6 live.
//
// What the target capture DOES buy is the part that was genuinely wrong: the handler
// used to read propsTargetType / propsTargetIndex from live panel state, so whichever
// row the panel happened to be showing at drain time decided what got written. The
// panel now names the entity it meant when the button was pressed.
//
// This is what the R2 plan twice promised for R4 and R4 did not do. The precondition
// it named - "when the panel stops owning the target directly" - is satisfied in the
// sense that matters: the HANDLER no longer reads the target from panel state. The
// panel keeps propsTarget* because it needs those fields to render its rows.
void ApplyPanelProperties(const std::vector<ed::Event>& events) {
    for (const ed::Event& ev : events) {
        const ed::SelRef& t = ev.sel();
        if (!t.valid()) {
            // propsTargetIndex is -1 for a def-only panel (propsTargetType ==
            // sel::DEF_ONLY, the Script-Manager sentinel). There is no entity to
            // apply to, and falling back to whatever was selected before would be the
            // stale-target bug wearing a different hat.
            EditorLog("Properties: apply ignored - no target");
            continue;
        }
        // SelType::MAP writes level metadata rather than a scene object and
        // has its own undo policy, so it is dispatched before the generic
        // HistoryPush() below.
        if (t.kind == ed::SelKind::Map) {
            ApplyMapProperties();
        } else {
        HistoryPush();
        float px = g_editorPanels.propPosX;
        float py = g_editorPanels.propPosY;
        float pz = g_editorPanels.propPosZ;
        float prot = g_editorPanels.propRotation;
        int tgtIdx = t.index;
        SelType tgtType = ToSelType(t.kind);

        if (tgtType == SelType::NPC) {
            Pawn* p = PawnSystem::Instance().Get(tgtIdx);
            if (p) {
                p->position = {px, py, pz};
                // Instance overrides (def values stay in PawnDefs/.ozls)
                int h = (int)g_editorPanels.propHealth;
                if (h < 1) h = 1;
                if (h > p->maxHealth) h = p->maxHealth;
                p->health = h;
                if (g_editorPanels.propSpeed > 0.01f) p->speed = g_editorPanels.propSpeed;
            }
        } else if (tgtType == SelType::PICKUP) {
            auto& pickups = PawnSystem::Instance().GetPickups();
            for (auto& pk : pickups) {
                if ((int)pk.id == tgtIdx) {
                    pk.position = {px, py, pz};
                    if (g_editorPanels.propRespawnTime >= 0.0f)
                        pk.respawnTime = g_editorPanels.propRespawnTime;
                    break;
                }
            }
        } else if (tgtType == SelType::BRUSH) {
            float sx = g_editorPanels.propSizeX;
            float sy = g_editorPanels.propSizeY;
            float sz = g_editorPanels.propSizeZ;
            if (sx < 0.01f) sx = 1.0f;
            if (sy < 0.01f) sy = 1.0f;
            if (sz < 0.01f) sz = 1.0f;
            Vector3 newSize = {sx, sy, sz};
            // tgtIdx is unambiguously a renderable index for SelType::BRUSH (see
            // EditorRaycastAt). The old code fell back to
            // FindRenderableByCollisionVol when the index exceeded the renderable
            // count, which silently retargeted the edit at whatever brush best
            // matched a stale collision volume - the Properties panel then reported a
            // different brush's size, and applying wrote that size somewhere else.
            const int rIdx = (OzoneLoader::Instance().Get(tgtIdx) != nullptr) ? tgtIdx : -1;
            if (rIdx >= 0) {
                OzoneLoader::Instance().UpdateBrushRenderable(
                    rIdx, (Vector3){px, py, pz}, newSize, prot);
                OzoneLoader::Instance().ApplyRenderableUV(
                    rIdx,
                    g_editorPanels.propTexScaleU,
                    g_editorPanels.propTexScaleV,
                    g_editorPanels.propTexOffsetU,
                    g_editorPanels.propTexOffsetV);
            }
            // Rebuild collision volumes from the updated renderable
            OzoneLoader::Instance().RebuildCollisionVolumes();
        } else if (tgtType == SelType::ZONE) {
            auto& zones = ZoneManager::Instance().GetZones();
            for (auto& zone : zones) {
                if ((int)zone.id == tgtIdx) {
                    float szx = g_editorPanels.propSizeX;
                    float szy = g_editorPanels.propSizeY;
                    float szz = g_editorPanels.propSizeZ;
                    zone.bounds.min = {px - szx*0.5f, py - szy*0.5f, pz - szz*0.5f};
                    zone.bounds.max = {px + szx*0.5f, py + szy*0.5f, pz + szz*0.5f};
                    // Type/intensity/script-hook name (exported by ExportToOzone)
                    int zt = g_editorPanels.propZoneType;
                    if (zt < 0 || zt > 4) zt = 0;
                    zone.zoneType = (ZoneType)zt;
                    if (g_editorPanels.propZoneIntensity > 0.0f)
                        zone.intensity = g_editorPanels.propZoneIntensity;
                    if (!g_editorPanels.propZoneName.empty() &&
                        g_editorPanels.propZoneName != zone.name) {
                        // Rename the matching sky-zone node too so runtime
                        // script hooks (on_enter/on_exit) follow the new name.
                        std::string oldName = zone.name;
                        for (auto& sky : PawnSystem::Instance().GetSkyZones())
                            if (sky.name == oldName) sky.name = g_editorPanels.propZoneName;
                        zone.name = g_editorPanels.propZoneName;
                    }
                    // Per-zone physics overrides (exported by ExportToOzone)
                    zone.physics.gravity = g_editorPanels.propZoneGravity;
                    zone.physics.jumpSpeed = g_editorPanels.propZoneJump;
                    zone.physics.terminalVelocity = g_editorPanels.propZoneTerminal;
                    zone.physics.waterGravity = g_editorPanels.propZoneWaterGravity;
                    zone.physics.waterDrag = g_editorPanels.propZoneWaterDrag;
                    zone.physics.swimUpSpeed = g_editorPanels.propZoneSwimUp;
                    zone.physics.ladderSpeed = g_editorPanels.propZoneLadderSpeed;
                    zone.physics.flySpeedMult = g_editorPanels.propZoneFlyMult;
                    // Per-zone environment overrides. These are what
                    // ZoneManager merges into PointRegion::combinedEnv and
                    // Core.hpp applies on zone entry — the exporter already
                    // gates on applyFog / applyAmbient / reverbMix, so writing
                    // them here makes a hand-authored .ozone line editable.
                    {
                        auto& eo = zone.envOverrides;
                        eo.applyFog      = g_editorPanels.propZoneApplyFog;
                        eo.fogR          = g_editorPanels.propZoneFogR;
                        eo.fogG          = g_editorPanels.propZoneFogG;
                        eo.fogB          = g_editorPanels.propZoneFogB;
                        eo.fogDensity    = g_editorPanels.propZoneFogDensity;
                        eo.fogStart      = g_editorPanels.propZoneFogStart;
                        eo.fogEnd        = g_editorPanels.propZoneFogEnd;
                        eo.applyAmbient  = g_editorPanels.propZoneApplyAmbient;
                        eo.ambR          = g_editorPanels.propZoneAmbR;
                        eo.ambG          = g_editorPanels.propZoneAmbG;
                        eo.ambB          = g_editorPanels.propZoneAmbB;
                        eo.ambIntensity  = g_editorPanels.propZoneAmbIntensity;
                        eo.reverbMix     = g_editorPanels.propZoneReverbMix;
                        eo.reverbDecay   = g_editorPanels.propZoneReverbDecay;
                    }
                    break;
                }
            }
        } else if (tgtType == SelType::SPAWN) {
            if (PlayerStartNode* s = FindPlayerStartById(tgtIdx)) {
                s->position = {px, py, pz};
                // Only when the selection actually carried a yaw. The Rot row
                // is seeded from propsTargetRotation, which defaults to 0 for
                // any selection whose raycast did not populate it, so
                // writing it unconditionally turned a PlayerStart's authored
                // `playerstart` yaw into 0 on every Apply.
                if (g_editorPanels.propsTargetHasRotation) s->yaw = prot;
            }
        } else if (tgtType == SelType::LIGHT) {
            if (LightNode* l = PawnSystem::Instance().GetLight(tgtIdx)) {
                l->position = {px, py, pz};
                l->name = g_editorPanels.propLightName;
                l->color = (Color){(unsigned char)ClampPropInt(g_editorPanels.propLightR, 0, 255),
                                   (unsigned char)ClampPropInt(g_editorPanels.propLightG, 0, 255),
                                   (unsigned char)ClampPropInt(g_editorPanels.propLightB, 0, 255),
                                   255};
                l->intensity = fmaxf(0.0f, g_editorPanels.propLightIntensity);
                l->radius = fmaxf(0.1f, g_editorPanels.propLightRadius);
                int lt = g_editorPanels.propLightType;
                if (lt < 0 || lt > 2) lt = (int)LitLightType::POINT;
                l->type = (LitLightType)lt;
                int le = g_editorPanels.propLightEffect;
                if (le < 0 || le > 4) le = 0;
                l->effect = (LitLightEffect)le;
                // The panel edits the cone in degrees; LightNode stores
                // cos(half-angle) because that is what the shader compares
                // against, so convert on the way in.
                float innerDeg = fminf(fmaxf(g_editorPanels.propLightInnerAngle, 0.5f), 89.0f);
                float outerDeg = fminf(fmaxf(g_editorPanels.propLightOuterAngle, 1.0f), 89.0f);
                l->innerCone = cosf(innerDeg * DEG2RAD);
                l->outerCone = cosf(outerDeg * DEG2RAD);
                l->flare  = g_editorPanels.propLightFlare;
                l->corona = g_editorPanels.propLightCorona;
                l->target = {g_editorPanels.propLightTarget[0],
                             g_editorPanels.propLightTarget[1],
                             g_editorPanels.propLightTarget[2]};
                // A directional light is authored by its SOURCE point and
                // aimed at the world origin (see OzOzoneLoader), so the
                // panel's target row must be forced back to the origin or
                // the exported `light directional` line stops meaning what
                // the editor shows.
                if (l->type == LitLightType::DIRECTIONAL)
                    l->target = {0.0f, 0.0f, 0.0f};
                // Lights are bound to the zone volume that contains them;
                // a moved light needs that recomputed or it keeps lighting
                // the volume it used to sit in.
                PawnSystem::Instance().AssignLightZones();
            }
        } else if (tgtType == SelType::PORTAL) {
            auto& portals = ZoneManager::Instance().GetPortals();
            if (tgtIdx >= 0 && tgtIdx < (int)portals.size()) {
                auto& p = portals[tgtIdx];
                float szx = g_editorPanels.propSizeX;
                float szy = g_editorPanels.propSizeY;
                float szz = g_editorPanels.propSizeZ;
                p.bounds.min = {px - szx*0.5f, py - szy*0.5f, pz - szz*0.5f};
                p.bounds.max = {px + szx*0.5f, py + szy*0.5f, pz + szz*0.5f};
                // Destination fields (exported by ExportToOzone)
                if (!g_editorPanels.propPortalWorld.empty())
                    p.targetWorld = g_editorPanels.propPortalWorld;
                p.targetSpawn = {g_editorPanels.propPortalSpawn[0],
                                 g_editorPanels.propPortalSpawn[1],
                                 g_editorPanels.propPortalSpawn[2]};
                p.bidirectional = g_editorPanels.propPortalBidir;
            }
        } else if (tgtType == SelType::MESH) {
            MeshObjectNode* m = PawnSystem::Instance().GetMeshObject(tgtIdx);
            if (m) {
                m->position = {px, py, pz};
                if (g_editorPanels.propsTargetHasRotation) m->yaw = prot;
                if (g_editorPanels.propScale > 0.001f) m->scale = g_editorPanels.propScale;
                bool pathChanged = (g_editorPanels.propMeshPath != m->meshPath) ||
                                   (g_editorPanels.propMeshTex != m->texturePath);
                if (!g_editorPanels.propMeshPath.empty())
                    m->meshPath = g_editorPanels.propMeshPath;
                m->texturePath = g_editorPanels.propMeshTex;
                bool animChanged = (g_editorPanels.propMeshAnimFile != m->animFile);
                m->animFile = g_editorPanels.propMeshAnimFile;
                if (g_editorPanels.propMeshAnimSpeed > 0.0f)
                    m->animSpeed = g_editorPanels.propMeshAnimSpeed;
                // Embedded clip name only matters when there's no external clip.
                if (m->animFile.empty() && m->skeletal)
                    m->animClip = g_editorPanels.propAnimClip;
                if (!m->animFile.empty()) m->skeletal = true;
                m->windAffected = g_editorPanels.propMeshWind;
                if (pathChanged || animChanged) m->mesh.reset(); // re-resolve
            }
        } else if (tgtType == SelType::PARTICLE) {
            ParticleEmitterNode* e = PawnSystem::Instance().GetParticleEmitter(tgtIdx);
            if (e) {
                e->position = {px, py, pz};
                if (g_editorPanels.propsTargetHasRotation) e->yaw = prot;
                if (!g_editorPanels.propEmitterType.empty()) e->type = g_editorPanels.propEmitterType;
                e->texturePath = g_editorPanels.propEmitterTex;
                if (g_editorPanels.propEmitterRate >= 0.0f) e->rate = g_editorPanels.propEmitterRate;
                if (g_editorPanels.propEmitterLife > 0.0f) e->lifetime = g_editorPanels.propEmitterLife;
                if (g_editorPanels.propEmitterSpeed >= 0.0f) e->speed = g_editorPanels.propEmitterSpeed;
                if (g_editorPanels.propEmitterSize > 0.0f) e->sizeStart = g_editorPanels.propEmitterSize;
                e->spread = g_editorPanels.propEmitterSpread;
                e->colorStart.r = (unsigned char)g_editorPanels.propEmitterR;
                e->colorStart.g = (unsigned char)g_editorPanels.propEmitterG;
                e->colorStart.b = (unsigned char)g_editorPanels.propEmitterB;
                // End-of-life size/colour, gravity, spawn radius and the
                // billboard flag. These had no editor row before, so an Apply
                // silently reset them to the struct defaults - which is how a
                // hand-authored smoke fade turned into a uniform grey puff the
                // first time anybody nudged its rate in the editor.
                e->sizeEnd = g_editorPanels.propEmitterSizeEnd;
                e->colorEnd.r = (unsigned char)g_editorPanels.propEmitterREnd;
                e->colorEnd.g = (unsigned char)g_editorPanels.propEmitterGEnd;
                e->colorEnd.b = (unsigned char)g_editorPanels.propEmitterBEnd;
                e->gravity = g_editorPanels.propEmitterGravity;
                e->radius = g_editorPanels.propEmitterRadius;
                e->billboard = g_editorPanels.propEmitterBillboard;
            }
        } else if (tgtType == SelType::PATHNODE) {
            PathNode* pn = PawnSystem::Instance().GetPathNode(tgtIdx);
            if (pn) {
                pn->position = {px, py, pz};
                if (g_editorPanels.propPathRadius > 0.0f) pn->radius = g_editorPanels.propPathRadius;
                // Rename â€” retarget any links that referenced the old name
                std::string newName = g_editorPanels.propPathName;
                if (!newName.empty() && newName != pn->name) {
                    std::string oldName = pn->name;
                    pn->name = newName;
                    for (auto& other : PawnSystem::Instance().GetPathNodes())
                        for (auto& link : other.next)
                            if (link == oldName) link = newName;
                }
                // Parse comma-separated successor list
                pn->next.clear();
                std::string ns = g_editorPanels.propPathNext;
                size_t start = 0;
                while (start <= ns.size()) {
                    size_t comma = ns.find(',', start);
                    std::string part = ns.substr(
                        start, comma == std::string::npos ? std::string::npos : comma - start);
                    while (!part.empty() && (part.front() == ' ' || part.front() == '\t')) part.erase(part.begin());
                    while (!part.empty() && (part.back() == ' ' || part.back() == '\t')) part.pop_back();
                    if (!part.empty()) pn->next.push_back(part);
                    if (comma == std::string::npos) break;
                    start = comma + 1;
                }
                pn->loop = g_editorPanels.propPathLoop;
            }
        } else if (tgtType == SelType::WINDZONE) {
            WindZoneNode* z = PawnSystem::Instance().GetWindZone(tgtIdx);
            if (z) {
                float sx = g_editorPanels.propWindSizeX;
                float sy = g_editorPanels.propWindSizeY;
                float sz = g_editorPanels.propWindSizeZ;
                if (sx < 0.01f) sx = 1.0f;
                if (sy < 0.01f) sy = 1.0f;
                if (sz < 0.01f) sz = 1.0f;
                z->bounds.min = {px - sx * 0.5f, py - sy * 0.5f, pz - sz * 0.5f};
                z->bounds.max = {px + sx * 0.5f, py + sy * 0.5f, pz + sz * 0.5f};
                z->direction = {g_editorPanels.propWindDirX,
                                g_editorPanels.propWindDirY,
                                g_editorPanels.propWindDirZ};
                if (g_editorPanels.propWindStrength >= 0.0f)
                    z->strength = g_editorPanels.propWindStrength;
                if (g_editorPanels.propWindFrequency > 0.0f)
                    z->frequency = g_editorPanels.propWindFrequency;
            }
        }
        EditorLog("Applied properties to %s idx=%d", g_sel.name.c_str(), tgtIdx);
        }    }
}