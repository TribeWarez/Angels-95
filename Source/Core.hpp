#include "Data.hpp"
#include "Log.hpp"
#include "Physics/PlayerPhysics.hpp"
#include "Client/JoinUri.hpp"
#include "Network/NetworkSession.hpp"
#include "PlayerProfile.hpp"
#include "Menu/CharacterRoster.hpp"
#include "Renderer/OzAssetMapper.hpp"
#include "Audio/SoundManager.hpp"
#include "World/OzOzoneLoader.hpp"
#include "Pawn/OzPawnSystem.hpp"
#include "Pawn/CombatMath.hpp"
#include "Package/PackageAssetLoader.hpp"
#include "Material/MaterialRegistry.hpp"
#include "Client/MasterList.hpp"
#include "Renderer/EngineBillboard.hpp"
#include "Renderer/ViewModel.hpp"
#include "Renderer/PlayerModel.hpp"
#include "Renderer/CullState.hpp"
#include "Particle/OzParticleSimulationManager.hpp"
#include "Pawn/AngelPlayer/PlayerController.hpp"
#include "Script/LightningEntityRegistry.hpp"
#include "Script/LightningEntityManager.hpp"
#include "Events/EventBus.hpp"
#include "Events/TriggerSystem.hpp"
#include "Game/GameManager.hpp"
#include "Game/GameBoxedTypes.hpp"
#include "Game/GameOpcodes.hpp"

#include "raymath.h"
#include "Renderer/rlights/rlights.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

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

// Set by the title/pause menu's Quit. The outer loop checks this and falls
// through to the single teardown block + CloseWindow at the end of main().
bool g_quitRequested = false;

// World name restored from POS.sav (the field appended after level). Empty when
// the save predates the field or no save exists.
std::string g_savedWorldName;

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
        // Brush obstacle test. Uses the box query, not the point query: the
        // point form takes (worldX, worldZ) and this call used to pass the
        // player's X max to the worldZ parameter, so obstacles were only found
        // when |x - z| happened to fall inside the 3x3 cell window. Worlds whose
        // spawn sits off that diagonal (World_endless_snow at z=-410,
        // Dust_Ravine, Dessert_Dreams) had no wall collision at all.
        chunkMgr.GetVolumesNearBox(player.min.x, player.min.z,
                                   player.max.x, player.max.z, nearIndices);
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

EngineData AngelTechData;

bool LoadFlag = false;

// STILL HARDCODED FILE PATH Loading for Worlds
// TODO: abreviate into handling by OzOzoneLoader and PawnBased/LightningScript Based object loading  // NOLINT

