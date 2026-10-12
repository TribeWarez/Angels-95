#include "Data.hpp"
#include "Log.hpp"
#include "Physics/PlayerPhysics.hpp"
#include "Client/JoinUri.hpp"
#include "Network/NetworkSession.hpp"
#include "PlayerProfile.hpp"
#include "Renderer/OzAssetMapper.hpp"
#include "Audio/SoundManager.hpp"
#include "World/OzOzoneLoader.hpp"
#include "Pawn/OzPawnSystem.hpp"
#include "Pawn/CombatMath.hpp"
#include "Package/PackageAssetLoader.hpp"
#include "Client/MasterList.hpp"
#include "Renderer/EngineBillboard.hpp"
#include "Renderer/ViewModel.hpp"
#include "Renderer/PlayerModel.hpp"
#include "Renderer/CullState.hpp"
#include "Particle/OzParticleSimulationManager.hpp"
#include "Pawn/AngelPlayer/PlayerController.hpp"
#include "Script/LightningEntityRegistry.hpp"
#include "Script/LightningEntityManager.hpp"

#include "raymath.h"
#include "Renderer/rlights/rlights.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "Screenshot.hpp"

// How long the damage vignette stays up after a hit, seconds.
static constexpr float kDamageFlashDuration = 0.45f;

// The world's default ambient, in ONE place. The startup upload (InitLighting)
// and the script `restore_ambient` opcode must agree: they used to be
// {0.1,0.1,0.1,1} and {1,1,1,1}, and the latter carried a comment claiming it
// "matches LitLightning default". It did not — the restore left the scene 10x
// brighter than the startup default AND pushed the wrong value into
// OzoneLoader::SetWorldAmbient, desyncing the surface program from LitFog.
static constexpr float kDefaultAmbient[4] = {0.1f, 0.1f, 0.1f, 1.0f};
static inline void FillDefaultAmbient(float out[4]) {
    out[0] = kDefaultAmbient[0]; out[1] = kDefaultAmbient[1];
    out[2] = kDefaultAmbient[2]; out[3] = kDefaultAmbient[3];
}

bool ObjectCollision = false;
bool g_showCollisionDebug = false;

char g_world_to_load[256] = "EngineTest";
char g_world_dir_override[256] = "";
bool g_skipMenu = false;

// Portal transitions (campaign system) — set by the portal trigger in
// UpdateEntities, consumed right after LoadWorld() repositions the player.
bool g_portalTransitionPending = false;
Vector3 g_portalSpawnPos = {0, 20, 0};
float g_portalCooldown = 0.0f;

// Bidirectional portals: remember where we came from so a reverse portal can
// be spawned at the arrival point. g_returnPortalId indexes ZoneManager's
// portals (-1 = none); it starts disabled and arms once the player steps away.
std::string g_portalReturnWorld;
Vector3 g_portalReturnPos = {0, 0, 0};
bool g_portalHasReturn = false;
int g_returnPortalId = -1;

// Cross-world state that must be reset on each LoadWorld()
float g_damageCooldown = 0.0f;
std::string g_activeEnvZone;
Texture2D g_skySideTex = {0};   // optional horizon/side skybox variant

// Distance from the camera to each skybox cube face, and how fast the sky
// returns to its authored colour above the horizon, both live in
// oz:: (Source/Renderer/Skybox.hpp) now that the editor shares them.

// Set from PlayHomeScreen to request a server join
bool SetServerJoinFlag = false;
const char *SetServerJoinIP = nullptr;
int SetServerJoinPort = 27015;

void LoadSave();
void SaveGame();
    void DrawRemotePlayers3D(Shader litShader);

#include "Renderer/CombatFX.hpp"
#include "Renderer/SurfaceMaterial.hpp"
#include "Renderer/SkyMaterial.hpp"
#include "Renderer/Skybox.hpp"

// ---------------------------------------------------------------------------
// OzoneCollisionQuery — adapter that feeds OzoneLoader's chunked collision
// volumes / heightmap into the oz::physics::PlayerPhysics module, keeping the
// module independent of the world loader.
// ---------------------------------------------------------------------------
namespace oz {
namespace physics {

class OzoneCollisionQuery : public PlayerPhysics::CollisionQuery {
public:
    GroundSample SampleGround(float x, float z, float feetY) override {
        auto& ozLoader = OzoneLoader::Instance();
        // Support tolerance: a surface up to kStepHeight above the feet counts,
        // so walking into a step or stair climbs it. Everything higher is
        // ignored, which also keeps underground areas working — a tunnel at
        // feet=-6 must fall through to its own floor rather than being snapped
        // up to the heightmap surface far above.
        const float supportCeiling = feetY + kStepHeight;

        // Heightmap candidate (heightmap first, then brush tops, then the higher
        // valid one wins). BOTH are always evaluated: returning early on a valid
        // heightmap would hide every platform, stair and balcony authored above
        // a heightmap-covered map.
        float best = kNoGroundY;
        if (ozLoader.HasHeightmap()) {
            float hmY = ozLoader.SampleHeightmapY(x, z);
            if (hmY > kNoGroundY && hmY <= supportCeiling)
                best = hmY;
        }

        // Brush-top candidate (chunk-accelerated).
        auto& chunkMgr = ozLoader.GetChunkManager();
        std::vector<int> nearIndices;
        chunkMgr.GetVolumesNear(x, z, nearIndices);
        auto& vols = ozLoader.GetCollisionVolumes();
        for (int idx : nearIndices) {
            if (idx < 0 || idx >= (int)vols.size())
                continue;
            auto& vol = vols[idx];
            if (x >= vol.aabb.min.x && x <= vol.aabb.max.x &&
                z >= vol.aabb.min.z && z <= vol.aabb.max.z)
            {
                float top = vol.aabb.max.y;
                if (top > best && top <= supportCeiling)
                    best = top;
            }
        }
        if (best > kNoGroundY)
            return GroundSample{best, true};
        return GroundSample{0.0f, false};
    }

    bool OverlapsObstacle(const BoundingBox& player, float feetY) override {
        auto& ozLoader = OzoneLoader::Instance();
        auto& chunkMgr = ozLoader.GetChunkManager();
        std::vector<int> nearIndices;
        chunkMgr.GetVolumesNear(player.min.x, player.max.x, nearIndices);
        auto& vols = ozLoader.GetCollisionVolumes();
        for (int idx : nearIndices) {
            if (idx >= 0 && idx < (int)vols.size() &&
                CheckCollisionBoxes(player, vols[idx].aabb))
            {
                // Floors, steps and stairs are not obstacles: anything whose top
                // face is within the step-up tolerance of the feet is walked
                // over by ClampToGround. This is a hard threshold, so anything
                // taller still blocks and walls stay solid.
                if (vols[idx].aabb.max.y <= feetY + kStepHeight)
                    continue;
                // The heightmap is the ground: support comes from the ground
                // clamp, not the obstacle test.
                if (vols[idx].isHeightmap)
                    continue;
                return true;
            }
        }
        return false;
    }
};
} // namespace physics
} // namespace oz


class EngineData
{
public:
    int LevelIndex = 1;
    Camera MainCamera = {0};
    Shader PixelShader;
    Shader FogShader;
    Shader LineShader;
    Shader ToonShader;
    Shader SobelShader;
    Shader JitterShader;
    Shader Lights;
    Shader WindShader;   // Lighting.vs + wind displacement (foliage)
    Light GameLights[MAX_LIGHTS];

    Texture HomeScreen;
    Texture PauseHeading;
    Texture BtnNormal;
    Texture BtnHover;
    Texture BtnClicked;
    ray_video_t HomeScreenVideo;
    Music HomeScreenMusic;
    
    bool FirstLoad = true;

    int Ticker = 0;
    int CameraSpeed = 1;
    int RenderRadius = 800;
    int Deaths;
    int BadPreformaceCounter = 0;
    bool SkyboxEnabled = false;
    int Ending = 0;

    // Damage-flash timer, seconds. Set by contact damage and by the scripted
    // `damage` opcode; drawn as a red screen vignette by the HUD pass. Was an
    // int saturating at 240 that nothing ever read.
    float DamageFlash = 0.0f;

    void InitCamera()
    {
        MainCamera.position = (Vector3){0.0f, 20.0f, 0.0f};
        MainCamera.target = (Vector3){0.0f, 20.0f, -10.0f};
        MainCamera.up = (Vector3){0.0f, 1.0f, 0.0f};
        MainCamera.fovy = 60.0f;
        MainCamera.projection = CAMERA_PERSPECTIVE;
        OZ_INFO("Camera initialized at (0, 20, 0)");
    }
};

EngineData OmegaTechData;

bool LoadFlag = false;

// STILL HARDCODED FILE PATH Loading for Worlds
// TODO: abreviate into handling by OzOzoneLoader and PawnBased/LightningScript Based object loading  // NOLINT

