#pragma once

#include "engine/math/Math.h"

namespace gdl::game {

/** A native barrel's single, duration-bound attack. Each victim is reached at
 * most once; an actor entering later may still be hit while the blast exists.
 * Flags and sound are native identifiers, not GDL attack flags or audio keys. */
struct LegendsFixtureEvent {
    enum class Kind { Explosion, Poison };
    Kind kind = Kind::Explosion;
    u32 nativeIndex = 0;
    Vec3 position{0};
    f32 radius = 0;
    f32 damage = 0;
    u32 flags = 0;
    u32 sound = 0;
    s32 player = -1;
    f32 seconds = 0;
    f32 growFraction = .1f;
    f32 shrinkFraction = .2f;
    bool noFalloff = false;
};

struct LegendsSoundEvent {
    u32 token = 0;
    Vec3 position{0};
};

/** A retained native fixture voice. Identity belongs to the motion target,
 * not whichever plate requested its current endpoint. */
struct LegendsSoundLoop {
    u32 id = 0;
    u32 token = 0;
    Vec3 position{0};
};

} // namespace gdl::game
