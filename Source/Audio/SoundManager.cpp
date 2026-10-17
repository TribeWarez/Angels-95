#include "SoundManager.hpp"

#include "../Log.hpp"
#include "../Package/PackageAssetLoader.hpp"
#include "DspReverb.hpp"

#include <raymath.h>

// How long a one-shot handle stays resident after playing before it is freed.
static constexpr float kOneShotLifetime = 2.0f;
// How long a script sound cache entry stays idle before it is unloaded.
static constexpr float kScriptSoundIdleLimit = 5.0f;

SoundManager& SoundManager::Instance() {
    static SoundManager instance;
    return instance;
}

// ---------------------------------------------------------------------------
// Per-category volume
// ---------------------------------------------------------------------------
void SoundManager::SetMusicGain(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    m_musicGain = v;
    ApplyMusicGain();
}

void SoundManager::SetSfxGain(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    m_sfxGain = v;
}

void SoundManager::ApplyMusicGain() {
    if (!m_sounds.MusicFound) return;
    // raylib's global function; the class member is deliberately named *Gain so
    // this unqualified call still finds ::SetMusicVolume.
    ::SetMusicVolume(m_sounds.BackgroundMusic, m_musicGain * m_musicDuck);
}

void SoundManager::PlaySfxAt(Sound s, float authored) {
    if (s.frameCount == 0) return;
    float v = authored * m_sfxGain;
    if (v < 0.0f) v = 0.0f;
    SetSoundVolume(s, v);
    PlaySound(s);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void SoundManager::AttachReverbProcessor() {
    DspReverb::Reset();
    AttachAudioMixedProcessor(DspReverb::AudioCallback);
}

void SoundManager::LoadCoreSounds() {
    // Wrap every core-sound load so a missing/unreadable file is logged at
    // startup instead of surfacing only when a caller (PlayJump, PlayDeath, ...)
    // silently no-ops. raylib's LoadSound returns Sound{0} when the audio
    // converter rejects the file (malformed WAV header, unsupported codec, ...),
    // so without this wrapper those failures are invisible until a player fires
    // the one weapon whose fire_sound points at the bad file.
    auto loadCore = [](const char* path) -> Sound {
        Sound s = LoadSoundWithFallback(path);
        if (s.frameCount == 0)
            OZ_WARN("core sound: '%s' not found or unreadable", path);
        return s;
    };

    GameSounds& s = m_sounds;
    s.CollisionSound = loadCore("GameData/Audio/Sounds/Entities/CollisionSound.mp3");
    s.WalkingSound    = loadCore("GameData/Audio/Sounds/Entities/WalkingSound.mp3");
    s.ChasingSound    = loadCore("GameData/Audio/Sounds/Entities/ChasingSound.mp3");
    s.UIClick         = loadCore("GameData/Audio/Sounds/UI/MenuSelect_Click.wav");
    s.Death           = loadCore("GameData/Audio/Sounds/Entities/Hurt.mp3");
    s.JumpSound       = loadCore("GameData/Audio/Sounds/Entities/jump1.wav");
    s.WeaponLoadSound = loadCore("GameData/Audio/Sounds/Entities/weapload.wav");
    s.MenuSelectSound = loadCore("GameData/Audio/Sounds/UI/MenuSelect_Click.wav");
    s.MatchStartSound = loadCore("GameData/Audio/Sounds/UI/MenuRoundBeginn.wav");
    m_textNoise       = loadCore("GameData/Audio/Sounds/UI/TalkingNoise.mp3");
}

void SoundManager::Shutdown() {
    ResetWorldAudio();

    GameSounds& s = m_sounds;
    for (Sound* snd : {&s.CollisionSound, &s.WalkingSound, &s.UIClick,
                       &s.ChasingSound, &s.Death, &s.JumpSound, &s.WeaponLoadSound,
                       &s.MenuSelectSound, &s.MatchStartSound, &m_textNoise}) {
        if (snd->frameCount > 0) {
            StopSound(*snd);
            UnloadSound(*snd);
        }
        *snd = Sound{0};
    }
    DspReverb::SetMix(0.0f);
    DspReverb::SetDecay(0.5f);
}

void SoundManager::ResetWorldAudio() {
    StopAmbience();
    m_prevSoundZone.clear();
    m_wasInReverb = false;

    if (m_defaultWorldMusic.ctxData != nullptr)
        m_defaultWorldMusic = Music{0};

    StopWorldMusic();
    DspReverb::SetMix(0.0f);
    DspReverb::SetDecay(0.5f);

    for (auto& kv : m_scriptSounds) {
        if (kv.second.sound.frameCount > 0)
            UnloadSound(kv.second.sound);
    }
    m_scriptSounds.clear();

    // Keyed loops (footsteps) would otherwise keep playing from the world
    // being unloaded, and their handles would leak into the next one.
    for (auto& [key, loop] : m_loops) {
        if (loop.sound.frameCount > 0) {
            StopSound(loop.sound);
            UnloadSound(loop.sound);
        }
    }
    m_loops.clear();

    PruneOneShot(kOneShotLifetime);   // force-release the one-shot handle
}

// ---------------------------------------------------------------------------
// One-shot effects
// ---------------------------------------------------------------------------
void SoundManager::PlayUIClick() {
    PlaySfxAt(m_sounds.UIClick);
}

void SoundManager::PlayChasing() {
    if (m_sounds.ChasingSound.frameCount == 0) return;
    // Throttle so a pack of NPCs cannot stack the stinger on one frame.
    if (!IsSoundPlaying(m_sounds.ChasingSound))
        PlaySfxAt(m_sounds.ChasingSound);
}

void SoundManager::PlayDeath() {
    if (m_sounds.Death.frameCount > 0 && !IsSoundPlaying(m_sounds.Death))
        PlaySfxAt(m_sounds.Death);
}

void SoundManager::PlayCollision() {
    PlaySfxAt(m_sounds.CollisionSound);
}

void SoundManager::PlayTextNoise() {
    PlaySfxAt(m_textNoise);
}

void SoundManager::StopTextNoise() {
    if (m_textNoise.frameCount > 0)
        StopSound(m_textNoise);
}

void SoundManager::PlayJump() {
    PlaySfxAt(m_sounds.JumpSound);
}

void SoundManager::PlayWeaponLoad() {
    PlaySfxAt(m_sounds.WeaponLoadSound);
}

void SoundManager::PlayMenuSelect() {
    PlaySfxAt(m_sounds.MenuSelectSound);
}

void SoundManager::PlayMatchStart() {
    PlaySfxAt(m_sounds.MatchStartSound);
}

void SoundManager::PlayScream(const Sound& scream) {
    PlaySfxAt(scream);
}

Sound SoundManager::LoadPawnScream(const std::string& defName,
                                  const std::string& screamPath) {
    if (!screamPath.empty()) {
        Sound snd = LoadSoundWithFallback(screamPath.c_str());
        if (snd.frameCount > 0)
            return snd;
    }
    // Convention fallbacks shipped next to the pawn sprites.
    const std::string base = "GameData/Pawn/Pawns/" + defName;
    for (const char* ext : {".wav", ".mp3"}) {
        Sound snd = LoadSoundWithFallback((base + ext).c_str());
        if (snd.frameCount > 0)
            return snd;
    }
    OZ_WARN("Pawn '%s': no scream sound found (path='%s')", defName.c_str(), screamPath.c_str());
    return Sound{0};
}

void SoundManager::StartWalkLoop() {
    if (m_sounds.WalkingSound.frameCount == 0) return;
    if (!IsSoundPlaying(m_sounds.WalkingSound))
        PlaySfxAt(m_sounds.WalkingSound);
}

void SoundManager::StopWalkLoop() {
    if (m_sounds.WalkingSound.frameCount > 0 && IsSoundPlaying(m_sounds.WalkingSound))
        StopSound(m_sounds.WalkingSound);
}

// ---------------------------------------------------------------------------
// Music
// ---------------------------------------------------------------------------
void SoundManager::PlayStream(Music music) {
    if (music.ctxData != nullptr) {
        ::SetMusicVolume(music, m_musicGain);
        PlayMusicStream(music);
    }
}

void SoundManager::StopStream(Music music) {
    if (music.ctxData != nullptr)
        StopMusicStream(music);
}

void SoundManager::UpdateStream(Music music) {
    if (music.ctxData != nullptr)
        UpdateMusicStream(music);
}

// The world directory name out of an asset prefix: "GameData/Worlds/<Name>/" ->
// "<Name>". Empty if the prefix is not a plain world directory.
static std::string WorldDirNameFromPrefix(const std::string& assetPrefix) {
    std::string p = assetPrefix;
    for (auto& c : p) if (c == '\\') c = '/';
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    const size_t slash = p.find_last_of('/');
    if (slash == std::string::npos) return {};
    return p.substr(slash + 1);
}

void SoundManager::PlayWorldMusic(const std::string& assetPrefix) {
    StopWorldMusic();

    const std::string worldTrack = assetPrefix + "Music/Main.mp3";
    // Fallback so worlds without their own track still get atmosphere.
    static const char* kGlobalAmbience = "GameData/Audio/Ambience/Music_Atmo_1.wav";

    Music m{0};
    std::string playingFrom;

    // 1. The loose file, which is what a source checkout and the release zip both have.
    if (IsPathFile(worldTrack.c_str())) {
        m = LoadMusicStream(worldTrack.c_str());
        playingFrom = worldTrack;
    }

    // 2. The world package. SCOPED, and that is the whole point: build-data.ps1 packs
    //    each world directory as world_<Name>.ozone with the track keyed
    //    "Music/Main.mp3", so every world package holds an entry with that basename.
    //    The obvious fix - delete the IsPathFile gate and let LoadMusicWithFallback
    //    resolve it - resolves through FindBasename(), which walks every loaded package
    //    and returns whichever comes first. With two worlds shipping a track that plays
    //    Dust_Ravine's music in Dessert_Dreams, silently. Verified by packing both
    //    worlds: each contains "Music/Main.mp3".
    if (m.ctxData == nullptr) {
        const std::string world = WorldDirNameFromPrefix(assetPrefix);
        if (!world.empty()) {
            const std::string pkg = "world_" + world + ".ozone";
            m = LoadMusicFromPackage(pkg.c_str(), "Music/Main.mp3");
            if (m.ctxData != nullptr) playingFrom = pkg + ":Music/Main.mp3";
        }
    }

    // 3. The shared ambience. Its basename is unique, so the plain wrapper is safe here.
    if (m.ctxData == nullptr) {
        m = LoadMusicWithFallback(kGlobalAmbience);
        if (m.ctxData != nullptr) playingFrom = kGlobalAmbience;
    }

    if (m.ctxData == nullptr) {
        OZ_WARN("PlayWorldMusic: nothing playable - tried '%s', world_%s.ozone, and '%s'",
                worldTrack.c_str(), WorldDirNameFromPrefix(assetPrefix).c_str(), kGlobalAmbience);
        return;
    }

    m_sounds.BackgroundMusic = m;
    m_sounds.MusicFound = true;
    PlayMusicStream(m_sounds.BackgroundMusic);
    ApplyMusicGain();
    OZ_INFO("PlayWorldMusic: %s", playingFrom.c_str());
}

void SoundManager::StopWorldMusic() {
    if (!m_sounds.MusicFound) return;
    StopMusicStream(m_sounds.BackgroundMusic);
    UnloadMusicStream(m_sounds.BackgroundMusic);
    m_sounds.BackgroundMusic = Music{0};
    m_sounds.MusicFound = false;
}

void SoundManager::Update() {
    PruneScriptSoundCache(GetFrameTime());
    PruneLoops();
    PruneOneShot(GetFrameTime());
    if (m_sounds.MusicFound)
        UpdateMusicStream(m_sounds.BackgroundMusic);
}

// ---------------------------------------------------------------------------
// Gameplay sound zones
// ---------------------------------------------------------------------------
void SoundManager::EnterSoundZone(const ZoneVolumeNode& zone) {
    const GameplaySoundProfile& sp = zone.soundProfile;

    if (sp.HasEnterMusic()) {
        // Capture the world track before crossfading so exit can restore it.
        if (m_prevSoundZone.empty() && m_sounds.MusicFound)
            m_defaultWorldMusic = m_sounds.BackgroundMusic;

        StopMusicStream(m_sounds.BackgroundMusic);
        Music newMusic = LoadMusicWithFallback(sp.music_on_enter.c_str());
        if (newMusic.ctxData != nullptr) {
            m_sounds.BackgroundMusic = newMusic;
            m_sounds.MusicFound = true;
            PlayMusicStream(m_sounds.BackgroundMusic);
            ApplyMusicGain();
        } else {
            OZ_WARN("Sound zone '%s': music '%s' not found",
                    zone.name.c_str(), sp.music_on_enter.c_str());
            // Keep the (stopped) world track so exit does not clobber it.
            m_sounds.BackgroundMusic = m_defaultWorldMusic;
            m_sounds.MusicFound = m_defaultWorldMusic.ctxData != nullptr;
        }
    }

    if (sp.HasAmbience()) {
        if (m_ambienceZoneName != zone.name) {
            StopAmbience();
            m_ambienceHandle = LoadSoundWithFallback(sp.ambience_loop.c_str());
            if (m_ambienceHandle.frameCount > 0)
                PlaySfxAt(m_ambienceHandle);
            m_ambienceZoneName = zone.name;
        }
        // Keep-alive: raylib stops a sound when its stream drains.
        if (m_ambienceHandle.frameCount > 0 && !IsSoundPlaying(m_ambienceHandle))
            PlaySfxAt(m_ambienceHandle);
    }

    if (sp.HasEnterSfx()) {
        Sound sfx = LoadSoundWithFallback(sp.sfx_on_enter.c_str());
        if (sfx.frameCount > 0) {
            if (m_oneShot.frameCount > 0)
                UnloadSound(m_oneShot);
            m_oneShot = sfx;
            m_oneShotIdle = 0.0f;
            PlaySfxAt(m_oneShot);
        }
    }

    m_prevSoundZone = zone.name;
}

void SoundManager::StopAmbience() {
    if (m_ambienceHandle.frameCount > 0) {
        StopSound(m_ambienceHandle);
        UnloadSound(m_ambienceHandle);
    }
    m_ambienceHandle = Sound{0};
    m_ambienceZoneName.clear();
}

void SoundManager::ExitSoundZone() {
    StopAmbience();

    if (m_defaultWorldMusic.ctxData != nullptr) {
        StopMusicStream(m_sounds.BackgroundMusic);
        m_sounds.BackgroundMusic = m_defaultWorldMusic;
        m_sounds.MusicFound = true;
        PlayMusicStream(m_sounds.BackgroundMusic);
        ApplyMusicGain();
        m_defaultWorldMusic = Music{0};
    }
    m_prevSoundZone.clear();
}

void SoundManager::UpdateSoundZones(const PointRegion& region) {
    const ZoneVolumeNode* soundZone = nullptr;
    if (region.primaryZoneId >= 0) {
        const ZoneVolumeNode* z = ZoneManager::Instance().GetZone(region.primaryZoneId);
        if (z && z->zoneType == ZoneType::ZONE_GAMEPLAY_SOUND)
            soundZone = z;
    }

    if (soundZone) {
        // Already inside this zone: only the ambience keep-alive is needed.
        if (m_prevSoundZone == soundZone->name) {
            if (soundZone->soundProfile.HasAmbience() &&
                m_ambienceHandle.frameCount > 0 &&
                !IsSoundPlaying(m_ambienceHandle)) {
                PlaySfxAt(m_ambienceHandle);
            }
            return;
        }
        EnterSoundZone(*soundZone);
        return;
    }

    if (region.primaryZoneId < 0 && !m_prevSoundZone.empty())
        ExitSoundZone();
}

// ---------------------------------------------------------------------------
// Reverb
// ---------------------------------------------------------------------------
void SoundManager::UpdateReverb(const PointRegion& region) {
    const bool inReverb = region.HasZoneType(ZoneType::ZONE_REVERB);
    if (inReverb == m_wasInReverb) return;

    if (inReverb) {
        float mix   = region.combinedEnv.reverbMix;
        float decay = region.combinedEnv.reverbDecay;
        if (mix <= 0.0f)   mix = 0.35f;
        if (decay <= 0.0f) decay = 0.5f;
        float vol = 1.0f - mix * 0.5f;
        OZ_INFO("ZONE_REVERB entered - mix=%.2f decay=%.2f vol=%.2f", mix, decay, vol);
        // Duck the world track under the reverb. This is a *relative* duck, so it
        // multiplies the player's music gain rather than replacing it - the old
        // code wrote a bare 1.0 on exit and would have discarded the setting.
        m_musicDuck = vol;
        ApplyMusicGain();
        DspReverb::SetMix(mix);
        DspReverb::SetDecay(decay);
    } else {
        OZ_INFO("ZONE_REVERB exited - restoring audio");
        m_musicDuck = 1.0f;
        ApplyMusicGain();
        DspReverb::SetMix(0.0f);
        DspReverb::SetDecay(0.5f);
    }
    m_wasInReverb = inReverb;
}

// ---------------------------------------------------------------------------
// LightningScript `play_sound`
// ---------------------------------------------------------------------------
bool SoundManager::PlayScriptSound(const std::string& path) {
    if (path.empty()) return false;

    auto it = m_scriptSounds.find(path);
    if (it == m_scriptSounds.end()) {
        // Package-aware: scripts referencing .ozsnd assets must resolve.
        Sound snd = LoadSoundWithFallback(path.c_str());
        if (snd.frameCount == 0) {
            OZ_WARN("play_sound: '%s' not found", path.c_str());
            return false;
        }
        it = m_scriptSounds.emplace(path, ScriptSound{snd, 0.0f}).first;
    } else {
        it->second.idle = 0.0f;   // keep alive
    }

    // Reset pitch to unity. The handle is shared with PlayStatSound, which
    // writes a weapon's authored volume/pitch onto it, so without this a
    // `play_sound` opcode of a path a weapon also uses would inherit that
    // weapon's pitch. Volume is set by PlaySfxAt (authored 1.0 * sfx gain).
    SetSoundPitch(it->second.sound, 1.0f);

    PlaySfxAt(it->second.sound);
    return true;
}

void SoundManager::PruneScriptSoundCache(float dt) {
    for (auto it = m_scriptSounds.begin(); it != m_scriptSounds.end(); ) {
        it->second.idle += dt;
        if (it->second.idle > kScriptSoundIdleLimit) {
            if (it->second.sound.frameCount > 0)
                UnloadSound(it->second.sound);
            it = m_scriptSounds.erase(it);
        } else {
            ++it;
        }
    }
}

// ---------------------------------------------------------------------------
// Data-driven sound stats (.ozls)
// ---------------------------------------------------------------------------
bool SoundManager::PlayStatSound(const std::string& path, float volume, float pitch) {
    if (path.empty()) return false;

    // Share the script-sound cache: same package-aware resolution, same
    // idle-eviction policy, so a .wav referenced by both a `play_sound`
    // opcode and a fire_sound stat is loaded exactly once.
    auto it = m_scriptSounds.find(path);
    if (it == m_scriptSounds.end()) {
        Sound snd = LoadSoundWithFallback(path.c_str());
        if (snd.frameCount == 0) {
            // Not fatal: an unauthored or missing path must not spam every
            // shot, but the author needs to know the stat did not resolve.
            OZ_WARN("stat sound: '%s' not found", path.c_str());
            return false;
        }
        it = m_scriptSounds.emplace(path, ScriptSound{snd, 0.0f}).first;
    } else {
        it->second.idle = 0.0f;   // keep alive
    }

    // Clamp rather than reject: a .ozls author writing fire_volume = 200
    // should get a loud shot, not silence from a rejected load.
    if (volume < 0.0f) volume = 0.0f;
    if (pitch <= 0.0f) pitch = 1.0f;
    // ALWAYS write both. Volume and pitch are properties of the shared,
    // path-keyed handle, not of this call, so skipping the write when the value
    // happens to be the default left whatever the PREVIOUS caller set in place.
    // Four shipped weapons resolve to machgf3b.wav with different values
    // (flux_carbine 0.85/1.15, Pistol_01 0.9, Rifle_01 1.0/0.85, automag 0.95):
    // firing the automag then switching to Rifle_01 skipped the volume write
    // (its authored value is exactly 1.0) and played the rifle at the automag's
    // 0.95, with the pitch still wherever the previous weapon left it. The
    // `!= 1.0f` guard WAS the bug. There is nothing to restore afterwards,
    // because every play now writes the value it wants.
    //
    // The authored volume is scaled by the player's SFX gain here, at play time,
    // because the handle is shared and re-used.
    SetSoundVolume(it->second.sound, volume * m_sfxGain);
    SetSoundPitch(it->second.sound, pitch);

    PlaySound(it->second.sound);
    return true;
}

void SoundManager::StartLoopSound(const std::string& key, const std::string& path,
                                  float volume, float pitch) {
    if (key.empty() || path.empty()) return;
    if (volume < 0.0f) volume = 0.0f;
    if (pitch <= 0.0f)  pitch  = 1.0f;

    auto it = m_loops.find(key);
    if (it != m_loops.end()) {
        // Already looping this exact asset: leave it alone rather than
        // restarting it, which would click and reset the loop phase every
        // frame the caller polls.
        if (it->second.path == path) return;
        StopLoopSound(key);   // asset changed (walk <-> run), swap it
    }

    LoopSound loop;
    loop.sound  = LoadSoundWithFallback(path.c_str());
    if (loop.sound.frameCount == 0) {
        OZ_WARN("loop sound '%s': '%s' not found", key.c_str(), path.c_str());
        return;
    }
    loop.path   = path;
    loop.volume = volume;
    loop.pitch  = pitch;
    // Persisted on the handle; PruneLoops re-plays it without re-writing volume,
    // so the SFX gain is baked in here. Changing the gain while a loop plays
    // takes effect on the next StartLoopSound for that key (a loop is short and
    // retriggered continuously for footsteps, so this is imperceptible).
    SetSoundVolume(loop.sound, volume * m_sfxGain);
    SetSoundPitch(loop.sound, pitch);
    PlaySound(loop.sound);
    m_loops.emplace(key, loop);
}

void SoundManager::StopLoopSound(const std::string& key) {
    auto it = m_loops.find(key);
    if (it == m_loops.end()) return;
    if (it->second.sound.frameCount > 0) {
        StopSound(it->second.sound);
        UnloadSound(it->second.sound);
    }
    m_loops.erase(it);
}

bool SoundManager::IsLoopPlaying(const std::string& key) const {
    auto it = m_loops.find(key);
    return it != m_loops.end() && it->second.sound.frameCount > 0;
}

void SoundManager::PruneLoops() {
    // Keep-alive: raylib stops a sound when its stream drains, so a looping
    // sound has to be retriggered or it dies mid-walk. Same reason
    // EnterSoundZone re-plays its ambience handle.
    for (auto& [key, loop] : m_loops) {
        if (loop.sound.frameCount > 0 && !IsSoundPlaying(loop.sound))
            PlaySound(loop.sound);
    }
}

void SoundManager::PruneOneShot(float dt) {
    if (m_oneShot.frameCount == 0) return;
    m_oneShotIdle += dt;
    if (m_oneShotIdle >= kOneShotLifetime) {
        StopSound(m_oneShot);
        UnloadSound(m_oneShot);
        m_oneShot = Sound{0};
        m_oneShotIdle = 0.0f;
    }
}