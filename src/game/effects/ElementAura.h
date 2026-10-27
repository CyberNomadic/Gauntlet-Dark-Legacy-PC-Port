#pragma once

#include <array>
#include <string_view>

#include "engine/core/Types.h"
#include "engine/world/ParticleSystem.h"

namespace gdl::game {

/**
 * Aggressor Mode's mark on an elemental one: a particle effect of the remake's own, in the
 * element's colour and manner, over the pixie sprite the weapons archive holds in each
 * colour. Fire's embers rise and redden, electric's sparks fly every way and die fast,
 * light's motes drift up large and slow, and acid's drips fall green. Each is endless and
 * follows the body it is placed on.
 */
struct ElementAura {
    static constexpr s32 kElements = 4;
    /** The sprite each element borrows: none, then fire, electric, light, acid. */
    static constexpr std::array<std::string_view, kElements + 1> kSprites{
        "", "PIXIE_R", "PIXIE_B", "PIXIE_Y", "PIXIE_G"};
    static constexpr u32 kMostParticles = 80;

    /** The sprite's name for `element`, empty for no element. */
    static std::string_view sprite(s32 element);
    /** The emitter about a body of `radius` and `height`, endless and following it; the
     * plain descriptor for no element. */
    static ParticleDescriptor descriptor(s32 element, f32 radius, f32 height);
};

} // namespace gdl::game
