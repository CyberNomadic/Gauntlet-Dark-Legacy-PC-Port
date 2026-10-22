#pragma once

#include "game/level/LevelWorld.h"

namespace gdl::game {
class Enemies;
class Critters;
class Bosses;
/** Test-only preloading for detailed diagnostic resource catalogs. This loads
 * artwork without creating actors or advancing gameplay; native online loading
 * uses PlayScene directly and does not depend on this catalog. */
class StageResources {
public:
    static bool preload(const LevelWorld& world, Enemies& enemies, Critters& critters,
                        Bosses& bosses);
    /** Stable archive order includes future container Deaths and boss generators,
     * not only fixtures/actors visible at the entrance. Owners remain borrowed. */
    static std::vector<ItemArchive*> fixtureArchives(LevelWorld& world, Enemies& enemies,
                                                     Critters& critters, Bosses& bosses);
};
} // namespace gdl::game
