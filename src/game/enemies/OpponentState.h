#pragma once

#include "game/enemies/Enemies.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/enemies/Generators.h"
#include "game/enemies/PlacedEnemies.h"

namespace gdl::game {

/** Pointer-free, same-tick state of the coupled ordinary-opponent owners.
 * Resource binding and world ownership are checked by the restore coordinator. */
struct OpponentState {
    Generators::Checkpoint generators;
    Enemies::Checkpoint enemies;
    EnemyMissiles::Checkpoint missiles;
    /** Absent only for legacy three-pool callers. Native online capture includes
     * the complete immutable placement roster, even after all entries stood. */
    std::optional<PlacedEnemies::Checkpoint> placements = std::nullopt;
    bool valid() const;
    bool operator==(const OpponentState&) const = default;
};

} // namespace gdl::game