auto LoadWorld()
{
    OZ_INFO("LoadWorld: loading world %d (frame=%llu)", AngelTechData.LevelIndex, (unsigned long long)AngelTechData.Ticker);
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
        AngelTechData.DamageFlash = 0.0f;

        LightningEntityManager::Instance().SetPlayerHealth(100.0f);

        AngelTechData.SkyboxEnabled = false;

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
            AngelTechData.SkyboxEnabled = (WorldModels.Skybox.id > 0);
            if (AngelTechData.SkyboxEnabled)
                OZ_INFO("World skybox: loaded %sModels/Skybox.png", assetPrefix.c_str());
        }

        // Clear all existing entities before loading new world
        PawnSystem::Instance().ClearLights();
        PawnSystem::Instance().ClearPlayerStarts();
        PawnSystem::Instance().ClearPickups();
        ZoneManager::Instance().ClearZones();
        ZoneManager::Instance().ClearPortals();
        oz::events::TriggerSystem::Instance().Clear();
        PawnSystem::Instance().ClearEmitters();
        PawnSystem::Instance().ClearParticleEmitters();
        PawnSystem::Instance().ClearPathNodes();
        PawnSystem::Instance().ClearWindZones();
        // Sky zones are per-world too. This call was missing, so every transition
        // kept the previous world's zones (all worlds are authored near the
        // origin, so the old zone matched first and won) and leaked its sky
        // texture. It is also the only in-source caller of ClearSkyZones.
        PawnSystem::Instance().ClearSkyZones();
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

            // EventBus: world.load. s0 carries the world name for a global game
            // script to read; the bridge finds no def by that name, so nothing is
            // dispatched by default.
            {
                oz::events::Event ev;
                ev.topic = "world.load";
                ev.s0 = g_world_to_load;
                oz::events::EventBus::Instance().Emit(ev);
            }

            // Re-assert the lit-fog shader on the freshly loaded world geometry.
            // The loader assigns it per-material when building brushes, but if
            // the shader was (re)loaded or the materials ended up with the
            // default shader the client rendered the world fullbright. The
            // editor already re-applies it after load; the client must too.
            if (AngelTechData.Lights.id > 0)
                OzoneLoader::Instance().SetLitFogShader(AngelTechData.Lights);
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
                    AngelTechData.SkyboxEnabled = true;
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
            PawnSystem::Instance().RespawnPlayerAtStart(AngelTechData.MainCamera);

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
    LitLightning_UpdateFrame(PawnSystem::Instance().GetLights(), AngelTechData.Lights,
                             AngelTechData.MainCamera, AngelTechData.GameLights[0],
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
                                                AngelTechData.MainCamera,
                                                GetFrameTime(), amb);
    }

    // The wind program (Wind.vs + LitFog.fs) is a THIRD consumer of the same
    // uniform contract, and it was the only one nothing refreshed. Every wind=1
    // mesh was therefore drawn with the program's GLSL defaults - no lights, no
    // viewPos, no ambient, no fog - and a lit, textured tree rendered as an
    // untinted pale blob. That is what made the pines look like they had no
    // texture at all, through two rounds of "the model must be wrong": the model
    // was fine, its uniforms were never uploaded.
    //
    // Deliberately LitLightning_Update and NOT LitLightning_UpdateFrame: the
    // frame-level version advances every light's phase, so running it once per
    // program runs all flicker at double speed (see its comment). This is the
    // per-program half only, exactly as SurfaceMaterial::UpdateFrame uses it.
    if (AngelTechData.WindShader.id > 0 &&
        AngelTechData.WindShader.id != AngelTechData.Lights.id) {
        static int s_windViewPosLoc = -2, s_windAmbientLoc = -2;
        static int s_windFogColorLoc = -2, s_windFogStartLoc = -2, s_windFogEndLoc = -2;
        static int s_windFogDensityLoc = -2, s_windFogIntensityLoc = -2;
        if (s_windViewPosLoc == -2) {
            s_windViewPosLoc = AngelTechData.WindShader.locs[SHADER_LOC_VECTOR_VIEW];
            s_windAmbientLoc = GetShaderLocation(AngelTechData.WindShader, "ambient");
            s_windFogColorLoc = GetShaderLocation(AngelTechData.WindShader, "fogColor");
            s_windFogStartLoc = GetShaderLocation(AngelTechData.WindShader, "fogStart");
            s_windFogEndLoc = GetShaderLocation(AngelTechData.WindShader, "fogEnd");
            s_windFogDensityLoc = GetShaderLocation(AngelTechData.WindShader, "fogDensity");
            s_windFogIntensityLoc = GetShaderLocation(AngelTechData.WindShader, "fogIntensity");
        }

        LitLightning_Update(PawnSystem::Instance().GetLights(), AngelTechData.WindShader,
                            AngelTechData.MainCamera, GetFrameTime());

        const float windViewPos[3] = { AngelTechData.MainCamera.position.x,
                                       AngelTechData.MainCamera.position.y,
                                       AngelTechData.MainCamera.position.z };
        if (s_windViewPosLoc >= 0)
            SetShaderValue(AngelTechData.WindShader, s_windViewPosLoc, windViewPos, SHADER_UNIFORM_VEC3);

        float windAmb[4];
        FillDefaultAmbient(windAmb);
        OzoneLoader::Instance().GetWorldAmbient(windAmb);
        if (s_windAmbientLoc >= 0)
            SetShaderValue(AngelTechData.WindShader, s_windAmbientLoc, windAmb, SHADER_UNIFORM_VEC4);

        float windFogCol[3] = {0.7f, 0.7f, 0.8f};
        float windFogStart = 10.0f, windFogEnd = 100.0f, windFogDensity = 1.0f, windFogIntensity = 1.0f;
        OzoneLoader::Instance().GetWorldFog(windFogCol, windFogStart, windFogEnd,
                                            windFogDensity, windFogIntensity);
        if (s_windFogColorLoc >= 0)
            SetShaderValue(AngelTechData.WindShader, s_windFogColorLoc, windFogCol, SHADER_UNIFORM_VEC3);
        if (s_windFogStartLoc >= 0)
            SetShaderValue(AngelTechData.WindShader, s_windFogStartLoc, &windFogStart, SHADER_UNIFORM_FLOAT);
        if (s_windFogEndLoc >= 0)
            SetShaderValue(AngelTechData.WindShader, s_windFogEndLoc, &windFogEnd, SHADER_UNIFORM_FLOAT);
        if (s_windFogDensityLoc >= 0)
            SetShaderValue(AngelTechData.WindShader, s_windFogDensityLoc, &windFogDensity, SHADER_UNIFORM_FLOAT);
        if (s_windFogIntensityLoc >= 0)
            SetShaderValue(AngelTechData.WindShader, s_windFogIntensityLoc, &windFogIntensity, SHADER_UNIFORM_FLOAT);
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

void AngelTechInit()
{
    OZ_INFO("=== AngelTech Engine starting ===");
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
    // Baked material graphs (GameData/Materials/*.ozmat). Must follow the package
    // loader because a released install may carry them inside a .ozpak.
    ozmat::MaterialRegistry::Instance().Init();

    // Initialize LightningScript entity system
    LightningEntityRegistry::Instance().Init();
    LightningEntityManager::Instance().Init();

    // Player character roster (GameData/Pawn/Player/characters.cfg, packed by
    // player.ozpak). Loaded after the package loader so a release install reads
    // the packaged copy; warnings name every skipped entry.
    {
        std::string rosterText =
            LoadFileTextWithFallback("GameData/Pawn/Player/characters.cfg");
        auto& roster = angelsmenu::CharacterRoster::Instance();
        roster.LoadFromText(rosterText);
        for (const auto& warning : roster.Warnings())
            OZ_WARN("CharacterRoster: %s", warning.c_str());
        if (!roster.Loaded())
            OZ_WARN("CharacterRoster: no characters.cfg entries; the Player def is the default");
    }
    // Phase 2: load the Angels95 game hierarchy (: game / : campaign / : sector
    // / : mission) now that the registry has parsed every .ozls def.
    oz::game::GameManager::Instance().Init();
    oz::game::RegisterGameBoxedTypes();
    oz::game::RegisterGameOpcodes();

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
        const char* defsDir = "GameData/Pawn/PawnDefs";
        namespace fs = std::filesystem;
        // Collect from the loose directory first, then from the packages: a
        // release install carries System/Data/*.oz* only, so the .cfg defs ship
        // inside pawns_defs.ozpak. Loose files win - a dev edit must not be
        // overridden by a packaged copy.
        std::vector<std::pair<std::string, std::string>> cfgFiles; // basename, text
        if (fs::exists(defsDir)) {
            for (auto& entry : std::filesystem::directory_iterator(defsDir)) {
                if (!entry.is_regular_file()) continue;
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext != ".cfg") continue;
                std::ifstream f(entry.path());
                if (!f.is_open()) continue;
                std::stringstream ss;
                ss << f.rdbuf();
                cfgFiles.emplace_back(entry.path().filename().string(), ss.str());
            }
        }
        for (const std::string& cfg : PackageAssetLoader::Instance().ListAssetBasenames(".cfg")) {
            bool seen = false;
            for (auto& cf : cfgFiles) if (cf.first == cfg) { seen = true; break; }
            if (seen) continue;
            std::string text = LoadFileTextWithFallback(("GameData/Pawn/PawnDefs/" + cfg).c_str());
            if (!text.empty()) cfgFiles.emplace_back(cfg, text);
        }

        if (!cfgFiles.empty()) {
            int loaded = 0;
            for (auto& cf : cfgFiles) {
                std::istringstream f(cf.second);
                std::string name, sp, sc, modelPath, modelTex;
                std::string meshType, animIdle, animPatrol, animChase, animReturn, animDeath;
                std::string animFile;
                float speed = 1.5f, aggroRange = 6.0f, attackRange = 1.5f, damage = 10.0f;
                float animSpeed = 1.0f, modelScale = 1.0f, modelYOffset = 0.0f;
                float hoverAmplitude = 0.35f, hoverSpeed = 1.6f;
                bool hover = false;
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
                    // Vertex-keyframe clip for an UNRIGGED model (.ozanim), plus
                    // vertical placement / hovering. Kept in step with the editor's
                    // parser in AngelEd/Source/Main.cpp.
                    else if (key == "animfile" || key == "anim_file") animFile = val;
                    else if (key == "model_y_offset") modelYOffset = std::stof(val);
                    else if (key == "hover") hover = (val == "1" || val == "true" || val == "True" || val == "yes");
                    else if (key == "hover_amplitude") hoverAmplitude = std::stof(val);
                    else if (key == "hover_speed") hoverSpeed = std::stof(val);
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
                    def.anim_file = animFile;
                    def.model_y_offset = modelYOffset;
                    def.hover = hover;
                    def.hover_amplitude = hoverAmplitude;
                    def.hover_speed = hoverSpeed;
                    def.baseDir = std::string(defsDir) + "/";
                    if (!def.baseDir.empty()) def.baseDir += "/";
                    ps.RegisterDef(def);
                    loaded++;
                }
            }
            OZ_INFO("Loaded %d pawn definitions from %s (loose + packaged)", loaded, defsDir);
        } else {
            // Fallback if no config files are available at all
            OZ_INFO("No pawn defs found loose or packaged, using hardcoded defaults");
            PawnDef d;
            d.name="Walker"; d.speed=1.5f; d.aggroRange=6.0f; d.attackRange=1.5f; d.damage=10.0f; d.maxHealth=100; ps.RegisterDef(d);
            d.name="Skaarj"; d.speed=2.5f; d.aggroRange=10.0f; d.attackRange=2.0f; d.damage=20.0f; d.maxHealth=150; ps.RegisterDef(d);
            d.name="Brute"; d.speed=1.0f; d.aggroRange=4.0f; d.attackRange=1.5f; d.damage=30.0f; d.maxHealth=250; ps.RegisterDef(d);
            d.name="Floater"; d.speed=1.2f; d.aggroRange=8.0f; d.attackRange=3.0f; d.damage=15.0f; d.maxHealth=80; ps.RegisterDef(d);
        }
    }

    AngelTechData.InitCamera();

    AngelTechData.PixelShader = LoadShaderWithFallback(0, "GameData/Shaders/Pixel.fs");
    // Legacy FogShader disabled - using LitFog material shader instead
    AngelTechData.FogShader = {0};
    AngelTechData.LineShader = LoadShaderWithFallback(0, "GameData/Shaders/Scanlines.fs");
    AngelTechData.SobelShader = LoadShaderWithFallback(0, "GameData/Shaders/Sobel.fs");
    AngelTechData.ToonShader = LoadShaderWithFallback(0, "GameData/Shaders/Toon.fs");
    AngelTechData.JitterShader = LoadShaderWithFallback(0, "GameData/Shaders/Jitter.fs");
    AngelTechData.Lights = LoadShaderWithFallback("GameData/Shaders/Lights/Lighting.vs", "GameData/Shaders/Lights/LitFog.fs");
    AngelTechData.WindShader = LoadShaderWithFallback("GameData/Shaders/Lights/Wind.vs", "GameData/Shaders/Lights/LitFog.fs");
    if (AngelTechData.WindShader.id == 0) {
        AngelTechData.WindShader = AngelTechData.Lights; // graceful fallback: no sway
        OZ_WARN("Wind.vs shader not found — foliage wind disabled");
    }
    OZ_INFO("Shaders loaded (Pixel=%d, Line=%d, Sobel=%d, Toon=%d, Jitter=%d, Lights=%d, Wind=%d)",
            AngelTechData.PixelShader.id, AngelTechData.LineShader.id,
            AngelTechData.SobelShader.id, AngelTechData.ToonShader.id,
            AngelTechData.JitterShader.id, AngelTechData.Lights.id, AngelTechData.WindShader.id);
    OzoneLoader::Instance().SetLitFogShader(AngelTechData.Lights);

    // Initialize all lights to disabled
    for (int i = 0; i < MAX_LIGHTS; i++)
    {
        AngelTechData.GameLights[i] = {0};
    }

    AngelTechData.Lights.locs[SHADER_LOC_VECTOR_VIEW] = GetShaderLocation(AngelTechData.Lights, "viewPos");
    int AmbientLoc = GetShaderLocation(AngelTechData.Lights, "ambient");
    float ambient[4];
    FillDefaultAmbient(ambient);
    SetShaderValue(AngelTechData.Lights, AmbientLoc, ambient, SHADER_UNIFORM_VEC4);
    OzoneLoader::Instance().SetWorldAmbient(ambient[0], ambient[1], ambient[2], ambient[3]);

    // Initialize fog uniforms
    int fogStartLoc = GetShaderLocation(AngelTechData.Lights, "fogStart");
    int fogEndLoc = GetShaderLocation(AngelTechData.Lights, "fogEnd");
    int fogDensityLoc = GetShaderLocation(AngelTechData.Lights, "fogDensity");
    int fogColorLoc = GetShaderLocation(AngelTechData.Lights, "fogColor");
    int fogIntensityLoc = GetShaderLocation(AngelTechData.Lights, "fogIntensity");

    float fogStart = 10.0f;
    float fogEnd = 100.0f;
    float fogDensity = 1.0f;
    float fogColor[3] = {0.7f, 0.7f, 0.8f};
    float fogIntensity = 1.0f;

    SetShaderValue(AngelTechData.Lights, fogStartLoc, &fogStart, SHADER_UNIFORM_FLOAT);
    SetShaderValue(AngelTechData.Lights, fogEndLoc, &fogEnd, SHADER_UNIFORM_FLOAT);
    SetShaderValue(AngelTechData.Lights, fogDensityLoc, &fogDensity, SHADER_UNIFORM_FLOAT);
    SetShaderValue(AngelTechData.Lights, fogColorLoc, fogColor, SHADER_UNIFORM_VEC3);
    SetShaderValue(AngelTechData.Lights, fogIntensityLoc, &fogIntensity, SHADER_UNIFORM_FLOAT);
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
    static int uTimeLoc = GetShaderLocation(AngelTechData.Lights, "uTime");
    float timeVal = (float)GetTime();
    SetShaderValue(AngelTechData.Lights, uTimeLoc, &timeVal, SHADER_UNIFORM_FLOAT);

    AngelTechData.HomeScreen = LoadTextureWithFallback("GameData/Tex/Title/Title.png");
    AngelTechData.PauseHeading = LoadTexture("GameData/Tex/Title/menu_heading.png");
    AngelTechData.BtnNormal = LoadTexture("GameData/Tex/Title/menu_button.png");
    AngelTechData.BtnHover = LoadTexture("GameData/Tex/Title/menu_button_hover.png");
    AngelTechData.BtnClicked = LoadTexture("GameData/Tex/Title/menu_button_clicked.png");
    if (IsPathFile("GameData/Tex/Title/Title.mpg"))
        AngelTechData.HomeScreenVideo = ray_video_open("GameData/Tex/Title/Title.mpg");
    AngelTechData.HomeScreenMusic = LoadMusicWithFallback("GameData/Audio/Music/Title.mp3");

    AngelTechTextSystem.Bar = LoadTextureWithFallback("GameData/Tex/Engine/TextBar.png");
    AngelTechTextSystem.BarFont = LoadFontWithFallback("GameData/Tex/Engine/Font.ttf");
    // UI/gameplay one-shots + the typewriter blip (all owned by SoundManager).
    SoundManager::Instance().LoadCoreSounds();

    AngelTechData.GameLights[0] = CreateLight(LIGHT_DIRECTIONAL, {AngelTechData.MainCamera.position.x, AngelTechData.MainCamera.position.y, AngelTechData.MainCamera.position.z}, Vector3Zero(), WHITE, AngelTechData.Lights);

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

    SoundManager::Instance().PlayStream(AngelTechData.HomeScreenMusic);
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

    Texture2D splash = LoadTexture("GameData/Tex/Title/splash.png");
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

