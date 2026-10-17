#pragma once
#include "../Log.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <functional>
#include <sstream>

// LightningScriptContext — per-instance script execution engine
// Refactored from Source/Parasite/ParasiteScript.hpp
// Original ParasiteScript by ODeLyZiD / OmegaTech

// Per-instance logger helper
#define LS_LOG(fmt, ...)  OZ_DEBUG("[LS:%s] " fmt, m_debugTag.c_str(), ##__VA_ARGS__)
#define LS_WARN(fmt, ...) OZ_WARN("[LS:%s] " fmt, m_debugTag.c_str(), ##__VA_ARGS__)

class LightningScriptContext {
public:
    static constexpr int MAX_FLAGS = 64;

    LightningScriptContext() = default;

    // Set a debug tag (typically "entityName:instanceIdx" or "zone:name")
    void SetDebugTag(const std::string& tag) { m_debugTag = tag; }
    const std::string& GetDebugTag() const { return m_debugTag; }

    // Optional external stat provider (host binds player runtimeStats here).
    // Used to resolve $name tokens that aren't local variables.
    void SetStatResolver(const std::function<float(const std::string&)>& resolver) {
        m_statResolver = resolver;
    }

    // Load script lines from a text buffer
    bool Load(const std::string& scriptText);

    // Execute one instruction, advance program counter
    bool ExecuteNext();

    // Register an opcode handler (game-specific opcodes plug in here).
    // The handler receives the current line parsed as an istringstream.
    // Return true if the opcode was consumed (pc was advanced), false to fall
    // through to the unknown-opcode warning.
    using OpcodeHandler = std::function<bool(std::istringstream&)>;
    void RegisterOpcode(const std::string& name, OpcodeHandler handler);

    // Rewind program counter to start
    void Reset();

    // Variable I/O
    void SetInt(const std::string& name, int val);
    int  GetInt(const std::string& name, int defaultVal = 0) const;
    void SetFloat(const std::string& name, float val);
    float GetFloat(const std::string& name, float defaultVal = 0.0f) const;
    void SetStr(const std::string& name, const std::string& val);
    std::string GetStr(const std::string& name, const std::string& defaultVal = "") const;

    // Toggle flags
    void SetFlag(int idx, int val);
    int  GetFlag(int idx) const;

    bool HasMore() const { return m_pc < (int)m_lines.size(); }
    int  LineCount() const { return (int)m_lines.size(); }
    int  ProgramCounter() const { return m_pc; }

    // Park the program counter at end-of-script so a freshly loaded instance
    // executes nothing until its first RunAction/label jump (Spawn calls this;
    // without it the first action body would auto-run via the Update leftover
    // executor).
    void MarkCompleted() { m_pc = (int)m_lines.size(); }

    // Jump label resolution. Labels are collected by Load(); see FindJumpLabel.
    int  FindJumpLabel(const std::string& label) const;

    // Next jump label at a line strictly greater than afterLine
    int FindNextJumpLabel(int afterLine) const;

    // Run an action block: reset, jump past its label, execute up to maxSteps
    // instructions and stop at the next action label (action bodies are linear
    // labels — without the stop, a run would fall through into the next action)
    int RunAction(const std::string& actionName, int maxSteps);

    struct PawnSpawnRequest {
        std::string name;
        float x = 0, y = 0, z = 0;
        bool valid = false;
    };

    // A deferred write to a host-owned stat (e.g. player runtimeStats).
    // op: 0='=', 1='+=', 2='-=', 3='*=', 4='/='
    struct PlayerStatOp {
        std::string name;
        int op = 1;
        float value = 0.0f;
    };

    // Pending pickup spawn (spawn_pickup opcode).
    struct PickupSpawnRequest {
        std::string name;
        float x = 0, y = 0, z = 0;
        float respawnTime = 30.0f;
        bool valid = false;
    };

