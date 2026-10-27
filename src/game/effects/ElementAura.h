#pragma once

#include <memory>
#include <vector>

#include "engine/core/Types.h"
#include "engine/render/RenderDevice.h"
#include "engine/render/RenderTypes.h"

namespace gdl::game {

/**
 * The artwork of Aggressor Mode's elemental auras, all of the remake's own: one atlas of
 * procedural sprites, premultiplied so they add and blend alike, made on the device on
 * first use. The show (ElementAuraShow) cuts its flames, arcs, rings, rays and sparks from
 * it: a soft glow, a soft ring, a light streak tapering to its tip, a beam soft across its
 * width, four tongues of flame with yellow cores and orange rims, a four-pointed glint and
 * a falling drop.
 */
class ElementAura {
public:
    enum class Sprite : u8 {
        Glow,
        Ring,
        Streak,
        Beam,
        Flame0,
        Flame1,
        Flame2,
        Flame3,
        Glint,
        Drop
    };
    static constexpr usize kSprites = 10;
    static constexpr u32 kFlames = 4;
    static constexpr u32 kWidth = 256;
    static constexpr u32 kHeight = 128;

    /** A sprite's place on the atlas, in pixels. */
    struct Place {
        u32 x = 0;
        u32 y = 0;
        u32 w = 0;
        u32 h = 0;
    };
    /** Its texture coordinates, half a texel inside its place so no neighbour bleeds in;
     * v0 is its top. */
    struct Cell {
        f32 u0 = 0.0f;
        f32 v0 = 0.0f;
        f32 u1 = 0.0f;
        f32 v1 = 0.0f;
    };

    static Place place(Sprite sprite);
    static Cell cell(Sprite sprite);
    /** The tongue of flame `frame` of kFlames, wrapping. */
    static Sprite flame(u32 frame) {
        return static_cast<Sprite>(static_cast<u8>(Sprite::Flame0) +
                                   static_cast<u8>(frame % kFlames));
    }
    /** The whole atlas, RGBA, kWidth by kHeight, premultiplied. */
    static std::vector<u8> pixels();

    /** The atlas on `device`, made on first use; null when the device refuses it. */
    const Texture* texture(RenderDevice& device);
    void release();

private:
    std::shared_ptr<Texture> m_texture; ///< shared by copies; never simulation state
    bool m_refused = false;
};

} // namespace gdl::game
