#include <algorithm>
#include <format>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"
#include "engine/world/ParticleSystem.h"
#include "engine/world/WorldCamera.h"

#include "FakeRenderDevice.h"
#include "game/effects/EffectTrees.h"
#include "game/effects/ElementAura.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("each element's aura is endless, born wide of its body and keeps its own manner",
          "[game][effects][aggression][elemental]") {
    for (s32 element = 1; element <= ElementAura::kElements; ++element) {
        CAPTURE(element);
        const ParticleDescriptor d = ElementAura::descriptor(element, 1.5f, 9.0f);
        CHECK(d.emitFrames == ParticleDescriptor::kEndless);
        CHECK(d.dynamic);
        CHECK(d.additive);
        CHECK_FALSE(d.depthWrite);
        CHECK(d.depthTest); // walls still hide it; the wide birth box keeps the body from it
        CHECK(d.volume == Vec3{4.5f, 9.0f, 4.5f});
        CHECK(d.maxParticles == ElementAura::kMostParticles);
        CHECK(d.rate[0] > 0.0f);
        CHECK(d.rate[3] == d.rate[0]);
        CHECK(d.particleLife >= 1);
        CHECK(d.speed > 0.0f);
        CHECK(d.texture == ElementAura::kSpriteName);
        CHECK(d.width.lifeStart >= 1.0f); // a mote at least a unit wide at birth, to be seen
        CHECK(d.alpha.lifeStart >= 200.0f); // colours run 0 to 255; a share of one is unseen
        CHECK(d.alpha.fadeEnd == 0.0f);     // every mote fades to nothing
    }
    // Fire's embers rise and redden; acid's drips fall green; electric's sparks fly every way;
    // light's motes drift up, the largest and slowest.
    const auto fire = ElementAura::descriptor(1, 1.0f, 6.0f);
    const auto electric = ElementAura::descriptor(2, 1.0f, 6.0f);
    const auto light = ElementAura::descriptor(3, 1.0f, 6.0f);
    const auto acid = ElementAura::descriptor(4, 1.0f, 6.0f);
    CHECK(fire.direction.y > 0.0f);
    CHECK(fire.gravity > 0.0f);
    CHECK(fire.red.lifeStart == 255.0f);
    CHECK(fire.green.lifeEnd < fire.green.lifeStart);
    CHECK(acid.direction.y < 0.0f);
    CHECK(acid.gravity < 0.0f);
    CHECK(acid.green.lifeStart > acid.red.lifeStart);
    CHECK(electric.angle == ParticleDescriptor::kSphere);
    CHECK(electric.speed > fire.speed);
    CHECK(electric.particleLife < fire.particleLife);
    CHECK(electric.blue.lifeStart == 255.0f);
    CHECK(light.width.lifeStart > fire.width.lifeStart);
    CHECK(light.speed < fire.speed);
    CHECK(light.particleLife > fire.particleLife);
    // No element, no aura: the plain descriptor, emitting once.
    const auto none = ElementAura::descriptor(0, 1.0f, 6.0f);
    CHECK(none.emitFrames == 1);
    CHECK_FALSE(none.dynamic);
}

TEST_CASE("the glow sprite is a soft white disc, premultiplied and clear at its corners",
          "[game][effects][aggression][elemental]") {
    const u32 size = ElementAura::kSpriteSize;
    const std::vector<u8> pixels = ElementAura::glowPixels(size);
    REQUIRE(pixels.size() == static_cast<usize>(size) * size * 4);
    const auto alphaAt = [&](u32 x, u32 y) { return pixels[(y * size + x) * 4 + 3]; };
    CHECK(alphaAt(size / 2, size / 2) >= 250);
    CHECK(alphaAt(size / 2 - 1, size / 2 - 1) >= 250);
    CHECK(alphaAt(0, 0) == 0);
    CHECK(alphaAt(size - 1, size - 1) == 0);
    CHECK(alphaAt(0, size - 1) == 0);
    // Softer with every step from the centre to the rim, and still there halfway out.
    u8 last = 255;
    for (u32 x = size / 2; x < size; ++x) {
        CHECK(alphaAt(x, size / 2) <= last);
        last = alphaAt(x, size / 2);
    }
    CHECK(alphaAt(size * 3 / 4, size / 2) > 0);
    CHECK(alphaAt(size * 3 / 4, size / 2) < 250);
    CHECK(alphaAt(size - 1, size / 2) < 10);
    for (usize i = 0; i < pixels.size(); i += 4) {
        REQUIRE(pixels[i] == pixels[i + 3]); // premultiplied: the colour is the coverage
        REQUIRE(pixels[i + 1] == pixels[i + 3]);
        REQUIRE(pixels[i + 2] == pixels[i + 3]);
    }
    CHECK(ElementAura::glowPixels(0).empty());
}

TEST_CASE("an aura over the sprite of its own births motes and draws them additive",
          "[game][effects][aggression][elemental]") {
    test::FakeRenderDevice device;
    ElementAura aura;
    const Texture* sprite = aura.sprite(device);
    REQUIRE(sprite != nullptr);
    CHECK(aura.sprite(device) == sprite); // made once
    CHECK(device.texturesCreated == 1);
    CHECK(device.lastTextureDesc.width == ElementAura::kSpriteSize);
    ItemArchive weapons; // unloaded: the aura borrows nothing from it
    for (s32 element = 1; element <= ElementAura::kElements; ++element) {
        CAPTURE(element);
        EffectTrees effects;
        const u32 id = effects.startParticles(
            weapons, ElementAura::descriptor(element, 1.5f, 9.0f), *sprite, Mat4{1.0f});
        REQUIRE(id != 0);
        for (s32 frame = 0; frame < 60; ++frame) {
            effects.update(1.0f / 30.0f);
        }
        CHECK(effects.playing(id)); // endless
        CHECK(effects.count() == 1);
        // Its motes are drawn: additive quads over the glow, no depth written.
        device.draws.clear();
        const CameraFrame camera = CameraFrame::at(Vec3{0.0f, 5.0f, -30.0f});
        effects.draw(device, Mat4{1.0f}, {}, &camera);
        usize motes = 0;
        std::string drawn;
        u8 brightest = 0;
        u8 mostOpaque = 0;
        for (const auto& draw : device.draws) {
            motes += draw.vertices.size() / 6;
            drawn += std::format("[{} verts, blend {}] ", draw.vertices.size(),
                                 static_cast<int>(draw.blend()));
            CHECK(draw.blend() == BlendMode::Additive);
            CHECK(draw.texture == sprite);
            CHECK_FALSE(draw.state.depthWrite);
            for (const ImmediateVertex& vertex : draw.vertices) {
                brightest = std::max({brightest, vertex.color.r, vertex.color.g, vertex.color.b});
                mostOpaque = std::max(mostOpaque, vertex.color.a);
            }
        }
        CAPTURE(drawn);
        CHECK(motes > 0);
        // Bright and opaque enough to pass the alpha test and show: colours are 0 to 255.
        CHECK(brightest >= 200);
        CHECK(mostOpaque >= 200);
        CHECK(motes <= ElementAura::kMostParticles);
        effects.stop(id);
        CHECK_FALSE(effects.playing(id));
        effects.clear();
    }
    aura.release();
    CHECK(aura.sprite(device) != nullptr); // made anew after a release
    CHECK(device.texturesCreated == 2);
}

} // namespace
