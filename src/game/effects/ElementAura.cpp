#include "game/effects/ElementAura.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <string>

#include "engine/core/Log.h"

namespace gdl::game {

namespace {

constexpr f32 kFrames = ParticleDescriptor::kFrameRate;

/** Frames of `seconds`, at least one. */
constexpr u32 frames(f32 seconds) {
    return seconds * kFrames < 1.0f ? 1U : static_cast<u32>(seconds * kFrames);
}

/** An envelope that goes from `start` to `end` over the life and on to `gone` over the fade. */
constexpr ParticleEnvelope over(f32 start, f32 end, f32 gone) {
    return {start, end, end, gone};
}

/** A colour lane the same way, from shares of one: the descriptor's colours run 0 to 255. */
constexpr ParticleEnvelope shade(f32 start, f32 end, f32 gone) {
    return over(255.0f * start, 255.0f * end, 255.0f * gone);
}

} // namespace

std::vector<u8> ElementAura::glowPixels(u32 size) {
    std::vector<u8> pixels(static_cast<usize>(size) * size * 4, 0);
    if (size == 0) {
        return pixels;
    }
    const f32 centre = 0.5f * static_cast<f32>(size - 1);
    const f32 reach = 0.5f * static_cast<f32>(size);
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            const f32 dx = (static_cast<f32>(x) - centre) / reach;
            const f32 dy = (static_cast<f32>(y) - centre) / reach;
            const f32 edge = std::clamp(1.0f - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);
            const f32 glow = edge * edge * (3.0f - 2.0f * edge); // smooth, centre to rim
            const auto value = static_cast<u8>(std::lround(255.0f * glow));
            const usize at = (static_cast<usize>(y) * size + x) * 4;
            for (usize channel = 0; channel < 4; ++channel) {
                pixels[at + channel] = value;
            }
        }
    }
    return pixels;
}

const Texture* ElementAura::sprite(RenderDevice& device) {
    if (!m_sprite && !m_refused) {
        try {
            m_sprite = device.createTexture(
                {kSpriteSize, kSpriteSize, TextureFilter::Linear, TextureWrap::ClampToEdge},
                glowPixels());
        } catch (const std::exception& error) {
            log::warn("Element aura: {}", error.what());
        }
        m_refused = m_sprite == nullptr;
    }
    return m_sprite.get();
}

void ElementAura::release() {
    m_sprite.reset();
    m_refused = false;
}

ParticleDescriptor ElementAura::descriptor(s32 element, f32 radius, f32 height) {
    ParticleDescriptor d;
    if (element < 1 || element > kElements) {
        return d;
    }
    // Every aura: endless, born about the body, carried along with it, lit by itself and
    // drawn over whatever stands behind it.
    d.emitFrames = ParticleDescriptor::kEndless;
    d.dynamic = true;
    d.volume = Vec3{kBirthReach * radius, height, kBirthReach * radius};
    d.maxParticles = kMostParticles;
    d.additive = true;
    d.depthWrite = false;
    d.texture = std::string{kSpriteName};
    const auto perSecond = [&](f32 rate) {
        d.rate = {rate / kFrames, rate / kFrames, rate / kFrames, rate / kFrames};
    };
    switch (element) {
    case 1: // fire: embers rise from the body and redden as they die
        d.direction = Vec3{0.0f, 1.0f, 0.0f};
        d.angle = 0.6f;
        d.speed = 5.0f / kFrames;
        d.gravity = -0.3f * ParticleDescriptor::kDefaultGravity; // up
        d.particleLife = frames(0.8f);
        d.particleFade = frames(0.3f);
        perSecond(36.0f);
        d.width = over(1.6f, 1.0f, 0.2f);
        d.red = shade(1.0f, 1.0f, 0.8f);
        d.green = shade(0.65f, 0.25f, 0.0f);
        d.blue = shade(0.1f, 0.0f, 0.0f);
        d.alpha = shade(1.0f, 0.7f, 0.0f);
        break;
    case 2: // electric: sparks fly every way and are gone in a blink
        d.angle = ParticleDescriptor::kSphere;
        d.speed = 14.0f / kFrames;
        d.drag = -0.08f;
        d.particleLife = frames(0.3f);
        d.particleFade = frames(0.15f);
        perSecond(48.0f);
        d.width = over(1.2f, 0.8f, 0.2f);
        d.red = shade(0.7f, 0.3f, 0.1f);
        d.green = shade(0.85f, 0.5f, 0.2f);
        d.blue = shade(1.0f, 1.0f, 1.0f);
        d.alpha = shade(1.0f, 0.8f, 0.0f);
        break;
    case 3: // light: motes drift up, large and slow
        d.direction = Vec3{0.0f, 1.0f, 0.0f};
        d.angle = 1.2f;
        d.speed = 1.5f / kFrames;
        d.particleLife = frames(1.2f);
        d.particleFade = frames(0.5f);
        perSecond(18.0f);
        d.width = over(2.6f, 1.8f, 0.6f);
        d.red = shade(1.0f, 1.0f, 1.0f);
        d.green = shade(1.0f, 0.9f, 0.7f);
        d.blue = shade(0.6f, 0.3f, 0.1f);
        d.alpha = shade(0.9f, 0.6f, 0.0f);
        break;
    default: // acid: drips fall green and pool
        d.direction = Vec3{0.0f, -1.0f, 0.0f};
        d.angle = 0.3f;
        d.speed = 2.0f / kFrames;
        d.gravity = 0.5f * ParticleDescriptor::kDefaultGravity; // down
        d.particleLife = frames(0.8f);
        d.particleFade = frames(0.3f);
        perSecond(30.0f);
        d.width = over(1.4f, 1.0f, 0.3f);
        d.red = shade(0.4f, 0.2f, 0.0f);
        d.green = shade(1.0f, 0.8f, 0.4f);
        d.blue = shade(0.2f, 0.1f, 0.0f);
        d.alpha = shade(1.0f, 0.8f, 0.0f);
        break;
    }
    return d;
}

} // namespace gdl::game
