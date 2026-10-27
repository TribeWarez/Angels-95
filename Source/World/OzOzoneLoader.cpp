#include "OzOzoneLoader.hpp"
#include "rlgl.h"
#include "OzoneFrustum.hpp"
#include "../Renderer/OzAssetMapper.hpp"
#include "../Renderer/SurfaceMaterial.hpp"
#include "../Renderer/CullState.hpp"
#include "../Pawn/OzPawnSystem.hpp"
#include "../Script/LightningEntityRegistry.hpp"
#include "../Events/TriggerSystem.hpp"
#include "../Boxed/BoxedTypeRegistry.hpp"
#include "OzoneParser.hpp"
#include "../Log.hpp"
#include "../Package/PackageAssetLoader.hpp"
#include "../Material/MaterialRegistry.hpp"
#include "../Physics/OzBsp.hpp"
#include "../Physics/AutoConvex.hpp"
#include <cstdio>
#include <cstring>
#include <cmath>

// Defined with the other per-face surface helpers further down, but needed by
// DrawSurface() which comes first.
static void BuildFaceMeshes(OzoneRenderable& r);
#include <algorithm>
#include <fstream>
#include <unordered_map>
#include <filesystem>
namespace fs = std::filesystem;

// StripQuotes is defined in OzoneParser.cpp (linked into both client and
// server) and declared in OzoneParser.hpp. It used to be static here, which
// meant GameState.cpp could not link against it.

// True for primitives that carry entity/level metadata rather than brush
// geometry â€” they are handled by ParseOzoneEntities, not the mesh builder.
//
// SKYBOX is a GEOMETRY primitive, so it is deliberately NOT in this list. It is
// filtered out of the collision pass instead, because a skybox is a hollow room:
// treating it as a solid would put an invisible wall around the player.
static bool IsEntityPrimitive(OzonePrimitiveType t) {
    switch (t) {
        case OzonePrimitiveType::ENTITY_PLAYERSTART:
        case OzonePrimitiveType::ENTITY_PICKUP:
        case OzonePrimitiveType::ENTITY_ZONE:
        case OzonePrimitiveType::ENTITY_NPC:
        case OzonePrimitiveType::ENTITY_LIGHT:
        case OzonePrimitiveType::ENTITY_PORTAL:
        case OzonePrimitiveType::ENTITY_LEVELINFO:
        case OzonePrimitiveType::ENTITY_PARTICLES:
        case OzonePrimitiveType::ENTITY_EMITTER:
        case OzonePrimitiveType::ENTITY_MESH_STATIC:
        case OzonePrimitiveType::ENTITY_MESH_SKELETAL:
        case OzonePrimitiveType::ENTITY_PARTICLE_EMITTER:
        case OzonePrimitiveType::ENTITY_PATH_NODE:
        case OzonePrimitiveType::ENTITY_WIND_ZONE:
            return true;
        default:
            return false;
    }
}

// Fill a light's unauthored fields from its `.ozls` def (EntityType::LIGHT),
// resolved by the light's `name=` kwarg - the same lookup zones use for their
// skyzone defs, so a world-scoped `torch.ozls` next to the world works without
// a new OZONE kwarg.
//
// DEFAULTS ONLY. A value the OZONE line authored always wins, which means
// intensity / radius / color / position / target can never be moved by editing
// the def: those are positional on every light line, and a def that could
// override them would silently retune every already saved level.
static void ApplyLightDefDefaults(LightNode& node, const OzonePrimitive& prim) {
    if (node.name.empty()) return;
    const EntityDef* def = LightningEntityRegistry::Instance().Find(node.name);
    if (!def || def->type != EntityType::LIGHT) return;

    const auto& S = def->stats;
    auto stat = [&](const char* key, float& dst, bool authored) {
        if (authored) return;
        auto it = S.floats.find(key);
        if (it != S.floats.end()) dst = it->second;
    };

    // effect / flare / corona apply only when the line omitted them, which the
    // parser records as -1 (see OzonePrimitive). They are authored as floats in
    // the def and clamped into the enum's range rather than reinterpret_cast -
    // a def is data, and a bad value in it must not reach memory as bits.
    if (prim.lightEffect < 0) {
        auto it = S.floats.find("effect");
        if (it != S.floats.end()) {
            const int e = (int)it->second;
            if (e >= (int)LitLightEffect::NONE && e <= (int)LitLightEffect::LAMP)
                node.effect = (LitLightEffect)e;
        }
    }
    if (prim.lightFlare < 0) {
        auto it = S.floats.find("flare");
        if (it != S.floats.end()) node.flare = (it->second != 0.0f);
    }
    if (prim.lightCorona < 0) {
        auto it = S.floats.find("corona");
        if (it != S.floats.end()) node.corona = (it->second != 0.0f);
    }

    // Always def-owned: the light line cannot express these at all.
    stat("period", node.period, false);
    if (auto it = S.floats.find("cast_shadow"); it != S.floats.end())
        node.castShadow = (it->second != 0.0f);
    if (auto it = S.floats.find("is_static"); it != S.floats.end())
        node.isStatic = (it->second != 0.0f);
    stat("inner_cone", node.innerCone, false);
    stat("outer_cone", node.outerCone, false);
}

// Apply the same defaults layer to a light built in CODE rather than parsed from
// an OZONE line (the editor's GameEngine.Light placement). A code-built node
// authored nothing, so it behaves as if every optional field on the line was
// omitted: effect / flare / corona and the def-owned period / cast_shadow /
// is_static / inner_cone / outer_cone all come from the def.
//
// Without this the editor hardcoded WHITE / 1.0 / radius 20 / cone 0.95,0.80 in
// the spawn handler, so editing Light.Point.ozls changed loaded worlds but not
// newly placed lights - two different sources of truth for the same thing.
void ApplyLightDefDefaultsToNode(LightNode& node) {
    OzonePrimitive prim;
    prim.type = OzonePrimitiveType::ENTITY_LIGHT;
    prim.name = node.name;
    prim.lightEffect = -1;
    prim.lightFlare = -1;
    prim.lightCorona = -1;
    ApplyLightDefDefaults(node, prim);
}

