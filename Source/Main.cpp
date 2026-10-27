#include "Core.hpp"
#include "Log.hpp"
#include "Events/EventBus.hpp"
#include "Menu/SettingsMenu.hpp"
#include "Menu/PauseMenu.hpp"
#include "ClientSettings.hpp"
#include "Input/Keybinds.hpp"
#include "Client/JoinUri.hpp"
#include "Client/ProtocolHandler.hpp"
#include "Client/Client.hpp"
#include "Script/LightningEntityManager.hpp"
#include "Script/LightningEntityRegistry.hpp"
#include "Script/LightningEntityDef.hpp"
#include "Pawn/OzPawnSystem.hpp"
#include "Pawn/AngelPlayer/GameUi.hpp"
#include "Pawn/AngelPlayer/InventoryBehaviour.hpp"
#include "Pawn/AngelPlayer/WeaponBehaviour.hpp"
#include "Pawn/AngelPlayer/PlayerController.hpp"
#include "Pawn/PickupPawns.hpp"
#include "Renderer/CombatFX.hpp"
#include "Renderer/Crosshair.hpp"
#include "Renderer/Mesh/MeshCache.hpp"
#include "Renderer/ViewModel.hpp"
#include "Renderer/PlayerModel.hpp"
#include "Menu/SkillTree.hpp"
#include "Menu/SpellBook.hpp"
#include "UI/UiHandler.hpp"
#include <cmath>
#include <memory>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <filesystem>
namespace fs = std::filesystem;

// Persist settings to System/Angels95.ini on every exit path (destructor runs
// at process teardown, after raylib has closed — stdio/stdlib only, so it is
// safe to run outside the window lifetime).
static struct ClientSettingsOnExit {
    ~ClientSettingsOnExit() { SaveClientSettings(); }
} g_saveClientSettings;

// The engine addresses every content path relative to the install root (the
// folder that holds GameData/ and System/). Launching System/Angels95.exe
// directly (double-click, shell shortcut, fullscreen launcher) leaves cwd
// inside System/, so the world OZONE, skybox and packages were never found
// even though network pawns from the server still spawned. Relocate to the
// root before anything reads a path.
static bool IsInstallRoot(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p / "GameData", ec) ||
           fs::exists(p / "System" / "Data", ec) ||
           fs::exists(p / "System" / "Angels95.ini", ec);
}

static void RelocateToInstallRoot() {
    std::error_code ec;
    fs::path cwd = fs::current_path(ec);
    if (cwd.empty() || IsInstallRoot(cwd)) return;

    fs::path exeDir = fs::path(ProtocolHandler::ExecutablePath()).parent_path();
    std::vector<fs::path> candidates;
    if (!exeDir.empty()) {
        candidates.push_back(exeDir);
        candidates.push_back(exeDir.parent_path());
    }
    if (!cwd.parent_path().empty()) candidates.push_back(cwd.parent_path());

    // Prefer a root that actually ships loose GameData; package-only installs
    // (System/Data without GameData) are the fallback.
    for (bool requireGameData : {true, false}) {
        for (const auto& c : candidates) {
            if (c.empty()) continue;
            std::error_code existsEc;
            if (requireGameData && !fs::exists(c / "GameData", existsEc)) continue;
            if (!IsInstallRoot(c)) continue;
            fs::current_path(c, ec);
            std::fprintf(stderr, "[INFO] Working directory relocated to install root: %s\n",
                         c.string().c_str());
            return;
        }
    }
}

static OmegaClient g_client;
// Network session state lives in Network/NetworkSession.hpp because Core.hpp is
// included above (line 1) and needs to read it.
bool g_network_enabled = false;

// Wire the client into InventoryBehaviour so the scoreboard can read SCORE_STATE.
// Done here (not in InventoryBehaviour) because g_client is a file-static.
static struct InventoryClientBinder {
    InventoryClientBinder() {
        InventoryBehaviour::Instance().SetClient(&g_client);
    }
} g_inventoryClientBinder;
int g_network_world_index = -1;

// The server's world list, as sent in the scene handshake. Kept so the portal path —
// which knows a world by NAME, from an OZONE `portal <WorldName>` line — can turn that
// into the INDEX the protocol uses. Populated by the same handler that derives
// g_network_world_index, because both come from one JSON payload and must agree.
static std::vector<std::string> g_serverWorldNames;

// The mission the server's current world implements, from the optional
// WORLD_CHANGE tail. Used only for the authored start/complete beats; it is NOT
// bound into GameManager, so a co-op session cannot overwrite a single-player
// Campaign.sav.
static std::string g_serverMissionName;

void NetworkRequestWorld(int world_index) {
    if (!g_network_enabled) return;      // offline: nothing to ask
    if (world_index < 0) return;         // unknown world; do not send a bad request
    g_client.request_world(world_index);
}

int NetworkWorldIndexByName(const char* world_name) {
    if (!world_name || !*world_name) return -1;
    for (size_t i = 0; i < g_serverWorldNames.size(); i++) {
        if (g_serverWorldNames[i] == world_name) return (int)i;
    }
    // -1, never 0: a miss must not silently resolve to whichever world is first.
    return -1;
}

static bool ShowInventory = false;
static bool ShowSkillTree = false;
// Spellbook modal: Q toggles it, Z/X/C/V/B cast the bar slots.
static bool ShowSpellbook = false;

// Deep-link / CLI join target. SetServerJoinIP is a raw pointer consumed long
// after parsing, so the host string must live for the whole process.
static std::string g_cliJoinHost;

// --host / --host-mission request, collected during arg parsing and resolved
// right after the loop (a --host anywhere on the line beats a --join
// anywhere else, regardless of order).
static bool g_cliHost = false;
static std::string g_cliHostMission;   // empty = plain --host, no campaign pin

// Single-player objective progression.
//
// Co-op objective state is the server's: it advances on NPC death / pickup
// collect and the client mirrors OBJECTIVE_STATE. Offline there is no server, so
// nothing advanced the local ObjectiveSystem and a campaign mission's objective
// stayed at 0 forever. These two handlers advance it from the facts the engine
// already emits, with the NPC def / pickup name in s0 (which is what the
// objective's `target` filter compares against).
static bool g_localObjectiveBridge = false;
static void LocalObjectiveFromDeath(const oz::events::Event& ev, void*) {
    oz::game::GameManager::Instance().AdvanceLocalObjectives("kill", ev.s0);
}
static void LocalObjectiveFromPickup(const oz::events::Event& ev, void*) {
    oz::game::GameManager::Instance().AdvanceLocalObjectives("collect", ev.s0);
}

static bool ApplyJoinTarget(const char* value)
{
    std::string host;
    int port = JoinUri::kDefaultPort;
    if (!JoinUri::Parse(value, host, port, JoinUri::kDefaultPort) &&
        !JoinUri::ParseHostPort(value, host, port, JoinUri::kDefaultPort))
        return false;

    g_cliJoinHost = host;
    SetServerJoinIP = g_cliJoinHost.c_str();
    SetServerJoinPort = port;
    SetServerJoinFlag = true;
    g_skipMenu = true;
    OZ_INFO("Deep link: joining %s:%d", SetServerJoinIP, SetServerJoinPort);
    return true;
}

// Recoil & crosshair state now owned by AngelPlayer::WeaponBehaviour.


static Color unpack_color(uint32_t packed) {
    return (Color){
        (unsigned char)(packed & 0xFF),
        (unsigned char)((packed >> 8) & 0xFF),
        (unsigned char)((packed >> 16) & 0xFF),
        (unsigned char)((packed >> 24) & 0xFF)
    };
}

// Best-effort damage source for the directional indicator. PLAYER_HURT carries
// no attacker id or position (`PlayerHurtData` is victim/damage/health only), so
// the nearest live hostile is the best attribution the client can make. If no
// pawn is alive the arc is simply skipped - a wrong direction is worse than
// none.
static void FeedDamageDirection() {
    const Vector3 cam = AngelTechData.MainCamera.position;
    Vector3 fwd = Vector3Subtract(AngelTechData.MainCamera.target, cam);
    fwd.y = 0.0f;
    const Pawn* best = nullptr;
    float bestDistSq = 0.0f;
    for (const Pawn& p : PawnSystem::Instance().GetPawns()) {
        if (!p.active || p.state == PawnState::DEAD) continue;
        const float d = Vector3DistanceSqr(p.position, cam);
        if (!best || d < bestDistSq) { best = &p; bestDistSq = d; }
    }
    if (best)
        oz::CombatFeedback::Instance().DamageFrom(best->position, cam, fwd);
}

// ---------------------------------------------------------------------------
// Remote player rendering — drawn inside DrawWorld()'s BeginMode3D pass
// (called from Core.hpp). TODO: Move into Render/
// ---------------------------------------------------------------------------
// Per-remote-player animation state (playback lives outside the shared mesh).
struct RemoteAnimState {
    net::NetVec3 last{0, 0, 0};
    bool hasLast = false;
    float time = 0.0f;
    int clip = -1;
};