auto LoadWorld()
{
    OZ_INFO("LoadWorld: loading world %d (frame=%llu)", OmegaTechData.LevelIndex, (unsigned long long)OmegaTechData.Ticker);
    PlayFade();
    PawnSystem::Instance().ClearLights();

    // Reset movement state so player falls to new world's collision
    g_playerMovement.onGround = false;
    g_playerMovement.velocityY = 0.0f;

    // Reset cross-world state that would otherwise persist across LoadWorld calls
    g_damageCooldown = 0.0f;
    g_activeEnvZone.clear();
    // Audio (world music, zone ambience, reverb hook, script sound cache) owns
    // its own per-world reset.
    SoundManager::Instance().ResetWorldAudio();

    {
        OmegaTechData.DamageFlash = 0.0f;

        LightningEntityManager::Instance().SetPlayerHealth(100.0f);

        OmegaTechData.SkyboxEnabled = false;

        // Resolve the asset directory prefix. g_world_to_load may be a world NAME
        // (assets live in GameData/Worlds/<name>/) or a direct file path (editor
        // playtest passes --world <path>; assets then live beside the file).
        std::string worldLoad = g_world_to_load;
        bool directWorldPath = worldLoad.find(".ozone") != std::string::npos;
        std::string assetPrefix;
        if (directWorldPath) {
            size_t slash = worldLoad.find_last_of("/\\");
            assetPrefix = (slash != std::string::npos) ? worldLoad.substr(0, slash + 1) : std::string();
        } else if (g_world_dir_override[0]) {
            assetPrefix = std::string("GameData/Worlds/") + g_world_dir_override + "/";
        } else {
            assetPrefix = std::string("GameData/Worlds/") + worldLoad + "/";
        }

        // World-scoped .ozls defs (every world ships a "zone_sky_0", so the
        // global registry collides by name). Make the by-name map point at THIS
        // world before anything resolves a zone name, otherwise on_enter never
        // fires and the level renders with the default black ambient.
        LightningEntityRegistry::Instance().LoadWorldOverrides(assetPrefix);

        // The registry is about to be re-pointed at a different world's defs, so
        // any icon EngineBillboard memoised from the PREVIOUS world's .ozls is
        // stale (and may already have been unloaded). Drop it here rather than
        // letting DrawPickup hand DrawBillboard a dangling Texture2D id.
        EngineBillboard::InvalidateIconCache();

        // Same reasoning for the player character: it resolves through the
        // "Player" def, which the override pass above may have just replaced.
        // Re-resolve rather than keep drawing the previous world's answer.
        oz::PlayerModel::Instance().Invalidate();

        // World skybox: filesystem first, then packages (resolves "Skybox.png"
        // inside the world's .ozone container in packaged builds)
        {
            if (WorldModels.Skybox.id > 0)
                UnloadTexture(WorldModels.Skybox);
            WorldModels.Skybox = LoadTextureWithFallback(TextFormat("%sModels/Skybox.png", assetPrefix.c_str()));
            OmegaTechData.SkyboxEnabled = (WorldModels.Skybox.id > 0);
            if (OmegaTechData.SkyboxEnabled)
                OZ_INFO("World skybox: loaded %sModels/Skybox.png", assetPrefix.c_str());
        }

        // Clear all existing entities before loading new world
        PawnSystem::Instance().ClearLights();
        PawnSystem::Instance().ClearPlayerStarts();
        PawnSystem::Instance().ClearPickups();
        ZoneManager::Instance().ClearZones();
        ZoneManager::Instance().ClearPortals();
        PawnSystem::Instance().ClearEmitters();
        PawnSystem::Instance().ClearParticleEmitters();
        PawnSystem::Instance().ClearPathNodes();
        PawnSystem::Instance().ClearWindZones();
        PawnSystem::Instance().DespawnAll();
        OzParticleSimulationManager::Instance().Clear();
        // Reset level metadata so stale settings never leak across worlds
        PawnSystem::Instance().GetWorldInfo() = WorldInfo{};
        ParticlesEnabled = false;
        CombatFX::Instance().ClearAll();
        if (g_skySideTex.id > 0) { UnloadTexture(g_skySideTex); g_skySideTex = {0}; }

        // The ONLY world source of truth: the OZONE container. A missing
        // World.ozone is a hard error (the legacy WDL fallback is gone).
        {
            char ozonePath[512];
            if (strstr(g_world_to_load, ".ozone") != nullptr)
                snprintf(ozonePath, sizeof(ozonePath), "%s", g_world_to_load); // direct path (editor playtest)
            else
                snprintf(ozonePath, sizeof(ozonePath), "%sWorld.ozone", assetPrefix.c_str());
            if (IsPathFile(ozonePath))
                OzoneLoader::Instance().LoadFile(ozonePath);
            else
                OZ_ERROR("LoadWorld: missing world '%s' (no World.ozone; WDL fallback removed)", ozonePath);

            // The loader only parses: it hands back the world's entities and
            // level metadata, and this orchestrator decides what to apply.
            InjectOzoneEntities(OzoneLoader::Instance().GetEntities(),
                                PawnSystem::Instance());

            // Re-assert the lit-fog shader on the freshly loaded world geometry.
            // The loader assigns it per-material when building brushes, but if
            // the shader was (re)loaded or the materials ended up with the
            // default shader the client rendered the world fullbright. The
            // editor already re-applies it after load; the client must too.
            if (OmegaTechData.Lights.id > 0)
                OzoneLoader::Instance().SetLitFogShader(OmegaTechData.Lights);
        }

        // Apply level metadata (LevelInfo/Particles) after entities are loaded
        {
            const LevelSettings& s = OzoneLoader::Instance().GetLevelSettings();
            if (!s.skyboxPath.empty())
            {
                Texture2D newSky = LoadTextureWithFallback(s.skyboxPath.c_str());
                if (newSky.id > 0)
                {
                    if (WorldModels.Skybox.id > 0) UnloadTexture(WorldModels.Skybox);
                    WorldModels.Skybox = newSky;
                    OmegaTechData.SkyboxEnabled = true;
                    OZ_INFO("LevelInfo: skybox '%s'", s.skyboxPath.c_str());
                }
                else
                {
                    OZ_WARN("LevelInfo: skybox '%s' not found", s.skyboxPath.c_str());
                }
            }
            // The user's global Particles toggle is a MASTER switch and must NOT
            // be derived from the level. It used to be assigned here, so loading
            // any weather level silently re-enabled particles for a user who had
            // turned them off in the settings menu.
            if (s.WeatherEnabled())
            {
                OzParticleSimulationManager::Instance().ConfigureWeather(
                    s.particleType, s.particleDensity, s.particleSpeed,
                    s.particleR, s.particleG, s.particleB,
                    s.particleWindX, s.particleWindZ);
                OZ_INFO("LevelInfo: weather type=%d density=%.0f speed=%.2f",
                        s.particleType, s.particleDensity, s.particleSpeed);
            }
            else
            {
                // Retire the previous world's field: without this, snow kept
                // falling in a level that authored no weather at all.
                OzParticleSimulationManager::Instance().ClearWeather();
            }
            if (!s.skyboxSidePath.empty())
            {
                Texture2D sideSky = LoadTextureWithFallback(s.skyboxSidePath.c_str());
                if (sideSky.id > 0)
                {
                    if (g_skySideTex.id > 0) UnloadTexture(g_skySideTex);
                    g_skySideTex = sideSky;
                    OZ_INFO("LevelInfo: skybox sides '%s'", s.skyboxSidePath.c_str());
                }
                else
                {
                    OZ_WARN("LevelInfo: skybox sides '%s' not found", s.skyboxSidePath.c_str());
                }
            }
        }

        // World music: <prefix>Music/Main.mp3, else the shared global ambience.
        SoundManager::Instance().PlayWorldMusic(assetPrefix);

        // Spawn at the world's playerstart unless resuming from a saved
        // position ("Continue" from the title menu sets SetCameraFlag).
        if (!SetCameraFlag)
            PawnSystem::Instance().RespawnPlayerAtStart(OmegaTechData.MainCamera);

        SaveGame();
    }
}

void LoadLaunchConfig()
{
    wstring Config = LoadFile("GameData/Launch.conf");

    wstring Resolution = WSplitValue(Config, 0);

    switch (Resolution[0])
    {
    case L'1':
        SetWindowSize(640, 480);
        break;
    case L'2':
        SetWindowSize(1280, 720);
        break;
    case L'3':
        SetWindowSize(1980, 1080);
        break;
    case L'4':
        SetWindowSize(2560, 1440);
        break;
    case L'5':
        SetWindowSize(3840, 2160);
        break;
    default:
        SetWindowSize(GetMonitorWidth(0), GetMonitorHeight(0));
        ToggleFullscreen();
        break;
    }
}

void UpdateLightSources()
{
    // Lighting now lives in Renderer/LitLightning (LitLightning_UpdateFrame).
    // Kept as a thin engine-side adapter that supplies the engine globals.
    LitLightning_UpdateFrame(PawnSystem::Instance().GetLights(), OmegaTechData.Lights,
                             OmegaTechData.MainCamera, OmegaTechData.GameLights[0],
                             GetFrameTime());

    // The surface program carries its OWN copy of the lights[]/viewPos/ambient/fog
    // uniforms, so it has to be refreshed here too. Skipping it would light
    // surface-flagged brushes with whatever the previous world's lights were,
    // which is invisible in an empty map and baffling in a real one.
    //
    // Ambient and fog are read back from OzoneLoader, which owns them: the world
    // pushes them to LitFog from several places (level defaults, zone entry,
    // zone exit) and the surface program needs the SAME numbers, or a decorated
    // brush disagrees with the room around it. Mirroring here means one place to
    // keep in step instead of one per fog-setting site.
    if (oz::SurfaceMaterial::Instance().Ready()) {
        float amb[4];
        FillDefaultAmbient(amb);
        OzoneLoader::Instance().GetWorldAmbient(amb);
        float fogCol[3] = {0.7f, 0.7f, 0.8f};
        float fogStart = 10.0f, fogEnd = 100.0f, fogDensity = 1.0f, fogIntensity = 1.0f;
        OzoneLoader::Instance().GetWorldFog(fogCol, fogStart, fogEnd,
                                            fogDensity, fogIntensity);
        oz::SurfaceMaterial::Instance().SetFog(fogCol, fogStart, fogEnd,
                                               fogDensity, fogIntensity);
        oz::SurfaceMaterial::Instance().UpdateFrame(PawnSystem::Instance().GetLights(),
                                                OmegaTechData.MainCamera,
                                                GetFrameTime(), amb);
    }
}

void DrawLights()
{
    const auto &lights = PawnSystem::Instance().GetLights();
    for (const auto &node : lights)
    {
        if (!node.active)
            continue;
        Color c = node.color;
        DrawSphereEx(node.position, 0.2f, 8, 8, c);
    }
}

// DrawLightFlares — additive billboard glows for lights with flare/corona.
// The glow texture is generated procedurally once (radial falloff) — no asset file.
inline Texture2D g_lightGlowTex = {0};
void DrawLightFlares(Camera3D& camera)
{
    const auto& lights = PawnSystem::Instance().GetLights();
    bool any = false;
    for (const auto& l : lights)
        if (l.active && (l.flare || l.corona)) { any = true; break; }
    if (!any) return;

    if (g_lightGlowTex.id == 0)
    {
        constexpr int S = 128;
        Image img = GenImageColor(S, S, BLANK);
        Color* px = (Color*)img.data;
        for (int y = 0; y < S; y++)
            for (int x = 0; x < S; x++) {
                float dx = (x - S/2 + 0.5f) / (S/2);
                float dy = (y - S/2 + 0.5f) / (S/2);
                float d = sqrtf(dx*dx + dy*dy);
                float a = (d < 1.0f) ? powf(1.0f - d, 2.5f) : 0.0f;
                px[y*S + x] = (Color){255, 255, 255, (unsigned char)(a * 255.0f)};
            }
        g_lightGlowTex = LoadTextureFromImage(img);
        UnloadImage(img);
    }

    BeginBlendMode(BLEND_ADDITIVE);
    for (const auto& l : lights)
    {
        if (!l.active || (!l.flare && !l.corona)) continue;
        // Size from light radius/intensity, clamped to sane screen presence
        float base = fminf(fmaxf(l.radius * 0.08f, 0.8f), 8.0f);
        if (l.corona)
            DrawBillboard(camera, g_lightGlowTex, l.position, base * 2.5f,
                          (Color){l.color.r, l.color.g, l.color.b, 70});
        if (l.flare)
            DrawBillboard(camera, g_lightGlowTex, l.position, base,
                          (Color){l.color.r, l.color.g, l.color.b, (unsigned char)(120 + l.intensity * 20)});
    }
    EndBlendMode();
}