// Move the borderless window with Alt+Left-drag.
//
// The window is created FLAG_WINDOW_UNDECORATED, so outside the title menu's
// custom chrome there is no OS title bar to grab - which made the window
// effectively immovable. This applies the frame-to-frame delta of the cursor's
// SCREEN-space position (window origin + window-relative mouse) to the window
// origin, which is stable while the window moves out from under the cursor
// (a window-relative delta would feed back into itself).
inline void HandleBorderlessWindowDrag() {
    if (!BorderlessWindow) return;

    static bool dragging = false;
    static Vector2 lastGlobal = {0.0f, 0.0f};

    const bool alt = IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
    const Vector2 win = GetWindowPosition();
    const Vector2 rel = GetMousePosition();
    const Vector2 global = { win.x + rel.x, win.y + rel.y };

    if (alt && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        dragging = true;
        lastGlobal = global;
    }
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) dragging = false;

    if (dragging) {
        const float dx = global.x - lastGlobal.x;
        const float dy = global.y - lastGlobal.y;
        if (dx != 0.0f || dy != 0.0f)
            SetWindowPosition((int)(win.x + dx), (int)(win.y + dy));
        lastGlobal = global;
    }
}

// Spawn a dedicated AngelServ subprocess. Shared by the Co-op pane (the
// ShouldStartServer path in PlayHomeScreen) and the CLI --host /
// --host-mission flags (Main.cpp), so the port/mission/master argument
// assembly exists exactly once and cannot drift between the two.
//
// The child's stdout/stderr APPEND to System/HostedServer.log. Mission
// validation is server-authoritative — AngelServ resolves --mission through
// its own GameManager and refuses an unknown name by name (exit 1) — and a
// refusal is only useful if it can be found: the old POSIX branch redirected
// the child to /dev/null, and a `start /B` child of a Windows GUI parent has
// no console to inherit.
bool LaunchHostedServer(int serverPort, const char* missionOrNull,
                        const char* worldOrNull = nullptr,
                        const char* serverNameOrNull = nullptr)
{
    std::string args = "--port " + std::to_string(serverPort) + " --dir GameData";
    if (worldOrNull && worldOrNull[0]) {
        args += " --world \"";
        args += worldOrNull;
        args += "\"";
    }
    if (serverNameOrNull && serverNameOrNull[0]) {
        args += " --server-name \"";
        args += serverNameOrNull;
        args += "\"";
    }
    // Co-op host: pin the campaign arc to the mission picked in the Co-op pane
    // or named on the CLI. Quoted because the name is author-supplied and the
    // command line goes through cmd/sh.
    if (missionOrNull && missionOrNull[0]) {
        args += " --campaign --mission \"";
        args += missionOrNull;
        args += "\"";
    }
    // Announce the hosted server to the configured masters.
    for (const auto& masterUrl : master::LoadMasterUrls("System/Angels95.ini")) {
        if (masterUrl.rfind("http://", 0) == 0)
            args += " --master-http \"" + masterUrl + "\"";
        else if (masterUrl.rfind("https://", 0) == 0)
            OZ_WARN("Host: ignoring https master '%s' (plain http only)", masterUrl.c_str());
        else
            args += " --master " + masterUrl;
    }
#ifdef _WIN32
    // `start /B` runs the child without a new window; the >> / 2>&1 are parsed
    // by the cmd that std::system invokes and the child inherits those handles.
    const std::string exePath = "System\\AngelServ.exe";
    std::string cmd = "start /B \"\" " + exePath + " " + args +
                      " >> System\\HostedServer.log 2>&1";
    const char* exeName = "AngelServ.exe";
#else
    // `start` is a cmd.exe builtin, so the old string reached /bin/sh as the command
    // `start /B "" System\AngelServ.exe ...`, which fails with "start: not found" and
    // reported a failed launch on every POSIX host. Detach explicitly instead: nohup
    // plus & backgrounds it with the child's stdout redirected, so the server does not
    // inherit our terminal and does not die with it.
    const std::string exePath = "./AngelServ";
    std::string cmd = "nohup " + exePath + " " + args +
                      " >> System/HostedServer.log 2>&1 &";
    const char* exeName = "AngelServ";
#endif
    // A MISSING binary does not fail — it HANGS. cmd's
    // `start /B "" <missing exe> >> log` never returns (measured: the cmd
    // child was still alive 15 minutes later and std::system had blocked the
    // whole time), which would freeze the client right here with no menu
    // (g_skipMenu), no join, and no error line — the `Failed to launch`
    // below is unreachable for this case on Windows. The existence check
    // turns the hang into a loud, findable error. The non-throwing overload:
    // this path exists to report failures and must not raise one.
    std::error_code exeStatError;
    if (!std::filesystem::exists(exePath, exeStatError)) {
        OZ_ERROR("Failed to launch %s: '%s' not found (run the build, or start "
                 "from the directory the run scripts use)", exeName, exePath.c_str());
        return false;
    }
    if (std::system(cmd.c_str()) == 0) {
        OZ_INFO("Launched %s on port %d (child output appends to System/HostedServer.log)",
                exeName, serverPort);
        return true;
    }
    OZ_ERROR("Failed to launch %s (see System/HostedServer.log)", exeName);
    return false;
}

