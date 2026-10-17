#pragma once
// ---------------------------------------------------------------------------
// ObjectiveSystem — engine-generic keyed objective/progress state.
//
// SCAFFOLD (Phase 0). Implemented but not consumed yet. Phase 1 wires the
// `objective` LightningScript opcode and the `objective.progress/complete/fail`
// bus topics to it; Phase 4 replicates it to COOP clients.
//
// Why it exists: the engine has per-instance script flags (LightningScriptContext
// m_flags[64]) but no world- or session-scoped progress store. Objectives
// ("clear the vault", "collect 3 relics") need durable, named, countable state
// that a script can advance and that the server can own and replicate.
//
// Raylib-free on purpose: links into headless test targets.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace oz {
namespace events {

struct ObjectiveState {
    std::string id;
    int  progress = 0;
    int  target   = 1;
    bool complete = false;
    bool failed   = false;
};

class ObjectiveSystem {
public:
    static ObjectiveSystem& Instance();

    // Start (or reset) an objective with a target count. Target <= 0 means
    // "binary" (complete on first progress).
    void Define(const std::string& id, int target);

    // Advance an objective. Emits objective.progress, and objective.complete
    // when progress first reaches target. Returns true when it just completed.
    bool Add(const std::string& id, int delta);

    // Set absolute progress (emits progress/complete as above).
    bool Set(const std::string& id, int progress);

    // Force completion / failure (emits the matching topic once).
    void Complete(const std::string& id);
    void Fail(const std::string& id);

    bool IsComplete(const std::string& id) const;
    const ObjectiveState* Get(const std::string& id) const;
    const std::unordered_map<std::string, ObjectiveState>& All() const { return m_objectives; }

    // --- World/session flags ------------------------------------------------
    // Cross-instance named counters (the `set_worldflag` / `get_worldflag`
    // opcodes). Distinct from the per-instance LightningScriptContext flags,
    // which die with the entity; these live with the world/mission and are
    // serialised alongside the objectives.
    void SetWorldFlag(const std::string& name, int value);
    int  GetWorldFlag(const std::string& name, int def = 0) const;
    const std::unordered_map<std::string, int>& WorldFlags() const { return m_flags; }

    // World unload / new mission.
    void Reset();

    // Persistence (mission/world save). Line format:
    //   obj  <id>=<progress>,<target>,<complete>,<failed>
    //   flag <name>=<value>
    std::string Serialize() const;
    void Deserialize(const std::string& text);

private:
    ObjectiveSystem() = default;
    ObjectiveState& Ensure(const std::string& id);

    std::unordered_map<std::string, ObjectiveState> m_objectives;
    std::unordered_map<std::string, int> m_flags;
};

} // namespace events
} // namespace oz
