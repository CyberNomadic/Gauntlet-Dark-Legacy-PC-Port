#pragma once

#include "game/enemies/Enemies.h"

namespace gdl::game {

/** An authored one-shot actor, translated to the existing GDL combat systems.
 * The pending presentation is not a live/solid/damageable actor. It is retired
 * only after its replacement has actually acquired a combat pool slot. */
struct ImportedActor {
    u32 nativeIndex = 0;
    EnemySpawn spawn;
    f32 yaw = 0;
    f32 idleYaw = 0;
    char costumeRealm = 0; ///< GDL counterpart's costume, not the imported map prefix
    u8 minimumPlayers = 0;
    f32 activationRadius = 0;
    f32 activationHeight = 0; ///< zero uses a sphere; otherwise a strict vertical window
    s32 floorObject = -1;     ///< native supporting object, for pending floor riders
};

} // namespace gdl::game
