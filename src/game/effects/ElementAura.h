#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/render/RenderDevice.h"
#include "engine/render/RenderTypes.h"
#include "engine/world/ParticleSystem.h"

namespace gdl::game {

/**
 * Aggressor Mode's mark on an elemental one: a particle effect of the remake's own, in the
 * element's colour and manner, over a glow sprite of the remake's own. Fire's embers rise
 * and redden, electric's sparks fly every way and die fast, light's motes drift up large and
 * slow, and acid's drips fall green. Each is endless, born about the body, wide of its
 * silhouette, and carried along with it.
 */
class ElementAura {
public:
    static constexpr s32 kElements = 4;
    static constexpr u32 kSpriteSize = 32;
    static constexpr u32 kMostParticles = 96;
    static constexpr std::string_view kSpriteName = "ELEMENT_GLOW";
    /** The width of the box the motes are born in, in the body's radius: most leave outside
     * its silhouette, where the body's own surface cannot hide them. */
    static constexpr f32 kBirthReach = 3.0f;

    /** The glow sprite's pixels, `size` square, RGBA: a white disc fading smoothly to
     * nothing at its rim, its colour premultiplied so it adds and blends alike. */
    static std::vector<u8> glowPixels(u32 size = kSpriteSize);
    /** The emitter about a body of `radius` and `height`, endless and following it; the
     * plain descriptor for no element. */
    static ParticleDescriptor descriptor(s32 element, f32 radius, f32 height);

    /** The glow sprite on `device`, made on first use; null when the device refuses it. */
    const Texture* sprite(RenderDevice& device);
    void release();

private:
    std::shared_ptr<Texture> m_sprite; ///< shared by copies; never simulation state
    bool m_refused = false;
};

} // namespace gdl::game
