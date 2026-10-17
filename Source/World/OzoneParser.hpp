#ifndef OMEGA_OZONE_PARSER_HPP
#define OMEGA_OZONE_PARSER_HPP

#include <string>
#include <vector>
#include <utility>
#include <sstream>
#include <fstream>
#include <cstdint>
#include "../Physics/PhysicsInfo.hpp"
#include "SurfaceFlags.hpp"

// Standalone .ozone parser - no raylib dependency.
// Parses the OZONE text format used by the OzWorld editor.

enum class OzonePrimitiveType : uint8_t {
    BOX,
    CYLINDER,
    SPHERE,
    PYRAMID,
    PLANE,
    ENTITY_PLAYERSTART,  // Player spawn point
    ENTITY_PICKUP,       // Pickup node (item)
    ENTITY_ZONE,         // Zone volume
    ENTITY_NPC,          // NPC spawn node
    ENTITY_LIGHT,        // Light node (point/spot/directional)
    ENTITY_PORTAL,       // Portal zone (level-to-level connection)
    ENTITY_LEVELINFO,    // Level metadata (game rules, skybox)
    ENTITY_PARTICLES,    // Ambient particle weather settings
    ENTITY_EMITTER,      // Sound/music emitter marker (emitter sound|music x y z)
    ENTITY_MESH_STATIC,  // GameEngine.Mesh.Static   (placed static prop/object)
    ENTITY_MESH_SKELETAL,// GameEngine.Mesh.Skeletal (placed animated object)
    ENTITY_PARTICLE_EMITTER, // GameEngine.ParticleEmitter (local 3D particles)
    ENTITY_PATH_NODE,    // GameEngine.PathNode (NPC waypoint)
    ENTITY_WIND_ZONE,    // WindZone (foliage wind region)
    ENTITY_TRIGGER,      // GameEngine.Trigger (volume -> EventBus trigger.enter/exit)
    SKYBOX,             // skybox (projected inner faces, render-only)
    HEIGHTMAP,           // Terrain heightmap (grayscale image)
    UNKNOWN
};

struct OzonePrimitive {
    OzonePrimitiveType type;
    // 1-based source line. Carried so diagnostics can name the line an author has
    // to fix; the parser used to drop bad lines and tokens with no locator at all.
    int line = 0;
    std::vector<float> args;        // position, dimensions, etc.
    std::string entityType;         // for entity types: "Walker", "HealthVial", etc.
    std::string entitySubType;      // for zones: "Water", "Ladder", "Sky", "Reverb"
    std::string name;               // optional explicit name (zone name= kwarg for script hooks)
    int csgOp = 0;                  // CSG operation: 0=SOLID, 1=ADD, 2=SUB, 3=INTERSECT, 4=DE_RESC
    int surfaceFlags = 0;           // surface behavior flags (e.g. SURF_FAKEBACKDROP)
    std::string texPath;            // custom diffuse texture path (texPath="..." attribute)
    float texScaleU = 1.0f;         // texture tiling U (applied to mesh UVs)
    float texScaleV = 1.0f;         // texture tiling V (applied to mesh UVs)
    float texOffsetU = 0.0f;        // texture shift U
    float texOffsetV = 0.0f;        // texture shift V
    std::string meshPath;           // GameEngine.Mesh.*: model path (mesh=Skeletal/default)
    std::string animClip;           // GameEngine.Mesh.Skeletal: embedded clip name
    std::string animFile;           // GameEngine.Mesh.Skeletal: external .ozanim file
    float animSpeed = 1.0f;         // GameEngine.Mesh.Skeletal: playback speed
    bool pathLoop = false;          // GameEngine.PathNode: loop to first node
    bool meshWind = false;          // GameEngine.Mesh.*: foliage wind-affection flag
    bool hasPhysics = false;        // zones: PhysicsInfo kwargs were authored
    oz::physics::PhysicsInfo physics; // zones: overrides (defaults when !hasPhysics)
    // Light optional attributes. Authored as named kwargs (effect=/flare=/
    // corona=/name=) because the positional tail is ambiguous: an omitted
    // `effect` slot shifted flare/corona into it, so a point light with no
    // effect but a flare round-tripped as a WATERY effect. -1 = not authored,
    // so the loader can fall back to the legacy positional tail.
    int  lightEffect = -1;          // LitLightEffect index
    int  lightFlare  = -1;          // 0/1
    int  lightCorona = -1;          // 0/1
    // levelinfo: optional `gametype=<name>` kwarg in the tail. Overrides the
    // positional arg(0) when it resolves to a known mode; unknown names warn
    // and fall back to arg(0) rather than silently becoming 0.
    std::string gametypeKey;
    // GameEngine.ParticleEmitter: `key=value` overrides of the positional tail,
    // stored as {positional index, value}. Twenty-three bare positional floats
    // with no way to retune one of them means an author who wants different
    // gravity rewrites the whole line and silently transposes two colours; a
    // named kwarg wins over the slot it names. Positional args stay fully
    // supported and the shipped emitters use it unchanged.
    std::vector<std::pair<int, float>> particleOverrides;
    // Brush surface state: a brush-wide default plus up to six per-face
    // overrides, parsed from the `face<name>_<field>=` kwargs. See
    // World/SurfaceFlags.hpp for the bit allocation and the face/axis mapping.
    // Raylib-free, so the server's worldcheck and tests can inspect it.
    oz::surface::BrushSurface surface;
};

class OzoneParser {
public:
    static std::vector<OzonePrimitive> parse_file(const std::string& path);
    static std::vector<OzonePrimitive> parse_string(const std::string& content);
};

// Strip surrounding quote characters (U+0022) from a string if present.
// Shared by OzOzoneLoader and GameState so both strip levelinfo paths
// identically. External linkage (not static) so it links across TUs.
std::string StripQuotes(std::string s);

#endif // OMEGA_OZONE_PARSER_HPP
