#pragma once
// ---------------------------------------------------------------------------
// Audio/SoundManager.hpp — single entry point for all game audio
//
// Every one-shot effect, the world/home music streams, gameplay sound-zone
// crossfades, the reverb DSP hookup and LightningScript `play_sound` playback
// funnel through this singleton. Call sites no longer poke at raylib sound
// handles directly, which means:
//
//   * package-aware loading (LoadSoundWithFallback / LoadMusicWithFallback) is
//     applied uniformly — assets inside .ozsnd / .ozmux packages now resolve in
//     every path, not just the hand-picked ones,
//   * per-world audio state is reset in one place (ResetWorldAudio) instead of
//     five ad-hoc globals in Core.hpp,
//   * unloading is balanced, so no sound leaks across a world load.
//
// Asset resolution and the reverb DSP itself stay in
// Package/PackageAssetLoader.hpp and Audio/DspReverb.hpp; this class only owns
// lifecycle and policy.
// ---------------------------------------------------------------------------

#include "raylib.h"
#include "../World/ZoneManager.hpp"

#include <string>
#include <unordered_map>

// Named, long-lived sound slots that used to live in Data.hpp as GameSounds.
struct GameSounds {
    Sound CollisionSound;
    Sound WalkingSound;
    Music BackgroundMusic;
    Sound UIClick;
    Sound ChasingSound;
    Sound Death;
    Sound JumpSound;
    Sound WeaponLoadSound;
    Sound MenuSelectSound;
    Sound MatchStartSound;

    bool MusicFound = false;
};

class SoundManager {
public:
    static SoundManager& Instance();

    // --- lifecycle ---------------------------------------------------------

    // Wire the reverb DSP into raylib's master output mix. Call once, right
    // after InitAudioDevice() succeeded.
    void AttachReverbProcessor();

    // Load the shared UI / gameplay one-shots plus the typewriter noise.
    // Called once during engine init, after the audio device is up.
    void LoadCoreSounds();

    // Stop and release everything (world teardown, leaving to menu, shutdown).
    void Shutdown();

    // Drop all per-world audio state: world music, zone ambience, the
    // gameplay sound-zone profile and the reverb hook. Called by LoadWorld.
    void ResetWorldAudio();

    // --- one-shot effects --------------------------------------------------

    void PlayUIClick();
    void PlayChasing();
    void PlayDeath();
    void PlayCollision();
    void PlayTextNoise();
    void StopTextNoise();
    void PlayJump();
    void PlayWeaponLoad();
    void PlayMenuSelect();
    void PlayMatchStart();
    // NPC aggro stinger; ignores empty handles.
    void PlayScream(const Sound& scream);

    // Resolve a pawn's aggro sound: the def's explicit scream_path first, then
    // the conventional GameData/Pawn/Pawns/<name>.wav / .mp3 fallbacks.
    // Returns an empty Sound when nothing resolves.
    static Sound LoadPawnScream(const std::string& defName,
                                const std::string& screamPath);

    // Footsteps are a loop that is retriggered while the player walks.
    void StartWalkLoop();
    void StopWalkLoop();

    // --- per-category volume -------------------------------------------------
    // raylib has only a global master volume; music and SFX need to be tunable
    // independently. These are 0..1 gains applied to every handle this manager
    // plays (music streams, one-shots, loops, script/stat sounds). Master stays
    // raylib's SetMasterVolume.
    //
    // Named *Gain rather than SetMusicVolume/SfxVolume so the member does not
    // hide raylib's global SetMusicVolume(Music, float) inside this class.
    void SetMusicGain(float v);
    void SetSfxGain(float v);
    float MusicGain() const { return m_musicGain; }
    float SfxGain() const { return m_sfxGain; }

    // --- music -------------------------------------------------------------

    // Generic stream helpers for callers that own their own Music handle (the
    // title screen owns OmegaTechData.HomeScreenMusic, which lives in Core.hpp
    // and therefore cannot be referenced from here).
    void PlayStream(Music music);
    void StopStream(Music music);
    void UpdateStream(Music music);

    // Resolve and start the world's own track: <prefix>Music/Main.mp3, falling
    // back to the shared global ambience. Package-aware.
    void PlayWorldMusic(const std::string& assetPrefix);
    void StopWorldMusic();
    // Per-frame pump — streams must be updated every frame to keep playing.
    void Update();
    bool HasMusic() const { return m_sounds.MusicFound; }
    Music& WorldMusic() { return m_sounds.BackgroundMusic; }

    // --- gameplay sound zones ---------------------------------------------

