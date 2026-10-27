#include "game/effects/ElementAura.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>

#include "engine/core/Log.h"
#include "engine/math/Math.h"

namespace gdl::game {

namespace {

using Place = ElementAura::Place;

/** Where each sprite lies, two pixels apart: the flames along the top, the small sprites
 * beside them, the streak and beam below. */
constexpr std::array<Place, ElementAura::kSprites> kPlaces{{
    {136, 0, 32, 32}, // Glow
    {170, 0, 32, 32}, // Ring
    {0, 50, 64, 16},  // Streak
    {66, 50, 16, 32}, // Beam
    {0, 0, 32, 48},   // Flame0
    {34, 0, 32, 48},  // Flame1
    {68, 0, 32, 48},  // Flame2
    {102, 0, 32, 48}, // Flame3
    {204, 0, 32, 32}, // Glint
    {238, 0, 16, 32}, // Drop
}};

f32 clamp01(f32 value) {
    return std::clamp(value, 0.0f, 1.0f);
}

f32 smooth(f32 t) {
    t = clamp01(t);
    return t * t * (3.0f - 2.0f * t);
}

f32 square(f32 value) {
    return value * value;
}

u8 byte(f32 share) {
    return static_cast<u8>(std::lround(255.0f * clamp01(share)));
}

/** One texel's coverage and colour, both shares of one. */
struct Texel {
    f32 coverage = 0.0f;
    Vec3 colour{1.0f, 1.0f, 1.0f};
};

/** Paints `place` with `shade(x, y)`, x and y running -1 to 1 across it with y up. */
template <typename Shade>
void paint(std::vector<u8>& pixels, const Place& place, Shade shade) {
    for (u32 py = 0; py < place.h; ++py) {
        for (u32 px = 0; px < place.w; ++px) {
            const f32 x = (static_cast<f32>(px) + 0.5f) / static_cast<f32>(place.w) * 2.0f - 1.0f;
            const f32 y = 1.0f - (static_cast<f32>(py) + 0.5f) / static_cast<f32>(place.h) * 2.0f;
            const Texel texel = shade(x, y);
            const f32 coverage = clamp01(texel.coverage);
            const usize at =
                (static_cast<usize>(place.y + py) * ElementAura::kWidth + place.x + px) * 4;
            pixels[at] = byte(coverage * texel.colour.x);
            pixels[at + 1] = byte(coverage * texel.colour.y);
            pixels[at + 2] = byte(coverage * texel.colour.z);
            pixels[at + 3] = byte(coverage);
        }
    }
}

/** A soft disc, full at the centre and gone at the rim. */
Texel glow(f32 x, f32 y) {
    return {smooth(1.0f - std::sqrt(x * x + y * y))};
}

/** A soft ring near the rim, clear within. */
Texel ring(f32 x, f32 y) {
    const f32 d = std::sqrt(x * x + y * y);
    const f32 off = (d - 0.72f) / 0.11f;
    return {d < 1.0f ? std::exp(-off * off) : 0.0f};
}

/** A ray of light: broad and bright at its base (u 0), tapering to nothing at its tip. */
Texel streak(f32 x, f32 y) {
    const f32 u = 0.5f * (x + 1.0f);
    const f32 thick = 0.9f * (1.0f - 0.75f * u);
    const f32 across = smooth(1.0f - std::abs(y) / thick);
    const f32 along = std::pow(1.0f - u, 1.5f);
    const f32 core = smooth(1.0f - std::abs(y) / (0.4f * thick));
    return {across * along * (0.6f + 0.4f * core)};
}

/** A beam, even along its length and soft across it. */
Texel beam(f32 /*x*/, f32 y) {
    return {std::pow(smooth(1.0f - std::abs(y)), 1.5f)};
}

/** A tongue of flame: round at the base, leaning and tapering to its tip, with a yellow
 * core inside an orange rim; each frame leans and wavers its own way. */
Texel flame(u32 frame, f32 x, f32 y) {
    const f32 h = 0.5f * (y + 1.0f); // 0 at the base, 1 at the tip
    const f32 phase = 1.7f * static_cast<f32>(frame);
    const f32 lean = 0.18f * h * std::sin(phase + 5.5f * h);
    f32 radius = h < 0.3f ? 0.88f * std::sqrt(std::max(1.0f - square((0.3f - h) / 0.3f), 0.0f))
                          : 0.88f * std::sqrt(std::max(1.0f - square((h - 0.3f) / 0.7f), 0.0f));
    radius *= 1.0f + 0.12f * std::sin(2.0f * phase + 9.0f * h);
    if (radius < 0.02f) {
        return {};
    }
    const f32 dx = (x - lean) / radius;
    const f32 edge = 1.0f - dx * dx;
    if (edge <= 0.0f) {
        return {};
    }
    const f32 along = 1.0f - 0.5f * h * h;
    const f32 core = smooth(1.0f - std::abs(dx) / 0.55f) * (1.0f - 0.6f * h);
    return {smooth(edge) * along,
            glm::mix(Vec3{1.0f, 0.45f, 0.08f}, Vec3{1.0f, 0.95f, 0.72f}, core)};
}

/** A four-pointed glint about a small disc. */
Texel glint(f32 x, f32 y) {
    const auto spike = [](f32 t) { return std::pow(std::max(1.0f - std::abs(t), 0.0f), 3.0f); };
    const f32 arms = std::max(spike(x) * smooth(1.0f - 6.0f * std::abs(y)),
                              spike(y) * smooth(1.0f - 6.0f * std::abs(x)));
    const f32 disc = smooth(1.0f - 3.0f * std::sqrt(x * x + y * y));
    return {arms + 0.8f * disc};
}

/** A falling drop: round above, pointed below. */
Texel drop(f32 x, f32 y) {
    const f32 h = 0.5f * (1.0f - y); // 0 at the top, 1 at the point
    const f32 radius =
        h < 0.45f ? 0.8f * std::sqrt(std::max(1.0f - square((0.45f - h) / 0.45f), 0.0f))
                  : 0.8f * std::pow(std::max(1.0f - (h - 0.45f) / 0.55f, 0.0f), 0.8f);
    if (radius < 0.02f) {
        return {};
    }
    const f32 dx = x / radius;
    const f32 edge = 1.0f - dx * dx;
    if (edge <= 0.0f) {
        return {};
    }
    return {smooth(edge) * (0.7f + 0.3f * smooth(1.0f - std::abs(dx) / 0.5f))};
}

} // namespace

ElementAura::Place ElementAura::place(Sprite sprite) {
    return kPlaces[static_cast<usize>(sprite)];
}

ElementAura::Cell ElementAura::cell(Sprite sprite) {
    const Place at = place(sprite);
    const auto width = static_cast<f32>(kWidth);
    const auto height = static_cast<f32>(kHeight);
    return {(static_cast<f32>(at.x) + 0.5f) / width, (static_cast<f32>(at.y) + 0.5f) / height,
            (static_cast<f32>(at.x + at.w) - 0.5f) / width,
            (static_cast<f32>(at.y + at.h) - 0.5f) / height};
}

std::vector<u8> ElementAura::pixels() {
    std::vector<u8> atlas(static_cast<usize>(kWidth) * kHeight * 4, 0);
    paint(atlas, place(Sprite::Glow), glow);
    paint(atlas, place(Sprite::Ring), ring);
    paint(atlas, place(Sprite::Streak), streak);
    paint(atlas, place(Sprite::Beam), beam);
    for (u32 frame = 0; frame < kFlames; ++frame) {
        paint(atlas, place(flame(frame)),
              [frame](f32 x, f32 y) { return gdl::game::flame(frame, x, y); });
    }
    paint(atlas, place(Sprite::Glint), glint);
    paint(atlas, place(Sprite::Drop), drop);
    return atlas;
}

const Texture* ElementAura::texture(RenderDevice& device) {
    if (!m_texture && !m_refused) {
        try {
            m_texture = device.createTexture(
                {kWidth, kHeight, TextureFilter::Linear, TextureWrap::ClampToEdge}, pixels());
        } catch (const std::exception& error) {
            log::warn("Element aura: {}", error.what());
        }
        m_refused = m_texture == nullptr;
    }
    return m_texture.get();
}

void ElementAura::release() {
    m_texture.reset();
    m_refused = false;
}

} // namespace gdl::game