// The LEVEL's fog, captured at startup.
//
// `restore_fog` promises to "hand fog back to the level defaults" but had nowhere
// to restore from: it read the dead post-process FogTint state, so it produced
// density 0 on LitFog and left SurfaceMaterial untouched. This is the thing both
// script fog opcodes now publish through OzoneLoader (the single owner, because
// raylib has no GetShaderValue) and restore to.
struct LevelFogState {
    float fogColor[3] = {0.7f, 0.7f, 0.8f};
    float fogStart = 10.0f;
    float fogEnd = 100.0f;
    float fogDensity = 1.0f;
    float fogIntensity = 1.0f;
};
static LevelFogState g_levelFog;

void OmegaTechInit()
{
    OZ_INFO("=== OmegaTech Engine starting ===");
    OZ_INFO("CWD: %s", fs::current_path().string().c_str());
    OZ_INFO("GameData/Worlds exists: %d", (int)fs::exists("GameData/Worlds"));
    OZ_INFO("System/Data/Zones exists: %d", (int)fs::exists("System/Data/Zones"));
    // Launch.conf calls SetWindowSize after InitWindow, which would silently
    // override an explicit --shot-res. A capture run is meant to be exactly the
    // requested size, so skip it there.
    if (!g_shot.active)
        LoadLaunchConfig();
    else if (g_shot.resWidth > 0 && g_shot.resHeight > 0)
        SetWindowSize(g_shot.resWidth, g_shot.resHeight);

    GuiLoadStyleDark();

    // Player profile slots (System/PlayerProfiles.ini). Loaded once here rather
    // than per menu visit so reopening the Character pane cannot discard edits
    // by re-reading the file. The title menu only reads from here.
    PlayerProfileManager::Instance().Init();

    // Initialize package-based asset loading
    PackageAssetLoader::Instance().Init();

    // Initialize LightningScript entity system
    LightningEntityRegistry::Instance().Init();
    LightningEntityManager::Instance().Init();

    // Initialize engine/item texture mapper (must be before EngineBillboard::Init)
    AssetMapper::Instance().Init();

    // Initialize engine billboard system
    EngineBillboard::Init();

    // Per-face surface shader (UT99-style flags: unlit, masked, translucent,
    // glow, U/V pan). Loaded separately from LitFog so nothing that already
    // depends on Lighting.vs/LitFog.fs output is affected. Failure is non-fatal
    // and logged by SurfaceMaterial: brushes then fall back to DrawModel.
    oz::SurfaceMaterial::Instance().Init("GameData/Shaders/");

    // Sky.vs/fs - horizon fog for the skybox cube. Also non-fatal and logged by
    // SkyMaterial: without it the skybox falls back to raylib's default material
    // shader, i.e. unfogged. Worth knowing before blaming a level: in all six
    // shipped worlds the FAKEBACKDROP ring stands ~13 degrees above the horizon and
    // occludes the whole band this affects, so it is a correctness fix that is
    // invisible there. See SkyMaterial.hpp.
    oz::SkyMaterial::Instance().Init("GameData/Shaders/");

    // Initialize combat FX (procedural decal/particle textures)
    CombatFX::Instance().Init();

    // Build the 3D skybox cube (6 planes, per-face UV orientation, distance,
    // per-face rotations and fog). oz::Skybox owns all of it and is shared with
    // AngelEd, so the editor viewport and the game cannot disagree about where
    // the sky is or how it fogs.
    oz::Skybox::Instance().Init();

    // Register pawn definitions from .cfg files (data-driven)
    {
        auto& ps = PawnSystem::Instance();
        const char* defsDir = "GameData/Global/PawnDefs";
        namespace fs = std::filesystem;
        if (fs::exists(defsDir)) {
            int loaded = 0;
            for (auto& entry : std::filesystem::directory_iterator(defsDir)) {
                if (!entry.is_regular_file()) continue;
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext != ".cfg") continue;
                std::ifstream f(entry.path());
                if (!f.is_open()) continue;
                std::string name, sp, sc, modelPath, modelTex;
                std::string meshType, animIdle, animPatrol, animChase, animReturn, animDeath;
                float speed = 1.5f, aggroRange = 6.0f, attackRange = 1.5f, damage = 10.0f;
                float animSpeed = 1.0f, modelScale = 1.0f;
                int maxHealth = 100;
                std::string line;
                while (std::getline(f, line)) {
                    line.erase(0, line.find_first_not_of(" \t\r\n"));
                    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
                    size_t eq = line.find('=');
                    if (eq == std::string::npos) continue;
                    std::string key = line.substr(0, eq);
                    std::string val = line.substr(eq + 1);
                    key.erase(0, key.find_first_not_of(" \t"));
                    key.erase(key.find_last_not_of(" \t") + 1);
                    val.erase(0, val.find_first_not_of(" \t"));
                    val.erase(val.find_last_not_of(" \t\r") + 1);
                    if (key == "name") name = val;
                    else if (key == "speed") speed = std::stof(val);
                    else if (key == "aggroRange") aggroRange = std::stof(val);
                    else if (key == "attackRange") attackRange = std::stof(val);
                    else if (key == "damage") damage = std::stof(val);
                    else if (key == "maxHealth") maxHealth = std::stoi(val);
                    else if (key == "sprite_path") sp = val;
                    else if (key == "scream_path") sc = val;
                    else if (key == "model_path") modelPath = val;
                    else if (key == "model_texture") modelTex = val;
                    else if (key == "mesh_type") meshType = val;
                    else if (key == "model_scale") modelScale = std::stof(val);
                    else if (key == "anim_idle") animIdle = val;
                    else if (key == "anim_patrol") animPatrol = val;
                    else if (key == "anim_chase") animChase = val;
                    else if (key == "anim_return") animReturn = val;
                    else if (key == "anim_death") animDeath = val;
                    else if (key == "anim_speed") animSpeed = std::stof(val);
                }
                if (!name.empty()) {
                    PawnDef def;
                    def.name = name;
                    def.speed = speed;
                    def.aggroRange = aggroRange;
                    def.attackRange = attackRange;
                    def.damage = damage;
                    def.maxHealth = maxHealth;
                    def.sprite_path = sp;
                    def.scream_path = sc;
                    def.model_path = modelPath;
                    def.model_texture = modelTex;
                    def.model_scale = modelScale;
                    def.mesh_type = meshType;
                    def.anim_idle = animIdle;
                    def.anim_patrol = animPatrol;
                    def.anim_chase = animChase;
                    def.anim_return = animReturn;
                    def.anim_death = animDeath;
                    def.anim_speed = animSpeed;
                    def.baseDir = entry.path().parent_path().string();
                    if (!def.baseDir.empty()) def.baseDir += "/";
                    ps.RegisterDef(def);
                    loaded++;
                }
            }
            OZ_INFO("Loaded %d pawn definitions from %s", loaded, defsDir);
        } else {
            // Fallback if no config directory exists
            OZ_INFO("PawnDefs dir not found, using hardcoded defaults");
            PawnDef d;
            d.name="Walker"; d.speed=1.5f; d.aggroRange=6.0f; d.attackRange=1.5f; d.damage=10.0f; d.maxHealth=100; ps.RegisterDef(d);
            d.name="Skaarj"; d.speed=2.5f; d.aggroRange=10.0f; d.attackRange=2.0f; d.damage=20.0f; d.maxHealth=150; ps.RegisterDef(d);
            d.name="Brute"; d.speed=1.0f; d.aggroRange=4.0f; d.attackRange=1.5f; d.damage=30.0f; d.maxHealth=250; ps.RegisterDef(d);
            d.name="Floater"; d.speed=1.2f; d.aggroRange=8.0f; d.attackRange=3.0f; d.damage=15.0f; d.maxHealth=80; ps.RegisterDef(d);
        }
    }

    OmegaTechData.InitCamera();

    OmegaTechData.PixelShader = LoadShaderWithFallback(0, "GameData/Shaders/Pixel.fs");
    // Legacy FogShader disabled - using LitFog material shader instead
    OmegaTechData.FogShader = {0};
    OmegaTechData.LineShader = LoadShaderWithFallback(0, "GameData/Shaders/Scanlines.fs");
    OmegaTechData.SobelShader = LoadShaderWithFallback(0, "GameData/Shaders/Sobel.fs");
    OmegaTechData.ToonShader = LoadShaderWithFallback(0, "GameData/Shaders/Toon.fs");
    OmegaTechData.JitterShader = LoadShaderWithFallback(0, "GameData/Shaders/Jitter.fs");
    OmegaTechData.Lights = LoadShaderWithFallback("GameData/Shaders/Lights/Lighting.vs", "GameData/Shaders/Lights/LitFog.fs");
    OmegaTechData.WindShader = LoadShaderWithFallback("GameData/Shaders/Lights/Wind.vs", "GameData/Shaders/Lights/LitFog.fs");
    if (OmegaTechData.WindShader.id == 0) {
        OmegaTechData.WindShader = OmegaTechData.Lights; // graceful fallback: no sway
        OZ_WARN("Wind.vs shader not found — foliage wind disabled");
    }
    OZ_INFO("Shaders loaded (Pixel=%d, Line=%d, Sobel=%d, Toon=%d, Jitter=%d, Lights=%d, Wind=%d)",
            OmegaTechData.PixelShader.id, OmegaTechData.LineShader.id,
            OmegaTechData.SobelShader.id, OmegaTechData.ToonShader.id,
            OmegaTechData.JitterShader.id, OmegaTechData.Lights.id, OmegaTechData.WindShader.id);
    OzoneLoader::Instance().SetLitFogShader(OmegaTechData.Lights);

    // Initialize all lights to disabled
    for (int i = 0; i < MAX_LIGHTS; i++)
    {
        OmegaTechData.GameLights[i] = {0};
    }

    OmegaTechData.Lights.locs[SHADER_LOC_VECTOR_VIEW] = GetShaderLocation(OmegaTechData.Lights, "viewPos");
    int AmbientLoc = GetShaderLocation(OmegaTechData.Lights, "ambient");
    float ambient[4];
    FillDefaultAmbient(ambient);
    SetShaderValue(OmegaTechData.Lights, AmbientLoc, ambient, SHADER_UNIFORM_VEC4);
    OzoneLoader::Instance().SetWorldAmbient(ambient[0], ambient[1], ambient[2], ambient[3]);

    // Initialize fog uniforms
    int fogStartLoc = GetShaderLocation(OmegaTechData.Lights, "fogStart");
    int fogEndLoc = GetShaderLocation(OmegaTechData.Lights, "fogEnd");
    int fogDensityLoc = GetShaderLocation(OmegaTechData.Lights, "fogDensity");
    int fogColorLoc = GetShaderLocation(OmegaTechData.Lights, "fogColor");
    int fogIntensityLoc = GetShaderLocation(OmegaTechData.Lights, "fogIntensity");

    float fogStart = 10.0f;
    float fogEnd = 100.0f;
    float fogDensity = 1.0f;
    float fogColor[3] = {0.7f, 0.7f, 0.8f};
    float fogIntensity = 1.0f;

    SetShaderValue(OmegaTechData.Lights, fogStartLoc, &fogStart, SHADER_UNIFORM_FLOAT);
    SetShaderValue(OmegaTechData.Lights, fogEndLoc, &fogEnd, SHADER_UNIFORM_FLOAT);
    SetShaderValue(OmegaTechData.Lights, fogDensityLoc, &fogDensity, SHADER_UNIFORM_FLOAT);
    SetShaderValue(OmegaTechData.Lights, fogColorLoc, fogColor, SHADER_UNIFORM_VEC3);
    SetShaderValue(OmegaTechData.Lights, fogIntensityLoc, &fogIntensity, SHADER_UNIFORM_FLOAT);
    // Publish the same defaults so the surface program starts in sync (its fog
    // uniforms were previously never set at all and stayed at the GLSL defaults,
    // which happened to match only by coincidence).
    OzoneLoader::Instance().SetWorldFog(fogColor, fogStart, fogEnd,
                                        fogDensity, fogIntensity);
    // Remember the LEVEL's fog, because `restore_fog` promises to "hand fog back
    // to the level defaults" and had nothing to restore from - it re-used the dead
    // post-process FogTint state instead. Nothing else records these, and a script
    // that sets and then restores fog should land exactly here.
    g_levelFog.fogStart    = fogStart;
    g_levelFog.fogEnd      = fogEnd;
    g_levelFog.fogDensity  = fogDensity;
    g_levelFog.fogIntensity = fogIntensity;
    for (int i = 0; i < 3; i++) g_levelFog.fogColor[i] = fogColor[i];

    // uTime uniform for GPU light animation
    static int uTimeLoc = GetShaderLocation(OmegaTechData.Lights, "uTime");
    float timeVal = (float)GetTime();
    SetShaderValue(OmegaTechData.Lights, uTimeLoc, &timeVal, SHADER_UNIFORM_FLOAT);

    OmegaTechData.HomeScreen = LoadTextureWithFallback("GameData/Global/Title/Title.png");
    OmegaTechData.PauseHeading = LoadTexture("GameData/Global/Title/menu_heading.png");
    OmegaTechData.BtnNormal = LoadTexture("GameData/Global/Title/menu_button.png");
    OmegaTechData.BtnHover = LoadTexture("GameData/Global/Title/menu_button_hover.png");
    OmegaTechData.BtnClicked = LoadTexture("GameData/Global/Title/menu_button_clicked.png");
    if (IsPathFile("GameData/Global/Title/Title.mpg"))
        OmegaTechData.HomeScreenVideo = ray_video_open("GameData/Global/Title/Title.mpg");
    OmegaTechData.HomeScreenMusic = LoadMusicWithFallback("GameData/Global/Title/Title.mp3");

    OmegaTechTextSystem.Bar = LoadTextureWithFallback("GameData/Global/TextBar.png");
    OmegaTechTextSystem.BarFont = LoadFontWithFallback("GameData/Global/Font.ttf");
    // UI/gameplay one-shots + the typewriter blip (all owned by SoundManager).
    SoundManager::Instance().LoadCoreSounds();

    OmegaTechData.GameLights[0] = CreateLight(LIGHT_DIRECTIONAL, {OmegaTechData.MainCamera.position.x, OmegaTechData.MainCamera.position.y, OmegaTechData.MainCamera.position.z}, Vector3Zero(), WHITE, OmegaTechData.Lights);

    Target = LoadRenderTexture(GetScreenWidth(), GetScreenHeight());

    // Pre-cache weapon object models from entity definitions (Object1-5)
    {
        static const char* objNames[] = {"Object1", "Object2", "Object3", "Object4", "Object5"};
        for (int o = 0; o < 5; o++) {
            int rIdx = LightningEntityManager::Instance().PrecacheModelForDef(objNames[o]);
            if (rIdx >= 0) {
                Model* m = LightningEntityManager::Instance().GetModelByResourceIdx(rIdx);
                if (m && m->meshes) {
                    WorldModels.objectModels[o] = *m;
                    WorldModels.objectModelsLoaded[o] = true;
                }
            }
        }
    }

    SoundManager::Instance().PlayStream(OmegaTechData.HomeScreenMusic);
}

