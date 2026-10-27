#pragma once
// ---------------------------------------------------------------------------
// World/ZoneTypes.hpp — runtime zone taxonomy + environment override data
//
// Extracted from Source/Pawn/OzPawnSystem.hpp so that world/level code (the
// OZONE parser/loader, the zone query facade in ZoneManager.hpp and the audio
// subsystem) can reason about zone kinds and their environment overrides
// without pulling in the whole pawn/entity system.
//
// Nothing in this header depends on raylib or on gameplay code: it is plain
// data so it can be included from tools and tests.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string>

// Zone volume types
enum class ZoneType : uint8_t {
    ZONE_WATER = 0,
    ZONE_LADDER = 1,
    ZONE_SKY = 2,
    ZONE_REVERB = 3,
    ZONE_GAMEPLAY_SOUND = 4,
    ZONE_PORTAL = 5,
    ZONE_GRAVITY = 6
};

// Zone environment overrides — fog/ambient/reverb applied on zone entry at runtime.
// Separate from the editor's ZoneProperties (which has GameType/Particle fields too).
struct ZoneEnvOverrides {
    // Fog (defaults match legacy hardcoded fallback in Core.hpp)
    int fogR = 179, fogG = 179, fogB = 204;
    float fogDensity = 1.0f;
    float fogStart = 10.0f, fogEnd = 100.0f;
    bool applyFog = false;
    // Ambient
    int ambR = 180, ambG = 180, ambB = 200;
    float ambIntensity = 0.4f;
    bool applyAmbient = false;
    // Reverb
    float reverbMix = 0.0f;
    float reverbDecay = 0.0f;

    // Merge `other` over this. `other` wins for every field it explicitly
    // enables, so callers can layer zones in priority order.
    void Merge(const ZoneEnvOverrides& other) {
        if (other.applyFog) {
            applyFog = true;
            fogR = other.fogR; fogG = other.fogG; fogB = other.fogB;
            fogDensity = other.fogDensity;
            fogStart = other.fogStart; fogEnd = other.fogEnd;
        }
        if (other.applyAmbient) {
            applyAmbient = true;
            ambR = other.ambR; ambG = other.ambG; ambB = other.ambB;
            ambIntensity = other.ambIntensity;
        }
        // Reverb is not a "last writer wins" field: reverb zones are additive
        // and the strongest/highest-priority one should win, so only replace it
        // when the incoming zone actually asks for reverb.
        if (other.reverbMix > 0.0f) {
            reverbMix = other.reverbMix;
            reverbDecay = other.reverbDecay;
        }
    }
};

// Sound profile - maps game types to music/sound actions.
// Consumed at runtime by SoundManager when the player enters a
// ZONE_GAMEPLAY_SOUND volume.
struct GameplaySoundProfile {
    std::string music_on_enter;   // Music to crossfade to on zone enter
    std::string music_on_exit;    // Music to restore on zone exit
    std::string sfx_on_enter;     // One-shot sound played on enter
    std::string sfx_on_combat;    // Combat stinger (DM/TDM modes)
    std::string ambience_loop;    // Ambience loop while inside zone
    float volume_mult = 1.0f;     // Volume multiplier for this zone

    bool HasAmbience() const { return !ambience_loop.empty(); }
    bool HasEnterMusic() const { return !music_on_enter.empty(); }
    bool HasEnterSfx() const { return !sfx_on_enter.empty(); }
};

// Parse an OZONE zone subtype token ("water"/"ladder"/"sky"/"reverb"/"sound")
// into a ZoneType. Unknown tokens fall back to ZONE_WATER (the historical
// default). Kept here so both the loader and the editor agree.
inline ZoneType ZoneTypeFromString(std::string name) {
    for (char& c : name) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    if (name == "ladder") return ZoneType::ZONE_LADDER;
    if (name == "sky") return ZoneType::ZONE_SKY;
    if (name == "reverb") return ZoneType::ZONE_REVERB;
    // Editor writes "sound" for gameplay-sound zones; accept both spellings so
    // exported worlds round-trip back to ZONE_GAMEPLAY_SOUND.
    if (name == "gameplay_sound" || name == "sound")
        return ZoneType::ZONE_GAMEPLAY_SOUND;
    if (name == "portal") return ZoneType::ZONE_PORTAL;
    if (name == "gravity") return ZoneType::ZONE_GRAVITY;
    return ZoneType::ZONE_WATER;   // default
}

inline const char* ZoneTypeName(ZoneType t) {
    switch (t) {
        case ZoneType::ZONE_WATER:            return "water";
        case ZoneType::ZONE_LADDER:           return "ladder";
        case ZoneType::ZONE_SKY:              return "sky";
        case ZoneType::ZONE_REVERB:           return "reverb";
        case ZoneType::ZONE_GAMEPLAY_SOUND:   return "gameplay_sound";
        case ZoneType::ZONE_PORTAL:           return "portal";
        case ZoneType::ZONE_GRAVITY:          return "gravity";
    }
    return "water";
}