// Start a brand-new session.
//
// LoadWorld() deliberately preserves the player's gear across a world change: a
// portal, a campaign transition and a server-driven mission move all reload the
// level around a character who keeps what they carry. A session chosen from the
// title menu is the opposite — nothing about the previous world, mode or server
// may leak into it. Without this reset, a campaign's hotbar/backpack followed the
// player into a practice match and into multiplayer, and SaveGame() then wrote the
// stale loadout into POS.sav so the next Continue resurrected it as well.
//
// Called once per outer-loop iteration, BEFORE PlayHomeScreen(). A Continue
// inside the menu overwrites this with the saved state through LoadSave().
void ResetSessionState()
{
    // Hotbar, equipment, spell bar, the entity instances and every cached
    // model/texture. This also re-spawns the Player entity, so health/mana/
    // stamina/XP/level start from the defs instead of the previous session.
    LightningEntityManager::Instance().Init();
    // Backpack + coins are a plain global with no owner to reset them.
    gInventory = InventorySystem{};
    // Campaign mission + objectives + world flags (a practice match is not the
    // previous campaign's next mission).
    oz::game::GameManager::Instance().ClearActiveMission();
    oz::ViewModel::Instance().Clear();
    PawnSystem::Instance().m_pickupFeedback = PawnSystem::PickupFeedback{};

    // Per-world latches that are not owned by LoadWorld's clear block.
    g_activeEnvZone.clear();
    g_damageCooldown = 0.0f;
    g_savedWorldName.clear();
    // A "Continue" from the previous session must not pin the next spawn.
    SetCameraFlag = false;
    // The game-over overlay counts deaths within a session.
    AngelTechData.Deaths = 1;
    // A queued portal/transition would otherwise fire on the first frame of the
    // new session's world, teleporting the player to the previous one's target.
    g_portalTransitionPending = false;
    g_portalCooldown = 0.0f;
    g_portalHasReturn = false;
    g_portalReturnWorld.clear();
    g_returnPortalId = -1;
}

