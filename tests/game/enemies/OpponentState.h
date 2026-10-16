#pragma once

#include "game/enemies/Enemies.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/enemies/Generators.h"
#include "game/enemies/PlacedEnemiesCheckpoint.h"

namespace gdl::game {

/** Pointer-free, same-tick state of the coupled ordinary-opponent owners.
 * Resource binding and world ownership are checked by the restore coordinator. */
struct OpponentState {
    Generators::Checkpoint generators;
    Enemies::Checkpoint enemies;
    EnemyMissiles::Checkpoint missiles;
    /** Optional when a diagnostic fixture binds only the three active pools. */
    std::optional<PlacedEnemiesCheckpoint::State> placements = std::nullopt;
    bool valid() const;
    bool operator==(const OpponentState&) const = default;

private:
    friend class OpponentCheckpoint;
    bool validAggregate() const;
};

} // namespace gdl::game
