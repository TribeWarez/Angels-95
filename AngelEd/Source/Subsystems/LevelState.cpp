#include "../Core/EditorShell.hpp"
#include "../Core/EditorState.hpp"
#include "LevelState.hpp"

// Subsystems/LevelState.cpp
//
// The owner of LevelMetadata, plus the apply path that writes the Map row back into
// it. Both halves moved here in R6: the accessors came out of UI/Panels/LevelState.cpp
// (a subsystem state object implemented inside a UI panel, reachable only because the
// UI layer is one translation unit) and ApplyMapProperties came out of History.cpp
// (it is level state, not undo/redo - it just happened to sit next to it).
//
// FRAGMENT - not a standalone translation unit. Included by Main.cpp.
// ============================================================================

#ifdef _WIN32
// Real definitions. Guarded to match the _WIN32 / stub split in LevelState.hpp; the
// stubs there are what a non-Windows build would link against instead.
static LevelMetadata g_levelMeta;

LevelMetadata GetLevelMetadata() { return g_levelMeta; }

void SetLevelMetadata(const LevelMetadata& meta) {
    // No mirror into a dialog any more. This used to copy all 17 fields into
    // g_zoneProps so the deleted Zone window would show current values - one extra
    // copy of the level state that had to be kept in step by hand. The Map row reads
    // g_levelMeta directly.
    g_levelMeta = meta;
}
#endif

// Parse an OZONE document's `levelinfo` / `particles` lines into g_levelMeta.
//
// Moved here from WorldIO's LoadWorldDocument, where being local to that one
// function meant two other world-loading paths skipped it - the no-argument
// startup load and HistoryRestore. Both left the metadata default, and
// ExportToOzone omits `levelinfo` when everything is default, so a plain
// open-save round trip DELETED the level's skybox and rules.
void LoadLevelMetadataFromPrims(const std::vector<OzonePrimitive>& prims) {
    // RESET first: this describes the document being loaded, not the session.
    // Without it, opening a world after one that had `levelinfo`/`particles` (or
    // File > New) kept the previous world's skybox paths, game rules and weather
    // and wrote them into the next file on save. The old comment above only
    // covered the opposite direction (defaults leaking into a loaded document).
    SetLevelMetadata(LevelMetadata{});
    for (const auto& pr : prims) {
        if (pr.type == OzonePrimitiveType::ENTITY_LEVELINFO) {
            LevelMetadata meta = GetLevelMetadata();
            auto arg = [&](int i) -> float {
                return (i >= 0 && i < (int)pr.args.size()) ? pr.args[i] : 0.0f;
            };
            meta.gameType = (GameType)(int)arg(0);
            meta.maxPlayers = (int)arg(1);
            meta.respawnTime = arg(2);
            meta.timeLimitEnabled = arg(3) != 0.0f;
            meta.timeLimitMinutes = arg(4);
            meta.scoreLimit = (int)arg(5);
            meta.friendlyFire = arg(6) != 0.0f;

            // Prefer the human-readable `gametype=<key>` tail when present.
            // OzoneParser classifies it separately (otherwise it would be mistaken
            // for the skybox path), and the runtime's ParseLevelInfo gives the key
            // priority over the numeric id with a warned fallback. Reading only
            // arg(0) meant the editor and the client could resolve a hand-edited
            // file to DIFFERENT modes, and then disagree on score limit and team
            // count.
            if (!pr.gametypeKey.empty()) {
                // Same rule as ParseLevelInfo (LevelSettings.hpp): GameTypeFromName
                // returns SINGLEPLAYER for an unknown key rather than signalling
                // failure, so validity is checked first.
                if (oz::gametype::IsGameTypeKey(pr.gametypeKey)) {
                    meta.gameType = oz::gametype::GameTypeFromName(pr.gametypeKey);
                } else {
                    OZ_WARN("levelinfo: unknown gametype='%s', using positional id %d",
                            pr.gametypeKey.c_str(), (int)meta.gameType);
                }
            }

            meta.skyboxTexturePath = pr.entityType;
            // Second path token = side/cap skybox. It was previously parsed into
            // OzonePrimitive::entitySubType and then dropped: nothing read it on
            // load and ExportToOzone never wrote it, so opening and re-saving a
            // world silently deleted it.
            meta.skyboxSidePath = pr.entitySubType;
            SetLevelMetadata(meta);
            EditorLog("OZONE levelinfo: mode=%d skybox='%s'%s%s",
                      (int)meta.gameType, meta.skyboxTexturePath.c_str(),
                      meta.skyboxSidePath.empty() ? "" : " side=",
                      meta.skyboxSidePath.c_str());
        } else if (pr.type == OzonePrimitiveType::ENTITY_PARTICLES) {
            LevelMetadata meta = GetLevelMetadata();
            auto arg = [&](int i) -> float {
                return (i >= 0 && i < (int)pr.args.size()) ? pr.args[i] : 0.0f;
            };
            meta.particleType = (ParticleType)(int)arg(0);
            meta.particleDensity = arg(1);
            meta.particleSpeed = arg(2);
            meta.particleColorR = (int)arg(3);
            meta.particleColorG = (int)arg(4);
            meta.particleColorB = (int)arg(5);
            meta.particleWindX = arg(6);
            meta.particleWindZ = arg(7);
            SetLevelMetadata(meta);
        }
    }
}