void PlaySplashScreen()
{
    // Screenshot runs are automated: the 2.5s splash is pure latency and would
    // also leave the title logo in the first captured frame on slow machines.
    if (g_shot.active)
        return;

    // Reveal gate. The window is created hidden (FLAG_WINDOW_HIDDEN in
    // Main.cpp) so the engine can boot, resolve the asset pipeline and prime
    // the first frame without flashing an undecorated title bar or an empty
    // client area. This function is the one place allowed to reveal it, which
    // is why it must be called before anything interactive: a hidden Win32
    // window is not hit-tested, so the title menu would never see a click.
    ClearWindowState(FLAG_WINDOW_HIDDEN);
    #ifdef _WIN32
    // ClearWindowState clears WS_VISIBLE through raylib's platform layer, but
    // SetWindowLongPtr cannot reliably *show* a window on every Windows
    // version, so state the intent explicitly and keep the HWND in step with
    // raylib's flag.
    if (HWND hwnd = GetActiveWindow()) ShowWindow(hwnd, SW_SHOW);
    #endif

    Texture2D splash = LoadTexture("GameData/Global/Title/splash.png");
    double startTime = GetTime();

    while (!WindowShouldClose() && GetTime() - startTime < 2.5)
    {
        BeginDrawing();
        ClearBackground(BLACK);
        if (splash.id > 0)
            DrawTexturePro(splash,
                           (Rectangle){0, 0, (float)splash.width, (float)splash.height},
                           (Rectangle){0, 0, (float)GetScreenWidth(), (float)GetScreenHeight()},
                           (Vector2){0, 0}, 0, WHITE);
        EndDrawing();
    }

    if (splash.id > 0)
        UnloadTexture(splash);
}

#include "Menu/TitleMenu.hpp"

void PlayHomeScreen()
{
    // Re-open the title video if a previous menu visit released it.
    if (!OmegaTechData.HomeScreenVideo.ok && IsPathFile("GameData/Global/Title/Title.mpg"))
        OmegaTechData.HomeScreenVideo = ray_video_open("GameData/Global/Title/Title.mpg");

    if (g_skipMenu)
    {
        g_skipMenu = false;
        UnloadRenderTexture(Target);
        Target = LoadRenderTexture(GetScreenWidth(), GetScreenHeight());
        OmegaTechData.Deaths = 1;
        return;
    }

    TitleMenu menu;

    while (!menu.Tick() && !WindowShouldClose())
    {
    }

    SoundManager::Instance().StopStream(OmegaTechData.HomeScreenMusic);

    // Release the title video (it will be re-opened on the next menu visit).
    if (OmegaTechData.HomeScreenVideo.ok)
    {
        ray_video_destroy(&OmegaTechData.HomeScreenVideo);
        OmegaTechData.HomeScreenVideo = ray_video_t{};
    }

    if (menu.ShouldLoadGame() || menu.GetSelectedWorld())
    {
        UnloadRenderTexture(Target);
        Target = LoadRenderTexture(GetScreenWidth(), GetScreenHeight());
        OmegaTechData.Deaths = 1;
    }

    if (menu.GetSelectedWorld())
    {
        strncpy(g_world_to_load, menu.GetSelectedWorld(), sizeof(g_world_to_load) - 1);
        g_world_to_load[sizeof(g_world_to_load) - 1] = '\0';
    }

    if (menu.ShouldJoinServer())
    {
        // Validate the typed/selected host so raygui textbox corruption can't
        // send a garbage address (e.g. a single stray char) into connect().
        std::string host;
        int port = menu.GetJoinPort();
        if (JoinUri::ParseHostPort(menu.GetJoinIP(), host, port, port > 0 ? port : 27015) &&
            !host.empty())
        {
            // SetServerJoinIP is consumed after PlayHomeScreen() returns, so it
            // must outlive the local menu (GetJoinIP() dangles once menu dies).
            static std::string s_menuJoinHost;
            s_menuJoinHost = host;
            SetServerJoinIP = s_menuJoinHost.c_str();
            SetServerJoinPort = port;
            SetServerJoinFlag = true;
        }
        else
        {
            OZ_WARN("Join: invalid address '%s' — not connecting", menu.GetJoinIP());
        }
    }

    if (menu.ShouldStartServer())
    {
        int port = menu.GetJoinPort();
        if (port <= 0 || port > 65535) port = 27015;
        SetServerJoinIP = "127.0.0.1";
        SetServerJoinPort = port;
        SetServerJoinFlag = true;
        // Launch dedicated server as a subprocess
        int serverPort = port;
        std::string args = "--port " + std::to_string(serverPort) + " --dir GameData";
        // Announce the hosted server to the configured masters.
        for (const auto& m : master::LoadMasterUrls("System/Angels95.ini")) {
            if (m.rfind("http://", 0) == 0)
                args += " --master-http \"" + m + "\"";
            else if (m.rfind("https://", 0) == 0)
                OZ_WARN("Host: ignoring https master '%s' (plain http only)", m.c_str());
            else
                args += " --master " + m;
        }
#ifdef _WIN32
        std::string cmd = "start /B \"\" System\\AngelServ.exe " + args;
#else
        // `start` is a cmd.exe builtin, so the old string reached /bin/sh as the command
        // `start /B "" System\AngelServ.exe ...`, which fails with "start: not found" and
        // reported a failed launch on every POSIX host. Detach explicitly instead: nohup
        // plus & backgrounds it with the child's stdout redirected, so the server does not
        // inherit our terminal and does not die with it.
        std::string cmd = "nohup ./AngelServ " + args + " >/dev/null 2>&1 &";
#endif
        int result = std::system(cmd.c_str());
        if (result == 0) {
#ifdef _WIN32
            OZ_INFO("Launched AngelServ.exe on port %d", serverPort);
#else
            OZ_INFO("Launched AngelServ on port %d", serverPort);
#endif
        } else {
#ifdef _WIN32
            OZ_ERROR("Failed to launch AngelServ.exe");
#else
            OZ_ERROR("Failed to launch AngelServ");
#endif
        }
    }

    OmegaTechData.Deaths = 1;
}