void PlayHomeScreen()
{
    // Re-open the title video if a previous menu visit released it.
    if (!AngelTechData.HomeScreenVideo.ok && IsPathFile("GameData/Tex/Title/Title.mpg"))
        AngelTechData.HomeScreenVideo = ray_video_open("GameData/Tex/Title/Title.mpg");

    if (g_skipMenu)
    {
        g_skipMenu = false;
        UnloadRenderTexture(Target);
        Target = LoadRenderTexture(GetScreenWidth(), GetScreenHeight());
        AngelTechData.Deaths = 1;
        return;
    }

    // The title track is stopped when the menu is left. On a return to the menu
    // nothing restarted it, so the title screen was silent after the first game.
    SoundManager::Instance().PlayStream(AngelTechData.HomeScreenMusic);
    // Returning from a game leaves the cursor captured/hidden. The title screen
    // is mouse-driven, so undo that here rather than relying on the previous
    // menu path.
    ShowCursor();
    EnableCursor();

    TitleMenu menu;
    if (g_menuShotPage >= 0) menu.SetStartPage(g_menuShotPage);

    int menuShotFrames = 0;
    while (!menu.Tick() && !WindowShouldClose())
    {
        HandleBorderlessWindowDrag();
        if (g_menuShotPage >= 0 && ++menuShotFrames >= g_menuShotFrames)
        {
            if (!g_menuShotOut.empty()) TakeScreenshot(g_menuShotOut.c_str());
            g_quitRequested = true;
            break;
        }
    }

    SoundManager::Instance().StopStream(AngelTechData.HomeScreenMusic);

    // Release the title video (it will be re-opened on the next menu visit).
    if (AngelTechData.HomeScreenVideo.ok)
    {
        ray_video_destroy(&AngelTechData.HomeScreenVideo);
        AngelTechData.HomeScreenVideo = ray_video_t{};
    }

    if (menu.ShouldQuit())
    {
        g_quitRequested = true;
        return;
    }

    if (menu.ShouldLoadGame() || menu.GetSelectedWorld())
    {
        UnloadRenderTexture(Target);
        Target = LoadRenderTexture(GetScreenWidth(), GetScreenHeight());
        AngelTechData.Deaths = 1;
    }

    if (menu.ShouldLoadGame())
    {
        // Load inventory/flags/campaign state first: it also restores the saved
        // camera position. The world must be named explicitly, because the old
        // path only set LoadFlag (never read) and then loaded whatever
        // g_world_to_load happened to hold — at startup, EngineTest.
        LoadSave();
        if (!menu.IsCampaignContinue() && !g_savedWorldName.empty())
        {
            strncpy(g_world_to_load, g_savedWorldName.c_str(), sizeof(g_world_to_load) - 1);
            g_world_to_load[sizeof(g_world_to_load) - 1] = '\0';
        }
        // The camera position in POS.sav belongs to the world that was saved.
        // A campaign continue may resolve to a different mission world (the
        // campaign save wins over the practice save), so only trust the saved
        // position when the worlds agree; otherwise use the mission's spawn.
        const char* contWorld = menu.GetSelectedWorld();
        if (menu.IsCampaignContinue() &&
            (!contWorld || g_savedWorldName.empty() || g_savedWorldName != contWorld)) {
            SetCameraFlag = false;
        }
    }

    if (menu.GetSelectedWorld())
    {
        // Campaign mission launch: record the active mission before LoadWorld,
        // so SaveGame (called from LoadWorld) writes the mission the player
        // actually chose instead of the previous session's.
        if (!menu.ShouldStartServer() && menu.GetSelectedMission() &&
            oz::game::GameManager::Instance().Loaded()) {
            oz::game::GameManager::Instance().SetActiveMission(menu.GetSelectedMission());
        }
        strncpy(g_world_to_load, menu.GetSelectedWorld(), sizeof(g_world_to_load) - 1);
        g_world_to_load[sizeof(g_world_to_load) - 1] = '\0';
    }
    else if (menu.ShouldLoadGame() && g_savedWorldName.empty())
    {
        OZ_WARN("Continue: the save has no world; staying in '%s'", g_world_to_load);
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
        // Co-op host pins a mission; plain host pins a world. Both go through
        // the single spawn path shared with --host / --host-mission.
        if (LaunchHostedServer(port, menu.GetCoopMission(), menu.GetSelectedWorld(),
                               menu.GetHostServerName())) {
            SetServerJoinIP = "127.0.0.1";
            SetServerJoinPort = port;
            SetServerJoinFlag = true;
        } else {
            OZ_ERROR("Host: server launch failed; staying offline");
        }
    }

    AngelTechData.Deaths = 1;
}

