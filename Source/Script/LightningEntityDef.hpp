#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

// LightningScript entity type system — replaces hardcoded Objects.hpp
// Entity types
enum class EntityType : uint8_t {
    WEAPON,
    ARMOR,
    CONSUMABLE,
    UPGRADE,
    PICKUP,
    PROJECTILE,
    SKYZONE,
    PAWN,
    // GameEngine.Mesh taxonomy + world effect nodes
    MESH_STATIC,
    MESH_SKELETAL,
    PARTICLE_EMITTER,
    WIND_ZONE,
    SKILL,
    // Declarative UI/HUD layer (data + hooks); drawn by C++ (AngelPlayer).
    GAMEUI,
    // Light defaults layer. A `light ... name=torch` OZONE line resolves the
    // def by that name (exactly as a zone's name= resolves its skyzone def) and
    // takes any value the line itself did not author. intensity/radius/color
    // stay line-owned so a def can never silently retune a saved level.
    LIGHT,
    // Metadata-only ruleset def (GameTypeInfo override). Not placeable.
    GAMETYPE,
    // An active cast: costs mana OR psychic energy (per-def), and resolves
    // either to an instant effect at the target or to spawned projectiles.
    // Distinct from SKILL (a passive stat node) and from WEAPON (which is
    // gated on ammo/stamina and drives the view-model).
    SPELL,
    // --- Game-hierarchy vocabulary (SCAFFOLD, Phase 0) ---
    // Metadata-only defs that describe the Angels95 World/Game hierarchy. They
    // parse and register but have NO runtime consumer yet; Phase 1/2 wires them
    // to GameManager/ObjectiveSystem/TriggerSystem. See Wiki/Game-Hierarchy.md.
    // Kept here (not a separate parser) so the game layer reuses one def system
    // instead of inventing a second file format.
    GAME,
    CAMPAIGN,
    SECTOR,
    MISSION,
    OBJECTIVE,
    TRIGGER,
    EVENT,
    UNKNOWN
};

inline const char* EntityTypeName(EntityType t) {
    switch (t) {
        case EntityType::WEAPON:          return "weapon";
        case EntityType::ARMOR:           return "armor";
        case EntityType::CONSUMABLE:      return "consumable";
        case EntityType::UPGRADE:         return "upgrade";
        case EntityType::PICKUP:          return "pickup";
        case EntityType::PROJECTILE:      return "projectile";
        case EntityType::SKYZONE:         return "skyzone";
        case EntityType::PAWN:            return "pawn";
        case EntityType::MESH_STATIC:     return "Mesh.Static";
        case EntityType::MESH_SKELETAL:   return "Mesh.Skeletal";
        case EntityType::PARTICLE_EMITTER:return "ParticleEmitter";
        case EntityType::WIND_ZONE:       return "WindZone";
        case EntityType::SKILL:           return "skill";
        case EntityType::GAMEUI:          return "GameUI";
        case EntityType::LIGHT:          return "light";
        case EntityType::GAMETYPE:       return "gametype";
        case EntityType::SPELL:          return "spell";
        case EntityType::GAME:           return "game";
        case EntityType::CAMPAIGN:       return "campaign";
        case EntityType::SECTOR:         return "sector";
        case EntityType::MISSION:        return "mission";
        case EntityType::OBJECTIVE:      return "objective";
        case EntityType::TRIGGER:        return "trigger";
        case EntityType::EVENT:          return "event";
        default:                          return "unknown";
    }
}

inline EntityType EntityTypeFromName(const std::string& n) {
    if (n == "weapon")        return EntityType::WEAPON;
    if (n == "armor")         return EntityType::ARMOR;
    if (n == "consumable")    return EntityType::CONSUMABLE;
    if (n == "upgrade")       return EntityType::UPGRADE;
    if (n == "pickup")        return EntityType::PICKUP;
    if (n == "projectile")    return EntityType::PROJECTILE;
    if (n == "skyzone")       return EntityType::SKYZONE;
    if (n == "pawn")          return EntityType::PAWN;
    if (n == "Mesh.Static")   return EntityType::MESH_STATIC;
    if (n == "Mesh.Skeletal") return EntityType::MESH_SKELETAL;
    if (n == "ParticleEmitter") return EntityType::PARTICLE_EMITTER;
    if (n == "WindZone")      return EntityType::WIND_ZONE;
    if (n == "skill")         return EntityType::SKILL;
    if (n == "GameUI")        return EntityType::GAMEUI;
    if (n == "light")          return EntityType::LIGHT;
    if (n == "gametype")       return EntityType::GAMETYPE;
    if (n == "spell")          return EntityType::SPELL;
    if (n == "game")           return EntityType::GAME;
    if (n == "campaign")       return EntityType::CAMPAIGN;
    if (n == "sector")         return EntityType::SECTOR;
    if (n == "mission")        return EntityType::MISSION;
    if (n == "objective")      return EntityType::OBJECTIVE;
    if (n == "trigger")        return EntityType::TRIGGER;
    if (n == "event")          return EntityType::EVENT;
    return EntityType::UNKNOWN;
}

// One stat (float) or string value parsed from .ozls
struct EntityStatBlock {
    std::unordered_map<std::string, float> floats;
    std::unordered_map<std::string, std::string> strings;
    std::unordered_map<std::string, float[3]> vec3s;  // e.g. fog_color
};

// Action block — a named list of script lines
struct EntityAction {
    std::string name;           // "on_fire", "on_enter", etc.
    std::vector<std::string> scriptLines;
};

// Level variant — mesh/texture override per level
struct EntityVariant {
    std::string name;
    std::string meshOverride;
    std::string textureOverride;
};

// Full entity definition parsed from .ozls
struct EntityDef {
    std::string name;
    EntityType type = EntityType::UNKNOWN;
    std::string mesh;
    std::string texture;
    std::string icon;
    EntityStatBlock stats;
    std::vector<EntityAction> actions;
    std::vector<EntityVariant> variants;
    // Skyzone-specific fields (stored in stats.strings/vec3s for simplicity)
    std::string skybox;
    std::string music;
    std::string sourcePath;  // where this was parsed from

    // Mesh taxonomy / animation (first-class body keys, GameEngine.Mesh.*)
    std::string meshType;    // "" | "static" | "skeletal"
    std::string animIdle;
    std::string animPatrol;
    std::string animChase;
    std::string animReturn;
    std::string animDeath;
    float animSpeed = 1.0f;
    // Movement speed multiplier authored per entity (`movement_speed = 1.4`).
    // Applied to the player's base walk speed; reserved for NPC/remote use too.
    float movementSpeed = 1.0f;

    // Player stat defaults (used when spawning "Player" entity)
    float defaultHealth = 100.0f;
    float defaultMaxHealth = 100.0f;
    float defaultMana = 0.0f;
    float defaultMaxMana = 100.0f;
    float defaultPsychicEnergy = 0.0f;
    float defaultMaxPsychicEnergy = 100.0f;
    int defaultLevel = 1;
    int defaultXP = 0;
    int defaultXPToNext = 100;
};