void UpdateEntitiesSim(float dt)
{
    Vector3 playerPos = OmegaTechData.MainCamera.position;

    // Single-pass zone scan for player â€” replaces 4 separate CheckZoneCollision calls
    ZoneManager::Instance().UpdatePlayerRegion(playerPos, g_playerMovement.PlayerBounds);

    // Portal trigger — level-to-level transitions (campaign system)
    if (g_portalCooldown > 0.0f)
        g_portalCooldown -= dt;
    else
    {
        ZonePortal* portal = ZoneManager::Instance().CheckPortalCollision(playerPos, g_playerMovement.PlayerBounds);
        if (portal && !SetSceneFlag)
        {
            OZ_INFO("Portal: entering level '%s'", portal->targetWorld.c_str());
            // Bidirectional portals record the origin so the destination gets a
            // reverse portal back to this exact spot. (g_world_to_load still
            // holds the CURRENT world name until the strncpy below.)
            if (portal->bidirectional) {
                g_portalReturnWorld = g_world_to_load;
                g_portalReturnPos = playerPos;
                g_portalHasReturn = true;
            } else {
                g_portalHasReturn = false;
            }
            strncpy(g_world_to_load, portal->targetWorld.c_str(), sizeof(g_world_to_load) - 1);
            g_world_to_load[sizeof(g_world_to_load) - 1] = '\0';
            g_portalSpawnPos = portal->targetSpawn;
            g_portalTransitionPending = true;
            SetSceneFlag = true;      // LoadWorld runs at end of frame
            g_portalCooldown = 3.0f;  // latch so the trigger can't re-fire mid-transition

            // Tell the server we moved. Without this the client rendered the target
            // world while the server still had the player in world 0 — because
            // ServerPlayer::world_index was written once at creation and never
            // updated. Every collect in the new level was then rejected
            // WRONG_WORLD, every melee hit was dropped for a world mismatch, and the
            // 15 s pickup resync kept re-sending world 0's pickups.
            //
            // The index is looked up by NAME because a portal line carries a world
            // name, not an index. -1 means the server's list does not contain it
            // (offline, or a portal pointing at a world the server does not load) and
            // NetworkRequestWorld then does nothing — so a single-player portal to an
            // unloaded world still works.
            {
                const int want = NetworkWorldIndexByName(portal->targetWorld.c_str());
                if (g_network_enabled && want < 0)
                    OZ_WARN("Portal: target world '%s' is not in the server's world list — "
                            "staying in world %d server-side, so collects there will be "
                            "rejected", portal->targetWorld.c_str(), g_network_world_index);
                NetworkRequestWorld(want);
            }
        }
    }

    // Arm the transient return portal once the player has walked away from it.
    if (g_returnPortalId >= 0)
    {
        const auto& portals = ZoneManager::Instance().GetPortals();
        if (g_returnPortalId < (int)portals.size())
        {
            ZonePortal& rp = ZoneManager::Instance().GetPortals()[g_returnPortalId];
            Vector3 c = {(rp.bounds.min.x + rp.bounds.max.x) * 0.5f,
                         (rp.bounds.min.y + rp.bounds.max.y) * 0.5f,
                         (rp.bounds.min.z + rp.bounds.max.z) * 0.5f};
            float dx = playerPos.x - c.x, dy = playerPos.y - c.y, dz = playerPos.z - c.z;
            if (!rp.enabled && sqrtf(dx*dx + dy*dy + dz*dz) > 3.5f) {
                rp.enabled = true; // armed — walking back in now returns
                OZ_INFO("Portal: return portal armed");
            }
        }
        else g_returnPortalId = -1;
    }

    // Update all pawns via PawnSystem (FSM: IDLE/PATROL/CHASE/RETURN). The
    // collision query is what gives them gravity and solid walls; without it the
    // local FSM never touched position.y and walked through geometry.
    {
        oz::physics::OzoneCollisionQuery pawnQuery;
        PawnSystem::Instance().Update(playerPos, dt, &pawnQuery);
    }

    // Update pickups (respawn timers, player collision).
    // Single-player only. In multiplayer the server owns pickup state and the
    // client additionally requests collects via PickupPawns; running this
    // unconditionally granted every pickup twice and ran two independent
    // respawn clocks that drifted apart.
    if (!g_network_enabled)
        PawnSystem::Instance().UpdatePickups(dt, playerPos, g_playerMovement.PlayerBounds);

    // Pickup collection feedback (console message + flash)
    {
        auto& fb = PawnSystem::Instance().m_pickupFeedback;
        if (fb.collected) {
            OmegaTechTextSystem.Write(TextFormat("Collected: %s", fb.typeName.c_str()));
            fb.collected = false;
        }
    }

    // NOTE: pawn/entity drawing moved out to DrawWorld (once per frame after
    // the fixed-step accumulator) so sim steps never double-draw.

    // Melee stamina regen. Runs every frame alongside the contact-damage
    // check so the pool refills whether or not an NPC is in range.
    LightningEntityManager::Instance().UpdateStamina(dt);

    // Mana / psychic-energy regen for spells. Same placement as stamina: every
    // frame, independent of anything being in range. Neither pool regenerated
    // on the client at all before spells existed, and the server's values never
    // reach the client (Client.cpp sends mana = 0), so without this a cast was
    // permanent.
    LightningEntityManager::Instance().UpdateSpellPools(dt);

    // Check if any pawn is attacking the player (contact damage)
    {
        g_damageCooldown -= dt;
        float damage = 0;
        if (g_damageCooldown <= 0.0f && PawnSystem::Instance().IsPlayerAttacked(playerPos, damage))
        {
            // Equipped armor mitigates incoming damage (diminishing returns).
            // Shared with the server via oz::MitigateDamage so both ends agree.
            auto& lem = LightningEntityManager::Instance();
            float mitigated = oz::MitigateDamage(damage, lem.GetPlayerDefense());
            lem.SetPlayerHealth(std::max(0.0f, lem.GetPlayerHealth() - mitigated));
            g_damageCooldown = 1.0f;
            if (OmegaTechData.DamageFlash <= 0.0f)
                OmegaTechData.DamageFlash = kDamageFlashDuration;
            if (OmegaTechData.Ticker % 2 == 0)
            {
                SoundManager::Instance().PlayChasing();
            }
        }
    }

    // Death / respawn check — respawn is delayed by the world's
    // LevelSettings.respawnTime (default 5s, immediate = 0).
    {
        static float s_respawnTimer = 0.0f;
        if (LightningEntityManager::Instance().GetPlayerHealth() <= 0.0f)
        {
            if (s_respawnTimer <= 0.0f)
            {
                // First death frame: start the countdown, reset stats now.
                LightningEntityManager::Instance().SetPlayerHealth(100.0f);
                LightningEntityManager::Instance().SetPlayerMana(100.0f);
                OmegaTechData.Deaths++;
                OZ_INFO("Player died! Death #%d", OmegaTechData.Deaths);
                OmegaTechTextSystem.Write(TextFormat("You died! Death #%d", OmegaTechData.Deaths));
                float respawnTime = 5.0f;
                const auto& settings = PawnSystem::Instance().GetWorldInfo().settings;
                if (settings.respawnTime >= 0.0f) respawnTime = settings.respawnTime;
                s_respawnTimer = respawnTime;
            }
            else
            {
                s_respawnTimer -= dt;
                if (s_respawnTimer <= 0.0f)
                {
                    s_respawnTimer = 0.0f;
                    PawnSystem::Instance().RespawnPlayerAtStart(OmegaTechData.MainCamera);
                    // Refill items from save
                    LoadSave();
                }
            }
        }
        else s_respawnTimer = 0.0f;
    }
}

void UpdatePlayer()
{
    if (IsKeyDown(KEY_W) || GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_Y) != 0 && !Debug)
    {
        if (g_playerMovement.HeadBob)
        {
            if (OmegaTechData.Ticker % 4 == 0)
            {
                float bob = (g_playerMovement.HeadBobDirection == 1) ? 0.05f : -0.05f;
                OmegaTechData.MainCamera.target.y += bob;
                if (g_playerMovement.HeadBob >= 1)
                    g_playerMovement.HeadBobDirection = 0;
                else if (g_playerMovement.HeadBob <= -1)
                    g_playerMovement.HeadBobDirection = 1;
                g_playerMovement.HeadBob += (g_playerMovement.HeadBobDirection == 1) ? 1 : -1;
            }
        }
        // Footsteps are now gait-aware and .ozls-driven (walk_sound /
        // run_sound on Player.ozls), with the preloaded WalkingSound as the
        // fallback when neither is authored.
        PlayerController::Instance().PlayWalkLoop(g_playerMovement.isSprinting);
    }
    else
    {
        PlayerController::Instance().StopWalkLoop();
    }

    g_playerMovement.UpdateBounds(OmegaTechData.MainCamera);
}

void SaveGame()
{
    wstring TFlags = L"";

    for (int i = 0; i <= 99; i++)
    {
        if (ToggleFlags[i].Value == 1)
        {
            TFlags += L'1';
        }
        if (ToggleFlags[i].Value == 0)
        {
            TFlags += L'0';
        }
    }

    TFlags += L':';

    // EntityManager hotbar+equipment state (replaces owned flags)
    {
        std::string emState = LightningEntityManager::Instance().SerializeState();
        TFlags += wstring(emState.begin(), emState.end());
    }
    TFlags += L':';
    // Backpack data
    TFlags += L':';
    for (int i = 0; i < BACKPACK_SLOTS; i++)
    {
        TFlags += to_wstring(gInventory.backpack[i].itemId) + L',' + to_wstring(gInventory.backpack[i].quantity) + L';';
    }
    TFlags += L':' + to_wstring(gInventory.coins);

    wofstream Outfile;
    Outfile.open("GameData/Saves/TF.sav");
    Outfile << TFlags;

    wstring Position = to_wstring(OmegaTechData.MainCamera.position.x) + L':' +
                       to_wstring(OmegaTechData.MainCamera.position.y) + L':' +
                       to_wstring(OmegaTechData.MainCamera.position.z) + L':' +
                       to_wstring(OmegaTechData.LevelIndex) + L':';

    wofstream Outfile1;
    Outfile1.open("GameData/Saves/POS.sav");
    Outfile1 << Position;
}