void UpdateEntitiesSim(float dt)
{
    Vector3 playerPos = AngelTechData.MainCamera.position;

    // Single-pass zone scan for player â€” replaces 4 separate CheckZoneCollision calls
    ZoneManager::Instance().UpdatePlayerRegion(playerPos, g_playerMovement.PlayerBounds);

    // Gameplay trigger volumes -> EventBus trigger.enter/exit (the bridge maps
    // them to on_trigger_enter/on_trigger_exit on the `: trigger` def). One call
    // per simulation step so enter/exit fire once per crossing.
    oz::events::TriggerSystem::Instance().UpdateForPoint(playerPos.x, playerPos.y, playerPos.z);

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
            AngelTechTextSystem.Write(TextFormat("Collected: %s", fb.typeName.c_str()));
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
            if (AngelTechData.DamageFlash <= 0.0f)
                AngelTechData.DamageFlash = kDamageFlashDuration;
            if (AngelTechData.Ticker % 2 == 0)
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
                AngelTechData.Deaths++;
                OZ_INFO("Player died! Death #%d", AngelTechData.Deaths);
                AngelTechTextSystem.Write(TextFormat("You died! Death #%d", AngelTechData.Deaths));
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
                    PawnSystem::Instance().RespawnPlayerAtStart(AngelTechData.MainCamera);
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
    // Head-bob. The setting is the global `HeadBob` (Settings -> H.Bob). The old
    // code read `g_playerMovement.HeadBob`, a counter that starts at 0 and is
    // only ever mutated inside the block that tested it - so the toggle was
    // wired to nothing and the effect never ran. This is a smooth sine driven by
    // actual horizontal movement, gated on the setting and on being grounded.
    static Vector3 s_prevPos = {0.0f, 0.0f, 0.0f};
    static bool s_prevInit = false;
    static float s_phase = 0.0f;
    static float s_prevBobY = 0.0f;

    const Vector3 camPos = AngelTechData.MainCamera.position;
    if (!s_prevInit) { s_prevPos = camPos; s_prevInit = true; }
    const float dt = GetFrameTime();
    const float dx = camPos.x - s_prevPos.x;
    const float dz = camPos.z - s_prevPos.z;
    const float speed = (dt > 1e-4f) ? sqrtf(dx * dx + dz * dz) / dt : 0.0f;
    s_prevPos = camPos;

    const bool walking = speed > 0.5f;
    float bobY = 0.0f;
    float bobAmp = 0.0f;
    if (HeadBob && !Debug && g_playerMovement.onGround && walking) {
        // ~1.6 cycles/sec at the base walk speed.
        s_phase += speed * 1.8f * dt;
        const float base = g_playerMovement.BaseSpeed > 0.0f ? g_playerMovement.BaseSpeed : 5.4f;
        // Sprinting raises `speed` by the sprint multiplier, so the same formula
        // bobbed harder AND faster. Damp the amplitude while sprinting (the
        // frequency is left alone so a run still reads as a stride) - "a little",
        // not a different feel.
        const float sprintBobScale = g_playerMovement.isSprinting ? 0.65f : 1.0f;
        bobAmp = 0.05f * fminf(speed / base, 1.4f) * sprintBobScale;
        bobY = sinf(s_phase) * bobAmp;
        PlayerController::Instance().PlayWalkLoop(g_playerMovement.isSprinting);
    } else {
        PlayerController::Instance().StopWalkLoop();
    }

    AngelTechData.MainCamera.position.y += bobY;
    // Move the target by the bob DELTA, not the absolute bob: UpdateLookAndMove
    // already folded last frame's bob into the target through the camera
    // position, so an absolute add would double it and pitch the view. The delta
    // keeps target - position == look direction, i.e. a true eye bob.
    AngelTechData.MainCamera.target.y += (bobY - s_prevBobY);
    s_prevBobY = bobY;

    // Feed the weapon view-model so it sways on the same phase.
    oz::ViewModel::Instance().SetViewBob(s_phase, bobAmp);

    g_playerMovement.UpdateBounds(AngelTechData.MainCamera);
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

    wstring Position = to_wstring(AngelTechData.MainCamera.position.x) + L':' +
                       to_wstring(AngelTechData.MainCamera.position.y) + L':' +
                       to_wstring(AngelTechData.MainCamera.position.z) + L':' +
                       to_wstring(AngelTechData.LevelIndex) + L':' +
                       wstring(g_world_to_load, g_world_to_load + strlen(g_world_to_load)) + L':';

    wofstream Outfile1;
    Outfile1.open("GameData/Saves/POS.sav");
    Outfile1 << Position;

    // Campaign state (active mission). Its own file so the fragile TF.sav colon
    // format is untouched.
    //
    // Only write it when a campaign mission is active. A practice match or a
    // multiplayer join has none, and SaveGame() runs from LoadWorld on every
    // session start — writing the empty serialisation used to TRUNCATE
    // Campaign.sav to 0 bytes, so playing a practice match erased the campaign
    // the player could have continued.
    {
        const std::string campaignState =
            oz::game::GameManager::Instance().SerializeState();
        if (!campaignState.empty()) {
            std::ofstream cf("GameData/Saves/Campaign.sav");
            if (cf) cf << campaignState;
        }
    }
}

