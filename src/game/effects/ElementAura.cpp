#include "game/effects/ElementAura.h"

#include <string>

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

} // namespace

std::string_view ElementAura::sprite(s32 element) {
    return element >= 1 && element <= kElements ? kSprites[static_cast<usize>(element)]
                                                : std::string_view{};
}

ParticleDescriptor ElementAura::descriptor(s32 element, f32 radius, f32 height) {
    ParticleDescriptor d;
    if (element < 1 || element > kElements) {
        return d;
    }
    // Every aura: endless, born anywhere in the body, carried along with it, lit by itself.
    d.emitFrames = ParticleDescriptor::kEndless;
    d.dynamic = true;
    d.volume = Vec3{radius, 0.5f * height, radius};
    d.maxParticles = kMostParticles;
    d.additive = true;
    d.depthWrite = false;
    d.texture = std::string{sprite(element)};
    const auto perSecond = [&](f32 rate) {
        d.rate = {rate / kFrames, rate / kFrames, rate / kFrames, rate / kFrames};
    };
    switch (element) {
    case 1: // fire: embers rise from the body and redden as they die
        d.direction = Vec3{0.0f, 1.0f, 0.0f};
        d.angle = 0.6f;
        d.speed = 4.0f / kFrames;
        d.gravity = -0.3f * ParticleDescriptor::kDefaultGravity; // up
        d.particleLife = frames(0.7f);
        d.particleFade = frames(0.2f);
        perSecond(24.0f);
        d.width = over(0.9f, 0.5f, 0.1f);
        d.red = over(1.0f, 1.0f, 0.8f);
        d.green = over(0.65f, 0.25f, 0.0f);
        d.blue = over(0.1f, 0.0f, 0.0f);
        d.alpha = over(1.0f, 0.7f, 0.0f);
        break;
    case 2: // electric: sparks fly every way and are gone in a blink
        d.angle = ParticleDescriptor::kSphere;
        d.speed = 12.0f / kFrames;
        d.drag = -0.08f;
        d.particleLife = frames(0.25f);
        d.particleFade = frames(0.12f);
        perSecond(36.0f);
        d.width = over(0.5f, 0.3f, 0.1f);
        d.red = over(0.7f, 0.3f, 0.1f);
        d.green = over(0.85f, 0.5f, 0.2f);
        d.blue = over(1.0f, 1.0f, 1.0f);
        d.alpha = over(1.0f, 0.8f, 0.0f);
        break;
    case 3: // light: motes drift up, large and slow
        d.direction = Vec3{0.0f, 1.0f, 0.0f};
        d.angle = 1.2f;
        d.speed = 1.5f / kFrames;
        d.particleLife = frames(1.2f);
        d.particleFade = frames(0.4f);
        perSecond(12.0f);
        d.width = over(1.2f, 0.8f, 0.3f);
        d.red = over(1.0f, 1.0f, 1.0f);
        d.green = over(1.0f, 0.9f, 0.7f);
        d.blue = over(0.6f, 0.3f, 0.1f);
        d.alpha = over(0.9f, 0.6f, 0.0f);
        break;
    default: // acid: drips fall green and pool
        d.direction = Vec3{0.0f, -1.0f, 0.0f};
        d.angle = 0.3f;
        d.speed = 2.0f / kFrames;
        d.gravity = 0.5f * ParticleDescriptor::kDefaultGravity; // down
        d.particleLife = frames(0.8f);
        d.particleFade = frames(0.25f);
        perSecond(18.0f);
        d.width = over(0.6f, 0.45f, 0.2f);
        d.red = over(0.4f, 0.2f, 0.0f);
        d.green = over(1.0f, 0.8f, 0.4f);
        d.blue = over(0.2f, 0.1f, 0.0f);
        d.alpha = over(1.0f, 0.8f, 0.0f);
        break;
    }
    return d;
}

} // namespace gdl::game
