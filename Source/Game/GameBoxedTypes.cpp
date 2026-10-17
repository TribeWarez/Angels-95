#include "GameBoxedTypes.hpp"
#include "../Boxed/BoxedTypeRegistry.hpp"

namespace oz {
namespace game {

// SCAFFOLD (Phase 0): no game boxed types yet. Phase 2/3 registers descriptors
// here, e.g.:
//   oz::boxed::BoxedTypeDesc d;
//   d.name = "faction_terminal";
//   d.source = "game";
//   d.parse = &ParseFactionTerminalLine;   // Phase 1 typed hooks
//   oz::boxed::BoxedTypeRegistry::Instance().Register(d);
void RegisterGameBoxedTypes() {
    (void)oz::boxed::BoxedTypeRegistry::Instance();   // referenced; no-op for now
}

} // namespace game
} // namespace oz
