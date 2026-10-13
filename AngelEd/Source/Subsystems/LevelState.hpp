#ifndef ANGEL_ED_SUBSYSTEMS_LEVELSTATE_HPP
#define ANGEL_ED_SUBSYSTEMS_LEVELSTATE_HPP
// ============================================================================
// Subsystems/LevelState.hpp
//
// Level state: the game type, the rules, the skyboxes and the ambient particles.
// Persisted through the OZONE `levelinfo` / `particles` lines.
//
// MOVED HERE IN R6 from UI/UiPanels.hpp. The plan document had this right and the
// code did not: "It is level state and belongs in Subsystems/LevelState; it only
// appears to live in UI/ because the deleted Zone window used to mirror it into a
// global." LevelMetadata is read by WorldIO on load and OzoneExport on save, and
// written by the Map row - none of which are UI. Its accessor was even IMPLEMENTED
// inside UI/Panels/LevelState.cpp, so the state a subsystem owns was reachable only
// because the whole UI layer is one translation unit.
//
// ParticleType moves with it for the same reason: it is the ambient-particle kind,
// i.e. level state, and it was only declared in a UI header because LevelMetadata
// lived there. Subsystems/LevelState.hpp must not include UI/UiPanels.hpp - that
// would be a Subsystems-to-UI dependency, which is the layer rule backwards.
// ============================================================================

#include "../../../Source/World/GameType.hpp"
#include "../../../Source/World/OzoneParser.hpp"

// GameType is now defined in Source/World/GameType.hpp (oz::gametype::GameType)
// so the editor, server and tests share one source of truth. The numeric values
// are identical to the legacy enum, so every saved levelinfo line still loads.
using oz::gametype::GameType;

enum class ParticleType : uint8_t {
    NONE,
    SNOW,
    RAIN,
    VOID_REALM,
    PSYCHIC_REALM
};

// Level metadata — persisted via LevelInfo/Particles instructions in both formats
struct LevelMetadata {
    // GameType / rules
    GameType gameType = GameType::SINGLEPLAYER;
    int maxPlayers = 8;
    float respawnTime = 5.0f;
    bool timeLimitEnabled = false;
    float timeLimitMinutes = 10.0f;
    int scoreLimit = 50;
    bool friendlyFire = false;
    std::string skyboxTexturePath;
    // Side/cap skybox, the second path token on the OZONE `levelinfo` line. It
    // was previously parsed into OzonePrimitive::entitySubType and then dropped:
    // nothing read it on load and ExportToOzone never wrote it, so opening and
    // re-saving a world silently deleted it.
    std::string skyboxSidePath;
    // Ambient particles
    ParticleType particleType = ParticleType::NONE;
    float particleDensity = 50.0f;
    float particleSpeed = 1.0f;
    int particleColorR = 200, particleColorG = 200, particleColorB = 200;
    float particleWindX = 0.0f, particleWindZ = 0.0f;
};

#ifdef _WIN32
// The Map row's only writer. g_levelMeta lives in Subsystems/LevelState.cpp.
LevelMetadata GetLevelMetadata();
void SetLevelMetadata(const LevelMetadata& meta);
#else
// AngelEd requires Win32 (AngelEd/Makefile refuses to build on Linux), so these are
// unreachable stubs kept only so the non-Windows include of this header parses. Same
// pattern as the block in UI/UiPanels.hpp.
inline LevelMetadata GetLevelMetadata() { return {}; }
inline void SetLevelMetadata(const LevelMetadata&) {}
#endif
// Parse an OZONE document's `levelinfo` and `particles` lines into the editor's
// metadata.
//
// ONE definition, and every path that opens a world must call it. The parse used
// to live inline in WorldIO's LoadWorldDocument, which left two paths that load a
// world WITHOUT it:
//   * the no-argument startup load in main(), which is what a normal launch takes
//     (the run scripts pass no world), and
//   * HistoryRestore, which reloads from a snapshot.
// Either one leaves the metadata at its DEFAULTS - empty skybox, default rules -
// and ExportToOzone omits `levelinfo` entirely when every field is default. So
// opening the editor normally, saving, and reopening silently deleted the level's
// skybox and its game rules, and re-saving from an undo stack did the same. The
// file looked fine; the line was simply gone.
void LoadLevelMetadataFromPrims(const std::vector<OzonePrimitive>& prims);

// Write the SelType::MAP property rows back into the metadata.
//
// Lives here rather than in PropsApply because it is level state, not a per-entity
// edit - and because it has its own undo policy: it pushes a snapshot only when
// something actually changed. Every other Apply path calls HistoryPush()
// unconditionally, which is fine because their apply always mutates something, but a
// level'"'"'s 17 metadata fields are edited in one screen, so re-opening and pressing
// Apply untouched would stack a no-op snapshot per click and make Ctrl+Z appear to do
// nothing but burn steps.
void ApplyMapProperties();
#endif // ANGEL_ED_SUBSYSTEMS_LEVELSTATE_HPP