// Write the SelType::MAP property rows back into LevelMetadata.
//
// Only pushes an undo entry when something actually changed. Every other Apply
// path calls HistoryPush() unconditionally before dispatching, which is fine for
// them because their apply always mutates something — but a level's 16 metadata
// fields are edited in one screen, so re-opening and pressing Apply without
// touching anything would otherwise stack a no-op snapshot per click and make
// Ctrl+Z appear to do nothing but burn steps.
void ApplyMapProperties() {
    const auto& P = g_editorPanels;
    LevelMetadata before = GetLevelMetadata();

    LevelMetadata meta = before;
    meta.gameType          = static_cast<GameType>(P.propMapGameType);
    meta.maxPlayers        = P.propMapMaxPlayers;
    meta.respawnTime       = P.propMapRespawnTime;
    meta.timeLimitEnabled  = P.propMapTimeLimitEnabled;
    meta.timeLimitMinutes  = P.propMapTimeLimitMinutes;
    meta.scoreLimit        = P.propMapScoreLimit;
    meta.friendlyFire      = P.propMapFriendlyFire;
    // The skybox path is also applied immediately by the Browse / Use-Active-Tex
    // handlers (the viewport reads it live), so `before` can already equal the new
    // value here; that is fine, it simply means the diff below reports no change
    // for that one field.
    meta.skyboxTexturePath = P.propMapSkybox;
    meta.skyboxSidePath    = P.propMapSkyboxSide;
    meta.particleType      = static_cast<ParticleType>(P.propMapParticleType);
    meta.particleDensity   = P.propMapParticleDensity;
    meta.particleSpeed     = P.propMapParticleSpeed;
    meta.particleColorR    = P.propMapParticleR;
    meta.particleColorG    = P.propMapParticleG;
    meta.particleColorB    = P.propMapParticleB;
    meta.particleWindX     = P.propMapParticleWindX;
    meta.particleWindZ     = P.propMapParticleWindZ;

    auto differs = [](const LevelMetadata& a, const LevelMetadata& b) {
        return a.gameType != b.gameType || a.maxPlayers != b.maxPlayers ||
               a.respawnTime != b.respawnTime ||
               a.timeLimitEnabled != b.timeLimitEnabled ||
               a.timeLimitMinutes != b.timeLimitMinutes ||
               a.scoreLimit != b.scoreLimit ||
               a.friendlyFire != b.friendlyFire ||
               a.skyboxTexturePath != b.skyboxTexturePath ||
               a.skyboxSidePath != b.skyboxSidePath ||
               a.particleType != b.particleType ||
               a.particleDensity != b.particleDensity ||
               a.particleSpeed != b.particleSpeed ||
               a.particleColorR != b.particleColorR ||
               a.particleColorG != b.particleColorG ||
               a.particleColorB != b.particleColorB ||
               a.particleWindX != b.particleWindX ||
               a.particleWindZ != b.particleWindZ;
    };

    if (!differs(before, meta)) {
        EditorLog("Map properties: no changes");
        return;
    }

    HistoryPush();
    SetLevelMetadata(meta);
    EditorLog("Applied map properties: mode=%d maxPlayers=%d respawn=%.1f "
              "timeLimit=%s scoreLimit=%d FF=%s skybox='%s' weather=%d",
              (int)meta.gameType, meta.maxPlayers, meta.respawnTime,
              meta.timeLimitEnabled ? "on" : "off", meta.scoreLimit,
              meta.friendlyFire ? "on" : "off", meta.skyboxTexturePath.c_str(),
              (int)meta.particleType);
    // name after an edit.
}