void LoadSave()
{
    wstring TFlags = LoadFile("GameData/Saves/TF.sav");
    size_t tfLen = TFlags.size();

    for (int i = 0; i <= 99 && i < (int)tfLen; i++)
    {
        if (TFlags[i] == L'1')
        {
            ToggleFlags[i].Value = 1;
        }
        if (TFlags[i] == L'0')
        {
            ToggleFlags[i].Value = 0;
        }
    }

    // Load EntityManager hotbar+equipment state (after first ':')
    size_t emSectionStart = TFlags.find(L':', 100);
    size_t emSectionEnd = string::npos;
    if (emSectionStart != string::npos && emSectionStart + 1 < TFlags.size()) {
        // EntityManager state ends at the next ':' (which separates from backpack data)
        emSectionEnd = TFlags.find(L':', emSectionStart + 1);
        if (emSectionEnd != string::npos) {
            std::string emState(TFlags.begin() + emSectionStart + 1, TFlags.begin() + emSectionEnd);
            LightningEntityManager::Instance().DeserializeState(emState);
        }
    }

    // Load backpack data (after EntityManager state section)
    size_t bpStart = emSectionEnd;
    if (bpStart != string::npos && bpStart + 1 < TFlags.size())
    {
        wstring bpData = TFlags.substr(bpStart + 1);
        size_t secondColon = bpData.find(L':');
        wstring slotData = (secondColon != string::npos) ? bpData.substr(0, secondColon) : bpData;
        // Parse slot data: "id,qty;id,qty;..."
        size_t pos = 0;
        int slotIdx = 0;
        while (pos < slotData.size() && slotIdx < BACKPACK_SLOTS)
        {
            size_t semi = slotData.find(L';', pos);
            if (semi == string::npos)
                break;
            wstring pair = slotData.substr(pos, semi - pos);
            size_t comma = pair.find(L',');
            if (comma != string::npos)
            {
                try {
                    int id = stoi(pair.substr(0, comma));
                    int qty = stoi(pair.substr(comma + 1));
                    gInventory.backpack[slotIdx].itemId = id;
                    gInventory.backpack[slotIdx].quantity = qty;
                } catch (...) { }
            }
            pos = semi + 1;
            slotIdx++;
        }
        // Coins
        if (secondColon != string::npos)
        {
            wstring coinStr = bpData.substr(secondColon + 1);
            if (!coinStr.empty()) {
                try { gInventory.coins = stoi(coinStr); } catch (...) { }
            }
        }
    }

    wstring Position = LoadFile("GameData/Saves/POS.sav");

    if (!Position.empty())
    {
        try {
            OmegaTechData.LevelIndex = int(ToFloat(WSplitValue(Position, 3)));
        } catch (...) {
            OmegaTechData.LevelIndex = 1;
        }

        SetCameraFlag = true;

        try {
            int X = ToFloat(WSplitValue(Position, 0));
            int Y = ToFloat(WSplitValue(Position, 1));
            int Z = ToFloat(WSplitValue(Position, 2));
            SetCameraPos = {float(X), float(Y), float(Z)};
        } catch (...) {
            SetCameraPos = {0.0f, 10.0f, 0.0f};
        }
    }
}

// Sweep every active projectile one frame-step ahead and test against OZONE
// brush collision volumes + heightmap terrain. On a hit the projectile is
// deactivated and an impact burst + decal are spawned at the contact point.
// Position integration itself stays in PawnSystem::UpdateProjectiles.
static void SweepProjectilesVsWorld(float dt)
{
    auto& loader = OzoneLoader::Instance();
    const auto& volumes = loader.GetCollisionVolumes();
    bool hasTerrain = loader.HasHeightmap();
    auto& projectiles = PawnSystem::Instance().GetProjectiles();

    for (auto& p : projectiles) {
        if (!p.active) continue;

        Vector3 next = {p.position.x + p.velocity.x * dt,
                        p.position.y + p.velocity.y * dt,
                        p.position.z + p.velocity.z * dt};

        float bestT = 1.0f;
        Vector3 bestNormal = {0, 1, 0};
        Vector3 hitPos = {0, 0, 0};
        bool hit = false;
        bool terrainHit = false;

        for (const auto& cv : volumes) {
            if (cv.isHeightmap) continue;
            float t;
            Vector3 n;
            if (SegmentVsAABB(p.position, next, cv.aabb, t, n) && t < bestT) {
                bestT = t;
                bestNormal = n;
                hit = true;
            }
        }

        if (!hit && hasTerrain) {
            float groundY = loader.SampleHeightmapY(next.x, next.z);
            if (next.y <= groundY) {
                hit = true;
                terrainHit = true;
                bestT = 1.0f;
                bestNormal = {0, 1, 0};
                hitPos = {next.x, groundY, next.z};
            }
        }

        if (hit) {
            if (!terrainHit)
                hitPos = Vector3Lerp(p.position, next, bestT);
            p.active = false;
            CombatFX::Instance().SpawnImpact(hitPos, bestNormal,
                                             Color{255, 180, 60, 255}, 12, 6.0f);
            CombatFX::Instance().AddDecal(hitPos, bestNormal, 0.35f);
        }
    }
}

