#include "../Core/EditorShell.hpp"
#include "../Core/EditorState.hpp"
#include "Placement.hpp"

#include "../../../Source/Pawn/OzPawnSystem.hpp"
#include "../../../Source/World/OzOzoneLoader.hpp"
#include "../../../Source/World/ZoneManager.hpp"
// Subsystems/Placement.cpp
//
// FRAGMENT - not a standalone translation unit. Included by ../Main.cpp, which
// is the single TU for AngelEd's core layer. See
// Wiki/Editor-Architecture-Refactor.md.
// ============================================================================
//
// ApplyPlacementSpawns - R5: the world-mutation half of entity placement.
//
// Until this existed, SpawnSelectedPawnTreeItem in UI/Panels/PawnPanel.cpp called
// PawnSystem::Add* / ZoneManager::AddZone directly from a WM_COMMAND handler. That
// broke the layer rule stated in Wiki/Editor-Architecture-Refactor.md:
// "A panel posts an event; it never mutates the world directly. If a UI/ file calls
// PawnSystem::Add*, the layer rule is already broken."
//
// The panel now does the validation (which is what its error MessageBox needs, and
// it must stay synchronous for that) and posts one ed::Spawn* event carrying
// everything this file needs. Nothing is read from live panel or selection state
// here, so a tree rebuild or a selection change between the click and the frame
// cannot place a different entity.
//
// ONE POSITION, NOT TWO. g_editorPanels.spawnPos is assigned from
// OTEditor.MainCamera.target every frame in Main.cpp, so there is no separate
// "ghost position" - the camera aim point IS the placement point. An earlier draft of
// the plan claimed the old actionSpawn* handlers would have placed "at MainCamera.target
// instead of the ghost position, so they were not merely redundant but wrong". That was
// wrong: they would have been behaviourally identical. Recorded in the R1 notes because
// it was the strongest stated reason to keep placement synchronous, and that reason
// does not exist.

// Placement events, drained from the bus by ApplyPlacementSpawns. Kept as its own
// vector, not shared with g_editorFrameEvents, for the reason the surface and anim
// passes have theirs: so this file's handlers can be lifted into a real translation
// unit later without that move having to reorder the selection dispatch.

// How many times ApplyPlacementSpawns ran without a recognised payload. A Spawn*
// kind with no case here would otherwise be a silent no-op, which is exactly how
// the nine dead actionSpawn* fields survived as long as they did.
static int g_editorUnplacedEvents = 0;

// Path node names are auto-generated and must be unique within a session. This
// counter USED to live in PawnPanel.cpp, beside the code that consumed it; it moves
// here because this is now the only writer. Note it resets when the editor restarts,
// exactly as before - a saved world carries its own node names.
static int s_pathNodeCounter = 0;

