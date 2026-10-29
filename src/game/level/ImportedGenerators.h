#pragma once

#include "engine/assets/WorldLayout.h"

namespace gdl::game {

/** Native placement translated to the existing GDL brood/AI rules. Artwork stays
 * with the imported level; native indices never index the GameCube item tables. */
struct ImportedGenerator {
    u32 nativeIndex = 0;
    s32 kind = 0;
    s32 tier = 1;
    Vec3 position{0};
    f32 yaw = 0;
    ItemInfo body;
    s32 minimumPlayers = 0;
};

struct ImportedGeneratorState {
    u32 nativeIndex = 0;
    s32 pool = -1;
    s32 state = 0;
    Vec3 position{0};
    bool standing = false;
};

} // namespace gdl::game
