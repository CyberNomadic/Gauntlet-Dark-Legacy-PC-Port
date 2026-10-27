#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"
#include "engine/world/ParticleSystem.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/effects/EffectTrees.h"
#include "game/effects/ElementAura.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("each element's aura is endless, follows its body and keeps its own manner",
          "[game][effects][aggression][elemental]") {
    CHECK(ElementAura::sprite(0).empty());
    CHECK(ElementAura::sprite(1) == "PIXIE_R");
    CHECK(ElementAura::sprite(2) == "PIXIE_B");
    CHECK(ElementAura::sprite(3) == "PIXIE_Y");
    CHECK(ElementAura::sprite(4) == "PIXIE_G");
    CHECK(ElementAura::sprite(5).empty());
    for (s32 element = 1; element <= ElementAura::kElements; ++element) {
        CAPTURE(element);
        const ParticleDescriptor d = ElementAura::descriptor(element, 1.5f, 9.0f);
        CHECK(d.emitFrames == ParticleDescriptor::kEndless);
        CHECK(d.dynamic);
        CHECK(d.additive);
        CHECK_FALSE(d.depthWrite);
        CHECK(d.volume == Vec3{1.5f, 4.5f, 1.5f});
        CHECK(d.maxParticles == ElementAura::kMostParticles);
        CHECK(d.rate[0] > 0.0f);
        CHECK(d.rate[3] == d.rate[0]);
        CHECK(d.particleLife >= 1);
        CHECK(d.speed > 0.0f);
        CHECK(d.texture == ElementAura::sprite(element));
        CHECK(d.alpha.fadeEnd == 0.0f); // every mote fades to nothing
    }
    // Fire's embers rise and redden; acid's drips fall green; electric's sparks fly every way;
    // light's motes drift up, the largest and slowest.
    const auto fire = ElementAura::descriptor(1, 1.0f, 6.0f);
    const auto electric = ElementAura::descriptor(2, 1.0f, 6.0f);
    const auto light = ElementAura::descriptor(3, 1.0f, 6.0f);
    const auto acid = ElementAura::descriptor(4, 1.0f, 6.0f);
    CHECK(fire.direction.y > 0.0f);
    CHECK(fire.gravity > 0.0f);
    CHECK(fire.red.lifeStart == 1.0f);
    CHECK(fire.green.lifeEnd < fire.green.lifeStart);
    CHECK(acid.direction.y < 0.0f);
    CHECK(acid.gravity < 0.0f);
    CHECK(acid.green.lifeStart > acid.red.lifeStart);
    CHECK(electric.angle == ParticleDescriptor::kSphere);
    CHECK(electric.speed > fire.speed);
    CHECK(electric.particleLife < fire.particleLife);
    CHECK(electric.blue.lifeStart == 1.0f);
    CHECK(light.width.lifeStart > fire.width.lifeStart);
    CHECK(light.speed < fire.speed);
    CHECK(light.particleLife > fire.particleLife);
    // No element, no aura: the plain descriptor, emitting once.
    const auto none = ElementAura::descriptor(0, 1.0f, 6.0f);
    CHECK(none.emitFrames == 1);
    CHECK_FALSE(none.dynamic);
}

TEST_CASE("an aura starts over the weapons archive's pixie sprite and keeps playing",
          "[game][effects][aggression][elemental][assets]") {
    const auto directory = test::assetOrSkip("WEAPONS/animations.json").parent_path();
    test::FakeRenderDevice device;
    ItemArchive weapons;
    REQUIRE(weapons.load(directory));
    for (s32 element = 1; element <= ElementAura::kElements; ++element) {
        CAPTURE(element);
        const auto sprite = weapons.textures.find(ElementAura::sprite(element));
        REQUIRE(sprite.has_value());
        EffectTrees effects;
        const u32 id = effects.startParticles(device, weapons,
                                              ElementAura::descriptor(element, 1.0f, 6.0f),
                                              *sprite, Mat4{1.0f});
        REQUIRE(id != 0);
        for (s32 frame = 0; frame < 60; ++frame) {
            effects.update(1.0f / 30.0f);
        }
        CHECK(effects.playing(id)); // endless
        CHECK(effects.count() == 1);
        effects.stop(id);
        CHECK_FALSE(effects.playing(id));
        effects.clear();
    }
}

} // namespace