void ApplyPlacementSpawns(const std::vector<ed::Event>& events) {
    for (const ed::Event& ev : events) {
        // ev.spawn() is the shape-checked accessor: on a payload mismatch it LOGS and
        // returns an inert SpawnDesc rather than reinterpret_cast-ing the variant,
        // which is the whole class of bug this bus exists to prevent. So there is no
        // holds_alternative check here - that would be a second, weaker mechanism
        // doing a job the accessor already does. An inert desc has an empty key and a
        // zero position, so a mismatch places nothing useful and is visible in the log.
        const ed::SpawnDesc& d = ev.spawn();
        if (d.key.empty() && ev.kind != ed::Ev::SpawnPlayerStart &&
            ev.kind != ed::Ev::SpawnWindZone) {
            // Every kind but these two needs a def name; an empty one means the event
            // was mis-shaped or was a stale default.
            EditorLog("Placement: event %d arrived with no def name, skipped", (int)ev.kind);
            g_editorUnplacedEvents++;
            continue;
        }
        auto& ps = PawnSystem::Instance();
        Vector3 pos = { d.at.x, d.at.y, d.at.z };

        // Every placement is undoable. This is a behaviour change: placement used to
        // be the one editor mutation that did NOT push a history snapshot, so a
        // Ctrl+Z after placing did nothing. It now matches delete / duplicate /
        // surface edit, which all call HistoryPush first.
        HistoryPush();

        switch (ev.kind) {
            case ed::Ev::SpawnPawn: {
                // Also the fallback for an unrecognised typeTag - the panel posts
                // SpawnPawn for anything it could not classify, which is what the
                // old trailing `else { ps.Spawn(...) }` did.
                int id = ps.Spawn(pos, d.key.c_str());
                if (id < 0)
                    EditorLog("Placement: pawn '%s' rejected by PawnSystem", d.key.c_str());
                else
                    EditorLog("Placement: pawn '%s' at %.1f %.1f %.1f", d.key.c_str(), pos.x, pos.y, pos.z);
                break;
            }

            case ed::Ev::SpawnPickup: {
                PickupNode n;
                n.position = pos;
                n.typeName = d.key;
                ps.AddPickup(n);
                break;
            }

            case ed::Ev::SpawnPlayerStart: {
                PlayerStartNode n;
                n.position = pos;
                n.yaw = d.at.yaw;
                ps.AddPlayerStart(n);
                break;
            }

            case ed::Ev::SpawnEmitter: {
                EmitterNode n;
                // MusicEmitter is the one def that is not a sound emitter. Keeping the
                // name test here rather than in the panel means the rule travels with
                // the mutation, not with the button that triggered it.
                n.type = (d.key == "MusicEmitter") ? EmitterType::MUSIC : EmitterType::SOUND;
                n.position = pos;
                ps.AddEmitter(n);
                break;
            }

            case ed::Ev::SpawnZone: {
                ZoneVolumeNode n;
                // w/h/d are HALF-extents. The panel sends 4/2/4, which reproduces the
                // old hardcoded bounds; a wind zone sends 5/5/5.
                n.bounds.min = { pos.x - d.at.w, pos.y - d.at.h, pos.z - d.at.d };
                n.bounds.max = { pos.x + d.at.w, pos.y + d.at.h, pos.z + d.at.d };
                n.zoneType = (ZoneType)d.kind;
                ZoneManager::Instance().AddZone(n);
                EditorLog("Placement: zone type %d at %.1f %.1f %.1f", (int)n.zoneType, pos.x, pos.y, pos.z);
                break;
            }

            case ed::Ev::SpawnMesh: {
                MeshObjectNode n;
                n.meshPath = d.key;          // the panel resolved the path already
                n.skeletal = d.skeletal;
                n.position = pos;
                n.yaw = d.at.yaw;
                n.scale = 1.0f;
                ps.AddMeshObject(n);
                break;
            }

            case ed::Ev::SpawnLight: {
                LightNode n;
                n.active = true;
                n.position = pos;
                n.color = WHITE;
                n.intensity = 1.0f;
                n.radius = 20.0f;
                n.type = (LitLightType)d.lightType;
                switch (n.type) {
                    case LitLightType::DIRECTIONAL:
                        // A directional light is authored by its SOURCE point and
                        // aimed at the world origin (see OzOzoneLoader), so it is
                        // seeded above the aim point rather than at it.
                        n.position = { pos.x, pos.y + 40.0f, pos.z };
                        n.target = { 0.0f, 0.0f, 0.0f };
                        n.intensity = 0.8f;
                        break;
                    case LitLightType::SPOT:
                        // Default aim: straight down from the emitter. inner_cone /
                        // outer_cone are left at their engine defaults so
                        // Light.Spot.ozls owns the cone, exactly as before.
                        n.target = { pos.x, pos.y - 10.0f, pos.z };
                        break;
                    default:
                        break;                 // POINT has no target
                }
                n.name = d.key;
                // The same defaults layer the OZONE load path uses, so editing
                // Light.Spot.ozls affects newly placed lights as well as loaded
                // worlds. color / intensity / radius / position / target stay local:
                // they are line-owned on every `light` line, so no def may move them.
                ApplyLightDefDefaultsToNode(n);
                ps.AddLight(n);
                break;
            }

            case ed::Ev::SpawnParticleEmitter: {
                ParticleEmitterNode n;
                n.type = "fire";
                n.position = pos;
                n.direction = { 0, 1, 0 };
                n.rate = 30.0f;
                n.lifetime = 0.9f;
                n.speed = 2.0f;
                n.spread = 12.0f;   // degrees, matching SpawnFromEmitter/ConeDirection
                n.sizeStart = 0.5f;
                n.sizeEnd = 0.0f;
                n.colorStart = { 255, 170, 60, 255 };
                n.colorEnd = { 80, 20, 10, 0 };
                n.radius = 0.2f;
                ps.AddParticleEmitter(n);
                break;
            }

            case ed::Ev::SpawnPathNode: {
                PathNode n;
                n.name = "path_" + std::to_string(s_pathNodeCounter++);
                n.position = pos;
                n.radius = 1.0f;
                ps.AddPathNode(n);
                break;
            }

            case ed::Ev::SpawnWindZone: {
                WindZoneNode n;
                n.bounds.min = { pos.x - d.at.w, pos.y - d.at.h, pos.z - d.at.d };
                n.bounds.max = { pos.x + d.at.w, pos.y + d.at.h, pos.z + d.at.d };
                n.direction = { 1, 0, 0 };
                n.strength = 1.0f;
                n.frequency = 1.0f;
                ps.AddWindZone(n);
                break;
            }

            default:
                // Not a Spawn* kind that reaches this function. g_editorFrameEvents
                // and this vector are drained separately, so in practice this is
                // unreachable; it is counted rather than ignored for the same reason
                // g_editorUnhandledEvents exists.
                EditorLog("Placement: unhandled event %d", (int)ev.kind);
                g_editorUnplacedEvents++;
                break;
        }
    }
}