    // Generic deferred gameplay command, emitted by the Phase-1 opcodes
    // (emit_event / objective / set_worldflag / give_xp / teleport /
    // change_world / grant_item / apply_area_damage / spawn_projectile /
    // play_clip). ONE queue so the host has one drain point and a new opcode
    // never needs a new accessor. Field usage is per-op and documented in
    // ExecuteNext.
    struct ScriptCommand {
        std::string op;              // "event","objective","worldflag_set","xp",
                                     // "teleport","change_world","grant_item",
                                     // "area_damage","projectile","clip"
        std::string s0;              // name / id / topic / clip / world
        float x = 0, y = 0, z = 0;   // generic floats
        float f0 = 0, f1 = 0, f2 = 0;
        int   i0 = 0, i1 = 0;
    };
    std::vector<ScriptCommand> PopCommands();

    // World-scope flag read provider (host binds ObjectiveSystem). `get_worldflag`
    // reads through this; `set_worldflag` is queued as a ScriptCommand so the
    // write reaches the same store.
    void SetWorldFlagGetter(const std::function<int(const std::string&)>& getter) {
        m_worldFlagGetter = getter;
    }

    // Pop pending side-effects (called by host after script execution)
    std::string PopPendingSound();              // returns __last_sound and clears it
    bool PopPendingFog(float& r, float& g, float& b, float& density);
    std::string PopPendingSkybox();             // returns __skybox name or empty
    bool PopPendingAmbient(float& r, float& g, float& b);

    // restore_* opcodes: these used to clear the pending value the host reads
    // (or blank a name the host rejects), so all three were silent no-ops.
    bool PopPendingFogRestore();
    bool PopPendingAmbientRestore();
    bool PopPendingSkyboxRestore();
    PawnSpawnRequest PopPendingPawnSpawn();     // returns __pawn_name and position

    // New side-effects (msg / player stats / consume / pickup spawn)
    std::string PopPendingMessage();            // returns __last_msg and clears it
    bool ConsumeRequested() const;              // true until ClearConsume()
    void ClearConsume();

    // Read a one-shot script variable and clear it in the same step.
    // set_cooldown used to be a permanent override: nothing ever reset
    // __cooldown, so one `set_cooldown 0.28` in a weapon def silently replaced
    // the authored fire_rate for the life of the instance.
    float TakePendingFloat(const std::string& name);
    std::vector<PlayerStatOp> PopPlayerStatOps();  // drains queued stat writes
    PickupSpawnRequest PopPendingPickupSpawn();    // returns __pickup_* data
    bool PopPendingHurt();                         // returns true if a scripted damage flash was requested

private:
    std::vector<std::string> m_lines;
    int m_pc = 0;

    // Per-instance variable memory
    std::unordered_map<std::string, int> m_intVars;
    std::unordered_map<std::string, float> m_floatVars;
    std::unordered_map<std::string, std::string> m_strVars;
    int m_flags[MAX_FLAGS] = {0};

    // External stat provider (player runtimeStats), set by LEM
    std::function<float(const std::string&)> m_statResolver;

    // Deferred side-effect queues
    std::vector<PlayerStatOp> m_pendingStatOps;
    std::vector<ScriptCommand> m_commands;

    // World-scope flag read provider (see SetWorldFlagGetter).
    std::function<int(const std::string&)> m_worldFlagGetter;

    // Jump labels: label_name → line_index
    std::unordered_map<std::string, int> m_jumpLabels;
    std::string m_debugTag;

    // Game-specific opcode handlers (registered via RegisterOpcode)
    std::unordered_map<std::string, OpcodeHandler> m_opcodeHandlers;

    // Internal: evaluate a condition string (e.g. "$x > 5")
    bool EvalCondition(const std::string& cond);

    // Internal: resolve a variable name (no leading '$') to a value.
    // Order: float vars → int vars → toggle flags ("flag<idx>") → stat resolver → 0.
    float ResolveVar(const std::string& vn) const;
};
