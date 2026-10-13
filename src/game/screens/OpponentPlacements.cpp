#include "game/screens/OpponentPlacements.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "game/enemies/Generators.h"
#include "game/world/ItemFigure.h"

namespace gdl::game {

std::optional<PlacedEnemies::Placement>
OpponentPlacements::ordinary(const ItemInstance& instance, const ItemInfo& info,
                             std::span<const LevelEnemy> roster, s32 players,
                             const LoadKind& load) {
    if (info.type != ItemInfo::kPlacedEnemy || !shownToParty(instance.minPlayers, players)) {
        return std::nullopt;
    }
    const auto named = enemyKindOf(info.name);
    if (!named) {
        return std::nullopt;
    }
    const s32 strength = Generators::paramOf(instance, 0);
    const s32 kind = levelKindOf(roster, *named, strength);
    // IT waits for company; great creatures use the separate statue/carried-item
    // loader, not this ordinary swarm-row path.
    if ((kind == kItKind && players <= 1) ||
        (kind >= kSwarmKindCount && kind != kDeathKind && kind != kItKind) || !load ||
        !load(kind)) {
        return std::nullopt;
    }
    const Mat4 stood = itemPlacement(instance.position, instance.rotation);
    PlacedEnemies::Placement placement;
    placement.kind = kind;
    placement.facing = std::atan2(stood[2][0], stood[2][2]);
    placement.viewRadius = 2 * std::max(info.radius, info.height);
    constexpr usize kSightOffset = 4;
    if (instance.params.size() >= kSightOffset + sizeof(placement.sight)) {
        std::memcpy(&placement.sight, &instance.params[kSightOffset], sizeof(placement.sight));
    }
    auto& spawn = placement.spawn;
    spawn.kind = kind;
    spawn.tier = std::max(strength, 1);
    spawn.algorithm = Generators::paramOf(instance, 1);
    // SetItem stores the interval at byte eight, in tenths of a second. One
    // removes the wait, whereas zero preserves EnemySpawn's default.
    if (const s32 interval = Generators::paramOf(instance, 4); interval > 0) {
        spawn.throwInterval = interval == 1 ? 0.0f : static_cast<f32>(interval) * 0.1f;
    }
    spawn.position = instance.position;
    spawn.direction = Vec3{stood[2][0], 0, stood[2][2]};
    spawn.sight = placement.sight;
    spawn.placed = true;
    spawn.priority = EnemySpawn::Priority::Visible;
    spawn.asleep = strength == 0 && kind != kDeathKind;
    return placement;
}

} // namespace gdl::game
