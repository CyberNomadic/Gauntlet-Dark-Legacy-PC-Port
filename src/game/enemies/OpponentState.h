#pragma once

#include "game/enemies/Enemies.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/enemies/Generators.h"

namespace gdl::game {

/** Pointer-free, same-tick state of the three coupled ordinary-opponent pools.
 * Resource binding and world ownership are checked by the restore coordinator. */
struct OpponentState {
    Generators::Checkpoint generators;
    Enemies::Checkpoint enemies;
    EnemyMissiles::Checkpoint missiles;
    bool valid() const;
    bool operator==(const OpponentState&) const = default;
};

} // namespace gdl::game
