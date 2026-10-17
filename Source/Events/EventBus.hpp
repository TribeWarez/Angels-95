#pragma once
// ---------------------------------------------------------------------------
// EventBus — engine-generic gameplay event bus.
//
// SCAFFOLD (Phase 0). The bus is implemented, but nothing emits or subscribes
// yet: existing hooks still dispatch through LightningEntityManager::RunAction
// / TriggerEntityAction. Phase 1 migrates those call sites onto Emit() and makes
// LightningScript `actions { on_<topic> }` subscribe here.
//
// Why it exists: gameplay hooks today are string-named RunAction("on_xxx")
// calls hardcoded at ~20 C++ sites. A topic-keyed bus lets the engine emit
// generic facts (an entity died, a player entered a volume, an objective
// progressed) and lets .ozls scripts subscribe, so game-specific behaviour
// lives in content instead of new `if (type == X)` branches.
//
// Raylib-free on purpose: the whole module links into headless test targets.
//
// Topics (Phase 1 vocabulary; see Wiki/Game-Hierarchy.md):
//   world.load/unload, zone.enter/exit, trigger.enter/exit/interact,
//   entity.spawn/death/damage/kill, player.spawn/death/hurt/level_up,
//   pickup.collect, item.use/equip, projectile.hit,
//   objective.progress/complete/fail, mission.start/complete, wave.start/clear,
//   plus the existing pawn FSM topics (patrol/chase/return/death).
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <vector>

namespace oz {
namespace events {

// A single emitted fact. Deliberately a small POD-ish struct with a fixed set
// of generic slots rather than a variant, so the bus stays allocation-light and
// serialisable over the network in COOP.
struct Event {
    std::string topic;          // "entity.death", "trigger.enter", ...
    uint32_t actor  = 0;        // entity/pawn/player id that caused it (0 = none)
    uint32_t target = 0;        // entity/pawn/player id it happened to
    int      worldIndex = -1;   // -1 = not world-scoped
    float    x = 0.0f, y = 0.0f, z = 0.0f;   // world position (Y-up engine space)
    int      i0 = 0, i1 = 0, i2 = 0, i3 = 0; // generic integer payload
    float    f0 = 0.0f, f1 = 0.0f, f2 = 0.0f, f3 = 0.0f; // generic float payload
    std::string s0;             // generic string payload (def name, objective id, ...)
};

// Handler signature. `user` is the opaque context passed at Subscribe time.
using EventHandler = void (*)(const Event& ev, void* user);

class EventBus {
public:
    static EventBus& Instance();

    // Subscribe `fn` to `topic`. `topic` may be a literal or a prefix ending in
    // '*' (e.g. "entity.*") once Phase 1 adds wildcard matching; Phase 0 matches
    // exact topics only.
    void Subscribe(const std::string& topic, EventHandler fn, void* user);

    // Remove every subscription registered with `user` (used when an entity /
    // script instance despawns).
    void Unsubscribe(void* user);

    // Publish an event to every matching subscriber, in registration order.
    void Emit(const Event& ev);

    // Drop all subscriptions (world unload / shutdown).
    void Clear();

    // Test/diagnostic hook: number of live subscriptions.
    size_t SubscriberCount() const { return m_subs.size(); }

private:
    EventBus() = default;

    struct Sub {
        std::string  topic;
        EventHandler fn = nullptr;
        void*        user = nullptr;
    };
    std::vector<Sub> m_subs;
};

} // namespace events
} // namespace oz