    // Drive zone music/ambience/enter-sfx from the player's current region.
    // Call once per frame after ZoneManager::UpdatePlayerRegion().
    void UpdateSoundZones(const PointRegion& region);

    // --- reverb ------------------------------------------------------------

    // Apply (or clear) the simulated reverb for the player's current region.
    void UpdateReverb(const PointRegion& region);

    // --- LightningScript `play_sound` --------------------------------------

    // Load (with caching) and play a script-authored sound by path. Returns
    // false when the path resolves to nothing.
    bool PlayScriptSound(const std::string& path);
    // Age the script sound cache; unloads handles idle for a few seconds.
    void PruneScriptSoundCache(float dt);

    // --- data-driven sound stats (.ozls) ---------------------------------

    // Play a one-shot authored as a path in a .ozls `stats` block
    // (fire_sound, reload_sound, jump_sound, hurt_sound, ...). Shares the
    // script-sound cache, so it is package-aware: a path inside a .ozsnd
    // resolves exactly like a `play_sound` opcode.
    //
    // volume/pitch default to 1.0 (unity). They are applied to the handle
    // immediately before playing rather than baked in at load time, so two
    // weapons sharing one .wav can play it at different volumes.
    //
    // This is the only entry point new stat-driven sound should use; do not
    // add per-stat Play* methods for one-shots. The preloaded GameSounds
    // handles stay reserved for UI/menu audio that has no .ozls owner.
    bool PlayStatSound(const std::string& path, float volume = 1.0f, float pitch = 1.0f);

    // Looping player addressed by key, so unrelated loops cannot stop each
    // other. The weapon view-model and the player footstep loop both need
    // this: StartWalkLoop/StopWalkLoop used to fight over a single global
    // handle, so switching weapons could silence the footsteps.
    //
    // Starting a key that is already playing is a no-op unless the path
    // differs, in which case the old handle is released and the new one
    // starts (that is what makes walk/run interchangeable). Each loop is
    // re-triggered from Update() while it is alive, because raylib stops a
    // sound once its stream drains.
    void StartLoopSound(const std::string& key, const std::string& path,
                        float volume = 1.0f, float pitch = 1.0f);
    void StopLoopSound(const std::string& key);
    // True while `key` is looping. Used by the footstep layer to keep the
    // walk/run pair from retriggering each other every frame.
    bool IsLoopPlaying(const std::string& key) const;

    // --- direct handle access ---------------------------------------------
    // For the rare caller that needs the raw handle (editor previews, mixer
    // tweaks). Prefer the methods above.
    GameSounds& Sounds() { return m_sounds; }
    const GameSounds& Sounds() const { return m_sounds; }

private:
    SoundManager() = default;

    // Apply the current music gain (times the reverb duck) to the live world
    // track. Called whenever the track changes or the gain/duck changes.
    void ApplyMusicGain();
    // Set an SFX handle's volume to authored * sfx gain, then play it.
    void PlaySfxAt(Sound s, float authored = 1.0f);

    // Resolve + play the zone enter music, remembering the world track so it
    // can be restored on exit.
    void EnterSoundZone(const ZoneVolumeNode& zone);
    // Stop zone ambience and restore the world track.
    void ExitSoundZone();
    void StopAmbience();

    GameSounds m_sounds;
    Sound m_textNoise{0};

    // Per-category gains (0..1) and the music duck applied inside a reverb zone.
    float m_musicGain = 1.0f;
    float m_sfxGain = 1.0f;
    float m_musicDuck = 1.0f;

    // Per-world zone audio state
    Music m_defaultWorldMusic{0};   // world track captured before a zone crossfade
    Sound m_ambienceHandle{0};      // looped ambience for the current sound zone
    std::string m_ambienceZoneName;
    std::string m_prevSoundZone;
    bool m_wasInReverb = false;

    // LightningScript sound cache: path -> {handle, seconds since last use}
    struct ScriptSound {
        Sound sound;
        float idle = 0.0f;
    };
    std::unordered_map<std::string, ScriptSound> m_scriptSounds;

    // Data-driven loop slots (walk/run footsteps, weapon view-model loops).
    // Keyed rather than singular so one loop cannot stop another.
    struct LoopSound {
        Sound sound;
        std::string path;
        float volume = 1.0f;
        float pitch = 1.0f;
    };
    std::unordered_map<std::string, LoopSound> m_loops;

    void PruneLoops();

    // Cached handle for a one-shot played without keeping it around (zone
    // enter stinger). Freed by PruneOneShot.
    Sound m_oneShot{0};
    float m_oneShotIdle = 0.0f;
    void PruneOneShot(float dt);
};