static bool ParseOzoneEntity(const OzonePrimitive& prim,
                             OzoneZoneCounters& zoneCounters,
                             const std::string& worldDir,
                             OzoneEntitySet& out) {
    auto& settings = out.settings;
    switch (prim.type) {
        case OzonePrimitiveType::ENTITY_PLAYERSTART:
            if (prim.args.size() >= 3) {
                out.playerStarts.push_back(
                    {0, {prim.args[0], prim.args[2], prim.args[1]},
                     prim.args.size() >= 4 ? prim.args[3] : 0.0f});
            }
            return true;
        case OzonePrimitiveType::ENTITY_PICKUP:
            if (prim.args.size() >= 3) {
                PickupNode node;
                node.position = {prim.args[0], prim.args[2], prim.args[1]};
                node.typeName = prim.entityType;
                if (prim.args.size() >= 4) node.respawnTime = prim.args[3];
                // Server id = file-order index among ENTITY_PICKUP primitives.
                // Must stay in lockstep with seed_world_entities' nextPickupId++;
                // see the PickupNode::netId comment for why.
                node.netId = (int)out.pickups.size();
                out.pickups.push_back(node);
            }
            return true;
        case OzonePrimitiveType::ENTITY_ZONE:
            if (prim.args.size() >= 6) {
                ZoneVolumeNode node;
                node.bounds.min = {
                    std::min(prim.args[0], prim.args[3]),
                    std::min(prim.args[2], prim.args[5]),
                    std::min(prim.args[1], prim.args[4])
                };
                node.bounds.max = {
                    std::max(prim.args[0], prim.args[3]),
                    std::max(prim.args[2], prim.args[5]),
                    std::max(prim.args[1], prim.args[4])
                };
                node.zoneType = ZoneTypeFromString(prim.entitySubType);
                if (prim.args.size() >= 7) node.intensity = prim.args[6];
                // Generate unique zone name for .ozls script hook matching.
                // An explicit name= kwarg overrides the auto-generated one so
                // scripted zones survive zone reordering in the editor.
                auto& counter = zoneCounters[prim.entitySubType];
                node.name = prim.name.empty()
                    ? ("zone_" + prim.entitySubType + "_" + std::to_string(counter++))
                    : prim.name;
                // Extended env override args (optional, after intensity):
                // fogR fogG fogB fogDensity fogStart fogEnd ambR ambG ambB ambIntensity reverbMix reverbDecay
                if (prim.args.size() >= 17) {
                    node.envOverrides.applyFog = true;
                    node.envOverrides.fogR = (int)prim.args[7];
                    node.envOverrides.fogG = (int)prim.args[8];
                    node.envOverrides.fogB = (int)prim.args[9];
                    node.envOverrides.fogDensity = prim.args[10];
                    node.envOverrides.fogStart = prim.args[11];
                    node.envOverrides.fogEnd = prim.args[12];
                    node.envOverrides.applyAmbient = true;
                    node.envOverrides.ambR = (int)prim.args[13];
                    node.envOverrides.ambG = (int)prim.args[14];
                    node.envOverrides.ambB = (int)prim.args[15];
                    node.envOverrides.ambIntensity = prim.args[16];
                    // Reverb args: 18 args => reverbMix only (decay keeps its
                    // default); 19 args => mix + decay (off-by-one fixed).
                    if (prim.args.size() >= 18)
                        node.envOverrides.reverbMix = prim.args[17];
                    if (prim.args.size() >= 19)
                        node.envOverrides.reverbDecay = prim.args[18];
                }
                // Per-zone physics overrides (named kwargs; absent = defaults)
                node.physics = prim.physics;
                out.zones.push_back(node);

                // For sky zones, also build a SkyZoneNode
                if (node.zoneType == ZoneType::ZONE_SKY) {
                    SkyZoneNode skyNode;
                    skyNode.bounds = node.bounds;
                    skyNode.position = {
                        (node.bounds.min.x + node.bounds.max.x) * 0.5f,
                        (node.bounds.min.y + node.bounds.max.y) * 0.5f,
                        (node.bounds.min.z + node.bounds.max.z) * 0.5f
                    };
                    skyNode.name = node.name;
                    skyNode.intensity = node.intensity;
                    // Look up .ozls SKYZONE entity by name for initial config.
                    // The global registry holds defs from ALL worlds, and zone
                    // names are generated per world (e.g. "zone_sky_0") â€” so a
                    // name hit may belong to another world. Disambiguate by
                    // matching the def's skybox path against this world dir.
                    const EntityDef* edef = LightningEntityRegistry::Instance().Find(node.name);
                    if (edef && edef->type == EntityType::SKYZONE) {
                        if (!worldDir.empty() && edef->skybox.find(worldDir) == std::string::npos)
                            edef = nullptr; // name collided with another world's def
                    } else {
                        edef = nullptr;
                    }
                    if (!edef) {
                        std::vector<const EntityDef*> skyDefs;
                        LightningEntityRegistry::Instance().FindByType(EntityType::SKYZONE, skyDefs);
                        for (auto* d : skyDefs) {
                            if (!worldDir.empty() &&
                                d->skybox.find(worldDir) != std::string::npos) {
                                edef = d;
                                break;
                            }
                        }
                    }
                    if (edef && edef->type == EntityType::SKYZONE) {
                        skyNode.def = edef;
                        skyNode.skyboxPath = edef->skybox;
                        OZ_INFO("OZONE sky zone '%s': resolved def '%s' skybox='%s'",
                                node.name.c_str(), edef->name.c_str(), edef->skybox.c_str());
                        // Look up fov from stats if available
                        auto fit = edef->stats.floats.find("fov");
                        if (fit != edef->stats.floats.end())
                            skyNode.fov = fit->second;
                        auto sit = edef->stats.vec3s.find("scroll_speed");
                        if (sit != edef->stats.vec3s.end()) {
                            skyNode.scrollSpeed = {sit->second[0], sit->second[1], sit->second[2]};
                        }
                    } else if (!worldDir.empty()) {
                        // Registry lookup failed (name collided with another world's
                        // def). Read the skybox path straight from this world's own
                        // .ozls files so the correct texture still applies.
                        namespace fs = std::filesystem;
                        std::error_code ec;
                        for (auto& entry : fs::directory_iterator(worldDir, ec)) {
                            if (ec || !entry.is_regular_file()) continue;
                            std::string ext = entry.path().extension().string();
                            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                            if (ext != ".ozls") continue;
                            std::ifstream in(entry.path());
                            if (!in.is_open()) continue;
                            std::string line;
                            while (std::getline(in, line)) {
                                size_t sky = line.find("skybox");
                                if (sky == std::string::npos) continue;
                                size_t q1 = line.find('"', sky);
                                size_t q2 = (q1 == std::string::npos)
                                    ? std::string::npos : line.find('"', q1 + 1);
                                if (q2 == std::string::npos) continue;
                                skyNode.skyboxPath = line.substr(q1 + 1, q2 - q1 - 1);
                                OZ_INFO("OZONE sky zone '%s': skybox from world file '%s'",
                                        node.name.c_str(), skyNode.skyboxPath.c_str());
                                break;
                            }
                            if (!skyNode.skyboxPath.empty()) break;
                        }
                    }
                    out.skyZones.push_back(skyNode);
                }
            }
            return true;
        case OzonePrimitiveType::ENTITY_NPC:
            // NPC spawning goes through PawnDef resolution, which only exists in
            // the entity system, so it is recorded as a pending spawn request.
            if (prim.args.size() >= 3) {
                PawnSpawnRequest req;
                req.position = {prim.args[0], prim.args[2], prim.args[1]};
                req.defName = prim.entityType;
                out.pawnSpawns.push_back(req);
            }
            return true;
        case OzonePrimitiveType::ENTITY_MESH_STATIC:
        case OzonePrimitiveType::ENTITY_MESH_SKELETAL: {
            // Mesh.Static|Skeletal <meshPath> x y z yaw [scale=N] [tex=path] [anim=Clip] [speed=N]
            if (prim.args.size() >= 4) {
                MeshObjectNode node;
                node.meshPath = StripQuotes(prim.meshPath);
                node.texturePath = StripQuotes(prim.texPath);
                node.position = {prim.args[0], prim.args[2], prim.args[1]}; // Z-up -> Y-up
                node.yaw = prim.args[3];
                node.scale = prim.args.size() >= 5 ? prim.args[4] : 1.0f;
                node.skeletal = (prim.type == OzonePrimitiveType::ENTITY_MESH_SKELETAL);
                node.windAffected = prim.meshWind;
                node.animClip = StripQuotes(prim.animClip);
                node.animFile = StripQuotes(prim.animFile);
                node.animSpeed = prim.animSpeed;
                node.materialName = StripQuotes(prim.materialName);
                node.baseDir = worldDir.empty() ? std::string() : (worldDir + "/");
                out.meshObjects.push_back(node);
            }
            return true;
        }
        case OzonePrimitiveType::ENTITY_WIND_ZONE: {
            // WindZone minX minY minZ maxX maxY maxZ dirX dirY dirZ strength [freq]
            if (prim.args.size() >= 10) {
                auto arg = [&](int i, float def) -> float {
                    return (i >= 0 && i < (int)prim.args.size()) ? prim.args[i] : def;
                };
                WindZoneNode z;
                z.bounds.min = {std::min(prim.args[0], prim.args[3]),
                                std::min(prim.args[2], prim.args[5]),
                                std::min(prim.args[1], prim.args[4])};
                z.bounds.max = {std::max(prim.args[0], prim.args[3]),
                                std::max(prim.args[2], prim.args[5]),
                                std::max(prim.args[1], prim.args[4])};
                z.direction = {arg(6, 1.0f), arg(8, 0.0f), arg(7, 0.0f)}; // Z-up -> Y-up
                z.strength = arg(9, 1.0f);
                z.frequency = arg(10, 1.0f);
                out.windZones.push_back(z);
            }
            return true;
        }
        case OzonePrimitiveType::ENTITY_DOOR: {
            // door <meshPath> x y z yaw [key=] [anim=] [locked=]
            if (prim.args.size() >= 5) {
                DoorNode d;
                d.meshPath = StripQuotes(prim.meshPath);
                d.position = {prim.args[0], prim.args[2], prim.args[1]}; // Z-up -> Y-up
                d.yaw = prim.args[3];
                d.keyName = prim.doorKey;
                d.animClip = StripQuotes(prim.doorAnim);
                d.locked = prim.doorLocked;
                out.doors.push_back(d);
            }
            return true;
        }
        case OzonePrimitiveType::ENTITY_TRIGGER: {
            // trigger <def> minX minY minZ maxX maxY maxZ [once] [team=N]
            // args[0..5] = bounds, args[6] = once, args[7] = team (see OzoneParser).
            if (prim.args.size() >= 6) {
                TriggerNode t;
                t.defName = StripQuotes(prim.entityType);
                t.bounds.min = {std::min(prim.args[0], prim.args[3]),
                                std::min(prim.args[2], prim.args[5]),
                                std::min(prim.args[1], prim.args[4])};
                t.bounds.max = {std::max(prim.args[0], prim.args[3]),
                                std::max(prim.args[2], prim.args[5]),
                                std::max(prim.args[1], prim.args[4])};
                if (prim.args.size() >= 7) t.once = prim.args[6] != 0.0f;
                if (prim.args.size() >= 8) t.team = (int)prim.args[7];
                out.triggers.push_back(t);
            }
            return true;
        }
        case OzonePrimitiveType::ENTITY_PATH_NODE: {
            // PathNode <name> x y z [radius=R] [next=a,b,c] [loop]
            if (prim.args.size() >= 3) {
                PathNode node;
                node.name = prim.entityType.empty()
                    ? ("path_" + std::to_string((int)prim.args[0]) + "_" +
                       std::to_string((int)prim.args[2]))
                    : prim.entityType;
                node.position = {prim.args[0], prim.args[2], prim.args[1]}; // Z-up -> Y-up
                if (prim.args.size() >= 4) node.radius = prim.args[3];
                node.loop = prim.pathLoop;
                std::string names = StripQuotes(prim.entitySubType);
                size_t start = 0;
                while (start <= names.size()) {
                    size_t comma = names.find(',', start);
                    std::string part = names.substr(
                        start, comma == std::string::npos ? std::string::npos : comma - start);
                    if (!part.empty()) node.next.push_back(part);
                    if (comma == std::string::npos) break;
                    start = comma + 1;
                }
                out.pathNodes.push_back(node);
            }
            return true;
        }
        case OzonePrimitiveType::ENTITY_PARTICLE_EMITTER: {
            // ParticleEmitter <type> x y z [rate life speed spread sizeStart sizeEnd
            //   r g b rEnd gEnd bEnd gravity radius dirX dirY dirZ yaw] [tex=path]
            //
            // `spread` is a HALF-ANGLE IN DEGREES (arg 6). It used to be a raw
            // unit-vector offset, so the `spread 10..20` every shipped fire
            // emitter authors was read as 10-20 world units of jitter per axis
            // and produced an isotropic spray rather than a cone. See
            // oz::particles::ConeDirection.
            if (prim.args.size() < 3) {
                // A short line used to vanish here with no diagnostic - the same
                // failure portal/trigger had: it parses, never reaches a runtime
                // container, and the next editor save deletes it.
                OZ_WARN("OZONE line %d: ParticleEmitter needs at least x y z - line dropped",
                        prim.line);
                return true;
            }
            {
                // A named kwarg (particleOverrides) wins over the positional
                // slot it names, so `gravity=-0.8` retunes one value without
                // restating the twenty-two around it. Overrides are applied
                // LAST, after every positional default is resolved, so a
                // kwarg never loses to a positional value.
                auto arg = [&](int i, float def) -> float {
                    for (const auto& ov : prim.particleOverrides)
                        if (ov.first == i) return ov.second;
                    return (i >= 0 && i < (int)prim.args.size()) ? prim.args[i] : def;
                };
                // Colours are components, so they must CLAMP rather than wrap.
                // A straight cast turned an out-of-range authored value (300)
                // into a different colour (44) with no warning at all.
                auto byte = [](float v) -> unsigned char {
                    return (unsigned char)std::clamp(v, 0.0f, 255.0f);
                };
                ParticleEmitterNode node;
                node.type = prim.entityType.empty() ? "fire" : prim.entityType;
                node.position = {prim.args[0], prim.args[2], prim.args[1]}; // Z-up -> Y-up
                node.rate = arg(3, 20.0f);
                node.lifetime = arg(4, 1.0f);
                node.speed = arg(5, 2.0f);
                node.spread = arg(6, 12.0f);
                node.sizeStart = arg(7, 0.4f);
                node.sizeEnd = arg(8, 0.0f);
                node.colorStart = (Color){byte(arg(9, 255)), byte(arg(10, 180)),
                                          byte(arg(11, 80)), 255};
                node.colorEnd = (Color){byte(arg(12, 60)), byte(arg(13, 20)),
                                        byte(arg(14, 10)), 0};
                node.gravity = arg(15, 0.0f);
                node.radius = arg(16, 0.0f);
                float dx = arg(17, 0.0f), dy = arg(18, 1.0f), dz = arg(19, 0.0f);
                node.direction = {dx, dz, dy}; // Z-up -> Y-up
                node.yaw = arg(20, 0.0f);
                node.texturePath = StripQuotes(prim.texPath);
                out.particleEmitters.push_back(node);
            }
            return true;
        }
        case OzonePrimitiveType::ENTITY_EMITTER:
            // emitter sound|music x y z  (Z-up conversion like the other entities)
            if (prim.args.size() >= 3) {
                EmitterNode node;
                node.type = (prim.entityType == "music") ? EmitterType::MUSIC : EmitterType::SOUND;
                node.position = {prim.args[0], prim.args[2], prim.args[1]};
                out.emitters.push_back(node);
            }
            return true;

        case OzonePrimitiveType::ENTITY_LIGHT: {
            LightNode node;
            node.active = true;
            std::string subtype = prim.entityType;
            auto arg = [&](int i) -> float {
                return (i >= 0 && i < (int)prim.args.size()) ? prim.args[i] : 0.0f;
            };
            if (subtype == "point" && prim.args.size() >= 7) {
                // light point x y z r g b intensity radius [effect] [flare] [corona]
                node.type = LitLightType::POINT;
                node.position = {arg(0), arg(2), arg(1)}; // Z-up conversion
                node.color = (Color){(unsigned char)arg(3), (unsigned char)arg(4), (unsigned char)arg(5), 255};
                node.intensity = arg(6);
                node.radius = arg(7);
                if (prim.args.size() >= 9) node.effect = (LitLightEffect)(int)arg(8);
                if (prim.args.size() >= 10) node.flare = arg(9) != 0.0f;
                if (prim.args.size() >= 11) node.corona = arg(10) != 0.0f;
            } else if (subtype == "spot" && prim.args.size() >= 12) {
                // light spot x y z tx ty tz r g b intensity radius innerCone
                //          outerCone [effect] [flare] [corona]
                node.type = LitLightType::SPOT;
                node.position = {arg(0), arg(2), arg(1)};
                node.target = {arg(3), arg(5), arg(4)};
                node.color = (Color){(unsigned char)arg(6), (unsigned char)arg(7), (unsigned char)arg(8), 255};
                node.intensity = arg(9);
                node.radius = arg(10);
                node.innerCone = arg(11);
                node.outerCone = arg(12);
                if (prim.args.size() >= 14) node.effect = (LitLightEffect)(int)arg(13);
                if (prim.args.size() >= 15) node.flare = arg(14) != 0.0f;
                if (prim.args.size() >= 16) node.corona = arg(15) != 0.0f;
            } else if (subtype == "directional" && prim.args.size() >= 6) {
                // light directional x y z r g b intensity [flare] [corona]
                node.type = LitLightType::DIRECTIONAL;
                node.target = {arg(0), arg(2), arg(1)};
                // A directional light has no emitter position, only an aim
                // point. The lighting shader resolves its direction as
                //     lightDir = normalize(position - target)
                // so leaving position at the origin made lightDir point from the
                // authored point DOWN to the world: for an overhead sun
                // (`... 100` in OZONE z) every up-facing surface got NdotL = 0 and
                // the ground was lit by ambient alone. That is why outdoor
                // levels needed absurd ambient values to be visible at all.
                //
                // Treat the authored point as the light SOURCE and aim at the
                // world origin, which is the intuitive reading of
                // `light directional <x> <y> <z>` and makes an overhead sun
                // light the ground.
                node.position = node.target;
                node.target = {0.0f, 0.0f, 0.0f};

                // A directional light's direction is `normalize(position - target)`
                // and the target is hardcoded to the world origin, so an authored
                // source at the ORIGIN makes the direction normalize(vec3(0)).
                //
                // In GLSL normalize() of a zero vector is undefined - in practice
                // NaN - and NaN propagates through NdotL, where max(NaN, 0.0) is
                // implementation-defined. The light then contributes nothing while
                // still occupying a slot in the 32-light budget, and
                // LitLightning_UpdateFrame's `anyActiveLight` scan counts it, so it
                // also SUPPRESSES the camera fill light: a level whose suns are all
                // degenerate renders with no light at all.
                //
                // `light directional 0 0 0 ...` appears three times in the shipped
                // worlds, so this is not hypothetical.
                //
                // Fall back to the same direction the light gizmo already uses for
                // the degenerate case (OzPawnSystem.cpp, `travel` fallback), so the
                // gizmo and the shader agree. Displace the SOURCE, keeping the aim
                // at the origin, so the resulting direction is straight down.
                if (Vector3Length(node.position) < 0.01f) {
                    node.position = {0.0f, -1.0f, 0.0f};
                    OZ_WARN("OZONE: directional light has no aim point "
                            "(source at the world origin); treated as a straight-down sun");
                }
                node.color = (Color){(unsigned char)arg(3), (unsigned char)arg(4), (unsigned char)arg(5), 255};
                node.intensity = arg(6);
                if (prim.args.size() >= 8) node.flare = arg(7) != 0.0f;
                if (prim.args.size() >= 9) node.corona = arg(8) != 0.0f;
            } else {
                OZ_WARN("OZONE: invalid light definition (subtype=%s args=%zu)", subtype.c_str(), prim.args.size());
                return true;
            }
            // Named kwargs win over the positional tail. They are what the
            // editor writes, because the positional form is ambiguous: with no
            // `effect` authored, a following flare float slid into the effect
            // slot and a TORCH-less flickering light came back as WATERY.
            if (prim.lightEffect >= 0) node.effect = (LitLightEffect)prim.lightEffect;
            if (prim.lightFlare  >= 0) node.flare  = prim.lightFlare != 0;
            if (prim.lightCorona >= 0) node.corona = prim.lightCorona != 0;
            if (!prim.name.empty())   node.name = prim.name;

            // `.ozls` defaults layer. `name=` resolves a def exactly the way a
            // zone's name= resolves its skyzone def, and the def fills only the
            // values the line did NOT author.
            //
            // intensity / radius / color / position / target stay line-owned on
            // purpose: the OZONE light line always writes them positionally, so a
            // def that could override them would silently retune every already
            // saved level the next time the def was touched - the exact hazard
            // the positional/kwarg split above was added to avoid.
            ApplyLightDefDefaults(node, prim);
            out.lights.push_back(node);
            return true;
        }
        case OzonePrimitiveType::ENTITY_PORTAL: {
            // portal targetWorld minX minY minZ maxX maxY maxZ [spawnX spawnY spawnZ] [bidir]
            // Coordinates are OZONE Z-up â€” convert to engine Y-up.
            if (prim.args.size() >= 6) {
                ZonePortal portal;
                portal.targetWorld = prim.entityType;
                portal.bounds.min = {
                    std::min(prim.args[0], prim.args[3]),
                    std::min(prim.args[2], prim.args[5]),
                    std::min(prim.args[1], prim.args[4])
                };
                portal.bounds.max = {
                    std::max(prim.args[0], prim.args[3]),
                    std::max(prim.args[2], prim.args[5]),
                    std::max(prim.args[1], prim.args[4])
                };
                if (prim.args.size() >= 9) {
                    portal.targetSpawn = {prim.args[6], prim.args[8], prim.args[7]}; // Z-up conversion
                } else {
                    portal.targetSpawn = {
                        (portal.bounds.min.x + portal.bounds.max.x) * 0.5f,
                        portal.bounds.min.y,
                        (portal.bounds.min.z + portal.bounds.max.z) * 0.5f
                    };
                }
                if (prim.args.size() >= 10) portal.bidirectional = prim.args[9] != 0.0f;
                out.portals.push_back(portal);
            }
            return true;
        }
        case OzonePrimitiveType::ENTITY_LEVELINFO:
            // levelinfo gameType maxPlayers respawnTime timeLimitEnabled timeLimitMinutes
            //           scoreLimit friendlyFire skyboxPath [skyboxSidePath]
            ParseLevelInfo(settings, prim.args,
                            StripQuotes(prim.entityType),
                            StripQuotes(prim.entitySubType),
                            StripQuotes(prim.gametypeKey));
            return true;
        case OzonePrimitiveType::ENTITY_PARTICLES:
            // particles type density speed r g b windX windZ
            ParseLevelParticles(settings, prim.args);
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Entity ingestion
// ---------------------------------------------------------------------------
void ParseOzoneEntities(const std::vector<OzonePrimitive>& primitives,
                        const std::string& worldDir,
                        OzoneZoneCounters& zoneCounters,
                        OzoneEntitySet& out) {
    for (const auto& prim : primitives)
        ParseOzoneEntity(prim, zoneCounters, worldDir, out);
}

// ---------------------------------------------------------------------------
// Boxed types — one inject hook per engine world-entity type. This TU is the
// only place that knows the concrete types, so it casts the opaque
// BoxedInjectContext back. InjectOzoneEntities iterates the registry, so a game
// type registers itself without editing this file.
// ---------------------------------------------------------------------------
namespace {

void InjectPlayerStarts(const oz::boxed::BoxedInjectContext& c) {
    for (const auto& n : c.entities->playerStarts) c.pawns->AddPlayerStart(n);
}
void InjectPickups(const oz::boxed::BoxedInjectContext& c) {
    for (const auto& n : c.entities->pickups) c.pawns->AddPickup(n);
}
void InjectZones(const oz::boxed::BoxedInjectContext& c) {
    for (const auto& n : c.entities->zones) c.zones->AddZone(n);
}
void InjectSkyZones(const oz::boxed::BoxedInjectContext& c) {
    for (const auto& n : c.entities->skyZones) c.pawns->AddSkyZone(n);
}
void InjectPortals(const oz::boxed::BoxedInjectContext& c) {
    for (const auto& n : c.entities->portals) c.zones->AddPortal(n);
}
void InjectLights(const oz::boxed::BoxedInjectContext& c) {
    for (const auto& n : c.entities->lights) c.pawns->AddLight(n);
}
void InjectMeshObjects(const oz::boxed::BoxedInjectContext& c) {
    for (const auto& n : c.entities->meshObjects) c.pawns->AddMeshObject(n);
}
void InjectPathNodes(const oz::boxed::BoxedInjectContext& c) {
    for (const auto& n : c.entities->pathNodes) c.pawns->AddPathNode(n);
}
void InjectWindZones(const oz::boxed::BoxedInjectContext& c) {
    for (const auto& n : c.entities->windZones) c.pawns->AddWindZone(n);
}
void InjectDoors(const oz::boxed::BoxedInjectContext& c) {
    for (const auto& n : c.entities->doors) c.pawns->AddDoor(n);
}
void InjectParticleEmitters(const oz::boxed::BoxedInjectContext& c) {
    for (const auto& n : c.entities->particleEmitters) c.pawns->AddParticleEmitter(n);
}
void InjectEmitters(const oz::boxed::BoxedInjectContext& c) {
    for (const auto& n : c.entities->emitters) c.pawns->AddEmitter(n);
}
void InjectTriggers(const oz::boxed::BoxedInjectContext& c) {
    for (const auto& t : c.entities->triggers) {
        oz::events::TriggerVolume v;
        v.defName = t.defName;
        v.minX = t.bounds.min.x; v.minY = t.bounds.min.y; v.minZ = t.bounds.min.z;
        v.maxX = t.bounds.max.x; v.maxY = t.bounds.max.y; v.maxZ = t.bounds.max.z;
        v.once = t.once;
        v.team = t.team;
        c.triggers->AddTrigger(v);
    }
}
void InjectPawnSpawns(const oz::boxed::BoxedInjectContext& c) {
    for (const auto& req : c.entities->pawnSpawns) c.pawns->Spawn(req.position, req.defName.c_str());
}

void RegisterEngineBoxedTypes() {
    using oz::boxed::BoxedTypeDesc;
    auto& r = oz::boxed::BoxedTypeRegistry::Instance();
    r.Register(BoxedTypeDesc{"playerstart",     "engine", &InjectPlayerStarts});
    r.Register(BoxedTypeDesc{"pickup",          "engine", &InjectPickups});
    r.Register(BoxedTypeDesc{"zone",            "engine", &InjectZones});
    r.Register(BoxedTypeDesc{"skyzone",         "engine", &InjectSkyZones});
    r.Register(BoxedTypeDesc{"portal",          "engine", &InjectPortals});
    r.Register(BoxedTypeDesc{"light",           "engine", &InjectLights});
    r.Register(BoxedTypeDesc{"Mesh.Static",     "engine", &InjectMeshObjects});
    r.Register(BoxedTypeDesc{"Mesh.Skeletal",   "engine", &InjectMeshObjects});
    r.Register(BoxedTypeDesc{"PathNode",        "engine", &InjectPathNodes});
    r.Register(BoxedTypeDesc{"WindZone",        "engine", &InjectWindZones});
    r.Register(BoxedTypeDesc{"door",            "engine", &InjectDoors});
    r.Register(BoxedTypeDesc{"ParticleEmitter", "engine", &InjectParticleEmitters});
    r.Register(BoxedTypeDesc{"emitter",         "engine", &InjectEmitters});
    r.Register(BoxedTypeDesc{"trigger",         "engine", &InjectTriggers});
    r.Register(BoxedTypeDesc{"npc",             "engine", &InjectPawnSpawns});
}

} // namespace

void InjectOzoneEntities(const OzoneEntitySet& entities, PawnSystem& pawns) {
    static bool s_registered = false;
    if (!s_registered) { RegisterEngineBoxedTypes(); s_registered = true; }

    oz::boxed::BoxedInjectContext ctx;
    ctx.entities = &entities;
    ctx.pawns    = &pawns;
    ctx.zones    = &ZoneManager::Instance();
    ctx.triggers = &oz::events::TriggerSystem::Instance();
    for (const auto& t : oz::boxed::BoxedTypeRegistry::Instance().All()) {
        if (t.inject) t.inject(ctx);
    }

    // Lights only make sense bound to a volume, and volumes live in ZoneManager.
    pawns.AssignLightZones();
    pawns.GetWorldInfo().settings = entities.settings;
}

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------
OzoneLoader& OzoneLoader::Instance() {
    static OzoneLoader instance;
    return instance;
}

Shader OzoneLoader::s_litFogShader = {0};
Shader OzoneLoader::s_backupLitFogShader = {0};

void OzoneLoader::SetLitFogShader(Shader shader) {
    s_litFogShader = shader;
    for (auto& r : m_renderables) {
        if (r.loaded && r.model.meshCount > 0 && shader.id > 0)
            r.model.materials[0].shader = shader;
    }
    if (m_hmReady && m_hmModel.meshCount > 0 && shader.id > 0)
        m_hmModel.materials[0].shader = shader;
}

void OzoneLoader::SetLitFogShaderEnabled(bool enabled) {
    if (enabled) {
        s_litFogShader = s_backupLitFogShader;
        // Re-apply restored shader to all existing renderables
        Shader sh = s_litFogShader;
        for (auto& r : m_renderables) {
            if (r.loaded && r.model.meshCount > 0 && sh.id > 0)
                r.model.materials[0].shader = sh;
        }
        if (m_hmReady && m_hmModel.meshCount > 0 && sh.id > 0)
            m_hmModel.materials[0].shader = sh;
    } else {
        if (s_litFogShader.id > 0)
            s_backupLitFogShader = s_litFogShader;
        for (auto& r : m_renderables) {
            if (r.loaded && r.model.meshCount > 0)
                r.model.materials[0].shader = Shader{0};
        }
        if (m_hmReady && m_hmModel.meshCount > 0)
            m_hmModel.materials[0].shader = Shader{0};
        s_litFogShader = Shader{0};
    }
}

// ---------------------------------------------------------------------------
// World texture loading â€” loads ALL .png from oztex/tileset/ into vector
// Textures are indexed 1..N by their sorted filename order.
// ---------------------------------------------------------------------------
void OzoneLoader::LoadWorldTextures(const std::string& worldDir) {
    UnloadTextures();
    std::string tilesetDir = worldDir + "oztex/tileset/";
    if (!fs::exists(tilesetDir)) return;

    std::vector<std::string> texFiles;
    for (auto& entry : fs::directory_iterator(tilesetDir)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".png")
            texFiles.push_back(entry.path().filename().string());
    }
    if (texFiles.empty()) return;

    std::sort(texFiles.begin(), texFiles.end());
    for (auto& f : texFiles) {
        Texture2D tex = LoadTexture((tilesetDir + f).c_str());
        if (tex.id) {
            m_tilesetTex.push_back(tex);
            OZ_INFO("OzoneLoader: tex[%zu] = %s", m_tilesetTex.size(), f.c_str());
        }
    }
    OZ_INFO("OzoneLoader: loaded %zu textures from %s", m_tilesetTex.size(), tilesetDir.c_str());
}

void OzoneLoader::UnloadTextures() {
    for (auto& tex : m_tilesetTex)
        if (tex.id) UnloadTexture(tex);
    m_tilesetTex.clear();
}

// ---------------------------------------------------------------------------
// Re-apply texture on a model based on texSlot (1-based index into tileset)
// ---------------------------------------------------------------------------
void OzoneLoader::ApplyTexSlotToModel(Model& model, int slot) {
    if (model.meshCount == 0) return;
    Texture2D tex{0};
    Color fallback = LIGHTGRAY;
    if (slot >= 1 && slot <= (int)m_tilesetTex.size()) {
        tex = m_tilesetTex[slot - 1];
    }
    if (tex.id)
        model.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = tex;
    else
        model.materials[0].maps[MATERIAL_MAP_DIFFUSE].color = fallback;
    if (GetLitFogShader().id > 0)
        model.materials[0].shader = GetLitFogShader();
}

// ---------------------------------------------------------------------------
// Apply a texture to a model's diffuse map (or flat color fallback)
// ---------------------------------------------------------------------------
static void ApplyTex(Model& model, Texture2D tex, Color fallback) {
    if (tex.id)
        model.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = tex;
    else
        model.materials[0].maps[MATERIAL_MAP_DIFFUSE].color = fallback;
    if (OzoneLoader::GetLitFogShader().id > 0)
        model.materials[0].shader = OzoneLoader::GetLitFogShader();
}

// ---------------------------------------------------------------------------
// Build* Ã¢â‚¬â€ each applies the best texture for its role
// ---------------------------------------------------------------------------
Model OzoneLoader::BuildBox(float w, float h, float d) {
    Mesh mesh = GenMeshCube(w, h, d);
    // Walls (h >= 1.0) get 16x UV tiling so the 32x32 tileset texture
    // doesn't look stretched across large faces
    // Floors (h < 1.0) get 8x tiling so ground tiles repeat sensibly
    float tiling = (h >= 1.0f) ? 16.0f : 8.0f;
    if (mesh.texcoords) {
        for (int i = 0; i < mesh.vertexCount; i++) {
            mesh.texcoords[i*2 + 0] *= tiling;
            mesh.texcoords[i*2 + 1] *= tiling;
        }
        UpdateMeshBuffer(mesh, 1, mesh.texcoords,
                         mesh.vertexCount * 2 * (int)sizeof(float), 0);
    }
    Model model = LoadModelFromMesh(mesh);
    // Auto-select: slot 1 (floor) if thin, slot 2 (wall) if tall
    Texture2D tex = (m_tilesetTex.size() >= 1 && h < 1.0f) ? m_tilesetTex[0] :
                    (m_tilesetTex.size() >= 2) ? m_tilesetTex[1] : Texture2D{0};
    Color fallback = (h < 1.0f) ? LIGHTGRAY : (Color){180,180,200,255};
    ApplyTex(model, tex, fallback);
    return model;
}

// Tapered cylinder (a cone frustum). GenMeshCylinder is uniform, so the taper
// was collapsed to max(rTop, rBot) and every `cyl` was a straight tube.
//
// AXIS CONVENTION: the mesh spans y in [-h, 0], so the authored `cz` is the TOP
// of the cylinder, not its centre or its base. That is the convention the level
// comments were written against ("support column, spans z 0..4" for cz=4,h=4;
// "finial, spans z 1.5..3.5" for cz=3.5,h=2) and the one the endless_snow
// building+roof-slab pair requires (building cz=12 h=12 with its roof at
// cz=12.2 h=0.4 only lines up if the building occupies [0,12]). `box` is
// centre-based, so the two primitives differ - see the Wiki.
//
// Sides get normals from the frustum slope: the surface tangent in the
// (radial, y) plane is (1, dr/dy) with dr/dy = (rTop - rBot)/h, so the outward
// normal is normalize(radial * h, rBot - rTop). A straight cylinder reduces to
// the radial direction, which is what GenMeshCylinder used to give.
// Tapered cylinder (cone frustum). The geometry lives in
// oz::brushmesh::BuildCylinderMesh (Source/World/BrushMesh.hpp) because building
// the raylib Mesh needs a GL context but TRIANGLE WINDING does not - and both caps
// of this cylinder shipped inside-out, which is invisible under backface culling:
// the lid simply was not there and you could see into the tube. A headless test
// can pin winding; it cannot pin it from in here.
//
// AXIS: the mesh spans y in [-h, 0], so the authored `cz` is the TOP. See the
// convention note on the builder for the level-file evidence.
Model OzoneLoader::BuildCylinder(float rTop, float rBot, float h, int slices) {
    BrushMeshData data;
    oz::brushmesh::BuildCylinderMesh(data, rTop, rBot, h, slices);

    Mesh mesh = {0};
    mesh.vertexCount   = data.VertexCount();
    mesh.triangleCount = data.TriangleCount();
    mesh.vertices  = (float*)RL_MALLOC(data.vertices.size()  * sizeof(float));
    mesh.normals   = (float*)RL_MALLOC(data.normals.size()   * sizeof(float));
    mesh.texcoords = (float*)RL_MALLOC(data.texcoords.size() * sizeof(float));
    mesh.indices   = (unsigned short*)RL_MALLOC(data.indices.size() * sizeof(unsigned short));
    if (!mesh.vertices || !mesh.normals || !mesh.texcoords || !mesh.indices) {
        // Free whatever did allocate, then report rather than hand back a Mesh
        // with null arrays - UploadMesh would read them.
        if (mesh.vertices)  RL_FREE(mesh.vertices);
        if (mesh.normals)   RL_FREE(mesh.normals);
        if (mesh.texcoords) RL_FREE(mesh.texcoords);
        if (mesh.indices)   RL_FREE(mesh.indices);
        OZ_WARN("BuildCylinder: out of memory for %d vertices", data.VertexCount());
        return Model{0};
    }
    memcpy(mesh.vertices,  data.vertices.data(),  data.vertices.size()  * sizeof(float));
    memcpy(mesh.normals,   data.normals.data(),   data.normals.size()   * sizeof(float));
    memcpy(mesh.texcoords, data.texcoords.data(), data.texcoords.size() * sizeof(float));
    memcpy(mesh.indices,   data.indices.data(),   data.indices.size()   * sizeof(unsigned short));

    UploadMesh(&mesh, false);
    Model model = LoadModelFromMesh(mesh);
    Texture2D tex = (m_tilesetTex.size() >= 3) ? m_tilesetTex[2] : Texture2D{0};
    ApplyTex(model, tex, SKYBLUE);
    return model;
}
Model OzoneLoader::BuildSphere(float r, int segments) {
    Mesh mesh = GenMeshSphere(r, segments, segments);
    Model model = LoadModelFromMesh(mesh);
    model.materials[0].maps[MATERIAL_MAP_DIFFUSE].color = PURPLE;
    if (GetLitFogShader().id > 0)
        model.materials[0].shader = GetLitFogShader();
    return model;
}

Model OzoneLoader::BuildPyramid(float w, float d, float h) {
    // Geometry (and the winding/normal derivation) lives in
    // oz::brushmesh::BuildPyramidMesh so tests/BrushMesh.test.cpp can check it.
    // This builder previously hard-coded (0, 0.5, 0) normals and wound all six
    // faces inside-out - a see-through pyramid. No shipped world uses `pyr`; the
    // editor's toolbox can still place one.
    BrushMeshData data;
    oz::brushmesh::BuildPyramidMesh(data, w, d, h);

    Mesh mesh = {0};
    mesh.vertexCount   = data.VertexCount();
    mesh.triangleCount = data.TriangleCount();
    mesh.vertices  = (float*)RL_MALLOC(data.vertices.size()  * sizeof(float));
    mesh.normals   = (float*)RL_MALLOC(data.normals.size()   * sizeof(float));
    mesh.texcoords = (float*)RL_MALLOC(data.texcoords.size() * sizeof(float));
    mesh.indices   = (unsigned short*)RL_MALLOC(data.indices.size() * sizeof(unsigned short));
    if (!mesh.vertices || !mesh.normals || !mesh.texcoords || !mesh.indices) {
        if (mesh.vertices)  RL_FREE(mesh.vertices);
        if (mesh.normals)   RL_FREE(mesh.normals);
        if (mesh.texcoords) RL_FREE(mesh.texcoords);
        if (mesh.indices)   RL_FREE(mesh.indices);
        OZ_WARN("BuildPyramid: out of memory");
        return Model{0};
    }
    memcpy(mesh.vertices,  data.vertices.data(),  data.vertices.size()  * sizeof(float));
    memcpy(mesh.normals,   data.normals.data(),   data.normals.size()   * sizeof(float));
    memcpy(mesh.texcoords, data.texcoords.data(), data.texcoords.size() * sizeof(float));
    memcpy(mesh.indices,   data.indices.data(),   data.indices.size()   * sizeof(unsigned short));

    UploadMesh(&mesh, false);
    Model model = LoadModelFromMesh(mesh);
    model.materials[0].maps[MATERIAL_MAP_DIFFUSE].color = GOLD;
    if (GetLitFogShader().id > 0)
        model.materials[0].shader = GetLitFogShader();
    return model;
}
Model OzoneLoader::BuildPlane(float nx, float ny, float nz, float dist) {
    // Oriented 10x10 quad, `n . x = dist`. `nx/ny/nz` is an ENGINE-space normal
    // (the caller converts from OZONE's Z-up) and the geometry - including the
    // winding that makes the front face look along +n - is in
    // oz::brushmesh::BuildPlaneMesh so a test can check it. Written as a fixed
    // horizontal GenMeshPlane before, which ignored all four arguments: a vertical
    // or sloped `pln` was impossible and its normal was inert.
    BrushMeshData data;
    oz::brushmesh::BuildPlaneMesh(data, nx, ny, nz, dist, 5.0f);

    Mesh mesh = {0};
    mesh.vertexCount   = data.VertexCount();
    mesh.triangleCount = data.TriangleCount();
    mesh.vertices  = (float*)RL_MALLOC(data.vertices.size()  * sizeof(float));
    mesh.normals   = (float*)RL_MALLOC(data.normals.size()   * sizeof(float));
    mesh.texcoords = (float*)RL_MALLOC(data.texcoords.size() * sizeof(float));
    mesh.indices   = (unsigned short*)RL_MALLOC(data.indices.size() * sizeof(unsigned short));
    if (!mesh.vertices || !mesh.normals || !mesh.texcoords || !mesh.indices) {
        if (mesh.vertices)  RL_FREE(mesh.vertices);
        if (mesh.normals)   RL_FREE(mesh.normals);
        if (mesh.texcoords) RL_FREE(mesh.texcoords);
        if (mesh.indices)   RL_FREE(mesh.indices);
        OZ_WARN("BuildPlane: out of memory");
        return Model{0};
    }
    memcpy(mesh.vertices,  data.vertices.data(),  data.vertices.size()  * sizeof(float));
    memcpy(mesh.normals,   data.normals.data(),   data.normals.size()   * sizeof(float));
    memcpy(mesh.texcoords, data.texcoords.data(), data.texcoords.size() * sizeof(float));
    memcpy(mesh.indices,   data.indices.data(),   data.indices.size()   * sizeof(unsigned short));

    UploadMesh(&mesh, false);
    Model model = LoadModelFromMesh(mesh);
    Texture2D tex = (m_tilesetTex.size() >= 1) ? m_tilesetTex[0] : Texture2D{0};
    ApplyTex(model, tex, DARKGRAY);
    return model;
}
// Carry a PLANE's normal and distance onto the renderable, in ENGINE space.
// Both load paths call this so the two cannot disagree about the conversion, and
// so the editor can re-emit them (a plane's orientation is not recoverable from
// its mesh bounding box).
static void StorePlaneOrientation(OzoneRenderable& r, const OzonePrimitive& prim) {
    if (prim.type != OzonePrimitiveType::PLANE) return;
    if (prim.args.size() > 5) {
        // OZONE is Z-up -> engine Y-up, the same swap the position gets.
        r.planeNX = prim.args[3];
        r.planeNY = prim.args[5];
        r.planeNZ = prim.args[4];
    }
    if (prim.args.size() > 6) r.planeDist = prim.args[6];
}

// Carry a CYLINDER's authored taper and slice count. The mesh bbox knows only
// max(rTop, rBot), so without these a cone would save back as a straight tube.
static void StoreCylinderShape(OzoneRenderable& r, const OzonePrimitive& prim) {
    if (prim.type != OzonePrimitiveType::CYLINDER) return;
    if (prim.args.size() > 4) {
        r.cylRTop = prim.args[3];
        r.cylRBot = prim.args[4];
    }
    if (prim.args.size() > 6) r.cylSlices = (int)prim.args[6];
}

// ---------------------------------------------------------------------------
// Resolve a texture/heightmap path from an OZONE file against the world dir.
// Accepts, in order: an existing path, a repo-relative GameData/ path, an
// absolute path, and finally a world-relative path (oztex/...).
// ---------------------------------------------------------------------------
static std::string ResolveWorldAssetPath(const std::string& raw,
                                         const std::string& gameDataWorldDir,
                                         const std::string& worldDir) {
    std::string s = StripQuotes(raw);
    if (s.empty()) return s;
    if (IsPathFile(s.c_str())) return s;
    if (s.rfind("GameData/", 0) == 0 || s.rfind("GameData\\", 0) == 0) return s;
    bool absolute = (s.size() > 1 && s[1] == ':') || s[0] == '/' || s[0] == '\\';
    if (absolute) return s;
    if (!gameDataWorldDir.empty()) return gameDataWorldDir + s;
    if (!worldDir.empty()) return worldDir + s;
    return s;
}

// ---------------------------------------------------------------------------
// BuildFromPrimitive
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// BuildSkybox — six INWARD-facing quads with projected UVs.
//
// OZONE:  skybox <texPath> cx cy cz size [topTex=]
//
// Each face's UVs are derived from its vertex direction relative to the cube
// centre, so the six authored textures behave like a cube-map seen from inside
// rather than six independently stretched pictures. Winding is reversed versus
// BuildBox so the faces are visible from the interior, and the surface carries
// SURF_UNLIT + SURF_TWO_SIDED so it is drawn at full value and survives the
// camera clipping into a corner.
//
// Render-only: a skybox contributes NOTHING to the CSG collision world, so
// subtracting the shell around it never removes the player's floor.
// ---------------------------------------------------------------------------
Model OzoneLoader::BuildSkybox(float cx, float cy, float cz, float size,
                               const char* topPath) {
    const float h = size * 0.5f;
    // 8 corners, then 6 quads of 4 verts each, wound so the normal points IN.
    //   +X, -X, +Y, -Y, +Z, -Z   (matches the SurfaceFace order)
    static const float kCorners[8][3] = {
        { -1, -1, -1 }, {  1, -1, -1 }, { -1,  1, -1 }, {  1,  1, -1 },
        { -1, -1,  1 }, {  1, -1,  1 }, { -1,  1,  1 }, {  1,  1,  1 },
    };
    // Corner indices per face, wound clockwise seen from OUTSIDE so the visible
    // side (from inside) is front-facing after the reversal.
    static const int kFaceCorners[6][4] = {
        { 1, 5, 7, 3 },   // +X
        { 4, 0, 2, 6 },   // -X
        { 2, 6, 7, 3 },   // +Y (ceiling in engine space)
        { 4, 0, 1, 5 },   // -Y (floor)
        { 5, 1, 0, 4 },   // +Z
        { 6, 2, 3, 7 },   // -Z
    };

    Mesh mesh = {0};
    mesh.vertexCount = 6 * 4;
    mesh.triangleCount = 6 * 2;
    float* verts  = (float*)RL_MALLOC((size_t)mesh.vertexCount * 3 * sizeof(float));
    float* norms  = (float*)RL_MALLOC((size_t)mesh.vertexCount * 3 * sizeof(float));
    float* uvs    = (float*)RL_MALLOC((size_t)mesh.vertexCount * 2 * sizeof(float));
    unsigned short* idx = (unsigned short*)RL_MALLOC((size_t)mesh.triangleCount * 3 * sizeof(unsigned short));
    if (!verts || !norms || !uvs || !idx) {
        if (verts)  RL_FREE(verts);
        if (norms)  RL_FREE(norms);
        if (uvs)    RL_FREE(uvs);
        if (idx)    RL_FREE(idx);
        return Model{0};
    }
    mesh.vertices = verts;
    mesh.normals = norms;
    mesh.texcoords = uvs;
    mesh.indices = idx;

    for (int f = 0; f < 6; f++) {
        for (int v = 0; v < 4; v++) {
            const int ci = kFaceCorners[f][v];
            const int o = (f * 4 + v);
            const float lx = kCorners[ci][0] * h;
            const float ly = kCorners[ci][1] * h;
            const float lz = kCorners[ci][2] * h;
            verts[o * 3 + 0] = lx;
            verts[o * 3 + 1] = ly;
            verts[o * 3 + 2] = lz;
            // Inward normal: from the face axis towards the centre.
            static const float kAxis[6][3] = {
                { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 },
            };
            norms[o * 3 + 0] = kAxis[f][0];
            norms[o * 3 + 1] = kAxis[f][1];
            norms[o * 3 + 2] = kAxis[f][2];
            // Projected UV: the corner direction, normalised on the face's own
            // plane, mapped to 0..1. Simple planar projection per face - stable,
            // seam-free inside one face, and the six faces line up because the
            // mapping is a function of direction alone.
            const float du = (f == 0 || f == 1) ? lz : lx;
            const float dv = (f == 2 || f == 3) ? lz : ly;
            uvs[o * 2 + 0] = du / (2.0f * h) + 0.5f;
            uvs[o * 2 + 1] = dv / (2.0f * h) + 0.5f;
        }
        const int base = f * 4;
        idx[(f * 2 + 0) * 3 + 0] = (unsigned short)(base + 0);
        idx[(f * 2 + 0) * 3 + 1] = (unsigned short)(base + 1);
        idx[(f * 2 + 0) * 3 + 2] = (unsigned short)(base + 2);
        idx[(f * 2 + 1) * 3 + 0] = (unsigned short)(base + 0);
        idx[(f * 2 + 1) * 3 + 1] = (unsigned short)(base + 2);
        idx[(f * 2 + 1) * 3 + 2] = (unsigned short)(base + 3);
    }

    Model model = LoadModelFromMesh(mesh);   // takes ownership of the buffers
    // Texture is applied by the caller: only it knows the world directory, and a
    // skybox's path is authored relative to the world.
    return model;
}

Model OzoneLoader::BuildFromPrimitive(int type, const std::vector<float>& args) {
    switch ((OzonePrimitiveType)type) {
        case OzonePrimitiveType::BOX: {
            float w = (args.size() > 3) ? args[3] : 2.0f;
            float h = (args.size() > 4) ? args[4] : 2.0f;
            float d = (args.size() > 5) ? args[5] : 2.0f;
            return BuildBox(w, h, d);
        }
        case OzonePrimitiveType::CYLINDER: {
            float rTop = (args.size() > 3) ? args[3] : 1.0f;
            float rBot = (args.size() > 4) ? args[4] : 1.0f;
            float h    = (args.size() > 5) ? args[5] : 2.0f;
            int slices = (args.size() > 6) ? (int)args[6] : 16;
            return BuildCylinder(rTop, rBot, h, slices);
        }
        case OzonePrimitiveType::SPHERE: {
            float r   = (args.size() > 3) ? args[3] : 1.0f;
            int segs  = (args.size() > 4) ? (int)args[4] : 16;
            return BuildSphere(r, segs);
        }
        case OzonePrimitiveType::PYRAMID: {
            float w = (args.size() > 3) ? args[3] : 2.0f;
            float d = (args.size() > 4) ? args[4] : 2.0f;
            float h = (args.size() > 5) ? args[5] : 2.0f;
            return BuildPyramid(w, d, h);
        }
        case OzonePrimitiveType::PLANE: {
            // Author a normal in OZONE's Z-up space; build in ENGINE Y-up, the
            // same swap the position gets (ozone x,y,z -> engine x,z,y). Without
            // it a `pln 0 0 1` (up in the file) would build a wall.
            float nx   = (args.size() > 3) ? args[3] : 0.0f;
            float ny   = (args.size() > 4) ? args[4] : 1.0f;
            float nz   = (args.size() > 5) ? args[5] : 0.0f;
            float dist = (args.size() > 6) ? args[6] : 0.0f;
            return BuildPlane(nx, nz, ny, dist);
        }
        default:
            return BuildBox(1, 1, 1);
    }
}

// ---------------------------------------------------------------------------
// LoadFile Ã¢â‚¬â€ also loads world textures from the .ozone file's directory
// ---------------------------------------------------------------------------
bool OzoneLoader::LoadFile(const char* path) {
    OZ_INFO("OzoneLoader: loading %s", path);
    Unload();

    // Extract world directory from the .ozone path and load textures
    std::string p(path);
    std::string worldDir;
    std::string gameDataWorldDir;
    size_t slash = p.find_last_of("/\\");
    if (slash != std::string::npos) {
        worldDir = p.substr(0, slash + 1);
        // Extract world name for GameData path resolution (e.g. "world_EngineTest.ozone" â†’ "EngineTest")
        std::string fname = p.substr(slash + 1);
        std::string prefix = "world_";
        std::string suffix = ".ozone";
        if (fname.rfind(prefix, 0) == 0 && fname.size() > prefix.size() + suffix.size()) {
            std::string worldName = fname.substr(prefix.size(), fname.size() - prefix.size() - suffix.size());
            gameDataWorldDir = std::string("GameData/Worlds/") + worldName + "/";
            // Prefer GameData tileset over package directory
            if (fs::exists(gameDataWorldDir + "oztex/tileset/")) {
                LoadWorldTextures(gameDataWorldDir);
            } else {
                LoadWorldTextures(worldDir);
            }
        } else {
            LoadWorldTextures(worldDir);
        }
    }
    // Remember the world directory for .ozls def matching (GameData path when
    // available so world-local skyzone defs win over same-named ones elsewhere)
    m_worldDir = gameDataWorldDir.empty() ? worldDir : gameDataWorldDir;

    auto primitives = OzoneParser::parse_file(path);
    if (primitives.empty()) return false;

    // Entities are parsed into a plain set first; the orchestrator decides when
    // (and whether) to inject them into the runtime systems.
    ParseOzoneEntities(primitives, m_worldDir, m_zoneCounters, m_entities);

    for (auto& prim : primitives) {
        if (IsEntityPrimitive(prim.type)) continue;

        // Skybox: a render-only room. Handled before the generic brush path
        // because it is NOT a solid - adding it to the CSG world would put an
        // invisible wall around the player.
        if (prim.type == OzonePrimitiveType::SKYBOX) {
            OzoneRenderable sb;
            sb.typeId = (int)OzonePrimitiveType::SKYBOX;
            sb.csgOp = 0;
            sb.surface = prim.surface;
            // args: cx cy cz size [scale]
            const float cx = prim.args.size() > 0 ? prim.args[0] : 0.0f;
            const float cy = prim.args.size() > 1 ? prim.args[1] : 0.0f;
            const float cz = prim.args.size() > 2 ? prim.args[2] : 0.0f;
            float size = prim.args.size() > 3 ? prim.args[3] : 512.0f;
            if (size <= 0.0f) size = 512.0f;
            if (prim.args.size() > 4 && prim.args[4] > 0.0f) size *= prim.args[4];
            // OZONE is Z-up; the engine is Y-up.
            sb.position = { cx, cz, cy };
            sb.model = BuildSkybox(0.0f, 0.0f, 0.0f, size, nullptr);
            sb.loaded = (sb.model.meshCount > 0);
            sb.scale = 1.0f;
            sb.surfaceFlags = (int)(sb.surface.def.flags &
                                    (SURF_FAKEBACKDROP | SURF_COLLISION_PROXY | SURF_INVISIBLE));
            if (sb.loaded) {
                // Unlit + two-sided so the interior is visible from inside, and
                // no-fog so a sky never greys out at distance.
                sb.surface.def.Set(SURF_UNLIT, true);
                sb.surface.def.Set(SURF_TWO_SIDED, true);
                sb.surface.def.Set(SURF_NO_FOG, true);
                // Bind the surface shader so the flags actually take effect.
                if (oz::SurfaceMaterial::Instance().Ready())
                    sb.model.materials[0].shader = oz::SurfaceMaterial::Instance().Get();
                const std::string tp = ResolveWorldAssetPath(
                    prim.entityType, gameDataWorldDir, worldDir);
                Texture2D tex = LoadTextureWithFallback(tp.c_str());
                if (tex.id > 0) {
                    sb.customTex = tex;
                    sb.model.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = tex;
                } else {
                    OZ_WARN("skybox: texture '%s' not found - falling back to flat colour",
                            tp.c_str());
                }
            } else {
                OZ_WARN("skybox: geometry build failed for '%s'", prim.entityType.c_str());
            }
            m_renderables.push_back(sb);
            continue;
        }

        // Heightmap is handled specially â€” builds its own model from image path
        if (prim.type == OzonePrimitiveType::HEIGHTMAP) {
        OzoneRenderable r;
        r.typeId = (int)prim.type;
        r.position = {0,0,0};
        r.scale = 1.0f;
        // Resolve relative paths: prefer GameData/Worlds/<name>/, fall back to package directory
        std::string imgPath = StripQuotes(prim.entityType);
        std::string texPath = StripQuotes(prim.entitySubType);
        // Keep the authored relative paths for round-trip export
        m_hmImageRel = imgPath;
        m_hmTexRel = texPath;
        imgPath = ResolveWorldAssetPath(imgPath, gameDataWorldDir, worldDir);
        if (!texPath.empty()) texPath = ResolveWorldAssetPath(texPath, gameDataWorldDir, worldDir);
        r.model = BuildHeightmap(imgPath, texPath, prim.args);
        r.loaded = m_hmReady;
        r.csgOp = 0;
        m_renderables.push_back(r);
        continue;
    }

    OzoneRenderable r;
    r.typeId = (int)prim.type;
r.csgOp = prim.csgOp;
    r.surface = prim.surface;   // per-face surface props (flags/texture/UV/pan/alpha/glow)
    // Derive, don't copy - see the twin load path above.
    DeriveLegacyRenderable(r);
    if (prim.args.size() >= 3)
        r.position = {prim.args[0], prim.args[2], prim.args[1]};

    r.scale = 1.0f;
    if (prim.type == OzonePrimitiveType::BOX && prim.args.size() >= 7)
        r.rotation = prim.args[6] * DEG2RAD;
    else if (prim.type == OzonePrimitiveType::CYLINDER && prim.args.size() >= 8)
        r.rotation = prim.args[7] * DEG2RAD;

    r.model = BuildFromPrimitive((int)prim.type, prim.args);
    r.loaded = (r.model.meshCount > 0);
    // NOTE: deliberately NOT seeding texScaleU/V from BuildBox's baked tiling
    // any more. texScaleU/V are extra multipliers ON TOP of the generated mesh
    // UVs (which already carry the tiling); seeding them with that same value
    // made every export write `texScaleU=16` for a 16x-tiled box, which on the
    // next load applied 16 to already-16x UVs -> 256x.
    // Parse optional texSlot after rotation: box has 7+1=8 args, cyl has 8+1=9
    if (prim.type == OzonePrimitiveType::BOX && prim.args.size() >= 8)
        r.texSlot = (int)prim.args[7];
    else if (prim.type == OzonePrimitiveType::CYLINDER && prim.args.size() >= 9)
        r.texSlot = (int)prim.args[8];
    else if (prim.type == OzonePrimitiveType::PYRAMID && prim.args.size() >= 7)
        r.texSlot = (int)prim.args[6];
    StorePlaneOrientation(r, prim);
    StoreCylinderShape(r, prim);
    // Apply texture override if set
    if (r.texSlot > 0) ApplyTexSlotToModel(r.model, r.texSlot);
    m_renderables.push_back(r);
    // Apply optional texture UV scaling/offset
    if (prim.texScaleU != 1.0f || prim.texScaleV != 1.0f ||
        prim.texOffsetU != 0.0f || prim.texOffsetV != 0.0f) {
        ApplyRenderableUV((int)m_renderables.size() - 1,
                          prim.texScaleU, prim.texScaleV,
                          prim.texOffsetU, prim.texOffsetV);
    }
    // Custom per-brush diffuse texture (texPath= attribute), resolved against
    // the world directory unless already rooted at GameData/ or absolute
    if (!prim.texPath.empty()) {
        std::string tp = ResolveWorldAssetPath(prim.texPath, gameDataWorldDir, worldDir);
        ApplyRenderableTexture((int)m_renderables.size() - 1, tp.c_str());
    }
    }

    // Lights are bound to zone volumes by InjectOzoneEntities(), not here.
    RebuildCollisionVolumes();
    int r0shader = (!m_renderables.empty() && m_renderables[0].model.materialCount > 0)
        ? m_renderables[0].model.materials[0].shader.id : -1;
    OZ_INFO("OZONE: entities=%zu litShader=%d renderable0.shader=%d",
            m_entities.Count(), GetLitFogShader().id, r0shader);
    OZ_INFO("OzoneLoader: loaded %zu primitives, %zu collision volumes from %s",
            primitives.size(), m_collisionVolumes.size(), path);
    return true;
}

// ---------------------------------------------------------------------------
// LoadString
// ---------------------------------------------------------------------------
bool OzoneLoader::LoadString(const char* data, const char* worldDir) {
    Unload();

    // Optional world directory: load the tileset before parsing so texSlot
    // indices resolve to real textures (editor snapshot restore path).
    if (worldDir && worldDir[0]) {
        std::string wd(worldDir);
        if (!wd.empty() && wd.back() != '/' && wd.back() != '\\')
            wd += '/';
        LoadWorldTextures(wd);
        m_worldDir = wd;
    }

    auto primitives = OzoneParser::parse_string(data);
    if (primitives.empty()) return false;

    ParseOzoneEntities(primitives, m_worldDir, m_zoneCounters, m_entities);

    for (auto& prim : primitives) {
        if (IsEntityPrimitive(prim.type)) continue;

        // Heightmap is handled specially
        if (prim.type == OzonePrimitiveType::HEIGHTMAP) {
        OzoneRenderable r;
        r.typeId = (int)prim.type;
        r.position = {0,0,0};
        r.scale = 1.0f;
        r.model = BuildHeightmap(StripQuotes(prim.entityType),
                                 StripQuotes(prim.entitySubType), prim.args);
        r.loaded = m_hmReady;
        r.csgOp = 0;
        m_renderables.push_back(r);
        continue;
    }

    OzoneRenderable r;
    r.typeId = (int)prim.type;
    r.csgOp = prim.csgOp;
    r.surface = prim.surface;   // per-face surface props (flags/texture/UV/pan/alpha/glow)
    // Derive the legacy views rather than copying the parser's copy of them. The
    // parser already ran DeriveLegacySurfaceFields, so the values agree today -
    // but one code path is one thing that cannot forget a field, and a
    // hand-assigned mirror is how surfaceFlags and surface.def.flags came to
    // disagree in the first place.
    DeriveLegacyRenderable(r);
    if (prim.args.size() >= 3)
        r.position = {prim.args[0], prim.args[2], prim.args[1]};

    r.scale = 1.0f;
    r.model = BuildFromPrimitive((int)prim.type, prim.args);
    r.loaded = (r.model.meshCount > 0);
    // World-space AABB for frustum culling
    ComputeCollisionAABB((int)prim.type, prim.args, r.position, r.bounds);
    r.hasBounds = true;
    // texScaleU/V stay at 1.0 here on purpose: they are extra multipliers on top
    // of the generated mesh UVs, not an absolute. Seeding them with BuildBox's
    // baked tiling (as this used to) meant an export wrote `texScaleU=16` for an
    // already-16x-tiled box and the next load applied 16 on top of 16 -> 256x.
    if (prim.type == OzonePrimitiveType::BOX && prim.args.size() >= 8)
        r.texSlot = (int)prim.args[7];
    else if (prim.type == OzonePrimitiveType::CYLINDER && prim.args.size() >= 9)
        r.texSlot = (int)prim.args[8];
    else if (prim.type == OzonePrimitiveType::PYRAMID && prim.args.size() >= 7)
        r.texSlot = (int)prim.args[6];
    StorePlaneOrientation(r, prim);
    StoreCylinderShape(r, prim);
    if (r.texSlot > 0) ApplyTexSlotToModel(r.model, r.texSlot);
    m_renderables.push_back(r);
    if (prim.texScaleU != 1.0f || prim.texScaleV != 1.0f ||
        prim.texOffsetU != 0.0f || prim.texOffsetV != 0.0f) {
        ApplyRenderableUV((int)m_renderables.size() - 1,
                          prim.texScaleU, prim.texScaleV,
                          prim.texOffsetU, prim.texOffsetV);
    }
    if (!prim.texPath.empty()) {
        std::string tp = StripQuotes(prim.texPath);
        if (!tp.empty() && tp.rfind("GameData/", 0) != 0 && !m_worldDir.empty())
            tp = m_worldDir + tp;
        ApplyRenderableTexture((int)m_renderables.size() - 1, tp.c_str());
    }
    }
    // Lights are bound to zone volumes by InjectOzoneEntities(), not here.
    RebuildCollisionVolumes();
    return true;
}


// ---------------------------------------------------------------------------
// DrawGlowGeometry — additive re-draw of every SURF_GLOW face.
//
// Runs after the opaque world pass, with depth TESTING still on but depth WRITE
// off and the blend additive. Without the depth test a glowing floor panel
// would shine through the wall in front of it; without the depth write it would
// also erase the very lighting it is meant to add.
//
// The glow term itself is already in Surface.fs (added after lighting, so a
// glowing sign stays bright in an unlit room). This pass is the HALO on top:
// the same face drawn again, additively, so the emissive area spills over its
// own silhouette instead of being clipped to the texture's alpha.
// ---------------------------------------------------------------------------
void OzoneLoader::DrawGlowGeometry(Camera3D& camera) {
    using namespace oz::surface;
    auto& sm = oz::SurfaceMaterial::Instance();
    if (!sm.Ready()) return;

    // Cheap pre-scan so a level with no glowing brush pays two vector walks and
    // no GL state changes. Resolved per FACE, not per brush, so a brush whose
    // def carries the flag but whose only glowing face is invisible does not open
    // a blend state for nothing.
    bool any = false;
    for (auto& r : m_renderables) {
        if (!r.loaded || (r.surfaceFlags & SURF_COLLISION_PROXY)) continue;
        if (r.typeId == (int)OzonePrimitiveType::HEIGHTMAP) continue;
        for (int f = 0; f < FACE_COUNT; f++) {
            const SurfaceProps& p = r.surface.Resolve((SurfaceFace)f);
            if (p.Has(SURF_GLOW) && !p.Has(SURF_INVISIBLE)) { any = true; break; }
        }
        if (any) break;
    }
    if (!any || !sm.Ready()) return;

    BeginBlendMode(BLEND_ADDITIVE);
    rlDisableDepthMask();
    // Emit only the emissive term for this pass. Adding the whole lit face a
    // second time is what made every textured glow surface read as a white
    // card: a 0.6 base became 1.2 and clipped, and surfGlowScale could not help
    // because it only ever scaled the emissive addend.
    sm.SetHaloPass(1);
    for (auto& r : m_renderables) {
        if (!r.loaded || (r.surfaceFlags & SURF_COLLISION_PROXY)) continue;
        if (r.typeId == (int)OzonePrimitiveType::HEIGHTMAP) continue;
        bool glows = false;
        for (int f = 0; f < FACE_COUNT; f++) {
            const SurfaceProps& p = r.surface.Resolve((SurfaceFace)f);
            if (p.Has(SURF_GLOW) && !p.Has(SURF_INVISIBLE)) { glows = true; break; }
        }
        if (!glows) continue;
        // GlowingOnly, NOT the whole brush. This used to re-draw all six faces of
        // any brush with one glowing face, so a wall with a single glowing sign
        // face picked up an additive ghost of its other five faces.
        DrawSurface(r, SurfacePass::GlowingOnly);
    }
    rlEnableDepthMask();
    EndBlendMode();
    sm.SetHaloPass(0);
}

// ---------------------------------------------------------------------------
// DrawSurface — the surface-flagged draw path.
//
// A brush with any non-default surface property is drawn one face at a time so
// each face can carry its own uniforms. A brush with nothing set keeps the
// original single DrawModel call, so the overwhelming majority of geometry is
// untouched (same shader, same batching, same cost).
//
// Invisible faces are skipped. SURF_FAKEBACKDROP is included in the shader's
// full-bright path, which is what finally lets a backdrop be drawn at its
// painted value WITHOUT mutating the shared LitFog uniforms the way the old
// DrawZoneGeometry did.
// ---------------------------------------------------------------------------
// A face is drawn in exactly ONE pass (see SurfacePass in OzOzoneLoader.hpp).
//
// The exclusion rules live here, in one loop, rather than as a brush-wide skip
// in each of the three pass entry points. That is the whole point: the old
// arrangement filtered on OzoneRenderable::surfaceFlags, which is a derived mirror
// of surface.def only, so a PER-FACE backdrop was invisible to every filter - it
// got drawn by the world pass AND by the sky pass. A per-face Invisible face is
// likewise only honoured by this loop.
static bool FaceBelongsToPass(oz::surface::SurfaceFace f,
                              const oz::surface::SurfaceProps& p,
                              OzoneLoader::SurfacePass pass) {
    using namespace oz::surface;
    if (p.Has(SURF_INVISIBLE)) return false;
    switch (pass) {
        case OzoneLoader::SurfacePass::Full:
            // Backdrops and portals belong to their own passes.
            return !p.Has(SURF_FAKEBACKDROP) && !p.Has(SURF_PORTAL);
        case OzoneLoader::SurfacePass::GlowingOnly:
            // A portal is a hole: an additive halo over it would glow the hole.
            return p.Has(SURF_GLOW) && !p.Has(SURF_PORTAL);
        case OzoneLoader::SurfacePass::BackdropOnly:
            // A face carrying both is a portal; the portal pass owns it.
            return p.Has(SURF_FAKEBACKDROP) && !p.Has(SURF_PORTAL);
        case OzoneLoader::SurfacePass::SkyPortalOnly:
            return p.Has(SURF_PORTAL);
    }
    return false;
}

// The ONE model matrix for a brush. Scale, then rotate about the brush's own
// origin, then translate - raylib's row-vector order, and exactly what
// DrawModelEx builds, so the fast path and the per-face path place a brush
// identically. RebuildCollisionVolumes' RotatedAabbY mirrors this composition;
// if this changes, that must too.
static inline Matrix RenderableModelMatrix(const OzoneRenderable& r) {
    Matrix mdl = MatrixTranslate(r.position.x, r.position.y, r.position.z);
    if (r.rotation != 0.0f)
        mdl = MatrixMultiply(MatrixRotateY(r.rotation), mdl);
    if (r.scale != 1.0f)
        mdl = MatrixMultiply(MatrixScale(r.scale, r.scale, r.scale), mdl);
    return mdl;
}

void OzoneLoader::DrawSurface(OzoneRenderable& r, SurfacePass pass) {
    using namespace oz::surface;
    auto& sm = oz::SurfaceMaterial::Instance();
    if (!sm.Ready() || r.model.meshCount == 0) {
        // Shader missing: fall back to the plain path so the brush still draws -
        // with the SAME transform the surface path would have used. Plain
        // DrawModel has no rotation term, so a rotated brush snapped back to
        // axis-aligned exactly when the shader was unavailable.
        DrawModelEx(r.model, r.position, {0.0f, 1.0f, 0.0f}, r.rotation * RAD2DEG,
                    {r.scale, r.scale, r.scale}, WHITE);
        return;
    }
    if (!r.faceMeshesBuilt) BuildFaceMeshes(r);

    // pos/rot/scale mirrors what DrawModel would have applied, so the geometry
    // lands in exactly the same place as the non-surface path.
    const Matrix mdl = RenderableModelMatrix(r);

    Material mat = r.model.materials[0];

    for (int f = 0; f < FACE_COUNT; f++) {
        const SurfaceProps& p = r.surface.Resolve((SurfaceFace)f);
        if (!FaceBelongsToPass((SurfaceFace)f, p, pass)) continue;
        if (r.faceMesh[f].vaoId == 0) continue;   // no geometry on this axis

        // Per-face texture. A face's texSlot/texPath were stored, exported and
        // round-tripped for as long as they existed and NEVER bound, so the
        // Surface Properties dialog's texture fields did nothing until a reload.
        //
        // `mat` copies the model's Material, but raylib's Material holds a
        // POINTER to its maps array, so writing maps[DIFFUSE] reaches the model's
        // own material. Save and restore around the draw: without the restore the
        // last face's texture would become the next frame's baseline.
        const Texture2D faceTex = ResolveFaceTextureHandle(p);
        MaterialMap* diff = &mat.maps[MATERIAL_MAP_DIFFUSE];
        const MaterialMap saved = *diff;
        if (faceTex.id > 0) diff->texture = faceTex;

        // Baked material graph for this face. Its diffuse REPLACES the face
        // texture (that is what the graph bakes); normal/emissive ride the
        // uniforms uploaded by ApplyUniforms. Empty material name = none.
        const ozmat::MaterialAsset* asset =
            p.material.empty() ? nullptr : ozmat::MaterialRegistry::Instance().Find(p.material);
        if (asset && asset->diffuse.id > 0) diff->texture = asset->diffuse;
        sm.SetMaterialMaps(asset ? asset->diffuse : Texture2D{0},
                           asset ? asset->normal : Texture2D{0},
                           asset ? asset->emissive : Texture2D{0},
                           asset ? asset->normalStrength : 1.0f,
                           asset ? asset->emissiveScale : 1.0f);

        mat.shader = sm.Get();
        sm.ApplyUniforms(p);
        sm.BeginSurfaceState(p);
        DrawMesh(r.faceMesh[f], mat, mdl);
        sm.EndSurfaceState();

        *diff = saved;
    }
}

// ---------------------------------------------------------------------------
// Sky portals (SURF_PORTAL)
//
// A portal face is drawn where it is and samples the pre-rendered sky view by
// SCREEN position (GameData/Shaders/SkyPortal.fs). The face is its own mask and
// the depth test against world geometry supplies occlusion, so there is no
// stencil buffer, no portal recursion and no second world render - which is what
// makes this cheap enough to run in the same frame as everything else.
//
// The view itself is rendered by oz::SkyPortal (Renderer/SkyPortal.hpp) BEFORE
// BeginTextureMode(Target): raylib cannot nest render textures.
// ---------------------------------------------------------------------------
bool OzoneLoader::EnsureSkyPortalShader() {
    if (m_skyPortalShader.id > 0) return true;
    if (m_skyPortalShaderTried) return false;   // one attempt; a missing shader must not re-log per frame
    m_skyPortalShaderTried = true;

    m_skyPortalShader = LoadShaderWithFallback("GameData/Shaders/SkyPortal.vs",
                                               "GameData/Shaders/SkyPortal.fs");
    if (m_skyPortalShader.id == 0) {
        OZ_WARN("SkyPortal: shader failed to load - SURF_PORTAL faces will not draw");
        return false;
    }
    m_skyPortalViewportLoc = GetShaderLocation(m_skyPortalShader, "uViewport");
    return true;
}

bool OzoneLoader::HasSkyPortalFaces() const {
    using namespace oz::surface;
    for (const auto& r : m_renderables) {
        if (!r.loaded) continue;
        for (int f = 0; f < FACE_COUNT; f++)
            if (r.surface.Resolve((SurfaceFace)f).Has(SURF_PORTAL)) return true;
    }
    return false;
}

void OzoneLoader::DrawSkyPortals(Camera3D& camera, Texture2D skyView, int vpW, int vpH) {
    using namespace oz::surface;   // FACE_COUNT / SurfaceFace, as in HasSkyPortalFaces
    (void)camera;   // geometry is positioned by RenderableModelMatrix; the VP comes from rlgl
    if (skyView.id == 0 || vpW <= 0 || vpH <= 0) return;
    if (!EnsureSkyPortalShader()) return;

    if (m_skyPortalViewportLoc >= 0) {
        const float vp[2] = {(float)vpW, (float)vpH};
        SetShaderValue(m_skyPortalShader, m_skyPortalViewportLoc, vp, SHADER_UNIFORM_VEC2);
    }

    for (auto& r : m_renderables) {
        if (!r.loaded) continue;
        if (r.surfaceFlags & SURF_COLLISION_PROXY) continue;
        if (r.typeId == (int)OzonePrimitiveType::HEIGHTMAP) continue;

        bool any = false;
        for (int f = 0; f < FACE_COUNT && !any; f++) {
            if (FaceBelongsToPass((SurfaceFace)f, r.surface.Resolve((SurfaceFace)f),
                                  SurfacePass::SkyPortalOnly)) any = true;
        }
        if (!any) continue;

        if (!r.faceMeshesBuilt) BuildFaceMeshes(r);

        const Matrix mdl = RenderableModelMatrix(r);
        Material mat = r.model.materials[0];
        // Material holds a POINTER to its maps array, shared with the model, so
        // the sky view must be saved/restored around the draw - the same rule
        // DrawSurface follows for per-face textures.
        MaterialMap* diff = &mat.maps[MATERIAL_MAP_DIFFUSE];
        const MaterialMap saved = *diff;
        diff->texture = skyView;
        mat.shader = m_skyPortalShader;

        for (int f = 0; f < FACE_COUNT; f++) {
            if (!FaceBelongsToPass((SurfaceFace)f, r.surface.Resolve((SurfaceFace)f),
                                   SurfacePass::SkyPortalOnly)) continue;
            if (r.faceMesh[f].vaoId == 0) continue;
            // Opaque and depth-tested: the portal writes depth so later passes
            // (glow, pawns, view-model) sort against it correctly.
            DrawMesh(r.faceMesh[f], mat, mdl);
        }

        *diff = saved;
    }
}

// Route a renderable through the right path. Callers use this instead of
// DrawModel so the fast path and the surface path cannot drift apart.
static inline void DrawRenderable(OzoneRenderable& r) {
    if (r.typeId == (int)OzonePrimitiveType::HEIGHTMAP) return;  // handled by caller
    if (r.surface.NeedsPerFaceDraw()) {
        OzoneLoader::Instance().DrawSurface(r);
        return;
    }
    // DrawModelEx composes scale * rotate * translate, the same matrix
    // RenderableModelMatrix builds for the surface path. This used to be
    // DrawModel(model, position, scale, WHITE), which has NO rotation term, so a
    // rotated brush with no surface decoration rendered axis-aligned while the
    // editor picked and drew its faces rotated.
    DrawModelEx(r.model, r.position, {0.0f, 1.0f, 0.0f}, r.rotation * RAD2DEG,
                {r.scale, r.scale, r.scale}, WHITE);
}

// ---------------------------------------------------------------------------
// Draw — all renderables (backward compat, used by editor)
// ---------------------------------------------------------------------------
void OzoneLoader::Draw(Camera3D& camera, bool cullBackfaces) {
    // Establish the frame's culling intent explicitly rather than inheriting it
    // from whatever ran before. See the declaration in OzOzoneLoader.hpp for why
    // the editor passes this in instead of the skybox block setting it.
    oz::SetBackfaceCulling(cullBackfaces);

    FrustumPlane planes[6];
    BuildFrustum(camera, planes);
    for (auto& r : m_renderables) {
        if (!r.loaded) continue;
        // Generated collision proxies are invisible unless the editor has
        // explicitly asked for them - otherwise a single AutoConvex pass would
        // bury the level in a wall of grey boxes.
        if ((r.surfaceFlags & SURF_COLLISION_PROXY) && !m_drawCollisionProxies) continue;
        if (r.hasBounds && (r.bounds.min.x < r.bounds.max.x ||
                            r.bounds.min.y < r.bounds.max.y ||
                            r.bounds.min.z < r.bounds.max.z) &&
            !AabbInFrustum(planes, r.bounds)) continue;
        if (r.typeId == (int)OzonePrimitiveType::HEIGHTMAP && m_hmReady) {
            DrawModelEx(m_hmModel, m_hmPosition, (Vector3){0,1,0}, 0,
                        (Vector3){m_hmScale, m_hmScale, m_hmScale}, WHITE);
        } else {
            DrawRenderable(r);
        }
    }
}

// ---------------------------------------------------------------------------
// DrawWorldGeometry - the main world pass. Facets that belong to another pass
// (backdrops) are excluded per-FACE inside DrawSurface, not per-brush here: a
// brush-wide skip on the derived mirror was invisible to a per-face backdrop, so
// such a face was drawn by BOTH this pass and the sky pass.
void OzoneLoader::DrawWorldGeometry(Camera3D& camera) {
    FrustumPlane planes[6];
    BuildFrustum(camera, planes);
    // Zone-based PVS: collect the camera's active zones (+ adjacent via portals
    // is approximated by all zones overlapping the camera point). Renderables
    // whose centre lies in none of those zones are skipped.
    BoundingBox camBox = { {camera.position.x-0.1f, camera.position.y-0.1f, camera.position.z-0.1f},
                           {camera.position.x+0.1f, camera.position.y+0.1f, camera.position.z+0.1f} };
    auto activeZones = ZoneManager::Instance().GetActiveZones(camera.position, camBox);
    std::vector<ZoneVolumeNode*> activeZonePtrs = activeZones;

    // Portal-plane occluders, collected ONCE (not per renderable): each is a
    // plane {normal, centre} in engine space. A `pln` carries its authored
    // normal/position; any other occluder falls back to its thinnest AABB axis.
    struct Occluder { Vector3 n; Vector3 c; };
    std::vector<Occluder> occluders;
    for (auto& o : m_renderables) {
        if (!o.loaded) continue;
        if (!(o.surface.def.flags & SURF_OCCLUDER)) continue;
        Occluder oc;
        oc.c = { (o.bounds.min.x+o.bounds.max.x)*0.5f,
                 (o.bounds.min.y+o.bounds.max.y)*0.5f,
                 (o.bounds.min.z+o.bounds.max.z)*0.5f };
        if (o.typeId == (int)OzonePrimitiveType::PLANE) {
            oc.n = {o.planeNX, o.planeNY, o.planeNZ};
        } else {
            float ex = o.bounds.max.x - o.bounds.min.x;
            float ey = o.bounds.max.y - o.bounds.min.y;
            float ez = o.bounds.max.z - o.bounds.min.z;
            oc.n = {0,1,0};
            if (ex <= ey && ex <= ez) oc.n = {1,0,0};
            else if (ez <= ex && ez <= ey) oc.n = {0,0,1};
        }
        occluders.push_back(oc);
    }

    for (auto& r : m_renderables) {
        if (!r.loaded) continue;
        // Collision proxies are never drawn in-game, whatever the editor toggle
        // says: they exist to stop the player falling through a prop.
        if (r.surfaceFlags & SURF_COLLISION_PROXY) continue;
        if (r.hasBounds && (r.bounds.min.x < r.bounds.max.x ||
                            r.bounds.min.y < r.bounds.max.y ||
                            r.bounds.min.z < r.bounds.max.z) &&
            !AabbInFrustum(planes, r.bounds)) continue;
        // PVS filter: if zones exist and the camera is in a subset of them,
        // skip renderables that lie inside a zone that is NOT active. Renderables
        // outside every zone are always drawn.
        if (!ZoneManager::Instance().GetZones().empty() && !activeZonePtrs.empty() &&
            activeZonePtrs.size() < ZoneManager::Instance().GetZones().size()) {
            Vector3 c = { (r.bounds.min.x+r.bounds.max.x)*0.5f,
                          (r.bounds.min.y+r.bounds.max.y)*0.5f,
                          (r.bounds.min.z+r.bounds.max.z)*0.5f };
            bool inAnyZone = false;
            bool inActive = false;
            for (auto& z : ZoneManager::Instance().GetZones()) {
                if (c.x >= z.bounds.min.x && c.x <= z.bounds.max.x &&
                    c.y >= z.bounds.min.y && c.y <= z.bounds.max.y &&
                    c.z >= z.bounds.min.z && c.z <= z.bounds.max.z) {
                    inAnyZone = true;
                    for (auto* a : activeZonePtrs) if (a->id == z.id) { inActive = true; break; }
                }
            }
            if (inAnyZone && !inActive) continue;
        }
        // Occlusion: a renderable on the far side of any occluder plane is skipped.
        if (!occluders.empty()) {
            Vector3 rc = { (r.bounds.min.x+r.bounds.max.x)*0.5f,
                           (r.bounds.min.y+r.bounds.max.y)*0.5f,
                           (r.bounds.min.z+r.bounds.max.z)*0.5f };
            bool occluded = false;
            for (auto& oc : occluders) {
                float dCam = (camera.position.x-oc.c.x)*oc.n.x +
                             (camera.position.y-oc.c.y)*oc.n.y +
                             (camera.position.z-oc.c.z)*oc.n.z;
                float dR   = (rc.x-oc.c.x)*oc.n.x +
                             (rc.y-oc.c.y)*oc.n.y +
                             (rc.z-oc.c.z)*oc.n.z;
                if (dCam * dR < 0) { occluded = true; break; }
            }
            if (occluded) continue;
        }
        if (r.typeId == (int)OzonePrimitiveType::HEIGHTMAP && m_hmReady) {
            DrawModelEx(m_hmModel, m_hmPosition, (Vector3){0,1,0}, 0,
                        (Vector3){m_hmScale, m_hmScale, m_hmScale}, WHITE);
        } else {
            DrawRenderable(r);
        }
    }
}

// ---------------------------------------------------------------------------
// ComputeCollisionAABB Ã¢â‚¬â€ generate world-space AABB from primitive params
// ---------------------------------------------------------------------------
void OzoneLoader::ComputeCollisionAABB(int type, const std::vector<float>& args,
                                       Vector3 position, BoundingBox& out) {
    out = {{0,0,0},{0,0,0}};
    switch ((OzonePrimitiveType)type) {
        case OzonePrimitiveType::BOX: {
            float w = (args.size() > 3) ? args[3] : 2.0f;
            float h = (args.size() > 4) ? args[4] : 2.0f;
            float d = (args.size() > 5) ? args[5] : 2.0f;
            out.min = {position.x - w/2, position.y - h/2, position.z - d/2};
            out.max = {position.x + w/2, position.y + h/2, position.z + d/2};
            break;
        }
        case OzonePrimitiveType::CYLINDER: {
            float rTop = (args.size() > 3) ? args[3] : 1.0f;
            float rBot = (args.size() > 4) ? args[4] : 1.0f;
            float h    = (args.size() > 5) ? args[5] : 2.0f;
            float maxR = (rTop > rBot) ? rTop : rBot;
            // `cz` is the TOP: the mesh spans [cz - h, cz]. This used to be
            // centre-based (position.y +/- h/2), which disagreed with the mesh
            // and with collision - three different stories for one brush.
            out.min = {position.x - maxR, position.y - h, position.z - maxR};
            out.max = {position.x + maxR, position.y,     position.z + maxR};
            break;
        }
        case OzonePrimitiveType::SPHERE: {
            float r = (args.size() > 3) ? args[3] : 1.0f;
            out.min = {position.x - r, position.y - r, position.z - r};
            out.max = {position.x + r, position.y + r, position.z + r};
            break;
        }
        case OzonePrimitiveType::PYRAMID: {
            float w = (args.size() > 3) ? args[3] : 2.0f;
            float d = (args.size() > 4) ? args[4] : 2.0f;
            float h = (args.size() > 5) ? args[5] : 2.0f;
            // Same top convention as the cylinder.
            out.min = {position.x - w/2, position.y - h, position.z - d/2};
            out.max = {position.x + w/2, position.y,     position.z + d/2};
            break;
        }
        case OzonePrimitiveType::PLANE: {
            // Oriented 10x10 quad: `n . x = dist`, so the AABB is the corner
            // spread of the rotated quad rather than a fixed horizontal slab.
            float nx   = (args.size() > 3) ? args[3] : 0.0f;
            float ny   = (args.size() > 4) ? args[4] : 1.0f;
            float nz   = (args.size() > 5) ? args[5] : 0.0f;
            float dist = (args.size() > 6) ? args[6] : 0.0f;
            // Same ozone -> engine swap BuildFromPrimitive applies.
            float enx = nx, eny = nz, enz = ny;
            float len = sqrtf(enx*enx + eny*eny + enz*enz);
            if (len < 1e-6f) { enx = 0.0f; eny = 1.0f; enz = 0.0f; len = 1.0f; }
            enx /= len; eny /= len; enz /= len;

            constexpr float kHalf = 5.0f;          // half of the 10x10 patch
            // Two tangents spanning the quad, then the four corners.
            float ux, uy, uz;
            if (fabsf(eny) > 0.99f) { ux = 1; uy = 0; uz = 0; } else { ux = 0; uy = 1; uz = 0; }
            float ax = uy*enz - uz*eny, ay = uz*enx - ux*enz, az = ux*eny - uy*enx;
            float al = sqrtf(ax*ax + ay*ay + az*az);
            if (al < 1e-6f) { ax = 1; ay = 0; az = 0; al = 1; }
            ax /= al; ay /= al; az /= al;
            const float vx = eny*az - enz*ay, vy = enz*ax - enx*az, vz = enx*ay - eny*ax;

            const float cx = position.x + enx * dist;
            const float cy = position.y + eny * dist;
            const float cz = position.z + enz * dist;
            float extX = kHalf * (fabsf(ax) + fabsf(vx));
            float extY = kHalf * (fabsf(ay) + fabsf(vy));
            float extZ = kHalf * (fabsf(az) + fabsf(vz));
            // A flat quad has no volume on one axis, which CSG cannot overlap;
            // the collision pass inflates that axis, and bounds only needs to
            // describe it.
            out.min = {cx - extX, cy - extY, cz - extZ};
            out.max = {cx + extX, cy + extY, cz + extZ};
            break;
        }
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// RebuildCollisionVolumes Ã¢â‚¬â€ iterate renderables and generate AABBs
// ---------------------------------------------------------------------------
void OzoneLoader::RebuildCollisionVolumes() {
    m_collisionVolumes.clear();

    // Phase 1: collect all brush AABBs with their CSG operations
    CsgProcessor csg;
    for (auto& r : m_renderables) {
        if (!r.loaded) continue;
        // A skybox is a hollow room, not a solid. Feeding it to CSG would put an
        // invisible wall around the player and, worse, any `sub` against it would
        // carve away the sky. Render-only by construction.
        //
        // This is a SPECIAL CASE of SURF_NO_BSP_CUTS, which the parser forces on
        // every skybox (OzoneParser.cpp) - but the type check runs first, so the
        // flag was never what kept a skybox out of the collision world. The docs
        // claimed the flag did. It did not, and the flag had no other consumer at
        // all, so it round-tripped through every .ozone doing nothing.
        if (r.typeId == (int)OzonePrimitiveType::SKYBOX) continue;

        // SURF_NO_BSP_CUTS, made real: opt a brush out of collision entirely.
        // Cosmetic-only geometry (a trigger volume's visual, a skybox authored as a
        // plain box, a decorative wall the player should pass through) previously
        // had no way to say so - the collision world is AABB-only and every loaded
        // brush was a solid unless it was an entity.
        if (r.surface.def.flags & (uint32_t)oz::surface::SURF_NO_BSP_CUTS) continue;
        // Skip entity types Ã¢â‚¬â€ handled by PawnSystem
        if (r.typeId == (int)OzonePrimitiveType::ENTITY_PLAYERSTART ||
            r.typeId == (int)OzonePrimitiveType::ENTITY_PICKUP    ||
            r.typeId == (int)OzonePrimitiveType::ENTITY_ZONE      ||
            r.typeId == (int)OzonePrimitiveType::ENTITY_NPC       ||
            r.typeId == (int)OzonePrimitiveType::ENTITY_LIGHT)
            continue;

        // Heightmap: emit an AABB covering the full terrain extent.
        // Per-cell collision is handled by SampleHeightmapY ground clamp.
        if (r.typeId == (int)OzonePrimitiveType::HEIGHTMAP && m_hmReady) {
            CsgBrush brush;
            brush.minX = m_hmPosition.x - m_hmSize.x * m_hmScale * 0.5f;
            brush.minY = m_hmPosition.y;
            brush.minZ = m_hmPosition.z - m_hmSize.z * m_hmScale * 0.5f;
            brush.maxX = m_hmPosition.x + m_hmSize.x * m_hmScale * 0.5f;
            brush.maxY = m_hmPosition.y + m_hmSize.y * m_hmScale;
            brush.maxZ = m_hmPosition.z + m_hmSize.z * m_hmScale * 0.5f;
            brush.op   = CsgOp::SOLID;
            csg.Apply(brush);
            continue;
        }

        // Conservative rotation-aware AABB. Collision is AABB-only, so a rotated
        // brush cannot be exact; using the UNROTATED mesh box let the ends of a
        // rotated beam stick out of their own collision. RotatedAabbY is the
        // tightest box containing the rotated shape, and mirrors the composition
        // RenderableModelMatrix applies at draw time.
        const BoundingBox mb = GetMeshBoundingBox(r.model.meshes[0]);
        const AabbBox local{mb.min.x, mb.min.y, mb.min.z, mb.max.x, mb.max.y, mb.max.z};
        const AabbBox world = RotatedAabbY(local, r.rotation,
                                           r.position.x, r.position.y, r.position.z,
                                           r.scale);
        CsgBrush brush;
        brush.minX = world.minX;
        brush.minY = world.minY;
        brush.minZ = world.minZ;
        brush.maxX = world.maxX;
        brush.maxY = world.maxY;
        brush.maxZ = world.maxZ;
        // Zero-thickness geometry cannot overlap in CSG — inflate slightly so
        // SUB/INTERSECT against it still produces a volume. Applied to EVERY
        // axis, not just Y: a horizontal plane slab needed the Y inflation, and
        // an oriented `pln` (which can be vertical) needs X or Z.
        if (brush.maxX - brush.minX < 0.02f) { brush.minX -= 0.05f; brush.maxX += 0.05f; }
        if (brush.maxY - brush.minY < 0.02f) { brush.minY -= 0.05f; brush.maxY += 0.05f; }
        if (brush.maxZ - brush.minZ < 0.02f) { brush.minZ -= 0.05f; brush.maxZ += 0.05f; }
        brush.op   = (CsgOp)r.csgOp;
        csg.Apply(brush);
    }

    // Overflow protection: merge adjacent coplanar AABBs
    int merges = csg.MergePass();
    if (merges > 0) {
        OZ_INFO("CSG: merged %d adjacent volumes (count: %d Ã¢â€ â€™ %d)",
                merges, csg.Count() + merges, csg.Count());
    }

    // Phase 2: convert CSG-processed volumes to collision volumes
    std::vector<CsgProcessor::Volume> vols;
    csg.GetVolumes(vols);
    m_collisionVolumes.reserve(vols.size());
    for (auto& v : vols) {
        OzoneCollisionVolume cv;
        cv.aabb.min = {v.minX, v.minY, v.minZ};
        cv.aabb.max = {v.maxX, v.maxY, v.maxZ};
        cv.typeId = 0;
        if (m_hmReady) {
            float hmMinX = m_hmPosition.x - m_hmSize.x * m_hmScale * 0.5f;
            float hmMinZ = m_hmPosition.z - m_hmSize.z * m_hmScale * 0.5f;
            float hmMaxX = m_hmPosition.x + m_hmSize.x * m_hmScale * 0.5f;
            float hmMaxY = m_hmPosition.y + m_hmSize.y * m_hmScale;
            float hmMaxZ = m_hmPosition.z + m_hmSize.z * m_hmScale * 0.5f;
            const float eps = 0.01f;
            if (fabsf(v.minX - hmMinX) < eps && fabsf(v.minY - m_hmPosition.y) < eps &&
                fabsf(v.minZ - hmMinZ) < eps && fabsf(v.maxX - hmMaxX) < eps &&
                fabsf(v.maxY - hmMaxY) < eps && fabsf(v.maxZ - hmMaxZ) < eps)
                cv.isHeightmap = true;
        }
        m_collisionVolumes.push_back(cv);
    }

    // Phase 2b: copy texture settings from renderables to collision volumes.
    //
    // ONLY valid when the two lists are in one-to-one correspondence. The comment
    // used to say "when the counts match (no CSG merging occurred)" and then never
    // checked that — it only bounded the write with `rCount < volumes.size()`, which
    // prevents an out-of-bounds write but not a MIS-MAPPED one.
    //
    // m_collisionVolumes is the CSG OUTPUT, and CSG does not preserve index order:
    //   * Subtract replaces one solid with up to 6 parts (OzBsp.cpp:127-133), so the
    //     list GROWS and every later solid keeps its index while the count shifts.
    //   * Intersect calls RemoveAt for a non-overlapping solid (:150), and RemoveAt is
    //     SWAP-WITH-LAST (:44) — so an unrelated solid is moved into the freed slot
    //     and the last one disappears.
    //   * MergePass merges adjacent boxes (:223/:239/:255), again via RemoveAt.
    // So once anything but plain `add` brushes is present, output index k bears no
    // relation to input index k, and the copy puts one brush's UVs onto a different
    // brush's collision box.
    //
    // Count equality is a heuristic, not a proof: CSG can split one solid and drop
    // another in the same rebuild and land back on the input count. It is adopted
    // anyway because it is exactly what the comment already claimed, it is exact for
    // every world built only from `add` brushes, and it fails to the SAFE side
    // (engine defaults) rather than the dangerous one (another brush's texture).
    // Carrying a source index through CsgProcessor would be exact but is a larger
    // change than the consumers justify: the only reader of these fields is the
    // editor's props-panel size/UV fallback, which is itself keyed on a selection
    // index rather than a volume index.
    //
    // Also surface the CSG overflow count here: a non-zero value means a `sub`
    // brush stopped carving part-way, which is invisible from the render mesh
    // alone. The budget is per BRUSH, so this is not "the world has too many
    // solids" - it is one brush whose carve was abandoned.
    if (csg.overflow_count() > 0) {
        OZ_WARN("CSG: %d brush(es) hit the %d-volume per-brush carve budget and "
                "stopped early — geometry you intended to cut is still solid",
                csg.overflow_count(), CsgProcessor::MAX_SPLITS);
    }
    m_lastCsgOverflow = csg.overflow_count();

    {
        int renderableBrushCount = 0;
        for (auto& r : m_renderables) {
            if (!r.loaded) continue;
            if (r.typeId == (int)OzonePrimitiveType::ENTITY_PLAYERSTART ||
                r.typeId == (int)OzonePrimitiveType::ENTITY_PICKUP    ||
                r.typeId == (int)OzonePrimitiveType::ENTITY_ZONE      ||
                r.typeId == (int)OzonePrimitiveType::ENTITY_NPC       ||
                r.typeId == (int)OzonePrimitiveType::ENTITY_LIGHT     ||
                r.typeId == (int)OzonePrimitiveType::HEIGHTMAP)
                continue;
            renderableBrushCount++;
        }

        const bool oneToOne = (renderableBrushCount == (int)m_collisionVolumes.size());
        if (!oneToOne && renderableBrushCount > 0) {
            OZ_INFO("CSG: %d brush renderables produced %d collision volumes — "
                    "skipping texture copy (index spaces no longer correspond)",
                    renderableBrushCount, (int)m_collisionVolumes.size());
        }

        if (oneToOne) {
            int rCount = 0;
            for (auto& r : m_renderables) {
                if (!r.loaded) continue;
                if (r.typeId == (int)OzonePrimitiveType::ENTITY_PLAYERSTART ||
                    r.typeId == (int)OzonePrimitiveType::ENTITY_PICKUP    ||
                    r.typeId == (int)OzonePrimitiveType::ENTITY_ZONE      ||
                    r.typeId == (int)OzonePrimitiveType::ENTITY_NPC       ||
                    r.typeId == (int)OzonePrimitiveType::ENTITY_LIGHT     ||
                    r.typeId == (int)OzonePrimitiveType::HEIGHTMAP)
                    continue;
                m_collisionVolumes[rCount].texScaleU  = r.texScaleU;
                m_collisionVolumes[rCount].texScaleV  = r.texScaleV;
                m_collisionVolumes[rCount].texOffsetU = r.texOffsetU;
                m_collisionVolumes[rCount].texOffsetV = r.texOffsetV;
                m_collisionVolumes[rCount].texSlot     = r.texSlot;
                m_collisionVolumes[rCount].texPath     = r.texPath;
                rCount++;
            }
        }
    }

    // Phase 3: rebuild spatial partition from processed volumes
    std::vector<WorldChunkManager::Volume> wcVols;
    wcVols.reserve(vols.size());
    for (auto& v : vols)
        wcVols.push_back({v.minX, v.minY, v.minZ, v.maxX, v.maxY, v.maxZ});
    m_chunkManager.Build(wcVols);
}

// ---------------------------------------------------------------------------
// AddBrushRenderable â€” editor helper to make a new brush visible
// ---------------------------------------------------------------------------
int OzoneLoader::AddBrushRenderable(int primType, const Vector3& pos,
                                    const Vector3& size, float rot,
                                    float scale, int csgOp, int surfaceFlags,
                                    float cylRTop, float cylRBot, int cylSlices) {
    Model mdl = {0};
    float builtCylRTop = 0.0f, builtCylRBot = 0.0f;
    int   builtCylSlices = 0;
    // Bake the ghost scale into the geometry and store scale = 1. The exporter
    // writes a box's size from the mesh bbox * r->scale, but a cylinder's
    // authored radii straight from cylRTop/RBot - so a non-baked scale made a
    // placed cylinder thinner after save/reload than it was in the editor.
    const float fit = (scale > 0.0001f) ? scale : 1.0f;
    switch (primType) {
        case 0: mdl = BuildBox(size.x * fit, size.y * fit, size.z * fit); break;
        case 1: {
            // size is a DIAMETER x height x depth; BuildCylinder wants RADII.
            // Passing the raw size made a placed cylinder twice the ghost's
            // width, with its height used as a radius.
            const float diameter = fmaxf(size.x, size.z) * fit;
            builtCylRTop = (cylRTop > 0.0f) ? cylRTop * fit : diameter * 0.5f;
            builtCylRBot = (cylRBot > 0.0f) ? cylRBot * fit : builtCylRTop;
            builtCylSlices = (cylSlices > 0) ? cylSlices : 16;
            mdl = BuildCylinder(builtCylRTop, builtCylRBot, size.y * fit, builtCylSlices);
            break;
        }
        case 2: {
            // size is a DIAMETER; BuildSphere wants a RADIUS.
            const float diameter = fmaxf(size.x, fmaxf(size.y, size.z)) * fit;
            mdl = BuildSphere(diameter * 0.5f, 16);
            break;
        }
        case 3: mdl = BuildPyramid(size.x * fit, size.z * fit, size.y * fit); break;
        case 4: mdl = BuildPlane(0, 1, 0, 0); break;
        case 5: {
            // Flat platform heightmap (no file paths needed for editor preview)
            int gw = 9, gh = 9;
            std::vector<float> h(gw * gh, 0.5f);
            float cx = size.x / (float)(gw - 1);
            float cz = size.z / (float)(gh - 1);
            mdl = BuildHeightmapMesh(h, gw, gh, cx, cz, size.y);
            if (mdl.meshCount > 0)
                ApplyTex(mdl, Texture2D{0}, DARKGRAY);
            break;
        }
        default: return -1;
    }
    if (mdl.meshes == nullptr) return -1;

    OzoneRenderable r;
    r.typeId = primType;
    r.scale = 1.0f;   // `fit` is already baked into the geometry above
    // Store rotation in radians (matches file-loaded renderables); the editor
    // passes degrees from its UI fields.
    r.rotation = rot * DEG2RAD;

    // `pos` is the desired WORLD CENTRE. Cylinder/pyramid meshes span [-h, 0],
    // so assigning pos directly put them half a height below the ghost they
    // were placed from. Derive the renderable position from the mesh's own
    // local centre (rotated into world space) instead.
    {
        r.model = mdl;
        const BoundingBox mb = GetMeshBoundingBox(mdl.meshes[0]);
        const float lcx = (mb.min.x + mb.max.x) * 0.5f * r.scale;
        const float lcy = (mb.min.y + mb.max.y) * 0.5f * r.scale;
        const float lcz = (mb.min.z + mb.max.z) * 0.5f * r.scale;
        const float c = cosf(r.rotation), s = sinf(r.rotation);
        r.position = { pos.x - (lcx * c + lcz * s),
                       pos.y - lcy,
                       pos.z - (-lcx * s + lcz * c) };
    }

    if (primType == 1) {
        r.cylRTop = builtCylRTop;
        r.cylRBot = builtCylRBot;
        r.cylSlices = builtCylSlices;
    }
    r.loaded = true;
    r.csgOp = csgOp;
    // Write the OWNER, then derive the legacy mirror from it. Assigning
    // r.surfaceFlags directly (as this used to) left surface.def.flags at 0, so
    // the two fields disagreed — the one invariant every other writer in this
    // file maintains. For a collision proxy that meant the exporter compared 0
    // against 16 and wrote a literal `flags=0`, i.e. a proxy that collided but was
    // then drawn in game on the next reload.
    r.surface.def.flags = (uint32_t)surfaceFlags;
    DeriveLegacyRenderable(r);
    m_renderables.push_back(r);
    return (int)m_renderables.size() - 1;
}

// ---------------------------------------------------------------------------
// AppendAutoConvexCollision - voxelise a mesh into convex collision boxes.
//
// The engine's collision world is AABB-only (CsgProcessor, see OzBsp.hpp), so a
// placed Mesh.Static prop has no collision whatsoever and the player walks
// through it. Each generated box is appended as a real SURF_COLLISION_PROXY
// brush so it exports to the .ozone as a plain `add box ... flags=N` line and
// survives a save/load round trip - the proxies are authored data, not a
// derived cache that would be lost on the next edit.
//
// Everything is built up front and only committed once the whole set is known
// to fit inside `maxBoxes`: a half-appended hull would leave a gap the player
// can fall through, which is exactly the bug this feature exists to fix.
// ---------------------------------------------------------------------------
int OzoneLoader::AppendAutoConvexCollision(const float* verts, int vertFloatCount,
                                           float cellSize, int maxBoxes,
                                           AutoConvexFail* failOut) {
    AutoConvexParams params;
    params.cellSize = cellSize;
    params.maxBoxes = maxBoxes;

    std::vector<ConvexBox> boxes;
    AutoConvexFail fail = AutoConvexFail::NONE;
    int n = AutoConvex_Build(verts, vertFloatCount, params, boxes, &fail);
    if (failOut) *failOut = fail;
    if (n <= 0) {
        // Say WHICH failure it was. The editor used to attribute every one of
        // these to the box budget, so a grid/geometry problem sent the author
        // hunting for a budget they were nowhere near.
        OZ_WARN("AutoConvex: refused (cell %.2f, budget %d): %s",
                cellSize, maxBoxes, AutoConvexFailName(fail));
        return 0;
    }

    int appended = 0;
    for (int i = 0; i < n; i++) {
        const ConvexBox& b = boxes[i];
        Vector3 size = {b.maxX - b.minX, b.maxY - b.minY, b.maxZ - b.minZ};
        Vector3 center = {(b.minX + b.maxX) * 0.5f,
                          (b.minY + b.maxY) * 0.5f,
                          (b.minZ + b.maxZ) * 0.5f};
        // SOLID (not ADD): a proxy is a plain solid volume, and SOLID is what
        // CsgProcessor treats as additive - see CsgOp in OzBsp.hpp.
        const int idx = AddBrushRenderable((int)OzonePrimitiveType::BOX, center, size,
                                           0.0f, 1.0f, (int)CsgOp::SOLID, SURF_COLLISION_PROXY);
        if (idx >= 0) appended++;
    }

    RebuildCollisionVolumes();

    // A rejected append leaves a HOLE in the hull. Reporting "n proxies" while
    // some were dropped contradicts the "never a partial hull" contract, so say
    // so and report the number actually added.
    if (appended != n) {
        OZ_WARN("AutoConvex: only %d of %d proxies were added - the hull has HOLES "
                "and the player can fall through them", appended, n);
    }
    OZ_INFO("AutoConvex: appended %d collision proxies (%zu total renderables)",
            appended, m_renderables.size());
    return appended;
}

// ---------------------------------------------------------------------------
// ApplyRenderableUV -- modify texture UV tiling/offset on a renderable's mesh
// The transform is: new_u = uvBaseU * su + ou, new_v = uvBaseV * sv + ov
// Always recomputed from OzoneRenderable::uvBase, the pristine texcoords the
// primitive generator produced. Deriving from the base rather than trying to
// invert the previous transform is what makes repeated Apply non-compounding.
// ---------------------------------------------------------------------------
void OzoneLoader::ApplyRenderableUV(int idx, float su, float sv, float ou, float ov) {
    OzoneRenderable* r = Get(idx);
    if (!r || !r->loaded || r->model.meshCount == 0) return;
    Mesh& mesh = r->model.meshes[0];
    if (!mesh.texcoords) return;

    const size_t need = (size_t)mesh.vertexCount * 2;
    if (r->uvBase.size() != need) {
        r->uvBase.assign(mesh.texcoords, mesh.texcoords + need);
    }

    oz::surface::ApplyUVTransform(r->uvBase.data(), mesh.vertexCount,
                                  su, sv, ou, ov, mesh.texcoords);

    // Store new params
    r->texScaleU = su;
    r->texScaleV = sv;
    r->texOffsetU = ou;
    r->texOffsetV = ov;

    // Upload updated UVs to GPU (attribute index 1 = texcoords)
    UpdateMeshBuffer(mesh, 1, mesh.texcoords,
                     mesh.vertexCount * 2 * (int)sizeof(float), 0);

    // A per-face brush is drawn from its face sub-meshes, which carry their OWN
    // texcoord arrays. Without this the model's UVs below would be updated and
    // the geometry actually on screen would not - a brush-wide UV edit from the
    // Properties panel would look like it did nothing.
    if (r->surface.NeedsPerFaceDraw()) RebuildSurfaceMeshes(idx);
}

// ---------------------------------------------------------------------------
// ApplyRenderableTexture -- load a custom texture from a file path and apply
// it to the renderable's model material. Replaces any tileset or previous
// custom texture. Stores the path on the renderable for persistence.
// Returns true if the texture was loaded and applied successfully.
// ---------------------------------------------------------------------------
Texture2D OzoneLoader::ResolveFaceTextureHandle(const oz::surface::SurfaceProps& p) {
    const oz::surface::FaceTextureRef ref = oz::surface::ResolveFaceTexture(p);

    if (!ref.path.empty()) {
        // The SAME resolver brush `texPath=` uses (repo-relative, world-relative
        // and absolute all handled), so a face texture behaves identically
        // wherever it was authored. A second copy of this rule would drift.
        const std::string tp = ResolveWorldAssetPath(ref.path, m_worldDir, m_worldDir);

        auto it = m_faceTexCache.find(tp);
        if (it != m_faceTexCache.end()) return it->second;

        Texture2D tex = LoadTextureWithFallback(tp.c_str());
        if (tex.id == 0) {
            OZ_WARN("Surface: face texture '%s' failed to load", tp.c_str());
            m_faceTexCache[tp] = Texture2D{0};   // cache the miss: one warning, not one per frame
            return Texture2D{0};
        }
        m_faceTexCache[tp] = tex;
        return tex;
    }

    if (ref.slot > 0) return GetTilesetTex(ref.slot);
    return Texture2D{0};
}

bool OzoneLoader::ApplyRenderableTexture(int idx, const char* path) {    OzoneRenderable* r = Get(idx);
    if (!r || !r->loaded || r->model.meshCount == 0) return false;

    Texture2D tex = LoadTextureWithFallback(path);
    if (tex.id == 0) return false;

    // Unload previous custom texture if any
    if (r->customTex.id > 0) UnloadTexture(r->customTex);

    r->customTex = tex;
    r->texSlot = 0; // custom texture overrides tileset

    // surface.def is the OWNER of a brush's texture path; r->texPath is a derived
    // view of it (DeriveLegacySurfaceMirror). Writing only the mirror left the
    // owner empty, so the next DeriveLegacyRenderable - which runs on ANY surface
    // edit - overwrote the mirror back to "" and the next export lost `texPath=`.
    // Write the owner and re-derive, exactly like every other writer.
    //
    // No mesh rebuild here on purpose: a texture is bound per draw, not baked into
    // the mesh, and setting a non-default path makes NeedsPerFaceDraw() true so
    // the lazy draw path builds the sub-meshes if they do not exist yet.
    r->surface.def.texPath = path ? path : "";
    DeriveLegacyRenderable(*r);

    r->model.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = tex;
    if (GetLitFogShader().id > 0)
        r->model.materials[0].shader = GetLitFogShader();

    OZ_INFO("Applied custom texture to renderable %d: %s", idx, path);
    return true;
}

// ---------------------------------------------------------------------------
// Per-face surface meshes
//
// A brush mesh is one raylib Mesh, but surface properties (unlit, masked,
// translucent, glow, pan, per-face texture) are SHADER UNIFORMS, and a uniform
// is per draw call. So a brush with any per-face difference has to be drawn as up
// to six DrawMesh calls, one per face, each with its own uniforms.
//
// Triangles are bucketed by the dominant axis of their normal, which works for
// every primitive generator (box, cylinder, sphere, pyramid, plane) instead of
// depending on raylib's internal face ordering.
//
// The sub-meshes are built lazily and cached on the renderable: an undecorated
// brush never pays for the split, and there is no per-frame cost either way.
// ---------------------------------------------------------------------------
static void BuildFaceMeshes(OzoneRenderable& r) {
    if (r.faceMeshesBuilt) return;
    r.faceMeshesBuilt = true;
    if (!r.loaded || r.model.meshCount <= 0) return;

    Mesh& src = r.model.meshes[0];
    if (!src.vertices || !src.indices || src.vertexCount <= 0) return;

    for (int f = 0; f < oz::surface::FACE_COUNT; f++)
        r.faceMesh[f] = Mesh{0};

    // First pass: which triangles belong to which face.
    std::vector<unsigned short> tris[oz::surface::FACE_COUNT];
    for (int t = 0; t < src.triangleCount; t++) {
        unsigned short i0 = src.indices[t * 3 + 0];
        unsigned short i1 = src.indices[t * 3 + 1];
        unsigned short i2 = src.indices[t * 3 + 2];
        if ((int)i0 >= src.vertexCount || (int)i1 >= src.vertexCount ||
            (int)i2 >= src.vertexCount) continue;
        const float* a = src.vertices + (size_t)i0 * 3;
        const float* b = src.vertices + (size_t)i1 * 3;
        const float* c = src.vertices + (size_t)i2 * 3;
        const float e1x = b[0] - a[0], e1y = b[1] - a[1], e1z = b[2] - a[2];
        const float e2x = c[0] - a[0], e2y = c[1] - a[1], e2z = c[2] - a[2];
        // Unnormalised cross product: only the direction matters for bucketing.
        const float nx = e1y * e2z - e1z * e2y;
        const float ny = e1z * e2x - e1x * e2z;
        const float nz = e1x * e2y - e1y * e2x;
        const int face = (int)oz::surface::FaceFromNormal(nx, ny, nz);
        if (face < 0 || face >= oz::surface::FACE_COUNT) continue;
        tris[face].push_back(i0);
        tris[face].push_back(i1);
        tris[face].push_back(i2);
    }

    // Second pass: one Mesh per face.
    //
    // Each sub-mesh ALIASES the source's vertices/normals/colors while UploadMesh
    // reads them and uploads its own VBO copies, but it OWNS its index buffer and
    // its texcoords: a face's UV scale/offset is its own, so the six faces cannot
    // share one texcoord array the way they used to.
    //
    // The borrowed aliases are DROPPED before the Mesh is stored. That is the
    // whole point: UnloadMesh frees EVERY CPU array a Mesh points at, so a stored
    // alias makes the four teardown paths (RebuildSurfaceMeshes,
    // RemoveRenderable, UpdateBrushRenderable, Unload) free arrays that
    // UnloadModel is about to free again. That double free silently corrupted the
    // heap, and it did not surface until the next world load - OzoneLoader::Unload()
    // is where the damage was detected, inside ClearScene(), BEFORE the new
    // world's "OZONE format:" log line. So the level you opened was never the
    // level at fault: any world with a surface-flagged brush (SURF_FAKEBACKDROP
    // backdrops in particular) poisons the heap, and the NEXT world you open dies
    // in ClearScene.
    //
    // The same rule is why the fresh texcoords are RL_MALLOC'd rather than
    // pointed at r.uvBase: a borrowed base would be freed by UnloadMesh and then
    // freed again when the vector dies. Nulling the remaining pointers is safe
    // because nothing reads a face sub-mesh's CPU arrays: the only consumer is
    // DrawMesh, which draws from the VBO ids UploadMesh set.
    const size_t uvCount = (size_t)src.vertexCount * 2;
    if (r.uvBase.size() != uvCount && src.texcoords)
        r.uvBase.assign(src.texcoords, src.texcoords + uvCount);
    const float* uvBase = (r.uvBase.size() == uvCount) ? r.uvBase.data() : nullptr;

    for (int f = 0; f < oz::surface::FACE_COUNT; f++) {
        const size_t n = tris[f].size();
        if (n < 3) continue;   // primitive has no geometry on this axis
        Mesh m = {0};
        m.vertexCount = src.vertexCount;
        m.triangleCount = (int)(n / 3);
        m.vertices = src.vertices;
        m.normals = src.normals;
        m.colors = src.colors;
        m.indices = (unsigned short*)RL_MALLOC(n * sizeof(unsigned short));
        if (!m.indices) continue;
        memcpy(m.indices, tris[f].data(), n * sizeof(unsigned short));

        m.texcoords = (float*)RL_MALLOC(uvCount * sizeof(float));
        if (!m.texcoords) { RL_FREE(m.indices); m.indices = nullptr; continue; }
        const oz::surface::SurfaceProps& fp =
            r.surface.Resolve((oz::surface::SurfaceFace)f);
        if (uvBase) {
            oz::surface::ApplyUVTransform(uvBase, src.vertexCount,
                                          fp.uvScaleU, fp.uvScaleV,
                                          fp.uvOffsetU, fp.uvOffsetV, m.texcoords);
        } else if (src.texcoords) {
            // NEVER zero the UVs. A face whose texcoords are all (0,0) samples a
            // single texel, so it renders as a flat slab of that texture's corner -
            // visually indistinguishable from "untextured white", and impossible to
            // diagnose from the frame. This is the only place a surface-flagged
            // brush gets its UVs, so it must always copy something real.
            memcpy(m.texcoords, src.texcoords, uvCount * sizeof(float));
        } else {
            memset(m.texcoords, 0, uvCount * sizeof(float));
            OZ_WARN("Surface: brush at (%.1f, %.1f, %.1f) has no texcoords at all; "
                    "face %d will render flat", r.position.x, r.position.y,
                    r.position.z, f);
        }

        UploadMesh(&m, false);
        // Keep indices + texcoords (both owned here); drop the three borrowed.
        m.vertices = nullptr;
        m.normals = nullptr;
        m.colors = nullptr;
        r.faceMesh[f] = m;
    }
}

void OzoneLoader::RebuildSurfaceMeshes(int idx) {
    OzoneRenderable* r = Get(idx);
    if (!r) return;
    for (int f = 0; f < oz::surface::FACE_COUNT; f++) {
        if (r->faceMesh[f].vaoId > 0) UnloadMesh(r->faceMesh[f]);
        r->faceMesh[f] = Mesh{0};
    }
    r->faceMeshesBuilt = false;
    if (r->surface.NeedsPerFaceDraw()) BuildFaceMeshes(*r);
}

void OzoneLoader::SetRenderableFace(int idx, oz::surface::SurfaceFace face,
                                    const oz::surface::SurfaceProps& p) {
    OzoneRenderable* r = Get(idx);
    if (!r) return;
    if (face == oz::surface::FACE_NONE) r->surface.def = p;
    else                                r->surface.SetFace(face, p);
    // Every derived view, not just surfaceFlags: ExportToOzone emits the
    // brush-wide UV fields from r->texScaleU/V and suppresses `surfTex=` when
    // r->texPath is non-empty, so re-deriving the flag bit alone would discard a
    // brush-wide texture or UV edit on save. See DeriveLegacySurfaceMirror.
    DeriveLegacyRenderable(*r);
    // Brush-wide U/V scale and offset live in surface.def but the MESH texcoords
    // are what actually get sampled, so a brush-wide UV edit has to be pushed into
    // the mesh too. Per-face UV is handled by RebuildSurfaceMeshes: each face
    // sub-mesh owns its own texcoord array and BuildFaceMeshes applies that face's
    // resolved uvScale/uvOffset to it.
    if (face == oz::surface::FACE_NONE) {
        const oz::surface::SurfaceProps& d = r->surface.def;
        ApplyRenderableUV(idx, d.uvScaleU, d.uvScaleV, d.uvOffsetU, d.uvOffsetV);
    }
    RebuildSurfaceMeshes(idx);
}

void OzoneLoader::ResetRenderableSurface(int idx) {
    OzoneRenderable* r = Get(idx);
    if (!r) return;
    r->surface.ResetToDefault();
    DeriveLegacyRenderable(*r);
    RebuildSurfaceMeshes(idx);
}

// Reset the BRUSH-WIDE default, leaving per-face overrides alone.
//
// BrushSurface::ResetToDefault deliberately KEEPS def - it means "push the
// brush default onto every face", not "wipe the brush". So the editor's brush-wide
// Reset cannot reuse ResetRenderableSurface; it needs this.
void OzoneLoader::ResetRenderableSurfaceDefault(int idx) {
    OzoneRenderable* r = Get(idx);
    if (!r) return;
    r->surface.def = oz::surface::SurfaceProps{};
    DeriveLegacyRenderable(*r);
    // def's UVs are back at 1/0, so re-tile the mesh from the pristine snapshot.
    ApplyRenderableUV(idx, 1.0f, 1.0f, 0.0f, 0.0f);
    RebuildSurfaceMeshes(idx);
}

// ---------------------------------------------------------------------------
// RemoveRenderable -- remove a brush renderable at the given index.
// Unloads the model and custom texture, then erases from the list.
// ---------------------------------------------------------------------------
void OzoneLoader::RemoveRenderable(int idx) {
    OzoneRenderable* r = Get(idx);
    if (!r) return;
    for (int f = 0; f < oz::surface::FACE_COUNT; f++) {
        // The face sub-meshes own their own index buffers, so UnloadModel (which
        // only knows about r->model) never freed them: a leak on every surface-
        // flagged brush deletion, and a dangling vaoId if the indices were reused.
        if (r->faceMesh[f].vaoId > 0) UnloadMesh(r->faceMesh[f]);
        r->faceMesh[f] = Mesh{0};
    }
    r->faceMeshesBuilt = false;
    if (r->customTex.id > 0) UnloadTexture(r->customTex);
    if (r->loaded) UnloadModel(r->model);
    m_renderables.erase(m_renderables.begin() + idx);
}

// ---------------------------------------------------------------------------
// UpdateBrushRenderable -- regenerate a brush renderable's mesh from new
// position, size, and rotation. Preserves texture slot, custom texture,
// and UV transform. Called from the editor Properties panel when the user
// applies changes.
// ---------------------------------------------------------------------------
void OzoneLoader::UpdateBrushRenderable(int idx, const Vector3& pos, const Vector3& size, float rot) {
    OzoneRenderable* r = Get(idx);
    if (!r || !r->loaded) return;
    if (r->typeId < 0 || r->typeId > 4) return; // only primitive types 0..4
    if (r->model.meshCount <= 0) return;

    // Save texture state to re-apply on the new mesh
    int oldTexSlot = r->texSlot;
    Texture2D oldCustomTex = r->customTex;
    float oldSu = r->texScaleU;
    float oldSv = r->texScaleV;
    float oldOu = r->texOffsetU;
    float oldOv = r->texOffsetV;

    // `size` is in WORLD units (the Properties panel shows mesh bounds * scale).
    // Keep the old world diameter before the model is replaced, so a tapered
    // cylinder can be resized without losing its taper.
    BoundingBox oldMb = GetMeshBoundingBox(r->model.meshes[0]);
    const float oldScale = (r->scale > 0.0001f) ? r->scale : 1.0f;
    const float oldDiameter = fmaxf(oldMb.max.x - oldMb.min.x,
                                    oldMb.max.z - oldMb.min.z) * oldScale;

    Model newModel = {0};
    float newCylRTop = r->cylRTop;
    float newCylRBot = r->cylRBot;
    int   newCylSlices = r->cylSlices;
    switch (r->typeId) {
        case 0: newModel = BuildBox(size.x, size.y, size.z); break;
        case 1: {
            // BuildCylinder takes RADII; the panel edits DIAMETERS. Passing the
            // raw size made radius = diameter and height = diameter, so any
            // Properties Apply on a cylinder exploded/sheared it. Scale the
            // stored taper uniformly from the old world diameter instead of
            // flattening it to one radius.
            const float newDiameter = fmaxf(size.x, size.z);
            const float ratio = (oldDiameter > 1e-4f) ? (newDiameter / oldDiameter) : 1.0f;
            // Stored radii are LOCAL; the rebuilt mesh is baked at scale 1, so
            // convert them to world radii first or a previously scaled brush
            // comes back smaller by that factor.
            const float oldWorldTop = (r->cylRTop > 1e-4f)
                                          ? r->cylRTop * oldScale
                                          : oldDiameter * 0.5f;
            const float oldWorldBot = (r->cylRBot > 1e-4f)
                                          ? r->cylRBot * oldScale
                                          : oldWorldTop;
            newCylRTop = oldWorldTop * ratio;
            newCylRBot = oldWorldBot * ratio;
            newCylSlices = (r->cylSlices > 0) ? r->cylSlices : 16;
            newModel = BuildCylinder(newCylRTop, newCylRBot, size.y, newCylSlices);
            break;
        }
        case 2: {
            // BuildSphere takes a RADIUS; the panel edits a DIAMETER.
            const float diameter = fmaxf(size.x, fmaxf(size.y, size.z));
            newModel = BuildSphere(diameter * 0.5f, 16);
            break;
        }
        case 3: newModel = BuildPyramid(size.x, size.z, size.y); break;
        case 4: newModel = BuildPlane(r->planeNX, r->planeNY, r->planeNZ, r->planeDist); break;
    }
    if (newModel.meshCount == 0) return;

    // The rebuilt geometry already encodes the requested world size; a leftover
    // r->scale would apply it a second time. The plane ignores `size` (its shape
    // is normal + distance), so it keeps whatever scale it had.
    if (r->typeId != 4) r->scale = 1.0f;
    if (r->typeId == 1) {
        r->cylRTop = newCylRTop;
        r->cylRBot = newCylRBot;
        r->cylSlices = newCylSlices;
    }

    // Re-apply tileset texture (only if no custom texture overrides it)
    if (oldCustomTex.id > 0) {
        newModel.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = oldCustomTex;
        if (GetLitFogShader().id > 0)
            newModel.materials[0].shader = GetLitFogShader();
    } else if (oldTexSlot > 0) {
        ApplyTexSlotToModel(newModel, oldTexSlot);
    }

    // Replace old model. The fresh mesh's texcoords are PRISTINE (they carry
    // only the generator's baked tiling), so drop the stale snapshot and let
    // ApplyRenderableUV - the single owner of uvBase - re-take it and re-apply
    // the brush-wide transform. Applying the old transform inline here and then
    // snapshotting would make the snapshot capture ALREADY-transformed UVs and
    // every later edit would compound on top of it.
    UnloadModel(r->model);
    r->model = newModel;
    r->uvBase.clear();
    // Face sub-meshes were built from the previous vertex streams and must go.
    for (int f = 0; f < oz::surface::FACE_COUNT; f++) {
        if (r->faceMesh[f].vaoId > 0) UnloadMesh(r->faceMesh[f]);
        r->faceMesh[f] = Mesh{0};
    }
    r->faceMeshesBuilt = false;

    if (r->model.meshCount > 0 && r->model.meshes[0].texcoords) {
        ApplyRenderableUV(idx, oldSu, oldSv, oldOu, oldOv);
    } else if (r->surface.NeedsPerFaceDraw()) {
        BuildFaceMeshes(*r);
    }

    // Update transform
    r->position = pos;
    r->rotation = rot * DEG2RAD; // caller passes degrees; store radians
}

void OzoneLoader::SetWorldAmbient(float r, float g, float b, float a) {
    m_worldAmbient[0] = r;
    m_worldAmbient[1] = g;
    m_worldAmbient[2] = b;
    m_worldAmbient[3] = a;
}

void OzoneLoader::SetWorldFog(const float color[3], float start, float end,
                              float density, float intensity) {
    if (color) {
        m_worldFogColor[0] = color[0];
        m_worldFogColor[1] = color[1];
        m_worldFogColor[2] = color[2];
    }
    m_worldFogStart = start;
    m_worldFogEnd = end;
    m_worldFogDensity = density;
    m_worldFogIntensity = intensity;
}

void OzoneLoader::GetWorldFog(float colorOut[3], float& start, float& end,
                              float& density, float& intensity) const {
    if (colorOut) {
        colorOut[0] = m_worldFogColor[0];
        colorOut[1] = m_worldFogColor[1];
        colorOut[2] = m_worldFogColor[2];
    }
    start = m_worldFogStart;
    end = m_worldFogEnd;
    density = m_worldFogDensity;
    intensity = m_worldFogIntensity;
}

// ---------------------------------------------------------------------------
// DrawZoneGeometry - draw renderables with SURF_FAKEBACKDROP flag set
// (with optional bounds filter for backward compat)
//
// SURF_FAKEBACKDROP brushes are 2D painted backdrops, the Ocarina of Time trick:
// they stand in for distant scenery and must be seen at their painted value.
// Through the lit shader they do not get that:
//   litColor = baseColor * (colDiffuse * lightAccum)
//            + baseColor * (ambient / 10) * colDiffuse
// so a panel facing away from the directional sun has colDiffuse ~ 0 and
// collapses to black, which is what turned the fortress perimeter into a void.
//
// THIS FUNCTION USED TO MUTATE THE SHARED LitFog UNIFORMS to force that value
// (set colDiffuse to white and ambient to 5.5, then "restore" it afterwards).
// Three things were wrong with that:
//   * the "restore" put back a hardcoded constant, so every world surface drawn
//     after the sky pass rendered at ambient/10 == 0.1 instead of the world's
//     authored ambient;
//   * the shader carried no idea WHY its uniforms were wrong, so the override
//     leaked into anything else that read them;
//   * a PER-FACE backdrop could not be honoured at all, because the filter read
//     OzoneRenderable::surfaceFlags, which is a derived mirror of surface.def
//     only.
//
// Surface.fs has a full-bright branch for SURF_FAKEBACKDROP whose override lives
// in that draw's own uniforms, so this pass only has to choose a gain and route
// through DrawSurface. See SurfaceMaterial::SetFullBright.
// ---------------------------------------------------------------------------
void OzoneLoader::DrawZoneGeometry(Camera3D& camera, const BoundingBox& zoneBounds) {
    using namespace oz::surface;
    auto& sm = oz::SurfaceMaterial::Instance();

// 0.55 reproduces the gain the old ambient/10 hack produced (5.5 / 10). A full
    // value of 1.0 renders the painted panels at 100% and blows pale stone out to
    // flat white, so the 0.55 is a deliberate tone curve, not an accident. See
    // oz::surface::kBackdropGain for the full reasoning.
    //
    // The old path also added `baseColor * colDiffuse * lightAccum` on top of that
    // gain, so a backdrop in direct sunlight used to brighten with the sun. It no
    // longer does, on purpose: a painting whose panels brighten independently
    // according to the level's one directional light is not a painting. See the
    // SF_FAKEBACKDROP branch in Surface.fs.
    const float kBackdropPassGain = oz::surface::kBackdropGain;

    bool any = false;
    for (auto& r : m_renderables) {
        if (!r.loaded) continue;
        if (r.surfaceFlags & SURF_COLLISION_PROXY) continue;
        if (r.typeId == (int)OzonePrimitiveType::HEIGHTMAP) continue;
        for (int f = 0; f < FACE_COUNT; f++) {
            const SurfaceProps& p = r.surface.Resolve((SurfaceFace)f);
            if (p.Has(SURF_FAKEBACKDROP) && !p.Has(SURF_INVISIBLE)) { any = true; break; }
        }
        if (any) break;
    }
if (!any) return;

    // No surface shader: DrawSurface's fallback would DrawModel the whole brush,
    // which for this pass is actually the right thing - a painted backdrop still
    // has to be visible. Reproduces the pre-surface-path behaviour exactly.
    if (!sm.Ready()) {
        for (auto& r : m_renderables) {
            if (!r.loaded) continue;
            if (r.surfaceFlags & SURF_COLLISION_PROXY) continue;
            // Resolve PER FACE. This branch used to read the brush-wide
            // `surfaceFlags` mirror, which derives from surface.def only -- so a
            // per-face backdrop was invisible to it. That mirror read is the
            // exact brush-wide filter the SurfacePass refactor removed; do not
            // reintroduce it. Without a ready SurfaceMaterial there is no
            // per-face draw available, so the whole brush is drawn when ANY face
            // is a backdrop.
            bool backdrop = false;
            for (int f = 0; f < FACE_COUNT; f++) {
                const SurfaceProps& p = r.surface.Resolve((SurfaceFace)f);
                if (p.Has(SURF_FAKEBACKDROP) && !p.Has(SURF_INVISIBLE)) { backdrop = true; break; }
            }
            if (!backdrop) continue;
            // Same transform as the surface path: plain DrawModel has no yaw, so
            // a rotated backdrop snapped axis-aligned without the shader.
            DrawModelEx(r.model, r.position, {0.0f, 1.0f, 0.0f}, r.rotation * RAD2DEG,
                        {r.scale, r.scale, r.scale}, WHITE);
        }
        return;
    }

    sm.SetFullBright(kBackdropPassGain);

    for (auto& r : m_renderables) {
        if (!r.loaded) continue;
        if (r.surfaceFlags & SURF_COLLISION_PROXY) continue;
        if (r.typeId == (int)OzonePrimitiveType::ENTITY_PLAYERSTART ||
            r.typeId == (int)OzonePrimitiveType::ENTITY_PICKUP    ||
            r.typeId == (int)OzonePrimitiveType::ENTITY_ZONE      ||
            r.typeId == (int)OzonePrimitiveType::ENTITY_NPC       ||
            r.typeId == (int)OzonePrimitiveType::ENTITY_LIGHT     ||
            r.typeId == (int)OzonePrimitiveType::HEIGHTMAP)
            continue;

        // Optional bounds filter: skip if the brush AABB doesn't overlap the zone.
        BoundingBox mb = GetMeshBoundingBox(r.model.meshes[0]);
        BoundingBox worldBounds;
        worldBounds.min = {r.position.x + mb.min.x * r.scale,
                           r.position.y + mb.min.y * r.scale,
                           r.position.z + mb.min.z * r.scale};
        worldBounds.max = {r.position.x + mb.max.x * r.scale,
                           r.position.y + mb.max.y * r.scale,
                           r.position.z + mb.max.z * r.scale};
        if (!CheckCollisionBoxes(worldBounds, zoneBounds)) continue;

        bool drawable = false;
        for (int f = 0; f < FACE_COUNT; f++) {
            const SurfaceProps& p = r.surface.Resolve((SurfaceFace)f);
            if (p.Has(SURF_FAKEBACKDROP) && !p.Has(SURF_INVISIBLE)) { drawable = true; break; }
        }
        if (!drawable) continue;

        DrawSurface(r, SurfacePass::BackdropOnly);
    }

    // Back to 1.0 for whichever pass runs next (the world pass). Restored here
    // rather than only on the early-out above so a level that loses its backdrops
    // cannot leave every following frame's world pass dimmed.
    sm.SetFullBright(oz::surface::kFullBrightGain);
}


// ---------------------------------------------------------------------------
// DrawZoneGeometry - draw all SURF_FAKEBACKDROP brushes (no bounds filter)
// ---------------------------------------------------------------------------
// Delegates to the bounds-filtered overload with a box that always contains the
// backdrop, so the unlit-painted-backdrop handling lives in exactly one place.
// (Two separate loops previously meant the fix had to be duplicated, and this
// overload - the one the renderer actually calls - was missed.)
void OzoneLoader::DrawZoneGeometry(Camera3D& camera) {
    const BoundingBox all = { {-1e9f, -1e9f, -1e9f}, {1e9f, 1e9f, 1e9f} };
    DrawZoneGeometry(camera, all);
}

// ---------------------------------------------------------------------------
// Unload
// ---------------------------------------------------------------------------
void OzoneLoader::Unload() {
    for (auto& r : m_renderables) {
        if (r.customTex.id > 0) UnloadTexture(r.customTex);
        for (int f = 0; f < oz::surface::FACE_COUNT; f++) {
            if (r.faceMesh[f].vaoId > 0) UnloadMesh(r.faceMesh[f]);
            r.faceMesh[f] = Mesh{0};
        }
        r.faceMeshesBuilt = false;
        // Heightmap model is owned by m_hmModel (unloaded in UnloadHeightmap)
        if (r.loaded && r.typeId != (int)OzonePrimitiveType::HEIGHTMAP)
            UnloadModel(r.model);
    }
    m_renderables.clear();
    m_collisionVolumes.clear();
    m_entities.Clear();
    m_zoneCounters.clear();
    m_worldDir.clear();
    m_lastCsgOverflow = 0;
    // Face textures are referenced by path from surface.def, which the
    // renderables just dropped, so nothing can still be using them.
    for (auto& kv : m_faceTexCache)
        if (kv.second.id > 0) UnloadTexture(kv.second);
    m_faceTexCache.clear();
    // The portal shader is a real GL program; release it with the face meshes.
    // `m_skyPortalShaderTried` must reset too, or a later world in the same
    // process would never retry the load after this unload.
    if (m_skyPortalShader.id > 0) {
        UnloadShader(m_skyPortalShader);
        m_skyPortalShader = Shader{0};
    }
    m_skyPortalViewportLoc = -1;
    m_skyPortalShaderTried = false;
    UnloadHeightmap();
    UnloadTextures();
}

// ---------------------------------------------------------------------------
// Get
// ---------------------------------------------------------------------------
OzoneRenderable* OzoneLoader::Get(int index) {
    if (index < 0 || index >= (int)m_renderables.size()) return nullptr;
    return &m_renderables[index];
}
