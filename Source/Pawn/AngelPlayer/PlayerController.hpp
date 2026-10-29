#pragma once
#include "raylib.h"
#include "../../Physics/PhysicsInfo.hpp"
#include <functional>
#include <string>

// PlayerController — AngelPlayer input/mode state that used to live inline in
// Main.cpp: overlay key toggles (K/Tab), the fixed-step accumulator, the
// Jump/Fly/Noclip/Water vertical integrator, and the game-over overlay.
//
// It owns no engine globals; UI visibility flags are passed by reference and
// the world-reload + return-to-menu actions are injected by the host.
class PlayerController {
public:
    static PlayerController& Instance() {
        static PlayerController instance;
        return instance;
    }

    // K toggles the skill tree, Tab toggles the inventory, Q toggles the
    // spellbook (mutually exclusive, all ignored while the console is open).
    // Handles the cursor state.
    void HandleKeyToggles(bool& showInventory, bool& showSkillTree, bool consoleOpen,
                          bool* showSpellbook = nullptr);

    // Fixed 60 Hz timestep: advance the accumulator and return how many sim
    // steps to run this frame (at most `maxSteps`).
    int ConsumeMoveSteps(float frameTime, int maxSteps = 4);
    static constexpr float MoveDeltaSeconds() { return 1.0f / 60.0f; }

    // Jump / Fly / Noclip / Water integration for one fixed step, ALONG the
    // player's gravity axis (`g_playerMovement.up`, advanced here). Gravity/water/
    // ladder constants come from the active zone's PhysicsInfo.
    void UpdateVertical(float dt, Camera3D& cam, Vector3 savedCamPos, bool uiBlocked,
                        const oz::physics::PhysicsInfo& phys);

    // Play the jump one-shot (called by UpdateVertical when a jump is detected).
    void PlayJump();

    // --- Data-driven sound stats (authored on Player.ozls) ----------------
    //
    // Player.ozls (`entity "Player" : upgrade`, auto-spawned by
    // LightningEntityManager::Init) is the player's .ozls def. Its stats block
    // may carry jump_sound / land_sound / walk_sound / run_sound / hurt_sound /
    // death_sound plus a matching _volume. Every accessor below falls back to
    // the preloaded SoundManager global when the key is absent, so an
    // unauthored level sounds exactly as it did before this existed.

    // Read a string stat off the Player entity def (runtimeStats first, then
    // the def's stats.strings). Returns `def` when the player entity does not
    // exist yet or the key is absent.
    std::string PlayerStatString(const std::string& key,
                                 const std::string& def = "") const;
    float PlayerStatFloat(const std::string& key, float def) const;

    // One-shots. Each prefers the authored stat and falls back to the current
    // global handle.
    void PlayLand();
    // `fatal` picks death_sound over hurt_sound. Splitting these matters:
    // PLAYER_HURT fires on every point of damage, and it used to play the death
    // sound for all of them.
    void PlayHurt(bool fatal);

    // Footsteps, addressed by the walk/run distinction rather than one global
    // loop. Called every frame the player is moving; `sprinting` selects
    // run_sound over walk_sound. Both are loops, so an already-playing key is
    // not restarted.
    void PlayWalkLoop(bool sprinting);
    void StopWalkLoop();

    // Draws the "3 deaths" game-over overlay. Returns true when the overlay is
    // shown (caller should EndDrawing + continue). `restartWorld` re-loads the
    // world on Restart.
    bool DrawGameOver(int& deaths,
                      Texture2D btnClicked, Texture2D btnHover, Texture2D btnNormal,
                      bool& returnToMenu,
                      const std::function<void()>& restartWorld);

private:
    PlayerController() = default;
    double m_moveAccumulator = 0.0;
};