void DrawRemotePlayers3D(Shader litShader) {
    if (!g_network_enabled || !g_client.is_connected()) return;

    // Per-character model cache. The id comes from PLAYER_PROFILE; unknown ids
    // fall through to the Player def, so an old server or a missing character
    // never makes a player invisible.
    auto& pm = oz::PlayerModel::Instance();
    const EntityDef* pdef = LightningEntityRegistry::Instance().Find("Player");
    const float dt = GetFrameTime();

    static std::unordered_map<uint32_t, RemoteAnimState> anim;
    const auto& players = g_client.remote_players();
    for (const auto& rp : players) {
        if (!rp.active) continue;
        oz::Mesh* mesh = pm.Get(rp.characterId);
        oz::SkeletalMesh* skel = pm.Skeletal(rp.characterId);
        const bool animated = skel != nullptr;

        // Scale the character to the local player's collision silhouette. A
        // `model_height` stat on the Player def overrides the target; a NEGATIVE
        // value means "draw at the authored scale, do not normalise".
        float modelScale = pm.NormalisedScale(rp.characterId, g_playerMovement.Height);
        if (pdef) {
            auto mh = pdef->stats.floats.find("model_height");
            if (mh != pdef->stats.floats.end()) {
                if (mh->second < 0.0f) modelScale = 1.0f;
                else if (mh->second > 0.0f)
                    modelScale = pm.NormalisedScale(rp.characterId, mh->second);
            }
        }

        // rp.position is the sender's camera (eye) position; draw from the feet
        // so remote bodies rest on the ground instead of floating at eye level.
        float eyeH = (rp.stance == net::STANCE_CROUCH)
                   ? g_playerMovement.CrouchEyeHeight
                   : g_playerMovement.StandEyeHeight;
        Vector3 pos = {rp.position.x, rp.position.y - eyeH, rp.position.z};
        Color col = unpack_color(rp.color_packed);

        if (mesh) {
            int clip = -1;
            float clipTime = 0.0f;
            if (animated) {
                RemoteAnimState& a = anim[rp.player_id];
                float dx = rp.position.x - a.last.x;
                float dz = rp.position.z - a.last.z;
                bool moving = a.hasLast && (dx * dx + dz * dz) > 0.0004f;
                const angelsmenu::CharacterDef* cdef =
                    angelsmenu::CharacterRoster::Instance().Find(rp.characterId);
                const std::string idle = (cdef && !cdef->animIdle.empty())
                    ? cdef->animIdle : (pdef ? pdef->animIdle : "");
                const std::string chase = (cdef && !cdef->animChase.empty())
                    ? cdef->animChase : (pdef ? pdef->animChase : "");
                int wanted = skel->ResolveClip(moving ? chase : idle);
                if (wanted < 0) wanted = 0;
                if (wanted != a.clip) { a.clip = wanted; a.time = 0.0f; }
                a.time += dt * (rp.stance == net::STANCE_SPRINT ? 1.5f : 1.0f);
                a.last = rp.position;
                a.hasLast = true;
                clip = a.clip;
                clipTime = a.time;
            }
            oz::MeshTransform mt;
            mt.position = pos;
            mt.yaw = rp.yaw * RAD2DEG;
            // Replicated crouch: squash the model from the feet so it reads as
            // a lowered stance (static stub - no skeletal crouch clip yet).
            float yScale = (rp.stance == net::STANCE_CROUCH) ? 0.65f : 1.0f;
            mt.scale = {modelScale, modelScale * yScale, modelScale};
            pm.DrawInstance(rp.characterId, mt, litShader, clip, clipTime);
        } else {
            // Fallback capsule scaled to the local player box so remote avatars
            // match the collision silhouette (body Height, width/2 radius).
            const float height = g_playerMovement.Height * (rp.stance == net::STANCE_CROUCH ? 0.65f : 1.0f);
            const float radius = g_playerMovement.Width * 0.5f;
            DrawCylinder(pos, radius, radius, height, 8, col);
            DrawSphere({pos.x, pos.y + height, pos.z}, radius * 0.8f, col);
        }

        // Facing indicator — kept in both paths so players stay distinguishable.
        DrawLine3D({pos.x, pos.y + g_playerMovement.Height * 0.6f, pos.z},
                   {pos.x + sinf(rp.yaw) * 3.0f, pos.y + g_playerMovement.Height * 0.6f,
                    pos.z + cosf(rp.yaw) * 3.0f},
                   col);
    }
}



// ---------------------------------------------------------------------------
// Inventory overlay (draw when Tab pressed)
// ---------------------------------------------------------------------------
// ---- Debug Console ----
// g_showCollisionDebug defined in Core.hpp
static bool g_consoleOpen = false;
static char g_consoleBuf[256] = "";
static int g_consoleCursor = 0;
static wstring g_consoleHistory[16];
static int g_consoleHistoryCount = 0;
static int g_consoleHistoryPos = 0;

static void ExecuteConsoleCommand(const char* cmd) {
    if (!cmd || !cmd[0]) return;
    // Store in history
    if (g_consoleHistoryCount < 16) {
        size_t len = strlen(cmd);
        for (size_t i = 0; i < len; i++) g_consoleHistory[g_consoleHistoryCount] += (wchar_t)cmd[i];
        g_consoleHistoryCount++;
    }
    g_consoleHistoryPos = g_consoleHistoryCount;

    // Parse command
    if (strncmp(cmd, "/summon ", 8) == 0) {
        const char* arg = cmd + 8;
        // Skip leading spaces
        while (*arg == ' ') arg++;
        if (!*arg) return;

        // Try matching item name
        int itemId = gInventory.SummonItem(arg);
        if (itemId >= 0) {
            gInventory.AddToBackpack(itemId, 1);
            return;
        }

        // Try matching weapon/entity name (LightningEntityRegistry)
        {
            auto& regAll = LightningEntityRegistry::Instance().GetAll();
            bool found = false;
            for (auto& [regName, def] : regAll) {
                const char* a = arg;
                const char* b = regName.c_str();
                bool match = true;
                while (*a && *b) {
                    char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
                    char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
                    if (ca != cb) { match = false; break; }
                    a++; b++;
                }
                if (match && *a == *b) {
                    int instIdx = LightningEntityManager::Instance().Spawn(def.name);
                    if (instIdx >= 0) {
                        // Place into the first empty hotbar slot AND select it
                        // so the freshly-summoned weapon is immediately ready
                        // to fire. HotbarAssign does not move m_selectedSlot on
                        // its own (by design: it is also called from save/load
                        // and from script actions that must not change the
                        // player's hand), so /summon does that explicitly here.
                        auto& lem = LightningEntityManager::Instance();
                        int slot = lem.HotbarFirstFreeSlot();
                        if (slot < 0) {
                            lem.Despawn(instIdx);
                        } else {
                            lem.HotbarAssign(slot, instIdx);
                            lem.SelectSlot(slot);
                        }
                    }
                    found = true;
                    break;
                }
            }
            if (!found)
                OZ_INFO("No match for /summon '%s'", arg);
        }
    } else if (strncmp(cmd, "/campaign", 9) == 0) {
        // Start the SP campaign (or a named mission) via the game hierarchy.
        auto& gm = oz::game::GameManager::Instance();
        if (!gm.Loaded()) {
            OZ_WARN("No game hierarchy loaded (GameData/Game)");
        } else {
            const char* arg = cmd + 9;
            while (*arg == ' ') arg++;
            std::string mission = (*arg) ? arg : std::string();
            std::string world;
            if (mission.empty()) {
                world = gm.FirstMissionWorld();
                const oz::game::SectorDef* sec = gm.FindSector(gm.Game().startSector);
                if (sec && !sec->missions.empty()) mission = sec->missions.front();
            } else {
                world = gm.MissionWorld(mission);
            }
            if (world.empty()) {
                OZ_WARN("Campaign mission '%s' not found", mission.c_str());
            } else {
                gm.SetActiveMission(mission);
                EmitMissionBeat("mission.start", mission);
                strncpy(g_world_to_load, world.c_str(), sizeof(g_world_to_load) - 1);
                g_world_to_load[sizeof(g_world_to_load) - 1] = '\0';
                SetSceneFlag = true;
                OZ_INFO("Campaign: starting mission '%s' in world '%s'",
                        mission.c_str(), world.c_str());
            }
        }
    } else if (strncmp(cmd, "/world ", 7) == 0) {
        const char* name = cmd + 7;
        while (*name == ' ') name++;
        if (*name) {
            // Check if the world exists
            char worldPath[512];
            snprintf(worldPath, sizeof(worldPath), "GameData/Worlds/%s/World.ozone", name);
            if (IsPathFile(worldPath)) {
                strncpy(g_world_to_load, name, sizeof(g_world_to_load) - 1);
                g_world_to_load[sizeof(g_world_to_load) - 1] = '\0';
                OZ_INFO("World switch: %s -> %s", g_world_to_load, name);
                SetSceneFlag = true;
                AngelTechData.MainCamera.position = (Vector3){0.0f, 20.0f, 0.0f};
                AngelTechData.MainCamera.target = (Vector3){0.0f, 20.0f, -10.0f};
            } else {
                OZ_WARN("World '%s' not found in GameData/Worlds/", name);
            }
        }
    } else if (strcmp(cmd, "/worlds") == 0) {
        // List available worlds
        OZ_INFO("Available worlds in GameData/Worlds/:");
        if (fs::exists("GameData/Worlds")) {
            for (auto& entry : fs::directory_iterator("GameData/Worlds")) {
                if (entry.is_directory()) {
                    std::string dirName = entry.path().filename().string();
                    std::string oz  = entry.path().string() + "/World.ozone";
                    if (IsPathFile(oz.c_str()))
                        OZ_INFO("  %s", dirName.c_str());
                }
            }
        }
    } else if (strcmp(cmd, "/world") == 0) {
        fprintf(stderr, "WORLD: current=%s (use /world <name> or /worlds to list)\n", g_world_to_load);
    } else if (strcmp(cmd, "/fly") == 0) {
        g_playerMovement.isFlying = !g_playerMovement.isFlying;
        if (g_playerMovement.isFlying) {
            g_playerMovement.onGround = false;
            g_playerMovement.velocityY = 0.0f;
            OZ_INFO("FLY enabled");
        } else {
            OZ_INFO("FLY disabled");
        }
    } else if (strcmp(cmd, "/noclip") == 0) {
        g_playerMovement.isNoClip = !g_playerMovement.isNoClip;
        if (g_playerMovement.isNoClip) {
            g_playerMovement.onGround = false;
            g_playerMovement.velocityY = 0.0f;
            OZ_INFO("NOCLIP enabled");
        } else {
            OZ_INFO("NOCLIP disabled");
        }
    } else if (strcmp(cmd, "/showcollisions") == 0) {
        g_showCollisionDebug = !g_showCollisionDebug;
        OZ_INFO("Collision debug %s", g_showCollisionDebug ? "ON" : "OFF");
    } else if (strncmp(cmd, "/connect ", 9) == 0 || strcmp(cmd, "/connect") == 0) {
        const char* arg = (cmd[8] == ' ') ? cmd + 9 : "";
        while (*arg == ' ') arg++;
        std::string host;
        int port = JoinUri::kDefaultPort;
        if (!JoinUri::ParseHostPort(arg, host, port, JoinUri::kDefaultPort)) {
            OZ_WARN("Usage: /connect <ip|host>[:port]");
        } else if (g_network_enabled) {
            OZ_WARN("Already connected - use /disconnect first");
        } else {
            static std::string s_consoleJoinHost;
            s_consoleJoinHost = host;
            SetServerJoinIP = s_consoleJoinHost.c_str();
            SetServerJoinPort = port;
            SetServerJoinFlag = true;
            g_network_enabled = g_client.connect(SetServerJoinIP, SetServerJoinPort);
            OZ_INFO("Connecting to %s:%d", SetServerJoinIP, SetServerJoinPort);
        }
    } else if (strcmp(cmd, "/registerprotocol") == 0) {
        if (ProtocolHandler::EnsureRegistered())
            OZ_INFO("angels95:// protocol handler registered -> %s",
                    ProtocolHandler::ExecutablePath().c_str());
        else
            OZ_WARN("Could not register angels95:// protocol handler");
    } else if (strcmp(cmd, "/disconnect") == 0) {
        if (g_network_enabled) {
            g_client.disconnect();
            g_network_enabled = false;
            OZ_INFO("Disconnected from server");
        } else {
            OZ_WARN("Not connected to any server");
        }
    } else if (strcmp(cmd, "/servers") == 0) {
        OZ_INFO("Use the Multiplayer tab in the main menu to discover servers");
    }
}

