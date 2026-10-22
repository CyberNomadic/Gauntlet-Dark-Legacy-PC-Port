#pragma once

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "engine/core/SpecialMembers.h"
#include "engine/render/RenderDevice.h"

#include "game/camera/TowerCamera.h"

namespace gdl::game {

class LevelWorld;
struct PlayerRuntime;

/**
 * A toggled text overlay naming the level and where everything in it stands: each player's
 * position, heading and floor, the follow camera and its marker, the nearest placed objects
 * and the live enemy count. For reporting what the screen shows; off by default.
 */
class LevelDebugOverlay {
public:
    LevelDebugOverlay() = default;
    ~LevelDebugOverlay() = default;
    GDL_NON_COPYABLE_NON_MOVABLE(LevelDebugOverlay);

    static constexpr usize kNearestObjects = 4;

    /** The overlay's lines for the level as it stands. */
    static std::vector<std::string> describe(const LevelWorld& world,
                                             std::span<const PlayerRuntime> players,
                                             const TowerCamera::State& camera, s32 enemiesAlive);

    void toggle() { m_visible = !m_visible; }
    bool visible() const { return m_visible; }
    /** Draws `lines` down the viewport's left edge, under the build label. The glyphs are
     * rasterized from the system font the first time; without one nothing is drawn. */
    void render(RenderDevice& device, Extent2D extent, std::span<const std::string> lines);
    void release();

private:
    struct Glyph {
        std::unique_ptr<Texture> texture;
        f32 advance = 0.0f; ///< in text-height units
    };
    static constexpr char kFirstGlyph = 32;
    static constexpr usize kGlyphCount = 95;

    bool loadGlyphs(RenderDevice& device);

    bool m_visible = false;
    bool m_glyphsTried = false;
    std::array<Glyph, kGlyphCount> m_glyphs;
};

} // namespace gdl::game
