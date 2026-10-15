#include "game/enemies/OpponentState.h"

#include <algorithm>

namespace gdl::game {

bool OpponentState::valid() const {
    return generators.valid() && enemies.valid() && missiles.valid() &&
           (!placements || placements->valid()) && validAggregate();
}
bool OpponentState::validAggregate() const {
    const auto hasGenerator = [this](s32 id) {
        return id >= 0 && static_cast<usize>(id) < generators.generators.size();
    };
    // Destroyed generators keep their roster slots. Detached children may use
    // -1, and delayed brood events may still name a destroyed slot; neither
    // requires a standing generator or a live-child census matching its quota.
    return std::ranges::all_of(enemies.enemies,
                               [&](const auto& enemy) {
                                   return enemy.generator == -1 || hasGenerator(enemy.generator);
                               }) &&
           std::ranges::all_of(enemies.generatorEvents,
                               [&](const auto& event) { return hasGenerator(event.generator); }) &&
           (!placements || std::ranges::all_of(placements->roster, [&](const auto& placement) {
               return placement.spawn.generator == -1 || hasGenerator(placement.spawn.generator);
           }));
}

} // namespace gdl::game