void LoadSave()
{
    g_savedWorldName.clear();

    // Campaign state (active mission).
    {
        std::ifstream cf("GameData/Saves/Campaign.sav");
        if (cf) {
            // Whole file, not one line: the v1 format is two lines
            // (campaign=1 / mission=<name>); the legacy format was one.
            std::string s, line;
            while (std::getline(cf, line)) {
                if (!s.empty()) s += '\n';
                s += line;
            }
            oz::game::GameManager::Instance().DeserializeState(s);
        }
    }

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
            AngelTechData.LevelIndex = int(ToFloat(WSplitValue(Position, 3)));
        } catch (...) {
            AngelTechData.LevelIndex = 1;
        }

        // The world field was appended after level. A save without it is the
        // legacy format; g_savedWorldName stays empty and the caller falls back
        // to the campaign mission or the current world.
        wstring savedWorld = WSplitValue(Position, 4);
        g_savedWorldName.clear();
        for (wchar_t wc : savedWorld) {
            if (wc == L':' || wc == L'\r' || wc == L'\n') break;
            if (wc < 128) g_savedWorldName.push_back((char)wc);
        }

        SetCameraFlag = true;

        try {
            // Keep the authored float precision. This used to cast to int, so a
            // save/continue round-trip moved the player by up to a unit.
            float X = ToFloat(WSplitValue(Position, 0));
            float Y = ToFloat(WSplitValue(Position, 1));
            float Z = ToFloat(WSplitValue(Position, 2));
            SetCameraPos = {X, Y, Z};
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
        AngelTechData.MainCamera.position,
        g_playerMovement.PlayerBounds);
    bool inSkyZone = PawnSystem::Instance().IsInSkyZone();

    BeginMode3D(AngelTechData.MainCamera);

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
    if (WorldModels.Skybox.id > 0 && AngelTechData.SkyboxEnabled) {
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
        oz::Skybox::Instance().Draw(AngelTechData.MainCamera.position,
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
        OzoneLoader::Instance().DrawZoneGeometry(AngelTechData.MainCamera);
        rlEnableDepthMask();
    }

    // GameplaySoundZone - zone music/ambience/enter-sfx profiles plus the
    // per-frame music stream pump are owned by SoundManager.
    SoundManager::Instance().UpdateSoundZones(ZoneManager::Instance().GetPlayerRegion());
    SoundManager::Instance().Update();

    OzoneLoader::Instance().DrawWorldGeometry(AngelTechData.MainCamera);

    // Additive re-draw of every SURF_GLOW face, after the opaque world so the
    // halo lands on top of the composited image. No-op unless a surface actually
    // carries the flag, so this costs two vector scans on a normal frame.
    OzoneLoader::Instance().DrawGlowGeometry(AngelTechData.MainCamera);

    // OZONE brush collision — chunk-accelerated query, delegated to the
    // physics module via the adapter below.
    {
        oz::physics::OzoneCollisionQuery q;
        Vector3 cp = AngelTechData.MainCamera.position;
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
        pphys.ClampToGround(AngelTechData.MainCamera, motion, g_playerMovement.EyeHeight, q);
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
        PawnSystem::Instance().DrawProjectiles(AngelTechData.MainCamera, AngelTechData.Lights);
        CombatFX::Instance().Draw3D(AngelTechData.MainCamera);
        DrawRemotePlayers3D(AngelTechData.Lights);
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
                GetFrameTime(), AngelTechData.MainCamera.position);

        // Draw pawns / entity billboards exactly once per frame (kept out of
        // the sim steps so a catch-up frame doesn't re-draw the scene).
        PawnSystem::Instance().DrawAll(AngelTechData.MainCamera, AngelTechData.Lights);
        PawnSystem::Instance().DrawEntities(AngelTechData.MainCamera, AngelTechData.Lights, AngelTechData.WindShader);
        DrawLightFlares(AngelTechData.MainCamera);

        // First-person weapon view-model (drawn on top of the world).
        // Suppressed for --shot captures: the view-model is camera-locked UI,
        // not the level, and it covers a third of the frame.
        if (!(g_shot.active && g_shot.hideHud)) {
            oz::ViewModel::Instance().Update(GetFrameTime());
            oz::ViewModel::Instance().Draw(AngelTechData.MainCamera, AngelTechData.Lights);
        }
    }
    // Collision rollback - delegated to the physics module.
    if (ObjectCollision)
    {
        oz::physics::PlayerPhysics().RestorePosition(
            AngelTechData.MainCamera,
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
            static int fogD = GetShaderLocation(AngelTechData.Lights, "fogDensity");
            static int fogC = GetShaderLocation(AngelTechData.Lights, "fogColor");
            static int fogS = GetShaderLocation(AngelTechData.Lights, "fogStart");
            static int fogE = GetShaderLocation(AngelTechData.Lights, "fogEnd");
            static int fogI = GetShaderLocation(AngelTechData.Lights, "fogIntensity");
            const LevelFogState& L = g_levelFog;
            SetShaderValue(AngelTechData.Lights, fogS, &L.fogStart, SHADER_UNIFORM_FLOAT);
            SetShaderValue(AngelTechData.Lights, fogE, &L.fogEnd, SHADER_UNIFORM_FLOAT);
            SetShaderValue(AngelTechData.Lights, fogD, &L.fogDensity, SHADER_UNIFORM_FLOAT);
            SetShaderValue(AngelTechData.Lights, fogC, L.fogColor, SHADER_UNIFORM_VEC3);
            SetShaderValue(AngelTechData.Lights, fogI, &L.fogIntensity, SHADER_UNIFORM_FLOAT);
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
            static int fogDensityLoc = GetShaderLocation(AngelTechData.Lights, "fogDensity");
            static int fogColorLoc = GetShaderLocation(AngelTechData.Lights, "fogColor");
            float density = lem.PendingFogDensity();
            float color[3] = {lem.PendingFogR(), lem.PendingFogG(), lem.PendingFogB()};
            SetShaderValue(AngelTechData.Lights, fogDensityLoc, &density, SHADER_UNIFORM_FLOAT);
            SetShaderValue(AngelTechData.Lights, fogColorLoc, color, SHADER_UNIFORM_VEC3);
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
            AngelTechData.SkyboxEnabled = (WorldModels.Skybox.id > 0);
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
                    AngelTechData.SkyboxEnabled = true;
                } else {
                    OZ_WARN("LightningScript: skybox '%s' not found", path.c_str());
                }
            }
        }

        // Apply pending Ambient changes from script contexts
        if (lem.TakePendingAmbientRestore()) {
            static int ambLoc = GetShaderLocation(AngelTechData.Lights, "ambient");
            float amb[4];
            FillDefaultAmbient(amb);   // the world default, not a hardcoded 1.0
            SetShaderValue(AngelTechData.Lights, ambLoc, amb, SHADER_UNIFORM_VEC4);
            OzoneLoader::Instance().SetWorldAmbient(amb[0], amb[1], amb[2], amb[3]);
            OZ_INFO("LightningScript: restore_ambient - reverted to level default");
        }
        if (lem.HasPendingAmbient())
        {
            float amb[4] = {lem.PendingAmbientR(), lem.PendingAmbientG(), lem.PendingAmbientB(), 1.0f};
            static int ambientLoc = GetShaderLocation(AngelTechData.Lights, "ambient");
            SetShaderValue(AngelTechData.Lights, ambientLoc, amb, SHADER_UNIFORM_VEC4);
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
                if (eo.applyFog && AngelTechData.Lights.id > 0)
                {
                    float fc[3] = {eo.fogR / 255.0f, eo.fogG / 255.0f, eo.fogB / 255.0f};
                    float fd = eo.fogDensity;
                    static int fogDensityLoc = GetShaderLocation(AngelTechData.Lights, "fogDensity");
                    static int fogColorLoc = GetShaderLocation(AngelTechData.Lights, "fogColor");
                    static int fogStartLoc = GetShaderLocation(AngelTechData.Lights, "fogStart");
                    static int fogEndLoc = GetShaderLocation(AngelTechData.Lights, "fogEnd");
                    SetShaderValue(AngelTechData.Lights, fogColorLoc, fc, SHADER_UNIFORM_VEC3);
                    SetShaderValue(AngelTechData.Lights, fogDensityLoc, &fd, SHADER_UNIFORM_FLOAT);
                    float fs = eo.fogStart, fe = eo.fogEnd;
                    SetShaderValue(AngelTechData.Lights, fogStartLoc, &fs, SHADER_UNIFORM_FLOAT);
                    SetShaderValue(AngelTechData.Lights, fogEndLoc, &fe, SHADER_UNIFORM_FLOAT);
                    // Publish alongside the uniform so the surface program gets
                    // the same fog from UpdateLightSources' single mirror point.
                    OzoneLoader::Instance().SetWorldFog(fc, fs, fe, fd);
                    FogEnabled = true;
                    FogIntensity = (fd > 0) ? fd : 0.3f;
                    FogTint = {(unsigned char)eo.fogR, (unsigned char)eo.fogG, (unsigned char)eo.fogB, 255};
                }
                if (eo.applyAmbient && AngelTechData.Lights.id > 0)
                {
                    float amb[4] = {eo.ambR / 255.0f * eo.ambIntensity,
                                    eo.ambG / 255.0f * eo.ambIntensity,
                                    eo.ambB / 255.0f * eo.ambIntensity, 1.0f};
                    static int ambientLoc = GetShaderLocation(AngelTechData.Lights, "ambient");
                    SetShaderValue(AngelTechData.Lights, ambientLoc, amb, SHADER_UNIFORM_VEC4);
                    OzoneLoader::Instance().SetWorldAmbient(amb[0], amb[1], amb[2], amb[3]);
                }
                g_activeEnvZone = envZoneName;
            }
            else if (!inEnvZone && !g_activeEnvZone.empty())
            {
                // Exited env zone â€” restore defaults from WorldInfo
                OZ_DEBUG("Zone env: restoring WorldInfo defaults");
                if (AngelTechData.Lights.id > 0)
                {
                    auto& wi = PawnSystem::Instance().GetWorldInfo();
                    float defFogColor[3] = {wi.defaultEnv.fogR / 255.0f,
                                            wi.defaultEnv.fogG / 255.0f,
                                            wi.defaultEnv.fogB / 255.0f};
                    float defFogDensity = wi.defaultEnv.fogDensity;
                    float defFogStart = wi.defaultEnv.fogStart, defFogEnd = wi.defaultEnv.fogEnd;
                    static int fogDensityLoc = GetShaderLocation(AngelTechData.Lights, "fogDensity");
                    static int fogColorLoc = GetShaderLocation(AngelTechData.Lights, "fogColor");
                    static int fogStartLoc = GetShaderLocation(AngelTechData.Lights, "fogStart");
                    static int fogEndLoc = GetShaderLocation(AngelTechData.Lights, "fogEnd");
                    SetShaderValue(AngelTechData.Lights, fogColorLoc, defFogColor, SHADER_UNIFORM_VEC3);
                    SetShaderValue(AngelTechData.Lights, fogDensityLoc, &defFogDensity, SHADER_UNIFORM_FLOAT);
                    SetShaderValue(AngelTechData.Lights, fogStartLoc, &defFogStart, SHADER_UNIFORM_FLOAT);
                    SetShaderValue(AngelTechData.Lights, fogEndLoc, &defFogEnd, SHADER_UNIFORM_FLOAT);
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
            AngelTechTextSystem.Write(lem.PendingMessage());
            lem.ClearPendingMessage();
        }

        // Scripted damage flash (damage opcode)
        if (lem.PlayerHurt())
        {
            lem.ClearPlayerHurt();
            if (AngelTechData.DamageFlash <= 0.0f)
                AngelTechData.DamageFlash = kDamageFlashDuration;
            // Same routed handler as the network PLAYER_HURT path, so a script
            // `damage` and a server hit cannot double-play and both respect the
            // authored hurt_sound / death_sound split.
            PlayerController::Instance().PlayHurt(
                LightningEntityManager::Instance().GetPlayerHealth() <= 0.0f);
        }

        // Phase-1 script world commands (teleport / change_world /
        // apply_area_damage / spawn_projectile / play_clip). LEM queues them
        // because it cannot see the camera or the world-load globals.
        for (auto& cmd : lem.TakePendingWorldCommands())
        {
            if (cmd.op == "teleport")
            {
                AngelTechData.MainCamera.position = {cmd.x, cmd.y, cmd.z};
                g_playerMovement.LookInit = false;   // re-derive look from the new spot
            }
            else if (cmd.op == "change_world")
            {
                if (!cmd.s0.empty())
                {
                    strncpy(g_world_to_load, cmd.s0.c_str(), sizeof(g_world_to_load) - 1);
                    g_world_to_load[sizeof(g_world_to_load) - 1] = '\0';
                    SetSceneFlag = true;   // consumed below -> LoadWorld()
                }
            }
            else if (cmd.op == "area_damage")
            {
                lem.ApplyAreaDamage(AngelTechData.MainCamera.position, cmd.f0, (int)cmd.f1);
            }
            else if (cmd.op == "projectile")
            {
                Vector3 fwd = Vector3Normalize(Vector3Subtract(AngelTechData.MainCamera.target,
                                                               AngelTechData.MainCamera.position));
                ProjectileNode pn;
                pn.position = Vector3Add(AngelTechData.MainCamera.position, Vector3Scale(fwd, 2.0f));
                pn.velocity = Vector3Scale(fwd, cmd.x > 0.0f ? cmd.x : 20.0f);
                pn.damage = cmd.y;
                pn.lifetime = cmd.z > 0.0f ? cmd.z : 2.0f;
                pn.speed = cmd.x > 0.0f ? cmd.x : 20.0f;
                pn.ownerId = -1;
                PawnSystem::Instance().SpawnProjectile(pn);
            }
            else if (cmd.op == "clip")
            {
                // View-model leg of play_clip: LEM queued this because the
                // action ran on the weapon currently held (it made the def
                // match there — ScriptCommand carries no def). Play the named
                // clip on the first-person copy; warn when neither leg could.
                if (!oz::ViewModel::Instance().PlayClip(cmd.s0))
                    OZ_WARN("play_clip '%s': no matching clip on the view-model",
                            cmd.s0.c_str());
            }
        }
    }

    EndMode3D();
    EndTextureMode();

    if (SetSceneFlag)
    {
        AngelTechData.LevelIndex = SetSceneId;
        LoadWorld();
        if (g_portalTransitionPending)
        {
            // Arriving via portal: place player at the portal's target spawn,
            // preserving view direction.
            Vector3 oldPos = AngelTechData.MainCamera.position;
            AngelTechData.MainCamera.position = g_portalSpawnPos;
            // Never arrive under the terrain: lift the camera to the OZONE
            // heightmap surface at this XZ when the raw spawn sits below grade.
            float groundY = OzoneLoader::Instance().HasHeightmap()
                                ? OzoneLoader::Instance().SampleHeightmapY(g_portalSpawnPos.x, g_portalSpawnPos.z)
                                : -99999.0f;
            if (groundY > -50000.0f &&
                AngelTechData.MainCamera.position.y < groundY + g_playerMovement.EyeHeight)
                AngelTechData.MainCamera.position.y = groundY + g_playerMovement.EyeHeight;
            AngelTechData.MainCamera.target.x += AngelTechData.MainCamera.position.x - oldPos.x;
            AngelTechData.MainCamera.target.y += AngelTechData.MainCamera.position.y - oldPos.y;
            AngelTechData.MainCamera.target.z += AngelTechData.MainCamera.position.z - oldPos.z;
            // Reset movement state to the arrival point so the collision
            // "restore" path can't yank the player back to a stale position
            // from the previous world.
            g_playerMovement.OldX = AngelTechData.MainCamera.position.x;
            g_playerMovement.OldY = AngelTechData.MainCamera.position.y;
            g_playerMovement.OldZ = AngelTechData.MainCamera.position.z;
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
                Vector3 c = AngelTechData.MainCamera.position;
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
        AngelTechData.MainCamera.position = SetCameraPos;
        SetCameraFlag = false;
    }

    if (AngelTechData.Ticker != 60)
    {
        AngelTechData.Ticker++;
    }
    else
    {
        AngelTechData.Ticker = 0;
    }
}
