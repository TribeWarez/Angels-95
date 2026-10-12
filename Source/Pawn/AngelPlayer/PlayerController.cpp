#include "PlayerController.hpp"
#include "../../Physics/PlayerPhysics.hpp"
#include "../PlayerMovement.hpp"
#include "../../Audio/SoundManager.hpp"
#include "../../Script/LightningEntityManager.hpp"
#include <cmath>

// Reads the movement-state global defined once in the client TU (Main).
extern PlayerMovement g_playerMovement;

namespace pphys = oz::physics;

// ---------------------------------------------------------------------------
// Overlay key toggles (K = skill tree, Tab = inventory, Q = spellbook)
// ---------------------------------------------------------------------------
void PlayerController::HandleKeyToggles(bool& showInventory, bool& showSkillTree, bool consoleOpen,
                                        bool* showSpellbook) {
    // K key toggles the ethereal (angelic) skill tree
    if (IsKeyPressed(KEY_K)) {
        if (!consoleOpen) {
            showSkillTree = !showSkillTree;
            if (showSkillTree) {
                showInventory = false;
                if (showSpellbook) *showSpellbook = false;
                ShowCursor();
                EnableCursor();
            } else {
                HideCursor();
                DisableCursor();
            }
        }
    }

    // Q key toggles the spellbook (spell bar bindings)
    if (showSpellbook && IsKeyPressed(KEY_Q)) {
        if (!consoleOpen) {
            *showSpellbook = !*showSpellbook;
            if (*showSpellbook) {
                showInventory = false;
                showSkillTree = false;
                ShowCursor();
                EnableCursor();
            } else {
                HideCursor();
                DisableCursor();
            }
        }
    }

    // Tab key toggles inventory
    if (IsKeyPressed(KEY_TAB)) {
        if (!consoleOpen) {
            showInventory = !showInventory;
            if (showInventory) {
                showSkillTree = false;
                if (showSpellbook) *showSpellbook = false;
                ShowCursor();
                EnableCursor();
            } else {
                HideCursor();
                DisableCursor();
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Fixed 60 Hz timestep accumulator
// ---------------------------------------------------------------------------
int PlayerController::ConsumeMoveSteps(float frameTime, int maxSteps) {
    static constexpr double kMoveTickS = 1.0 / 60.0;
    m_moveAccumulator += frameTime;
    int steps = 0;
    while (m_moveAccumulator >= kMoveTickS && steps < maxSteps) {
        m_moveAccumulator -= kMoveTickS;
        ++steps;
    }
    if (steps >= maxSteps && m_moveAccumulator >= kMoveTickS)
        m_moveAccumulator = 0.0; // drift guard
    return steps;
}

// ---------------------------------------------------------------------------
// Jump / Fly / Noclip / Water vertical integration (one fixed step)
// ---------------------------------------------------------------------------
void PlayerController::UpdateVertical(float dt, Camera3D& cam, float savedCamY, bool uiBlocked,
                                      const oz::physics::PhysicsInfo& phys) {
    pphys::PlayerPhysics physics;
    pphys::PlayerPhysics::Motion motion;
    motion.onGround = g_playerMovement.onGround;
    motion.velocityY = g_playerMovement.velocityY;

    const bool wasOnGround = motion.onGround;

    if (g_playerMovement.isNoClip || g_playerMovement.isFlying) {
        // Noclip / flying: direct vertical control (Space up / Ctrl down)
        physics.UpdateFlyVertical(cam, g_playerMovement.BaseSpeed, phys, dt, true, true);
    } else if (g_playerMovement.isClimbing) {
        // Ladder: W/S climb at ladderSpeed, no gravity. Checked before water so
        // a ladder volume inside a pool still behaves as a ladder.
        physics.UpdateLadderVertical(cam, motion, savedCamY, uiBlocked, phys, dt);
    } else if (g_playerMovement.inWater) {
        // Water: restore Y, reduced gravity, dampen fall
        physics.UpdateWaterVertical(cam, motion, savedCamY, g_playerMovement.EyeHeight,
                                    uiBlocked, phys, dt);
    } else {
        // Normal / grounded: restore Y, jump impulse, gravity
        physics.UpdateGroundVertical(cam, motion, savedCamY, uiBlocked, phys, dt);
    }

    if (wasOnGround && !motion.onGround && motion.velocityY > 0.0f)
        PlayJump();
    // Mirror of the jump edge: the transition back to grounded. Only from
    // actual airtime (wasOnGround was false), so a jump's own first frame does
    // not immediately fire a landing on the same step.
    if (!wasOnGround && motion.onGround)
        PlayLand();

    g_playerMovement.onGround = motion.onGround;
    g_playerMovement.velocityY = motion.velocityY;
}

// ---------------------------------------------------------------------------
// Data-driven sound stats (Player.ozls)
// ---------------------------------------------------------------------------
namespace {

// Footstep loop slot name. Distinct from any weapon loop so the two cannot
// stop one another.
const char* kFootstepLoop = "player_footsteps";

}  // namespace

std::string PlayerController::PlayerStatString(const std::string& key,
                                              const std::string& def) const {
    auto& lem = LightningEntityManager::Instance();
    if (!lem.HasPlayerEntity()) return def;
    EntityInstance* inst = lem.Get(lem.PlayerEntityIndex());
    if (!inst || !inst->def) return def;

    auto sit = inst->def->stats.strings.find(key);
    if (sit == inst->def->stats.strings.end()) return def;

    // The stats parser stores values verbatim (no quote handling, and an
    // embedded space truncates), so trim defensively -- an all-quotes value
    // means "absent", which lets the fallback apply.
    std::string v = sit->second;
    size_t b = v.find_first_not_of(" \t\r\n\"");
    if (b == std::string::npos) return def;
    size_t e = v.find_last_not_of(" \t\r\n\"");
    v = v.substr(b, e - b + 1);
    return v.empty() ? def : v;
}

float PlayerController::PlayerStatFloat(const std::string& key, float def) const {
    auto& lem = LightningEntityManager::Instance();
    if (!lem.HasPlayerEntity()) return def;
    EntityInstance* inst = lem.Get(lem.PlayerEntityIndex());
    if (!inst || !inst->def) return def;
    auto rit = inst->runtimeStats.find(key);
    if (rit != inst->runtimeStats.end()) return rit->second;
    auto fit = inst->def->stats.floats.find(key);
    return (fit != inst->def->stats.floats.end()) ? fit->second : def;
}

void PlayerController::PlayJump() {
    const std::string authored = PlayerStatString("jump_sound");
    if (authored.empty()) {
        SoundManager::Instance().PlayJump();   // preloaded global fallback
        return;
    }
    SoundManager::Instance().PlayStatSound(authored,
                                           PlayerStatFloat("jump_volume", 1.0f), 1.0f);
}

void PlayerController::PlayLand() {
    const std::string authored = PlayerStatString("land_sound");
    // No global fallback: land.wav shipped with the commit but is not part of
    // LoadCoreSounds, and a silent landing is the correct reading of an
    // unauthored land_sound.
    if (authored.empty()) return;
    SoundManager::Instance().PlayStatSound(authored,
                                           PlayerStatFloat("land_volume", 1.0f), 1.0f);
}

void PlayerController::PlayHurt(bool fatal) {
    auto& sm = SoundManager::Instance();
    const std::string authored = fatal ? PlayerStatString("death_sound")
                                       : PlayerStatString("hurt_sound");
    if (!authored.empty()) {
        const float vol = fatal ? PlayerStatFloat("death_volume", 1.0f)
                                : PlayerStatFloat("hurt_volume", 1.0f);
        sm.PlayStatSound(authored, vol, 1.0f);
        return;
    }

    // No authored key. There is no dedicated global hurt handle, so a fatal
    // hit keeps the preloaded death sound -- that is what it played before.
    //
    // A NON-fatal hit must NOT fall back to it: PlayDeath() is the death cue,
    // and reusing it meant every point of damage in a firefight sounded like
    // the player died. Non-fatal falls back to the collision thump instead,
    // which is an existing handle rather than a new global.
    if (fatal) sm.PlayDeath();
    else      sm.PlayCollision();
}

void PlayerController::PlayWalkLoop(bool sprinting) {
    const char* pathKey = sprinting ? "run_sound" : "walk_sound";
    const char* volKey  = sprinting ? "run_volume" : "walk_volume";
    const std::string authored = PlayerStatString(pathKey);

    if (authored.empty()) {
        // Nothing authored for this gait: fall back to the global loop, but
        // only while walking. Sprinting on the shared WalkingSound would just
        // be the same clip at the same speed, so leave the existing loop alone
        // rather than restarting it every frame.
        if (!sprinting) SoundManager::Instance().StartWalkLoop();
        return;
    }

    SoundManager::Instance().StartLoopSound(kFootstepLoop, authored,
                                            PlayerStatFloat(volKey, 1.0f), 1.0f);
}

void PlayerController::StopWalkLoop() {
    // Stop both the authored loop and the global one: if a def authored
    // walk_sound we never started the global loop, and if it did not we never
    // started the keyed one. Stopping an absent key is a no-op.
    SoundManager::Instance().StopLoopSound(kFootstepLoop);
    SoundManager::Instance().StopWalkLoop();
}

// ---------------------------------------------------------------------------
// Game-over overlay (after 3 deaths)
// ---------------------------------------------------------------------------
bool PlayerController::DrawGameOver(int& deaths,
                                    Texture2D btnClicked, Texture2D btnHover, Texture2D btnNormal,
                                    bool& returnToMenu,
                                    const std::function<void()>& restartWorld) {
    if (deaths < 3) return false;

    int sw = GetScreenWidth(), sh = GetScreenHeight();
    DrawRectangle(0, 0, sw, sh, (Color){0, 0, 0, 200});

    const char* gameOverText = "GAME OVER";
    int fontSize = 48;
    int textW = MeasureText(gameOverText, fontSize);
    DrawText(gameOverText, (sw - textW) / 2, sh / 2 - 100, fontSize, RED);

    DrawText("You have perished three times...",
             (sw - MeasureText("You have perished three times...", 16)) / 2,
             sh / 2 - 40, 16, LIGHTGRAY);

    const char* labels[] = {"Restart", "Main Menu"};
    int btnCount = 2;
    int btnW = 220, btnH = 50, gap = 10;
    int totalH = btnCount * btnH + (btnCount - 1) * gap;
    int startY = sh / 2 + 10;

    for (int i = 0; i < btnCount; i++) {
        int bx = (sw - btnW) / 2;
        int by = startY + i * (btnH + gap);
        Rectangle r = {(float)bx, (float)by, (float)btnW, (float)btnH};
        bool hover = CheckCollisionPointRec(GetMousePosition(), r);
        bool clicked = hover && IsMouseButtonPressed(MOUSE_LEFT_BUTTON);

        Texture2D tex = clicked ? btnClicked : hover ? btnHover : btnNormal;
        if (tex.id > 0) {
            DrawTexturePro(tex,
                (Rectangle){0, 0, (float)tex.width, (float)tex.height},
                r, (Vector2){0, 0}, 0, WHITE);
        } else {
            DrawRectangleRec(r, (Color){80, 20, 20, 220});
            DrawRectangleLinesEx(r, 2, (Color){180, 60, 60, 255});
        }
        DrawText(labels[i], bx + (btnW - MeasureText(labels[i], 18)) / 2,
                 by + (btnH - 18) / 2, 18, WHITE);

        if (clicked) {
            if (i == 0) {
                deaths = 1;
                ShowCursor();
                EnableCursor();
                if (restartWorld) restartWorld();
                HideCursor();
                DisableCursor();
            } else if (i == 1) {
                // Main Menu. The gameplay loop calls HideCursor/DisableCursor
                // every frame and PlayHomeScreen otherwise inherits that state,
                // so the cursor stays invisible and clicks do nothing. Show it
                // here, then let g_returnToMenu fall out of the game loop and
                // enter the home screen with the cursor already visible.
                returnToMenu = true;
                ShowCursor();
                EnableCursor();
            }
        }
    }
    return true;
}