void DrawWorld()
{
    BeginTextureMode(Target);
    ClearBackground(BLACK);

// Detect sky zone BEFORE 3D mode begins (needed for sky camera setup)
    PawnSystem::Instance().UpdateSkyZone(
        OmegaTechData.MainCamera.position,
        g_playerMovement.PlayerBounds);
    bool inSkyZone = PawnSystem::Instance().IsInSkyZone();

    BeginMode3D(OmegaTechData.MainCamera);

// ---------------------------------------------------------------------------
// 3D Skybox Cube - drawn first with depth-write disabled so it sits behind all
// world geometry. Top/bottom use the active skybox (cap), 4 sides use the side
// texture from LevelInfo (g_skySideTex).
//
// The cube itself - face geometry, per-face rotations, distance, fog and the
// single draw loop - lives in oz::Skybox (Source/Renderer/Skybox.hpp), which the
// editor shares. This block is only the client's half of that contract: WHICH
// textures to draw, and the world's fog.
//
// Precedence: the level skybox (levelinfo / set_skybox / world default) is
// authoritative; a zone's authored skybox is only used when the level defines
// none. Previously the zone texture always won, so a saved levelinfo skybox
// appeared to be ignored inside sky zones.
// ---------------------------------------------------------------------------
{
    Texture2D capTex = {0};
    if (WorldModels.Skybox.id > 0 && OmegaTechData.SkyboxEnabled) {
        capTex = WorldModels.Skybox;
    } else if (inSkyZone) {
        SkyZoneNode* sky = PawnSystem::Instance().GetActiveSkyZone();
        if (sky && sky->skyboxTex.id > 0)
            capTex = sky->skyboxTex;
        else if (WorldModels.Skybox.id > 0)
            capTex = WorldModels.Skybox;
    }

    // Sides use an authored side texture when available; otherwise oz::Skybox
    // falls back to the cap so the sky renders all around (previously they drew a
    // flat colour and appeared black).
    if (capTex.id > 0 || g_skySideTex.id > 0) {
        // The world's fog is owned by OzoneLoader (raylib has no GetShaderValue)
        // and UpdateLightSources() is the single mirror point into SurfaceMaterial;
        // the sky is a third consumer of the SAME state, read back from the owner
        // rather than from a local. OzoneLoader owns both because Core.hpp sets fog
        // on LitFog from three sites (startup, zone entry, zone exit) and one
        // mirror point cannot drift from three writers.
        oz::SkyboxFog fog;
        fog.active = true;
        OzoneLoader::Instance().GetWorldFog(fog.color, fog.start, fog.end,
                                            fog.density, fog.intensity);

        // Culling ON for the rest of the frame: the generated OZONE brushes have
        // correct winding and want it. Imported meshes opt back out via
        // oz::ScopedCullOff in oz::Mesh::Draw (see Renderer/CullState.hpp - this
        // used to be a raw call that nothing ever turned back off).
        oz::Skybox::Instance().Draw(OmegaTechData.MainCamera.position,
                                    capTex, g_skySideTex,
                                    /*cullBackfaces=*/true, &fog);
    }
}

    // -----------------------------------------------------------------------
    // SKY PASS — render SURF_FAKEBACKDROP brushes from the main camera with
    // depth disabled so the skybox shows through behind them.
    // -----------------------------------------------------------------------
if (inSkyZone)
    {
        rlDisableDepthMask();
        OzoneLoader::Instance().DrawZoneGeometry(OmegaTechData.MainCamera);
        rlEnableDepthMask();
    }

    // GameplaySoundZone - zone music/ambience/enter-sfx profiles plus the
    // per-frame music stream pump are owned by SoundManager.
    SoundManager::Instance().UpdateSoundZones(ZoneManager::Instance().GetPlayerRegion());
    SoundManager::Instance().Update();

    OzoneLoader::Instance().DrawWorldGeometry(OmegaTechData.MainCamera);

    // Additive re-draw of every SURF_GLOW face, after the opaque world so the
    // halo lands on top of the composited image. No-op unless a surface actually
    // carries the flag, so this costs two vector scans on a normal frame.
    OzoneLoader::Instance().DrawGlowGeometry(OmegaTechData.MainCamera);

    // OZONE brush collision — chunk-accelerated query, delegated to the
    // physics module via the adapter below.
    {
        oz::physics::OzoneCollisionQuery q;
        Vector3 cp = OmegaTechData.MainCamera.position;
        // Effective eye height shrinks while crouching (dynamic player stance)
        float playerFeet = cp.y - g_playerMovement.EyeHeight;
        if (oz::physics::PlayerPhysics().TestObstacleOverlap(g_playerMovement.PlayerBounds,
                                                             playerFeet, q))
            ObjectCollision = true;
    }

    // OZONE ground clamp -- delegated to the physics module (heightmap first,
    // then brush primitives, as supplied by the adapter).
    if (!g_playerMovement.isFlying && !g_playerMovement.isNoClip && !g_playerMovement.isClimbing)
    {
        oz::physics::OzoneCollisionQuery q;
        oz::physics::PlayerPhysics pphys;
        oz::physics::PlayerPhysics::Motion motion;
        motion.onGround = g_playerMovement.onGround;
        motion.velocityY = g_playerMovement.velocityY;
        pphys.ClampToGround(OmegaTechData.MainCamera, motion, g_playerMovement.EyeHeight, q);
        g_playerMovement.onGround = motion.onGround;
        g_playerMovement.velocityY = motion.velocityY;
    }

    UpdatePlayer();

    // Collision debug overlay (/showcollisions)
    if (g_showCollisionDebug)
    {
        // OZONE brush collision volumes
        auto &vols = OzoneLoader::Instance().GetCollisionVolumes();
        for (auto &cv : vols)
            DrawBoundingBox(cv.aabb, PURPLE);

        // Heightmap bounds
        if (OzoneLoader::Instance().HasHeightmap())
        {
            auto &hmModel = OzoneLoader::Instance().GetHeightmapModel();
            Vector3 hmPos = OzoneLoader::Instance().GetHeightmapPosition();
            float hmScale = OzoneLoader::Instance().GetHeightmapScale();
            BoundingBox hmBox = GetMeshBoundingBox(hmModel.meshes[0]);
            hmBox.min.x = hmPos.x + hmBox.min.x * hmScale;
            hmBox.min.y = hmPos.y + hmBox.min.y * hmScale;
            hmBox.min.z = hmPos.z + hmBox.min.z * hmScale;
            hmBox.max.x = hmPos.x + hmBox.max.x * hmScale;
            hmBox.max.y = hmPos.y + hmBox.max.y * hmScale;
            hmBox.max.z = hmPos.z + hmBox.max.z * hmScale;
            DrawBoundingBox(hmBox, ORANGE);
        }

        // Zone volumes
        for (auto &zone : ZoneManager::Instance().GetZones())
            DrawBoundingBox(zone.bounds, BLUE);

        // Player start markers
        for (auto &ps : PawnSystem::Instance().GetPlayerStarts())
            DrawCubeWires(ps.position, 0.5f, 1.0f, 0.5f, GREEN);
    }

    // Combat FX — projectile rendering, impacts, decals, muzzle flash and
    // remote players all live here so they render inside the 3D camera pass.
    {
        CombatFX::Instance().Update(GetFrameTime());
        PawnSystem::Instance().DrawProjectiles(OmegaTechData.MainCamera, OmegaTechData.Lights);
        CombatFX::Instance().Draw3D(OmegaTechData.MainCamera);
        DrawRemotePlayers3D(OmegaTechData.Lights);
    }

    if (Debug)
    {
        DrawLights();
    }
    else
    {
        // Fixed 60 Hz timestep for the client simulation. Physics, pawn FSM,
        // projectiles and portal scans advance in discrete 1/60s steps (up to 4
        // per render frame to avoid an unbounded catch-up spiral) so gameplay
        // is frame-rate independent. Rendering happens once per frame below.
        static double s_sim_accumulator = 0.0;
        static constexpr double kSimTick = 1.0 / 60.0;
        s_sim_accumulator += GetFrameTime();
        int steps = 0;
        while (s_sim_accumulator >= kSimTick && steps < 4) {
            s_sim_accumulator -= kSimTick;
            UpdateEntitiesSim(static_cast<float>(kSimTick));
            // Wall/terrain impact sweep, then update projectiles (age, movement, gravity)
            SweepProjectilesVsWorld(static_cast<float>(kSimTick));
            PawnSystem::Instance().UpdateProjectiles(static_cast<float>(kSimTick));
            ++steps;
        }
        if (steps >= 4 && s_sim_accumulator >= kSimTick)
            s_sim_accumulator = 0.0; // drift guard: discard excess catch-up

        // GameEngine.ParticleEmitter — simulated in isolation *after* the
        // weapon/NPC/projectile ticks so particles cannot re-enter gameplay.
        // The user's Particles toggle is pushed in here, once a frame, because
        // it is a header-static and every TU including Settings.hpp has its own
        // copy of it.
        OzParticleSimulationManager::Instance().SetEnabled(ParticlesEnabled);
        OzParticleSimulationManager::Instance().Update(GetFrameTime());

        // Ambient weather spawns around the camera, so it needs the camera and
        // is deliberately NOT part of the manager's Update(): that function is
        // fed only emitter definitions and must stay free of camera/gameplay
        // input. Gated on the user's global Particles toggle, which is a master
        // switch and is not derived from the level.
        if (ParticlesEnabled)
            OzParticleSimulationManager::Instance().UpdateWeather(
                GetFrameTime(), OmegaTechData.MainCamera.position);

        // Draw pawns / entity billboards exactly once per frame (kept out of
        // the sim steps so a catch-up frame doesn't re-draw the scene).
        PawnSystem::Instance().DrawAll(OmegaTechData.MainCamera, OmegaTechData.Lights);
        PawnSystem::Instance().DrawEntities(OmegaTechData.MainCamera, OmegaTechData.Lights, OmegaTechData.WindShader);
        DrawLightFlares(OmegaTechData.MainCamera);

        // First-person weapon view-model (drawn on top of the world).
        // Suppressed for --shot captures: the view-model is camera-locked UI,
        // not the level, and it covers a third of the frame.
        if (!(g_shot.active && g_shot.hideHud)) {
            oz::ViewModel::Instance().Update(GetFrameTime());
            oz::ViewModel::Instance().Draw(OmegaTechData.MainCamera, OmegaTechData.Lights);
        }
    }
    // Collision rollback - delegated to the physics module.
    if (ObjectCollision)
    {
        oz::physics::PlayerPhysics().RestorePosition(
            OmegaTechData.MainCamera,
            g_playerMovement.OldX, g_playerMovement.OldY, g_playerMovement.OldZ,
            g_playerMovement.isNoClip);
        ObjectCollision = false;
    }

    // Zone reverb - the simulated DSP (mix/decay + music ducking) is applied
    // by SoundManager on zone transitions.
    SoundManager::Instance().UpdateReverb(ZoneManager::Instance().GetPlayerRegion());

    // LightningScript entity tick
    {
        float dt = GetFrameTime();
        LightningEntityManager::Instance().Update(dt);

        // Sync pending script effects into PawnSystem's active SkyZoneNode
        PawnSystem::Instance().SyncSkyboxState();

        // Apply pending Fog changes from script contexts
        auto &lem = LightningEntityManager::Instance();

        // restore_fog: hand fog back to the level defaults. The three restore_*
        // opcodes used to clear the pending value instead of signalling the host,
        // so they never restored anything.
        if (lem.TakePendingFogRestore()) {
            static int fogD = GetShaderLocation(OmegaTechData.Lights, "fogDensity");
            static int fogC = GetShaderLocation(OmegaTechData.Lights, "fogColor");
            static int fogS = GetShaderLocation(OmegaTechData.Lights, "fogStart");
            static int fogE = GetShaderLocation(OmegaTechData.Lights, "fogEnd");
            static int fogI = GetShaderLocation(OmegaTechData.Lights, "fogIntensity");
            const LevelFogState& L = g_levelFog;
            SetShaderValue(OmegaTechData.Lights, fogS, &L.fogStart, SHADER_UNIFORM_FLOAT);
            SetShaderValue(OmegaTechData.Lights, fogE, &L.fogEnd, SHADER_UNIFORM_FLOAT);
            SetShaderValue(OmegaTechData.Lights, fogD, &L.fogDensity, SHADER_UNIFORM_FLOAT);
            SetShaderValue(OmegaTechData.Lights, fogC, L.fogColor, SHADER_UNIFORM_VEC3);
            SetShaderValue(OmegaTechData.Lights, fogI, &L.fogIntensity, SHADER_UNIFORM_FLOAT);
            // PUBLISH, or SurfaceMaterial keeps the previous fog.
            //
            // OzoneLoader owns the world's fog because raylib has no
            // GetShaderValue, and UpdateLightSources() is the single mirror point
            // into the surface program. Writing LitFog's uniforms directly - which
            // is what both script opcodes used to do - leaves OzoneLoader stale, so
            // a flagged brush (including every painted backdrop) rendered with the
            // STARTUP fog while the lit geometry around it used the level's. The
            // two programs disagreed in the same frame, which is exactly the class
            // of bug the ambient leak and the backdrop hack produced.
            //
            // This was measured: with set_fog active, LitFog carried density 0.0022
            // while SurfaceMaterial still carried the startup 1.0.
            OzoneLoader::Instance().SetWorldFog(L.fogColor, L.fogStart, L.fogEnd,
                                                L.fogDensity, L.fogIntensity);
            FogEnabled = true;      // the post-process is dead either way; the flags
            FogIntensity = 0.0f;    // track the real state rather than lie about it
            OZ_INFO("LightningScript: restore_fog - reverted to level default");
        }

        if (lem.HasPendingFog())
        {
            static int fogDensityLoc = GetShaderLocation(OmegaTechData.Lights, "fogDensity");
            static int fogColorLoc = GetShaderLocation(OmegaTechData.Lights, "fogColor");
            float density = lem.PendingFogDensity();
            float color[3] = {lem.PendingFogR(), lem.PendingFogG(), lem.PendingFogB()};
            SetShaderValue(OmegaTechData.Lights, fogDensityLoc, &density, SHADER_UNIFORM_FLOAT);
            SetShaderValue(OmegaTechData.Lights, fogColorLoc, color, SHADER_UNIFORM_VEC3);
            // PUBLISH. See the note on restore_fog above: writing only this
            // program leaves SurfaceMaterial - and therefore every painted backdrop
            // and every surface-flagged brush - on the startup fog.
            OzoneLoader::Instance().SetWorldFog(color, g_levelFog.fogStart, g_levelFog.fogEnd,
                                                density, 1.0f);
            FogEnabled = true;
            FogIntensity = (density > 0) ? density : 0.3f;
            FogTint = {(unsigned char)(color[0] * 255), (unsigned char)(color[1] * 255),
                       (unsigned char)(color[2] * 255), 255};
            lem.ClearPendingFog();
        }

        // restore_skybox: drop the script override and go back to the level skybox.
        // This used to set __skybox to "", which PopPendingSkybox rejects as
        // "nothing pending", so the skybox was never restored.
        if (lem.TakePendingSkyboxRestore()) {
            SkyZoneNode* sky = PawnSystem::Instance().GetActiveSkyZone();
            if (sky && sky->skyboxTex.id > 0) {
                UnloadTexture(sky->skyboxTex);
                sky->skyboxTex = Texture2D{0};
            }
            // WorldModels.Skybox is left alone: it is the level's own texture and
            // is still what the sky pass falls back to.
            OmegaTechData.SkyboxEnabled = (WorldModels.Skybox.id > 0);
            OZ_INFO("LightningScript: restore_skybox - reverted to level skybox");
        }

        // Apply pending Skybox changes from script contexts.
        // Only an explicit script request (set_skybox) overrides the levelinfo
        // skybox; the zone's authored skyboxPath is kept for inside-zone
        // rendering by the sky pass and must NOT clobber the global level skybox
        // every frame (that made a saved levelinfo skybox appear ignored).
        if (lem.HasPendingSkybox()) {
            std::string path = lem.PendingSkybox();
            lem.ClearPendingSkybox();
            if (!path.empty()) {
                OZ_INFO("LightningScript: loading skybox '%s'", path.c_str());
                Texture2D newSky = LoadTextureWithFallback(path.c_str());
                if (newSky.id > 0) {
                    SkyZoneNode* sky = PawnSystem::Instance().GetActiveSkyZone();
                    if (sky) {
                        if (sky->skyboxTex.id > 0)
                            UnloadTexture(sky->skyboxTex);
                        sky->skyboxTex = newSky;
                    }
                    if (WorldModels.Skybox.id > 0)
                        UnloadTexture(WorldModels.Skybox);
                    WorldModels.Skybox = newSky;
                    OmegaTechData.SkyboxEnabled = true;
                } else {
                    OZ_WARN("LightningScript: skybox '%s' not found", path.c_str());
                }
            }
        }

        // Apply pending Ambient changes from script contexts
        if (lem.TakePendingAmbientRestore()) {
            static int ambLoc = GetShaderLocation(OmegaTechData.Lights, "ambient");
            float amb[4];
            FillDefaultAmbient(amb);   // the world default, not a hardcoded 1.0
            SetShaderValue(OmegaTechData.Lights, ambLoc, amb, SHADER_UNIFORM_VEC4);
            OzoneLoader::Instance().SetWorldAmbient(amb[0], amb[1], amb[2], amb[3]);
            OZ_INFO("LightningScript: restore_ambient - reverted to level default");
        }
        if (lem.HasPendingAmbient())
        {
            float amb[4] = {lem.PendingAmbientR(), lem.PendingAmbientG(), lem.PendingAmbientB(), 1.0f};
            static int ambientLoc = GetShaderLocation(OmegaTechData.Lights, "ambient");
            SetShaderValue(OmegaTechData.Lights, ambientLoc, amb, SHADER_UNIFORM_VEC4);
            OzoneLoader::Instance().SetWorldAmbient(amb[0], amb[1], amb[2], amb[3]);
            lem.ClearPendingAmbient();
        }

        // Zone music authored on a skyzone def (`music = "..."`).
        if (lem.HasPendingMusic()) {
            const std::string track = lem.PendingMusic();
            lem.ClearPendingMusic();
            SoundManager::Instance().PlayWorldMusic(track);
        }

        // Zone environment override application (from combined player region)
        {
            auto& region = ZoneManager::Instance().GetPlayerRegion();
            bool inEnvZone = region.combinedEnv.applyFog || region.combinedEnv.applyAmbient;
            std::string envZoneName = (region.primaryZoneId >= 0) ? std::to_string(region.primaryZoneId) : "";

            if (inEnvZone && g_activeEnvZone != envZoneName)
            {
                auto &eo = region.combinedEnv;
                OZ_DEBUG("Zone env: applyFog=%d applyAmbient=%d", eo.applyFog, eo.applyAmbient);
                if (eo.applyFog && OmegaTechData.Lights.id > 0)
                {
                    float fc[3] = {eo.fogR / 255.0f, eo.fogG / 255.0f, eo.fogB / 255.0f};
                    float fd = eo.fogDensity;
                    static int fogDensityLoc = GetShaderLocation(OmegaTechData.Lights, "fogDensity");
                    static int fogColorLoc = GetShaderLocation(OmegaTechData.Lights, "fogColor");
                    static int fogStartLoc = GetShaderLocation(OmegaTechData.Lights, "fogStart");
                    static int fogEndLoc = GetShaderLocation(OmegaTechData.Lights, "fogEnd");
                    SetShaderValue(OmegaTechData.Lights, fogColorLoc, fc, SHADER_UNIFORM_VEC3);
                    SetShaderValue(OmegaTechData.Lights, fogDensityLoc, &fd, SHADER_UNIFORM_FLOAT);
                    float fs = eo.fogStart, fe = eo.fogEnd;
                    SetShaderValue(OmegaTechData.Lights, fogStartLoc, &fs, SHADER_UNIFORM_FLOAT);
                    SetShaderValue(OmegaTechData.Lights, fogEndLoc, &fe, SHADER_UNIFORM_FLOAT);
                    // Publish alongside the uniform so the surface program gets
                    // the same fog from UpdateLightSources' single mirror point.
                    OzoneLoader::Instance().SetWorldFog(fc, fs, fe, fd);
                    FogEnabled = true;
                    FogIntensity = (fd > 0) ? fd : 0.3f;
                    FogTint = {(unsigned char)eo.fogR, (unsigned char)eo.fogG, (unsigned char)eo.fogB, 255};
                }
                if (eo.applyAmbient && OmegaTechData.Lights.id > 0)
                {
                    float amb[4] = {eo.ambR / 255.0f * eo.ambIntensity,
                                    eo.ambG / 255.0f * eo.ambIntensity,
                                    eo.ambB / 255.0f * eo.ambIntensity, 1.0f};
                    static int ambientLoc = GetShaderLocation(OmegaTechData.Lights, "ambient");
                    SetShaderValue(OmegaTechData.Lights, ambientLoc, amb, SHADER_UNIFORM_VEC4);
                    OzoneLoader::Instance().SetWorldAmbient(amb[0], amb[1], amb[2], amb[3]);
                }
                g_activeEnvZone = envZoneName;
            }
            else if (!inEnvZone && !g_activeEnvZone.empty())
            {
                // Exited env zone â€” restore defaults from WorldInfo
                OZ_DEBUG("Zone env: restoring WorldInfo defaults");
                if (OmegaTechData.Lights.id > 0)
                {
                    auto& wi = PawnSystem::Instance().GetWorldInfo();
                    float defFogColor[3] = {wi.defaultEnv.fogR / 255.0f,
                                            wi.defaultEnv.fogG / 255.0f,
                                            wi.defaultEnv.fogB / 255.0f};
                    float defFogDensity = wi.defaultEnv.fogDensity;
                    float defFogStart = wi.defaultEnv.fogStart, defFogEnd = wi.defaultEnv.fogEnd;
                    static int fogDensityLoc = GetShaderLocation(OmegaTechData.Lights, "fogDensity");
                    static int fogColorLoc = GetShaderLocation(OmegaTechData.Lights, "fogColor");
                    static int fogStartLoc = GetShaderLocation(OmegaTechData.Lights, "fogStart");
                    static int fogEndLoc = GetShaderLocation(OmegaTechData.Lights, "fogEnd");
                    SetShaderValue(OmegaTechData.Lights, fogColorLoc, defFogColor, SHADER_UNIFORM_VEC3);
                    SetShaderValue(OmegaTechData.Lights, fogDensityLoc, &defFogDensity, SHADER_UNIFORM_FLOAT);
                    SetShaderValue(OmegaTechData.Lights, fogStartLoc, &defFogStart, SHADER_UNIFORM_FLOAT);
                    SetShaderValue(OmegaTechData.Lights, fogEndLoc, &defFogEnd, SHADER_UNIFORM_FLOAT);
                    OzoneLoader::Instance().SetWorldFog(defFogColor, defFogStart,
                                                        defFogEnd, defFogDensity);
                }
                OzoneLoader::Instance().SetWorldAmbient(0.1f, 0.1f, 0.1f, 1.0f);
                FogEnabled = false;
                FogIntensity = 0.3f;
                FogTint = {200, 200, 210, 255};
                g_activeEnvZone.clear();
            }
        }
    // Route script HUD messages (msg opcode) to the on-screen text system
        if (!lem.PendingMessage().empty())
        {
            OmegaTechTextSystem.Write(lem.PendingMessage());
            lem.ClearPendingMessage();
        }

        // Scripted damage flash (damage opcode)
        if (lem.PlayerHurt())
        {
            lem.ClearPlayerHurt();
            if (OmegaTechData.DamageFlash <= 0.0f)
                OmegaTechData.DamageFlash = kDamageFlashDuration;
            // Same routed handler as the network PLAYER_HURT path, so a script
            // `damage` and a server hit cannot double-play and both respect the
            // authored hurt_sound / death_sound split.
            PlayerController::Instance().PlayHurt(
                LightningEntityManager::Instance().GetPlayerHealth() <= 0.0f);
        }
    }

    EndMode3D();
    EndTextureMode();

    if (SetSceneFlag)
    {
        OmegaTechData.LevelIndex = SetSceneId;
        LoadWorld();
        if (g_portalTransitionPending)
        {
            // Arriving via portal: place player at the portal's target spawn,
            // preserving view direction.
            Vector3 oldPos = OmegaTechData.MainCamera.position;
            OmegaTechData.MainCamera.position = g_portalSpawnPos;
            // Never arrive under the terrain: lift the camera to the OZONE
            // heightmap surface at this XZ when the raw spawn sits below grade.
            float groundY = OzoneLoader::Instance().HasHeightmap()
                                ? OzoneLoader::Instance().SampleHeightmapY(g_portalSpawnPos.x, g_portalSpawnPos.z)
                                : -99999.0f;
            if (groundY > -50000.0f &&
                OmegaTechData.MainCamera.position.y < groundY + g_playerMovement.EyeHeight)
                OmegaTechData.MainCamera.position.y = groundY + g_playerMovement.EyeHeight;
            OmegaTechData.MainCamera.target.x += OmegaTechData.MainCamera.position.x - oldPos.x;
            OmegaTechData.MainCamera.target.y += OmegaTechData.MainCamera.position.y - oldPos.y;
            OmegaTechData.MainCamera.target.z += OmegaTechData.MainCamera.position.z - oldPos.z;
            // Reset movement state to the arrival point so the collision
            // "restore" path can't yank the player back to a stale position
            // from the previous world.
            g_playerMovement.OldX = OmegaTechData.MainCamera.position.x;
            g_playerMovement.OldY = OmegaTechData.MainCamera.position.y;
            g_playerMovement.OldZ = OmegaTechData.MainCamera.position.z;
            g_playerMovement.velocityY = 0.0f;
            g_playerMovement.onGround = true;
            g_portalTransitionPending = false;

            // Bidirectional portal: drop a return portal at the arrival point.
            // It starts disabled and arms once the player steps away (the
            // arming check lives in the portal trigger block above).
            if (g_portalHasReturn && !g_portalReturnWorld.empty())
            {
                ZonePortal rp;
                rp.targetWorld = g_portalReturnWorld;
                rp.targetSpawn = g_portalReturnPos;
                rp.bidirectional = true;
                rp.enabled = false; // armed after the player moves away
                Vector3 c = OmegaTechData.MainCamera.position;
                rp.bounds = {
                    {c.x - 1.5f, c.y - 1.5f, c.z - 1.5f},
                    {c.x + 1.5f, c.y + 1.5f, c.z + 1.5f}
                };
                g_returnPortalId = ZoneManager::Instance().AddPortal(rp);
                OZ_INFO("Portal: return portal to '%s' placed at arrival point",
                        g_portalReturnWorld.c_str());
            }
            else g_returnPortalId = -1;
        }
        SetSceneFlag = false;
    }

    if (SetCameraFlag)
    {
        OmegaTechData.MainCamera.position = SetCameraPos;
        SetCameraFlag = false;
    }

    if (OmegaTechData.Ticker != 60)
    {
        OmegaTechData.Ticker++;
    }
    else
    {
        OmegaTechData.Ticker = 0;
    }
}
