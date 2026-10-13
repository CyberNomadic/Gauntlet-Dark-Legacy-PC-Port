#pragma once

#include <functional>
#include <optional>
#include <span>

#include "engine/assets/WorldData.h"
#include "engine/assets/WorldLayout.h"

#include "game/enemies/PlacedEnemies.h"

namespace gdl::game {

/** The native ordinary placement row, decoded independently of spawning. Great
 * creatures and statue creation retain their own native load-time path. */
class OpponentPlacements {
public:
    using LoadKind = std::function<bool(s32)>;
    static std::optional<PlacedEnemies::Placement> ordinary(const ItemInstance& instance,
                                                            const ItemInfo& info,
                                                            std::span<const LevelEnemy> roster,
                                                            s32 players, const LoadKind& load);
};

} // namespace gdl::game