static void DrawConsole() {
    if (!g_consoleOpen) return;
    int sw = GetScreenWidth();
    int ch = 200;
    int cy = GetScreenHeight() - ch;

    DrawRectangle(0, cy, sw, ch, (Color){0, 0, 0, 200});
    DrawRectangleLines(0, cy, sw, ch, (Color){100, 100, 180, 200});

    // History
    int lineY = cy + ch - 40;
    for (int i = g_consoleHistoryCount - 1; i >= 0 && lineY > cy + 5; i--) {
        std::string hist(g_consoleHistory[i].begin(), g_consoleHistory[i].end());
        DrawText(hist.c_str(), 10, lineY - 18, 14, LIGHTGRAY);
        lineY -= 18;
    }

    // Input line
    DrawText("> ", 10, cy + ch - 22, 14, GREEN);
    DrawText(g_consoleBuf, 28, cy + ch - 22, 14, WHITE);
    // Cursor blink
    if ((int)(GetTime() * 4) % 2 == 0) {
        int cx = 28 + MeasureText(g_consoleBuf, 14);
        DrawText("_", cx, cy + ch - 22, 14, WHITE);
    }
}

static void HandleConsoleInput() {
    if (!g_consoleOpen) return;
    int key = GetCharPressed();
    while (key > 0) {
        if (key >= 32 && key <= 126 && g_consoleCursor < 255) {
            g_consoleBuf[g_consoleCursor++] = (char)key;
            g_consoleBuf[g_consoleCursor] = '\0';
        }
        key = GetCharPressed();
    }
    if (IsKeyPressed(KEY_BACKSPACE) && g_consoleCursor > 0) {
        g_consoleBuf[--g_consoleCursor] = '\0';
    }
    if (IsKeyPressed(KEY_ENTER)) {
        ExecuteConsoleCommand(g_consoleBuf);
        g_consoleBuf[0] = '\0';
        g_consoleCursor = 0;
    }
    if (IsKeyPressed(KEY_UP) && g_consoleHistoryPos > 0) {
        g_consoleHistoryPos--;
        std::string hist(g_consoleHistory[g_consoleHistoryPos].begin(), g_consoleHistory[g_consoleHistoryPos].end());
        strncpy(g_consoleBuf, hist.c_str(), 255);
        g_consoleCursor = strlen(g_consoleBuf);
    }
    if (IsKeyPressed(KEY_DOWN) && g_consoleHistoryPos < g_consoleHistoryCount) {
        g_consoleHistoryPos++;
        if (g_consoleHistoryPos >= g_consoleHistoryCount) {
            g_consoleBuf[0] = '\0';
            g_consoleCursor = 0;
        } else {
            std::string hist(g_consoleHistory[g_consoleHistoryPos].begin(), g_consoleHistory[g_consoleHistoryPos].end());
            strncpy(g_consoleBuf, hist.c_str(), 255);
            g_consoleCursor = strlen(g_consoleBuf);
        }
    }
}

