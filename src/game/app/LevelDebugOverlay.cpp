#include "game/app/LevelDebugOverlay.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>
#include <numbers>

#include "engine/core/Log.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/SystemFont.h"

#include "game/app/BuildLabel.h"
#include "game/level/LevelWorld.h"
#include "game/party/PlayerRuntime.h"

namespace gdl::game {
namespace {
constexpr f32 kTextHeight = 11.0f; ///< in the build label's 540-tall reference frame
constexpr f32 kLineGap = 2.0f;
constexpr f32 kMargin = 1.0f;
constexpr f32 kTop = 16.0f; ///< under the build label
constexpr f32 kTracking = 0.06f;
constexpr u32 kGlyphPixels = 36;
constexpr Color kTint = Color::rgba(255, 255, 160, 230);

f32 degrees(f32 radians) {
    return radians * 180.0f / std::numbers::pi_v<f32>;
}

std::string point(const Vec3& v) {
    return std::format("{:.2f} {:.2f} {:.2f}", v.x, v.y, v.z);
}
} // namespace

std::vector<std::string> LevelDebugOverlay::describe(const LevelWorld& world,
                                                     std::span<const PlayerRuntime> players,
                                                     const TowerCamera::State& camera,
                                                     s32 enemiesAlive) {
    std::vector<std::string> lines;
    const LevelRef& ref = world.ref();
    lines.push_back(std::format("Level {} \"{}\" realm {} index {}", ref.name, ref.title,
                                ref.realmId, ref.index));
    const auto& objects = world.layout().objects();
    const auto objectName = [&](s32 object) -> std::string {
        if (object >= 0 && static_cast<usize>(object) < objects.size()) {
            return std::format("{} ({})", objects[static_cast<usize>(object)].name, object);
        }
        return object < 0 ? std::string{"none"} : std::format("#{}", object);
    };
    for (usize i = 0; i < players.size(); ++i) {
        const PlayerRuntime& player = players[i];
        if (player.life == PlayerLife::InTower) {
            continue;
        }
        const PlayerActor& actor = player.actor;
        lines.push_back(std::format("Player {}: at {}  yaw {:.0f} deg  floor y {:.2f} on {}{}",
                                    i + 1, point(actor.position()), degrees(actor.yaw()),
                                    actor.floorBase(), objectName(actor.floorObject()),
                                    player.life == PlayerLife::Dying ? "  (dying)" : ""));
    }
    const WorldCamera& view = camera.camera;
    lines.push_back(std::format("Camera: at {}  pitch {:.1f} yaw {:.1f} deg  distance {:.1f}",
                                point(view.position), degrees(view.pitch), degrees(view.yaw),
                                camera.distance));
    const auto& markers = world.cameraMarkers();
    if (camera.marker >= 0 && static_cast<usize>(camera.marker) < markers.size()) {
        const WorldLocator& marker = markers[static_cast<usize>(camera.marker)];
        lines.push_back(std::format(
            "Marker {} of {}: at {}  pitch {:.1f} yaw {:.1f} deg{}", camera.marker, markers.size(),
            point(marker.position), degrees(marker.rotation.x), degrees(marker.rotation.y),
            camera.aimedMarker != camera.marker ? std::format("  turning to {}", camera.aimedMarker)
                                                : std::string{}));
    } else {
        lines.push_back(std::format("Marker none of {}", markers.size()));
    }
    if (!players.empty() && players.front().life != PlayerLife::InTower) {
        const Vec3 feet = players.front().actor.position();
        std::vector<std::pair<f32, usize>> nearest;
        for (usize i = 0; i < objects.size(); ++i) {
            if ((objects[i].flags & WorldObject::kParticles) != 0) {
                continue;
            }
            nearest.emplace_back(glm::distance(world.layout().worldPosition(i), feet), i);
        }
        std::ranges::sort(nearest);
        std::string line = "Nearest objects:";
        for (usize n = 0; n < std::min(nearest.size(), kNearestObjects); ++n) {
            const auto& [distance, i] = nearest[n];
            line += std::format("  {} ({}) {:.1f}", objects[i].name, i, distance);
        }
        lines.push_back(std::move(line));
    }
    lines.push_back(std::format("Enemies alive {}", enemiesAlive));
    return lines;
}

bool LevelDebugOverlay::loadGlyphs(RenderDevice& device) {
    if (m_glyphsTried) {
        return m_glyphs[0].texture != nullptr;
    }
    m_glyphsTried = true;
    try {
        for (usize i = 0; i < kGlyphCount; ++i) {
            const char c = static_cast<char>(kFirstGlyph + static_cast<char>(i));
            const Image image = rasterizeSystemText(std::string{c}, kGlyphPixels);
            if (image.width == 0 || image.height == 0) {
                m_glyphs[i].advance = 0.3f; // a space has no coverage to rasterize
                continue;
            }
            m_glyphs[i].texture = device.createTexture(
                {image.width, image.height, TextureFilter::Linear, TextureWrap::ClampToEdge},
                image.pixels);
            m_glyphs[i].advance =
                static_cast<f32>(image.width) / static_cast<f32>(image.height) + kTracking;
        }
    } catch (const std::exception& error) {
        release();
        m_glyphsTried = true;
        log::warn("Level debug overlay: {}", error.what());
        return false;
    }
    if (m_glyphs[0].texture == nullptr) {
        // The space is drawn as a gap; the test for readiness looks at a visible glyph.
        m_glyphs[0].texture = device.createTexture(
            {1, 1, TextureFilter::Nearest, TextureWrap::ClampToEdge}, std::vector<u8>{0, 0, 0, 0});
    }
    return true;
}

void LevelDebugOverlay::render(RenderDevice& device, Extent2D extent,
                               std::span<const std::string> lines) {
    if (!m_visible || extent.isZero() || lines.empty() || !loadGlyphs(device)) {
        return;
    }
    Canvas canvas;
    canvas.begin(device, BuildLabel::projection(extent), {.depthWrite = false, .depthTest = false});
    f32 y = kTop;
    for (const std::string& line : lines) {
        f32 x = kMargin;
        for (const char c : line) {
            const auto index =
                static_cast<usize>(static_cast<unsigned char>(c)) - static_cast<usize>(kFirstGlyph);
            if (index >= kGlyphCount) {
                continue;
            }
            const Glyph& glyph = m_glyphs[index];
            const f32 width = glyph.advance * kTextHeight;
            if (glyph.texture != nullptr && index != 0) {
                canvas.draw(*glyph.texture, {x, y, width - kTracking * kTextHeight, kTextHeight},
                            kTint);
            }
            x += width;
        }
        y += kTextHeight + kLineGap;
    }
    canvas.end();
}

void LevelDebugOverlay::release() {
    for (Glyph& glyph : m_glyphs) {
        glyph.texture.reset();
        glyph.advance = 0.0f;
    }
    m_glyphsTried = false;
}

} // namespace gdl::game