int main(int argc, char** argv){
    RelocateToInstallRoot();

    // CLI args
    bool worldExplicit = false;
    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];
        const std::string argLower = JoinUri::Lower(arg);
        if (strcmp(arg, "--world") == 0 && i + 1 < argc) {
            strncpy(g_world_to_load, argv[++i], sizeof(g_world_to_load) - 1);
            g_world_to_load[sizeof(g_world_to_load) - 1] = '\0';
            g_skipMenu = true;
            worldExplicit = true;
        } else if (strcmp(arg, "--world-dir") == 0 && i + 1 < argc) {
            strncpy(g_world_dir_override, argv[++i], sizeof(g_world_dir_override) - 1);
            g_world_dir_override[sizeof(g_world_dir_override) - 1] = '\0';
        } else if (strcmp(arg, "--shot") == 0 && i + 1 < argc) {
            g_shot.active = true;
            g_shot.outPath = argv[++i];
            g_skipMenu = true;   // a shot run never wants the title menu
        } else if (strcmp(arg, "--shot-delay") == 0 && i + 1 < argc) {
            g_shot.delayFrames = atoi(argv[++i]);
        } else if (strcmp(arg, "--shot-res") == 0 && i + 1 < argc) {
            int w = 0, h = 0;
            if (sscanf(argv[++i], "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                g_shot.resWidth = w;
                g_shot.resHeight = h;
            } else {
                OZ_WARN("Bad --shot-res (expected WxH): %s", argv[i]);
            }
        } else if (strcmp(arg, "--shot-cam") == 0 && i + 1 < argc) {
            ShotCam cam;
            if (ParseShotCam(argv[++i], cam))
                g_shot.cams.push_back(cam);
            else
                OZ_WARN("Bad --shot-cam (expected x,y,z,yaw[,pitch]): %s", argv[i]);
        } else if (strcmp(arg, "--shot-hud") == 0) {
            g_shot.hideHud = false;
        } else if (strcmp(arg, "--menu-shot") == 0 && i + 1 < argc) {
            const std::string page = JoinUri::Lower(argv[++i]);
            if (page == "home") g_menuShotPage = 0;
            else if (page == "play") g_menuShotPage = 1;
            else if (page == "practice") { g_menuShotPage = 1; g_menuShotPractice = true; }
            else if (page == "multiplayer") g_menuShotPage = 2;
            else if (page == "character") g_menuShotPage = 3;
            else if (page == "settings") g_menuShotPage = 4;
            else OZ_WARN("Bad --menu-shot page (home|play|practice|multiplayer|character|settings): %s", page.c_str());
            g_skipMenu = false;   // menu capture must run the real menu
        } else if (strcmp(arg, "--menu-shot-out") == 0 && i + 1 < argc) {
            g_menuShotOut = argv[++i];
        } else if (strcmp(arg, "--menu-shot-frames") == 0 && i + 1 < argc) {
            g_menuShotFrames = std::max(2, atoi(argv[++i]));
        } else if ((strcmp(arg, "--join") == 0 || strcmp(arg, "--connect") == 0) && i + 1 < argc) {
            if (!ApplyJoinTarget(argv[++i]))
                OZ_WARN("Invalid --join target: %s", argv[i]);
        } else if (strncmp(arg, "--join=", 7) == 0 || strncmp(arg, "--connect=", 10) == 0) {
            const char* value = arg + ((arg[2] == 'j') ? 7 : 10);
            if (!ApplyJoinTarget(value))
                OZ_WARN("Invalid --join target: %s", value);
        } else if (strcmp(arg, "--host") == 0) {
            g_cliHost = true;
        } else if (strcmp(arg, "--host-mission") == 0 && i + 1 < argc) {
            g_cliHost = true;
            g_cliHostMission = argv[++i];
        } else if (strncmp(arg, "--host-mission=", 15) == 0) {
            g_cliHost = true;
            g_cliHostMission = arg + 15;
        } else if (argLower.rfind("angels95://", 0) == 0) {
            if (!ApplyJoinTarget(arg))
                OZ_WARN("Unrecognized join URI: %s", arg);
        }
    }

    // --host / --host-mission: spawn a local dedicated server, then
    // self-join it. Resolved after the whole loop so --host anywhere on the
    // line beats a --join / deep link anywhere else, regardless of order.
    if (g_cliHost) {
        if (SetServerJoinFlag) {
            OZ_WARN("--host: ignoring --join target '%s' (hosting wins)", SetServerJoinIP);
            SetServerJoinFlag = false;
        }
        // No client-side mission check: the server resolves the name through
        // its own GameManager and refuses to start on an unknown one. A
        // refusal is loud in System/HostedServer.log and this client's
        // self-join then fails visibly rather than silently joining nothing.
        if (!LaunchHostedServer(27015, g_cliHostMission.empty() ? nullptr : g_cliHostMission.c_str())) {
            OZ_ERROR("--host: server launch failed");
            return 1;
        }
        SetServerJoinIP = "127.0.0.1";
        SetServerJoinPort = 27015;
        SetServerJoinFlag = true;
        g_skipMenu = true;   // same auto-skip route --join / deep links take
    }

    // A --shot run without an explicit --world falls back to the compiled-in
    // default. Make that loud rather than silently capturing the wrong map.
    if (g_shot.active && !worldExplicit)
        OZ_WARN("--shot given without --world; falling back to '%s'", g_world_to_load);
    if (g_shot.active) {
        if (g_shot.outPath.empty())
            OZ_ERROR("Screenshot mode needs an output path (--shot <file.png>)");
        EnsureShotOutputDir(g_shot.outPath);
        OZ_INFO("Screenshot mode: world='%s' shots=%d delay=%d hideHud=%d out='%s'",
                g_world_to_load, (int)g_shot.cams.size(), g_shot.delayFrames,
                (int)g_shot.hideHud, g_shot.outPath.c_str());
    }
    if (g_menuShotPage >= 0) {
        if (g_menuShotOut.empty()) g_menuShotOut = "System/menu_shot.png";
        EnsureShotOutputDir(g_menuShotOut);
        OZ_INFO("Menu shot: page=%d out='%s' frames=%d",
                g_menuShotPage, g_menuShotOut.c_str(), g_menuShotFrames);
    }

    // Load persisted user settings BEFORE window creation so window size,
    // VSync and MSAA are applied by InitWindow.
    LoadClientSettings();

    if (g_menuShotPage >= 0 && g_shot.resWidth > 0 && g_shot.resHeight > 0) {
        ConfigWindowWidth = g_shot.resWidth;
        ConfigWindowHeight = g_shot.resHeight;
        OZ_INFO("Menu shot resolution: %dx%d", ConfigWindowWidth, ConfigWindowHeight);
    }

    if (g_shot.active) {
        // Deterministic, reproducible captures: nothing that varies run to run
        // may be left on.
        if (g_shot.resWidth > 0 && g_shot.resHeight > 0) {
            ConfigWindowWidth = g_shot.resWidth;
            ConfigWindowHeight = g_shot.resHeight;
        }
        VSYNCToggle = false;   // a capped frame rate makes --shot-delay a wall-clock guess
        MXAAToggle  = false;   // the 3D pass renders into a non-MSAA render texture anyway
        PixelShader = false;  // post effects are the user's taste, not the level's
        JitterEnabled = false;
        FogEnabled  = false;
        HeadBob     = false;
        Debug       = false;  // light gizmos
        FPSEnabled  = false;
        ShowSettings = false;
        ShowInventory = false;
ShowSkillTree = false;
ShowSpellbook = false;
    }

    // Browser deep links (angels95://join/...) need an OS protocol handler.
    // HKCU / user .desktop only, so no elevation is required.
    if (!g_shot.active && ProtocolHandler::EnsureRegistered())
        OZ_INFO("Protocol handler: angels95:// -> %s", ProtocolHandler::ExecutablePath().c_str());
    else if (!g_shot.active)
        OZ_WARN("Protocol handler: angels95:// registration unavailable");
    if (VSYNCToggle) SetConfigFlags(FLAG_VSYNC_HINT);
    if (MXAAToggle)  SetConfigFlags(FLAG_MSAA_4X_HINT);
    // Borderless splash window: stays hidden while the engine boots, the asset
    // pipeline resolves, and the first world frame is rendered. PlaySplashScreen
    // makes the window visible only after that gate fires, so the user never
    // sees an undecorated title-bar or an empty client area. Honor it only on
    // the client - the editor owns its own splash and its window configuration
    // (FLAG_VSYNC_HINT is set in AngelEd/Source/Main.cpp).
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    // Borderless unless the player turned it off (Settings -> Window). A shot
    // run always wants the plain window so a capture is not affected.
    if (!g_shot.active && BorderlessWindow) SetConfigFlags(FLAG_WINDOW_UNDECORATED);

    InitWindow(ConfigWindowWidth, ConfigWindowHeight,
               g_shot.active ? "Angels95 [shot]" : "Angels95");
    SetWindowMinSize(960, 600);
    SetExitKey(0);
    SetTargetFPS(60);

    // Audio is pointless for a capture run and the device init adds latency and
    // a failure mode on headless CI boxes.
    if (!g_shot.active) {
        InitAudioDevice();

        if (IsAudioDeviceReady()) {
            // Wire the zone reverb DSP into the master output mix.
            SoundManager::Instance().AttachReverbProcessor();
        } else {
            CloseAudioDevice();
        }
        ApplyMasterVolume();
        // Per-category gains (music/sfx), restored from System/Angels95.ini.
        SoundManager::Instance().SetMusicGain(MusicVolume / 100.0f);
        SoundManager::Instance().SetSfxGain(SfxVolume / 100.0f);
    }

    AngelTechInit();
    GameUi::Instance().Init();
    InventoryBehaviour::Instance().SetMessageSink([](const std::string& msg) {
        AngelTechTextSystem.Write(msg);
    });
    InventoryBehaviour::Instance().SetFeedbackSink([]() {
        SoundManager::Instance().PlayUIClick();
    });
    WeaponBehaviour::Instance().SetClient(&g_client, &g_network_enabled);
    PickupPawns::Instance().SetClient(&g_client);

    // Pause state lives outside the per-frame loop so the native menu's
    // Settings item can pause the world too. `g_settingsPausedGame` distinguishes
    // "settings paused a running game" (resume on close) from "settings was
    // opened from the pause menu" (stay paused).
    static bool g_gamePaused = false;
    static bool g_settingsPausedGame = false;

    oz::ui::CreateNativeMenuBar({
        []() { SetSceneId = AngelTechData.LevelIndex; SetSceneFlag = true; },
        []() { SaveGame(); },
        []() { g_quitRequested = true; },
        []() {
            ShowSettings = !ShowSettings;
            if (ShowSettings) {
                if (!g_gamePaused) g_settingsPausedGame = true;
                g_gamePaused = true;
                ShowCursor(); EnableCursor();
            } else if (g_settingsPausedGame) {
                g_gamePaused = false;
                g_settingsPausedGame = false;
                HideCursor(); DisableCursor();
            }
        },
        []() { oz::ui::ShowAboutDialog(); },
    });

    static bool g_returnToMenu = false;

    // Reveal gate for the borderless window created hidden above. The splash
    // must run HERE - after the engine and the asset pipeline have finished
    // booting, but BEFORE PlayHomeScreen(), which blocks on menu.Tick(): a
    // hidden Win32 window is not hit-tested, so the title menu would receive no
    // clicks at all and the process would look hung.
    PlaySplashScreen();

    // Set by the screenshot tick once every camera is captured. It ends both
    // loops without closing the window, so the single CloseWindow() at the end
    // of main stays the only one (see the note at the shot-complete branch).
    bool shotRunComplete = false;

    // Outer loop: return to menu after gameplay
    while (!WindowShouldClose() && !shotRunComplete && !g_quitRequested) {
    // EVERY pass through this loop is a new session: the previous one ended when
    // the inner loop broke (menu/quit). Reset all cross-session state HERE, before
    // the menu can restore a save, so campaign -> practice and -> multiplayer
    // start clean. Doing it inside LoadWorld() would be wrong: portals and
    // campaign world transitions intentionally keep the player's gear.
    if (g_client.is_connected()) g_client.disconnect();
    g_network_enabled = false;
    g_network_world_index = -1;
    g_serverWorldNames.clear();
    g_serverMissionName.clear();
    // UI overlays are Main.cpp statics, so the reset cannot live in Core.hpp.
    ShowInventory = false;
    ShowSkillTree = false;
    ShowSpellbook = false;
    ShowSettings = false;
    g_consoleOpen = false;
    ResetSessionState();
    PlayHomeScreen();
    if (WindowShouldClose() || g_quitRequested) break;
    g_gamePaused = false;
    g_settingsPausedGame = false;
    g_returnToMenu = false;

    // Arm the session before the world loads. After PlayHomeScreen on purpose:
    // a Continue has restored its save at that point, so an armed player is left
    // alone and only an empty hotbar is filled.
    LightningEntityManager::Instance().EnsureStartingWeapon();

    // Local objective bridge: registered once per process (the bus is not
    // cleared between sessions, so a per-session Subscribe would stack).
    if (!g_localObjectiveBridge) {
        g_localObjectiveBridge = true;
        oz::events::EventBus::Instance().Subscribe("entity.death", &LocalObjectiveFromDeath, nullptr);
        oz::events::EventBus::Instance().Subscribe("pickup.collect", &LocalObjectiveFromPickup, nullptr);
    }

    LoadWorld();
    g_client.set_on_chat_received([](const std::string& msg) {
        AngelTechTextSystem.Write(msg);
    });
    g_client.set_on_item_collected([](int item_id, int quantity) {
        InventoryBehaviour::Instance().OnItemCollected(item_id, quantity);
    });

    g_client.set_on_player_hurt([](int damage, float remaining_health) {
        LightningEntityManager::Instance().SetPlayerHealth(remaining_health);
        // Directional damage cue: the vignette below says "you were hit", this
        // says from where. Best-effort source, see FeedDamageDirection.
        FeedDamageDirection();
        // Split from death: this fires on every point of damage, and it
        // previously played the death sound for all of them, so a firefight
        // sounded like a montage of deaths. Routed through PlayerController so
        // it shares the .ozls hurt_sound / death_sound resolution and the
        // single-frame dedupe with the script `damage` opcode path.
        PlayerController::Instance().PlayHurt(remaining_health <= 0.0f);
        // EventBus: player.hurt / player.death, dispatched to the Player def as
        // on_player_hurt / on_player_death (script subscription point).
        oz::events::Event ev;
        ev.topic = (remaining_health <= 0.0f) ? "player.death" : "player.hurt";
        ev.s0 = "Player";
        ev.f0 = (float)damage;
        oz::events::EventBus::Instance().Emit(ev);
    });

    // Weapon pickup granted by the server (item_id 15) — add to hotbar.
    g_client.set_on_weapon_collected([](const char* weapon_def_name) {
        InventoryBehaviour::Instance().OnWeaponCollected(weapon_def_name);
    });

    // The server's pickup list drives the DRAWN PickupNodes. Without this the
    // networked collect only flipped `active` on OmegaClient's own copy of the
    // list, which nothing renders — so a collected pickup stayed on the floor and
    // every walk back over it re-requested something already consumed.
    g_client.set_on_pickups_changed([](const std::vector<ClientPickup>& snapshot) {
        // Convert to PickupNode so PawnSystem stays unaware of the wire type.
        //
        // Filtered to OUR world here, on purpose: pickup ids restart at 0 in every
        // world, so a snapshot holding several worlds would collide (world 1's
        // pickup 3 and world 0's pickup 3 are different things with the same id)
        // and whichever came last would win. The filter lives next to
        // cp.world_index so the collision is visible.
        std::vector<PickupNode> net;
        net.reserve(snapshot.size());
        for (const auto& cp : snapshot) {
            if (cp.world_index != g_network_world_index) continue;
            PickupNode n;
            n.netId    = cp.id;
            n.position = {cp.position.x, cp.position.y, cp.position.z};
            n.active   = cp.active;
            n.typeName = cp.typeName;
            net.push_back(std::move(n));
        }
        PawnSystem::Instance().ApplyPickupNetState(net, g_network_world_index);
    });

    // The server's authoritative world assignment.
    //
    // This is the reconciliation point between the two world indices, and it has to
    // update g_network_world_index because THAT is the value the pickup filter above
    // and WeaponBehaviour's NPC-damage reports read. Without it, g_network_world_index
    // only ever moved in the scene handshake (which follows the SERVER's active world,
    // not the player's), so after a portal the client filtered pickups by the wrong
    // world and stamped NPC damage with the wrong index.
    //
    // Guard the -1 case explicitly: WORLD_CHANGE carries a real index, but if a future
    // peer ever sent none, assigning -1 would make PickupPawns' filter reject every
    // pickup and the server reject every collect — the exact failure the "must never be
    // -1" rule exists to prevent.
    g_client.set_on_world_changed([](int world_index, bool accepted) {
        if (world_index < 0) {
            OZ_WARN("Network: WORLD_CHANGE carried no world index — keeping %d",
                    g_network_world_index);
            return;
        }
        if (world_index != g_network_world_index) {
            OZ_INFO("Network: server moved us to world %d (accepted=%d, was %d)",
                    world_index, (int)accepted, g_network_world_index);
            g_network_world_index = world_index;
            // Co-op narrative: the mission name rides on WORLD_CHANGE, so a
            // client that never launched a campaign still gets the authored
            // beats. `complete` fires before `start` because stepping from one
            // mission's world into the next is both.
            {
                const std::string serverMission = g_client.server_mission();
                if (!serverMission.empty() && serverMission != g_serverMissionName) {
                    if (!g_serverMissionName.empty())
                        EmitMissionBeat("mission.complete", g_serverMissionName);
                    g_serverMissionName = serverMission;
                    EmitMissionBeat("mission.start", g_serverMissionName);
                }
            }
            // Load the world the server moved us to (index -> name). A
            // server-driven COOP mission transition arrives here.
            if (world_index < (int)g_serverWorldNames.size()) {
                const std::string& wn = g_serverWorldNames[world_index];
                // A player who did NOT open the exit door still completes the
                // mission: the server moves the whole party together. The
                // requester already advanced at its own portal, so for them the
                // active mission no longer points at this world.
                {
                    auto& gm = oz::game::GameManager::Instance();
                    const std::string active = gm.ActiveMission();
                    if (!wn.empty() && gm.Loaded() && !active.empty() &&
                        gm.NextMissionWorld(active) == wn) {
                        EmitMissionBeat("mission.complete", active);
                        if (const oz::game::MissionDef* nm = gm.FindMissionByWorld(wn))
                            gm.SetActiveMission(nm->name);
                    }
                }
                if (!wn.empty() && wn != g_world_to_load) {
                    strncpy(g_world_to_load, wn.c_str(), sizeof(g_world_to_load) - 1);
                    g_world_to_load[sizeof(g_world_to_load) - 1] = '\0';
                    // Apply the authoritative spawn if the server sent one.
                    if (g_client.has_world_spawn()) {
                        net::NetVec3 sp = g_client.world_spawn();
                        g_portalSpawnPos = {sp.x, sp.y, sp.z};
                        g_portalTransitionPending = true;
                    }
                    SetSceneFlag = true;
                }
            }
        } else if (!accepted) {
            // Refused: we are already where the server thinks we are, so the index is
            // right. Worth logging because the client's own world did NOT change and a
            // player who walked into a portal would otherwise see pickups simply stop
            // working with no explanation.
            OZ_WARN("Network: world request refused — server has us in world %d",
                    world_index);
        }
        // A refused request can leave the client rendering a world the server is not
        // in. Ask for pickups so the drawn world is populated rather than empty, and
        // so anything already drawn from a stale snapshot gets corrected.
        if (g_network_enabled) g_client.request_pickup_resync();
    });

    // Ammo changes (fire/reload) are reported to the server for remote sync.
    LightningEntityManager::Instance().set_on_ammo_changed([](int slot, int ammo, int magazine, int action) {
        if (g_network_enabled) g_client.send_weapon_ammo(slot, ammo, magazine, action);
    });

    // Melee hits are resolved client-side (nearest pawn in reach, unobstructed)
    // and reported here; the server re-validates reach and applies the damage.
    LightningEntityManager::Instance().set_on_melee_hit(
        [](int world, int npc, int part, int dmg, float reach, float cost,
           float ox, float oy, float oz, float dx, float dy, float dz) {
            if (g_network_enabled)
                g_client.send_melee_hit(world, npc, part, dmg, reach, cost,
                                        ox, oy, oz, dx, dy, dz);
        });

    // Server tells us which world is active — switch to it on mismatch.
    g_client.set_on_scene_received([](const std::string& sceneJson) {
        // Tiny hand-parse of the fixed server template: "active_world":"<name>"
        static const std::string key = "\"active_world\":\"";
        size_t kpos = sceneJson.find(key);
        if (kpos == std::string::npos) return;
        size_t start = kpos + key.size();
        // Tolerate servers that emitted a second pair of quotes around the
        // value ("active_world":""Name"") before this was fixed.
        while (start < sceneJson.size() && sceneJson[start] == '"') ++start;
        size_t end = sceneJson.find('"', start);
        if (end == std::string::npos) return;
        std::string activeWorld = sceneJson.substr(start, end - start);

        // Record which world index the server has us in. The server indexes
        // worlds by their position in the "worlds":[...] array it sent, so the
        // index is that array position — this is the id the pickup/NPC packets
        // carry and the only one collect_pickup() will accept.
        //
        // On a parse miss we leave the previous value in place and ask for a
        // pickup re-sync. This used to assign -1, which made PickupPawns'
        // `p.world_index != local_world` filter reject EVERY pickup and the
        // server's own world check reject every collect — silently, with no log,
        // no request ever sent, and pickups drawn on the floor that could not be
        // picked up. A stale-but-plausible index is strictly better than -1.
        {
            static const std::string wkey = "\"worlds\":[";
            size_t wpos = sceneJson.find(wkey);
            if (wpos != std::string::npos) {
                size_t p = wpos + wkey.size();
                int idx = 0;
                bool found = false;
                // Repopulate from scratch: this handler runs on every scene packet, and
                // a stale entry would let NetworkWorldIndexByName resolve a portal
                // target against a world the server has since stopped offering.
                g_serverWorldNames.clear();
                while (p < sceneJson.size()) {
                    while (p < sceneJson.size() && (sceneJson[p] == ' ' || sceneJson[p] == ',')) ++p;
                    if (p >= sceneJson.size() || sceneJson[p] == ']') break;
                    if (sceneJson[p] != '"') { ++p; continue; }
                    size_t s2 = ++p;
                    while (p < sceneJson.size() && sceneJson[p] != '"') ++p;
                    std::string entry = sceneJson.substr(s2, p - s2);
                    g_serverWorldNames.push_back(entry);
                    if (entry == activeWorld) { g_network_world_index = idx; found = true; }
                    ++idx;
                    if (p < sceneJson.size()) ++p;
                }
                if (!found)
                    OZ_WARN("Network: active world '%s' is not in the server's world list — "
                            "keeping world index %d",
                            activeWorld.c_str(), g_network_world_index);
            }
        }

        if (activeWorld.empty() || activeWorld == g_world_to_load) return;
        OZ_INFO("Network: server active world is '%s' — switching", activeWorld.c_str());
        strncpy(g_world_to_load, activeWorld.c_str(), sizeof(g_world_to_load) - 1);
        g_world_to_load[sizeof(g_world_to_load) - 1] = '\0';
        SetSceneFlag = true;
        // No REQUEST_WORLD here: this path is the server TELLING us to switch (the
        // scene handshake's active_world), so the server already has us in that
        // world. Asking again would be a no-op round trip. WORLD_CHANGE — sent
        // separately on join and on any portal request — is the client's
        // authoritative view, and server_world_index() is what gates pickups and
        // melee regardless of what g_world_to_load says.
    });

    if (SetServerJoinFlag && SetServerJoinIP) {
        // Final guard: never hand a malformed host to connect() (defends against
        // a corrupted menu textbox producing e.g. a one-character address).
        std::string host;
        int port = SetServerJoinPort;
        if (JoinUri::ParseHostPort(SetServerJoinIP, host, port, SetServerJoinPort) && !host.empty()) {
            g_network_enabled = g_client.connect(host.c_str(), (uint16_t)port);
            if (g_network_enabled)
                OZ_INFO("Network: connected to %s:%d", host.c_str(), port);
            else
                OZ_WARN("Network: connect to %s:%d failed", host.c_str(), port);
        } else {
            OZ_WARN("Network: refusing invalid join address '%s'", SetServerJoinIP);
        }
        SetServerJoinFlag = false;
    }

    HideCursor(); 
    DisableCursor();  
    
    while (!WindowShouldClose() && !g_returnToMenu && !g_quitRequested)
    {
        // Borderless window drag (Alt+Left), so the window can be moved while
        // paused or in a menu even though it has no OS title bar.
        HandleBorderlessWindowDrag();

        // Console toggle with ^ (grave) key
        if (Keybinds::Pressed(GameAction::Console)) {
            g_consoleOpen = !g_consoleOpen;
            if (g_consoleOpen) {
                ShowCursor();
                EnableCursor();
            } else if (!ShowInventory) {
                HideCursor();
                DisableCursor();
            }
        }
        PlayerController::Instance().HandleKeyToggles(ShowInventory, ShowSkillTree, g_consoleOpen,
                                                  &ShowSpellbook);

        // Console input processing
        if (g_consoleOpen) {
            HandleConsoleInput();
        }

        // Pause menu state
        {
            static bool pauseKeyWasDown = false;
            bool pauseKeyNow = Keybinds::Pressed(GameAction::Pause);
            // While Settings is open, Escape belongs to the settings panel, not
            // to the pause toggle. Otherwise Escape would unpause the world and
            // leave the settings overlay floating over live gameplay.
            if (!ShowSettings && pauseKeyNow && !pauseKeyWasDown) {
                g_gamePaused = !g_gamePaused;
                if (g_gamePaused) { ShowCursor(); EnableCursor(); }
                else if (!ShowSettings && !ShowInventory && !ShowSkillTree &&
                         !ShowSpellbook && !g_consoleOpen) { HideCursor(); DisableCursor(); }
            }
            pauseKeyWasDown = pauseKeyNow;
        }

        // Settings may have been closed by its own close button (which does not
        // go through the native-menu callback). Resume only when the settings
        // panel was what paused a running game.
        if (!ShowSettings && g_settingsPausedGame) {
            g_gamePaused = false;
            g_settingsPausedGame = false;
            if (!ShowInventory && !ShowSkillTree && !ShowSpellbook && !g_consoleOpen) {
                HideCursor();
                DisableCursor();
            }
        }

        // Skip game input/update when paused
        if (g_gamePaused) {
            // Put the frozen world back on screen. Use the RESOLVED (graded) image
            // when the post chain has one - drawing the float scene here would skip
            // the grade and could clip.
            Texture2D frozen = oz::PostFX::Instance().Ready()
                                   ? oz::PostFX::Instance().ResolvedTexture()
                                   : Target.texture;
            DrawTexturePro(frozen,
                           (Rectangle){0, 0, (float)frozen.width, (float)-frozen.height},
                           (Rectangle){0, 0, (float)GetScreenWidth(),
                                       (float)GetScreenHeight()},
                           (Vector2){0, 0}, 0.0f, WHITE);

            if (ShowSettings) {
                UpdateSettings();
                if (IsKeyPressed(KEY_ESCAPE)) ShowSettings = false;
            } else {
                auto metrics = angelsmenu::MenuMetrics::Compute(
                    (float)GetScreenWidth(), (float)GetScreenHeight(),
                    GetWindowScaleDPI().y, UiScale);
                const auto action = angelsmenu::DrawPauseMenu(metrics, GetFrameTime());
                switch (action) {
                    case angelsmenu::PauseAction::Resume:
                        g_gamePaused = false;
                        if (!ShowSettings && !ShowInventory && !ShowSkillTree &&
                            !ShowSpellbook && !g_consoleOpen) { HideCursor(); DisableCursor(); }
                        break;
                    case angelsmenu::PauseAction::Settings:
                        ShowSettings = true;
                        ShowCursor();
                        EnableCursor();
                        break;
                    case angelsmenu::PauseAction::SaveGame:
                        SaveGame();
                        break;
                    case angelsmenu::PauseAction::MainMenu:
                        g_gamePaused = false;
                        g_returnToMenu = true;
                        break;
                    case angelsmenu::PauseAction::Quit:
                        g_gamePaused = false;
                        g_quitRequested = true;
                        break;
                    default: break;
                }
            }

            EndDrawing();
            continue;
        }

        // Game-over overlay when player has died 3 times
        if (PlayerController::Instance().DrawGameOver(AngelTechData.Deaths,
                AngelTechData.BtnClicked, AngelTechData.BtnHover, AngelTechData.BtnNormal,
                g_returnToMenu,
                []() { LoadWorld(); })) {
            EndDrawing();
            continue;
        }

        // Capture left-mouse state before camera (handle after)
        static bool left_click_was_down = false;
        bool left_click_now = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
        bool left_just_pressed = left_click_now && !left_click_was_down;

        g_playerMovement.OldX = AngelTechData.MainCamera.position.x;
        g_playerMovement.OldY = AngelTechData.MainCamera.position.y;
        g_playerMovement.OldZ = AngelTechData.MainCamera.position.z;

        // Save Y so the vertical physics below can own it (the horizontal
        // controller never touches Y).
        float savedCamY = AngelTechData.MainCamera.position.y;

        // Custom first-person controller: mouse look + WASD at a speed derived
        // from the player entity's authored movement_speed, with hold-Shift
        // sprint and hold-Ctrl crouch.
        {
            bool uiBlocked = ShowSettings || ShowInventory || ShowSkillTree || ShowSpellbook || g_consoleOpen;
            float moveSpeedScalar = LightningEntityManager::Instance().GetPlayerMovementSpeed();
            g_playerMovement.UpdateLookAndMove(
                AngelTechData.MainCamera, GetFrameTime(), moveSpeedScalar, uiBlocked);
        }

        // Fixed 60 Hz timestep for movement physics (zone/water/jump/gravity).
        // Discrete 1/60s steps, at most 4 per render frame, with leftover time
        // carried across frames so the sim advances at a constant 60 Hz cadence
        // regardless of render FPS.
        int move_steps = PlayerController::Instance().ConsumeMoveSteps(GetFrameTime());
        const float kMoveDt = PlayerController::MoveDeltaSeconds();

        // --- Zone volume detection + movement effects (uses pre-computed player region) ---
        // --- Zone volume detection + movement effects (uses pre-computed player region) ---
        oz::physics::PhysicsInfo zonePhysics; // defaults; overridden per zone
        for (int s = 0; s < move_steps; ++s) {
        {
            const float dt = kMoveDt;
            Vector3 playerPos = AngelTechData.MainCamera.position;
            static std::string lastZoneName;

            // Get active zones from pre-computed player region (set in UpdateEntities)
            auto& region = ZoneManager::Instance().GetPlayerRegion();
            ZoneVolumeNode* activeZone = (region.primaryZoneId >= 0)
                ? ZoneManager::Instance().GetZone(region.primaryZoneId) : nullptr;
            g_playerMovement.inWater = false;
            g_playerMovement.isClimbing = false;

            if (activeZone) {
                // Per-zone authored physics overrides (falls back to defaults
                // for worlds that do not export gravity= style kwargs).
                zonePhysics = activeZone->physics;

                ZoneType zt = activeZone->zoneType;
                // Skip zones already handled elsewhere
                if (zt != ZoneType::ZONE_SKY && zt != ZoneType::ZONE_GAMEPLAY_SOUND) {
                    // Fire zone enter/exit events using the zone name
                    // (triggers .ozls script hooks). Exit fires BEFORE enter:
                    // the old zone's on_exit restore_* opcodes must not stomp
                    // the fog/ambient the new zone's on_enter is about to
                    // apply (PawnSystem::UpdateSkyZone emits in the same
                    // order).
                    if (activeZone->name != lastZoneName) {
                        if (!lastZoneName.empty()) {
                            oz::events::Event ev2;
                            ev2.topic = "zone.exit";
                            ev2.s0 = lastZoneName;
                            ev2.x = playerPos.x; ev2.y = playerPos.y; ev2.z = playerPos.z;
                            oz::events::EventBus::Instance().Emit(ev2);
                        }
                        oz::events::Event ev;
                        ev.topic = "zone.enter";
                        ev.s0 = activeZone->name;
                        ev.x = playerPos.x; ev.y = playerPos.y; ev.z = playerPos.z;
                        oz::events::EventBus::Instance().Emit(ev);
                        lastZoneName = activeZone->name;
                    }
                }

                switch (zt) {
                    case ZoneType::ZONE_WATER:
                        g_playerMovement.inWater = true;
                        break;
                    case ZoneType::ZONE_LADDER:
                        // Flag only — the actual climb (and gravity suppression)
                        // lives in PlayerController::UpdateVertical /
                        // PlayerPhysics::UpdateLadderVertical, which runs once per
                        // fixed sub-step after this loop.
                        g_playerMovement.isClimbing = true;
                        g_playerMovement.onGround = false;
                        break;
                    case ZoneType::ZONE_REVERB:
                        // Reverb is handled by SoundManager::UpdateReverb, driven
                        // from the per-frame zone pass (Core.hpp), not from this
                        // zone-entry hook. The case is intentionally empty; the
                        // old "placeholder, will be applied" comment was stale --
                        // DspReverb exists and is attached.
                        break;
                    case ZoneType::ZONE_GRAVITY:
                        // Gravity is driven entirely by the zone's PhysicsInfo
                        // (gravity_vector= / gravity=), applied in
                        // PlayerPhysics::UpdateGroundVertical via zonePhysics.
                        // No flag here: the vector is the state. Horizontal
                        // gravity + camera re-orientation are follow-up work.
                        break;
                    default:
                        break;
                }
            } else if (!lastZoneName.empty()) {
                oz::events::Event ev;
                ev.topic = "zone.exit";
                ev.s0 = lastZoneName;
                ev.x = playerPos.x; ev.y = playerPos.y; ev.z = playerPos.z;
                oz::events::EventBus::Instance().Emit(ev);
                lastZoneName.clear();
            }
        }
        }
        // --- Jump / Fly / Noclip Y management ---
        const bool verticalBlocked = g_consoleOpen || ShowInventory || ShowSkillTree || ShowSpellbook;
        for (int s = 0; s < move_steps; ++s) {
            PlayerController::Instance().UpdateVertical(kMoveDt, AngelTechData.MainCamera,
                                                        savedCamY, verticalBlocked, zonePhysics);
        }

        const bool uiBlocking = ShowSettings || ShowInventory || ShowSkillTree || ShowSpellbook || g_consoleOpen;

        // Weapon fire AFTER camera so left-click does not disrupt movement
        WeaponBehaviour::Instance().HandleInput(uiBlocking, AngelTechData.MainCamera);

        // Spell bar (Z/X/C/V/B). Also after the camera, because a cast aims
        // along the view direction. uiBlocking is the same modal flag, so a cast
        // cannot fire underneath the spellbook while it is open.
        LightningEntityManager::Instance().HandleSpellInput(uiBlocking,
                                                           AngelTechData.MainCamera);

        // ADS / Zoom (right-click)
        WeaponBehaviour::Instance().SetAdsActive(!uiBlocking && Keybinds::Down(GameAction::AltFire));

        // Recoil recovery + camera application
        WeaponBehaviour::Instance().Update(AngelTechData.MainCamera);

        left_click_was_down = left_click_now;

        OmegaInputController.UpdateInputs();

        UpdateLightSources();

        if (Direction == 1)
        {
            R += 1;
            G += 1;
            B += 1;

            if (R == 254)
            {
                FadeDone = true;
                Direction = 3;
            }
        }

        FadeColor = (Color){R, G, B, 255};

        // Screenshot mode: drive the requested camera, then hold it perfectly
        // still. isNoClip is what makes that work — it exempts RestorePosition
        // and the ground clamp, and UpdateFlyVertical leaves Y untouched with no
        // keys held, so gravity cannot drift the framing over the settle frames.
        if (g_shot.active) {
            g_playerMovement.isNoClip = true;
            g_playerMovement.LookInit = true;
            if (!g_shot.camApplied && g_shot.shotIndex < (int)g_shot.cams.size()) {
                ApplyShotCamera(AngelTechData.MainCamera, g_shot.cams[g_shot.shotIndex]);
                g_shot.camApplied = true;
                if (g_shot.startTime < 0.0)
                    g_shot.startTime = GetTime();
            }
        }

        DrawWorld();

        BeginDrawing();  

        ClearBackground(BLACK);

        // Push the persisted grade settings into the chain. Kept here so the dev
        // panel and the title Video tab take exactly the same path.
        {
            auto& grade = oz::PostFX::Instance().Grade();
            grade.exposure      = PostFXEnabled ? PostExposure : 1.0f;
            grade.bloomStrength = PostFXEnabled ? PostBloom : 0.0f;
            grade.vignette      = PostFXEnabled ? PostVignette : 0.0f;
        }

        // Resolve the HDR scene through bloom + grade into the LDR result. The
        // Pixel/Jitter post shaders below still expect a display-ready texture, so
        // they draw the RESOLVED image, not the float scene.
        Texture2D resolved = oz::PostFX::Instance().Resolve(
            Target, Target.texture.width, Target.texture.height);
        Texture2D finalTex = (resolved.id > 0) ? resolved : Target.texture;

        // Apply post-processing shaders during final blit
        {
            bool shaderActive = false;
            if (PixelShader && AngelTechData.PixelShader.id > 0) {
                BeginShaderMode(AngelTechData.PixelShader);
                SetShaderValue(AngelTechData.PixelShader, GetShaderLocation(AngelTechData.PixelShader, "pixelWidth"), &PixelSize, SHADER_UNIFORM_FLOAT);
                SetShaderValue(AngelTechData.PixelShader, GetShaderLocation(AngelTechData.PixelShader, "pixelHeight"), &PixelSize, SHADER_UNIFORM_FLOAT);
                shaderActive = true;
            }
            if (JitterEnabled && AngelTechData.JitterShader.id > 0) {
                if (!shaderActive) BeginShaderMode(AngelTechData.JitterShader);
                float t = float(GetTime());
                SetShaderValue(AngelTechData.JitterShader, GetShaderLocation(AngelTechData.JitterShader, "uTime"), &t, SHADER_UNIFORM_FLOAT);
                SetShaderValue(AngelTechData.JitterShader, GetShaderLocation(AngelTechData.JitterShader, "uIntensity"), &JitterIntensity, SHADER_UNIFORM_FLOAT);
                shaderActive = true;
            }
            if (FogEnabled && AngelTechData.FogShader.id > 0) {
                if (!shaderActive) BeginShaderMode(AngelTechData.FogShader);
                float fc[4] = { FogTint.r / 255.0f, FogTint.g / 255.0f, FogTint.b / 255.0f, FogTint.a / 255.0f };
                SetShaderValue(AngelTechData.FogShader, GetShaderLocation(AngelTechData.FogShader, "fogColor"), fc, SHADER_UNIFORM_VEC4);
                SetShaderValue(AngelTechData.FogShader, GetShaderLocation(AngelTechData.FogShader, "fogIntensity"), &FogIntensity, SHADER_UNIFORM_FLOAT);
                shaderActive = true;
            }
            DrawTexturePro(finalTex, (Rectangle){ 0, 0, (float)finalTex.width, (float)-finalTex.height }, (Rectangle){ 0, 0, float(GetScreenWidth()), float(GetScreenHeight())}, (Vector2){ 0, 0 } , 0.f , WHITE);
            if (shaderActive) EndShaderMode();
        }
        // LightningScript dynamic hotbar. Hidden while the inventory overlay is
        // open: the overlay draws its own interactive mini bar, and the two bars
        // share the hotbar drag state - the in-world bar ran first each frame and
        // processed the release with no hovered cell, cancelling the overlay's
        // reorder before it could complete.
        if ((!g_shot.active || !g_shot.hideHud) && !ShowInventory)
            LightningEntityManager::Instance().DrawHotbar();
        // Hotbar input. Suppressed while a modal is up or during --shot captures:
        // these keys/wheel previously fired underneath the inventory, skill tree
        // and console.
        LightningEntityManager::Instance().HandleInput(
            uiBlocking || (g_shot.active && g_shot.hideHud));

        // Reload clip. Driven by the manager's own flag rather than a raw KEY_R
        // poll, so it only plays when a reload actually started: the key poll
        // ran the clip even when the reload was rejected (full magazine, no
        // magazine stat, non-weapon selected). Auto-reload on firing dry is
        // picked up here too.
        if (LightningEntityManager::Instance().ConsumeReloadStarted())
            oz::ViewModel::Instance().TriggerReload();

        if (FPSEnabled && !(g_shot.active && g_shot.hideHud)){
            DrawFPS(0,0);
        }

        AngelTechTextSystem.Update();

        // Network client update
        if (g_network_enabled) {
            Camera3D& cam = AngelTechData.MainCamera;
            Vector3 fwd = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
            float yaw = atan2f(fwd.x, fwd.z);
            float pitch = asinf(fwd.y);
            uint8_t stance = g_playerMovement.isCrouching ? net::STANCE_CROUCH
                           : (g_playerMovement.isSprinting ? net::STANCE_SPRINT
                                                           : net::STANCE_STAND);
            g_client.update(cam.position.x, cam.position.y, cam.position.z,
                            yaw, pitch, stance);

            if (g_client.is_connected()) {
                LightningEntityManager::Instance().SetPlayerLevel(g_client.get_level());
                LightningEntityManager::Instance().SetPlayerXP(g_client.get_xp());
                LightningEntityManager::Instance().SetPlayerXPToNext(g_client.get_xp_to_next());

                // Map each server NPC (world+partition+index) to the local pawn
                // spawned for it. The previous code assumed pawn id == npc
                // index+1, which breaks as soon as LoadWorld() calls DespawnAll()
                // (map switch): Get() then misses, so it spawned a brand-new pawn
                // AND script entity every single frame -> unbounded growth and
                // ~0 FPS. Entries are dropped when their pawn disappears.
                static std::unordered_map<uint64_t, int> s_npcPawnId;
                const auto& npcs = g_client.npcs();
                for (size_t i = 0; i < npcs.size(); i++) {
                    const uint64_t key =
                        ((uint64_t)(uint32_t)npcs[i].world_index     << 42) |
                        ((uint64_t)(uint32_t)npcs[i].npc_index       << 21) |
                         (uint64_t)(uint32_t)npcs[i].partition_index;

                    Pawn* p = nullptr;
                    auto it = s_npcPawnId.find(key);
                    if (it != s_npcPawnId.end()) {
                        p = PawnSystem::Instance().Get(it->second);
                        if (!p) s_npcPawnId.erase(it); // despawned/reloaded -> respawn once
                    }

                    if (p) {
                        const Vector3 srvPos = { npcs[i].position.x, npcs[i].position.y, npcs[i].position.z };
                        // NpcStateUpdateData has always carried `yaw` and `state`;
                        // this block used to copy only `position`. Because Update()
                        // returns early for server-owned pawns, `state` stayed IDLE
                        // and `velocity` stayed zero forever - so every multiplayer
                        // NPC slid around playing the idle clip and faced whatever
                        // yaw it spawned with. PawnState and NpcState are the same
                        // order (IDLE/PATROL/CHASE/RETURN/DEAD).
                        const int srvState = npcs[i].state;
                        const PawnState wireState =
                            (srvState >= 0 && srvState <= (int)PawnState::DEAD)
                                ? (PawnState)srvState : PawnState::IDLE;

                        // Velocity is not on the wire: derive it from the snapshot
                        // delta so the static-model walk bob has real motion.
                        // Packets arrive at ~10 Hz, so keep the last direction
                        // between them and only zero it once the server says IDLE.
                        if (p->netHasPrev) {
                            const Vector3 d = Vector3Subtract(srvPos, p->netPrevPosition);
                            const float dtf = GetFrameTime();
                            if (Vector3LengthSqr(d) > 1e-6f && dtf > 0.0f)
                                p->velocity = Vector3Scale(d, 1.0f / dtf);
                            else if (wireState == PawnState::IDLE)
                                p->velocity = {0, 0, 0};
                        }
                        p->netPrevPosition = srvPos;
                        p->netHasPrev = true;
                        p->position = srvPos;
                        // The server stores NpcStateUpdateData::yaw in DEGREES
                        // (GameState::tick_npcs does atan2 * RAD2DEG) and Pawn::yaw
                        // is degrees too, so this is a straight copy - no RAD2DEG.
                        p->yaw = npcs[i].yaw;

                        if (p->state != PawnState::DEAD && p->active && !npcs[i].active) {
                            // Died on the server. Spawn the gore once (the local
                            // ApplyPawnDamage path is skipped for networkControlled
                            // pawns) and become a corpse: Update() holds the death
                            // clip and despawns it after kPawnCorpseSeconds.
                            if (g_goreEnabled)
                                CombatFX::Instance().SpawnGibs(
                                    {p->position.x, p->position.y + 0.6f, p->position.z});
                            p->velocity = {0, 0, 0};
                            p->deadTimer = kPawnCorpseSeconds;
                            p->animClip = -1;   // restart the clip at the death pose
                            p->state = PawnState::DEAD;
                        } else if (npcs[i].active) {
                            // Revived / still alive: trust the server's state.
                            p->active = true;
                            if (wireState == PawnState::DEAD) {
                                // A server that reports DEAD before flipping
                                // `active` off must still get a corpse window,
                                // or the pawn is despawned on the same frame.
                                if (p->state != PawnState::DEAD) {
                                    p->deadTimer = kPawnCorpseSeconds;
                                    p->animClip = -1;
                                }
                                p->state = PawnState::DEAD;
                            } else {
                                p->state = wireState;
                            }
                        }
                    } else if (npcs[i].active) {
                        // Spawn with the server-reported def type (falls back to Walker)
                        const char* type = npcs[i].npc_type[0] ? npcs[i].npc_type : "Walker";
                        int pawnId = PawnSystem::Instance().Spawn({ npcs[i].position.x, npcs[i].position.y, npcs[i].position.z }, type);
                        if (pawnId < 0)
                            pawnId = PawnSystem::Instance().Spawn({ npcs[i].position.x, npcs[i].position.y, npcs[i].position.z }, "Walker");
                        if (pawnId >= 0) {
                            s_npcPawnId[key] = pawnId;
                            Pawn* np = PawnSystem::Instance().Get(pawnId);
                            if (np) {
                                np->networkControlled = true;
                                // Server identity, so a local melee hit can be
                                // reported to the server (which owns the damage).
                                np->netWorldIndex     = npcs[i].world_index;
                                np->netNpcIndex       = npcs[i].npc_index;
                                np->netPartitionIndex = npcs[i].partition_index;
                                // Server owns AI/position; don't double-simulate locally.
                                if (strcmp(np->defName.c_str(), type) != 0) np->defName = type;
                            }
                        }
                    }
                }

                // Auto-collect when walking over a pickup (also E) — handled
                // by PickupPawns (networked pickups only; throttled internally).
                PickupPawns::Instance().Update(Keybinds::Pressed(GameAction::Interact),
                                               AngelTechData.MainCamera.position,
                                               g_network_world_index,
                                               GetTime());
            }
        }

        // HUD: player stats (always visible)
        if (!(g_shot.active && g_shot.hideHud))
            InventoryBehaviour::Instance().DrawHud(AngelTechData.MainCamera, AngelTechData.Ticker,
                                                   g_playerMovement.isCrouching, g_playerMovement.isSprinting);

        // Screen flash on pickup collect (fades out). Driven by
        // PawnSystem::m_pickupFeedback. In SP the walk-over path sets it; in MP
        // the grant path InventoryBehaviour::OnItemCollected sets it when the
        // server's PICKUP_COLLECTED arrives. (This comment used to claim the
        // networked reply set it directly — it does not, and the flash is why
        // that indirection matters.)
        {
            auto& fb = PawnSystem::Instance().m_pickupFeedback;
            if (fb.flashTimer > 0.0f) {
                fb.flashTimer -= GetFrameTime();
                if (fb.flashTimer < 0.0f) fb.flashTimer = 0.0f;
                Color flashColor = {255, 255, 255, 0};
                const ItemDBEntry* def = GetItemDef(fb.itemId);
                if (def) flashColor = ItemFlashColor(*def);
                // 0.5s fade: alpha peaks at 80/160 depending on category.
                const unsigned char peak = (def && def->category != ItemCategory::WEAPON &&
                                            def->category != ItemCategory::ARMOR) ? 160 : 80;
                flashColor.a = (unsigned char)(peak * fb.flashTimer * 2.0f);
                if (flashColor.a > 0)
                    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), flashColor);
            }
        }

        // Damage vignette (red edge flash on taking damage). Driven by
        // AngelTechData.DamageFlash, set by both contact damage and the scripted
        // `damage` opcode. Drawn under the HUD but over the world.
        if (AngelTechData.DamageFlash > 0.0f && !(g_shot.active && g_shot.hideHud)) {
            AngelTechData.DamageFlash -= GetFrameTime();
            if (AngelTechData.DamageFlash < 0.0f) AngelTechData.DamageFlash = 0.0f;
            // ease-out: sharp on impact, gentle fade.
            const float t = AngelTechData.DamageFlash / kDamageFlashDuration;
            const float ease = t * t;
            const int sw = GetScreenWidth(), sh = GetScreenHeight();
            const int band = std::max(40, sh / 8);
            // Four edge bands rather than a full-screen wash, so the centre of
            // the view stays readable while still reading as "you were hit".
            const unsigned char a = (unsigned char)(150.0f * ease);
            if (a > 0) {
                BeginBlendMode(BLEND_ADDITIVE);
                DrawRectangle(0, 0, sw, band, {90, 0, 0, a});
                DrawRectangle(0, sh - band, sw, band, {90, 0, 0, a});
                DrawRectangle(0, band, band, sh - 2 * band, {90, 0, 0, a});
                DrawRectangle(sw - band, band, band, sh - 2 * band, {90, 0, 0, a});
                EndBlendMode();
            }
        }

        // Combat feedback (2D): hit marker, damage bearing ring, low-health
        // pulse. Timers advance even when the HUD is hidden so a later capture
        // cannot show a stale cue; only the drawing is gated.
        {
            auto& cf = oz::CombatFeedback::Instance();
            cf.Update(GetFrameTime());
            if (!(g_shot.active && g_shot.hideHud)) {
                auto& lem = LightningEntityManager::Instance();
                const float maxHp = lem.GetPlayerMaxHealth();
                const float hp = lem.GetPlayerHealth();
                cf.Draw(maxHp > 0.0f ? (hp / maxHp) : 1.0f);
                // Floating damage numbers sit in the world, so they need the
                // camera; drawn after the screen-space cues so they overlay the
                // hit marker rather than being hidden by it.
                cf.DrawDamageNumbers(AngelTechData.MainCamera);
            }
        }

        // Network ping (only when connected)
        if (g_network_enabled && g_client.is_connected()) {
            DrawText(TextFormat("Ping: %dms", g_client.get_ping_ms()),
                     10, GetScreenHeight() - 20, 12, GREEN);
        }

        // Inventory overlay
        if (ShowInventory) {
            InventoryBehaviour::Instance().DrawOverlay(AngelTechData.MainCamera);
        }

        // Ethereal skill tree overlay
        if (ShowSkillTree) {
            oz::skilltree::DrawOverlay(ShowSkillTree);
        }

        // Spellbook overlay + the persistent spell bar. The bar is suppressed in
        // screenshot mode along with the rest of the HUD.
        if (!(g_shot.active && g_shot.hideHud))
            oz::spellbook::DrawSpellBar(GetScreenWidth(), GetScreenHeight());
        if (ShowSpellbook)
            oz::spellbook::DrawSpellbook(ShowSpellbook);

        // Developer settings overlay. Drawn with the other overlays, i.e. AFTER the
        // HUD, so its window sits on top of the HP/MP columns (which open exactly
        // under it) instead of the HUD painting over the panel.
        if (!(g_shot.active && g_shot.hideHud))
            UpdateSettings();

        // Console overlay (always on top)
        DrawConsole();

        // Crosshair. Style/size/gap/colour are settings; ADS and the fire bloom
        // come from WeaponBehaviour and are passed in, so the renderer stays
        // decoupled from the weapon system. (The inline version this replaced
        // also drew the DOWN tick at cx+gap instead of cx - an off-centre cross.)
        if (!ShowInventory && !ShowSkillTree && !ShowSettings && !g_consoleOpen &&
            !(g_shot.active && g_shot.hideHud)) {
            oz::CrosshairFrame frame;
            frame.ads   = WeaponBehaviour::Instance().AdsActive();
            frame.bloom = WeaponBehaviour::Instance().CrosshairBloom();
            oz::DrawCrosshair(frame);
        }

        EndDrawing();

        // Screenshot capture. Runs after EndDrawing so LoadImageFromScreen sees
        // the finished frame, and it consumes the loop when every camera is done.
        if (g_shot.active && TickScreenshot()) {
            OZ_INFO("Screenshot run complete: %d capture(s)", (int)g_shot.cams.size());
            // End the loops WITHOUT closing the window. CloseWindow() is not
            // idempotent-safe: the old code closed here and then again after
            // this loop, so rlglClose() ran twice and its second
            // rlUnloadRenderBatch() double-freed the default render batch -
            // STATUS_HEAP_CORRUPTION on every single --shot run, after the
            // capture had already succeeded. One CloseWindow(), at the end.
            shotRunComplete = true;
            break;
        }

        if (IsKeyPressed(KEY_F11))ToggleFullscreen();


    } // end inner game loop

    // Cleanup before returning to menu
    g_client.disconnect();
    SoundManager::Instance().ResetWorldAudio();

    } // end outer menu/game loop
    
    // Cleanup after the game loop (and after a shot run): the GL context is
    // still alive here, because the only CloseWindow() is the one below. The
    // old ordering closed the window in the shot tick, which forced this whole
    // teardown block to be skipped for shot runs (touching a dead context would
    // fault) and left every GPU object to leak at process exit.
    oz::PostFX::DestroyTarget(Target);
    oz::PostFX::Instance().Shutdown();
    EngineBillboard::Shutdown();
    oz::SurfaceMaterial::Instance().Shutdown();
    oz::Skybox::Instance().Shutdown();
    oz::SkyMaterial::Instance().Shutdown();
    // Portal view render texture (see Renderer/SkyPortal.hpp).
    oz::SkyPortal::Instance().Shutdown();
    // Instanced particle VAO/VBOs + shader. Released with the other GPU
    // resources and BEFORE CloseWindow(), while the GL context is still alive -
    // the manager is an intentionally-leaked singleton, so without this its GL
    // objects would outlive the context.
    OzParticleSimulationManager::Instance().Shutdown();
    // Baked material textures are owned by the registry; release them while the
    // GL context is still alive.
    ozmat::MaterialRegistry::Instance().Shutdown();
    CloseWindow();
}