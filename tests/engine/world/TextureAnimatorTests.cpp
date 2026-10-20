#include <array>
#include <cmath>
#include <limits>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/ModelSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/world/TextureAnimator.h"
#include "engine/world/WorldScene.h"

#include "FakeRenderDevice.h"
#include "SampleLevel.h"

namespace {

using namespace gdl;
using Catch::Approx;

TextureAnimationInfo cycle(std::string_view name, s32 texture, s32 source, s32 frames, s32 start,
                           s32 rate, std::string_view frameName = "") {
    TextureAnimationInfo info;
    info.name = std::string(name);
    info.frameName = std::string(frameName);
    info.texture = texture;
    info.source = source;
    info.frames = frames;
    info.start = start;
    info.rate = rate;
    return info;
}

struct Fixture {
    test::FakeRenderDevice device;
    ModelSet models;
    TextureSet textures;
    TextureSet lender;
    WorldLayout layout;
    WorldScene scene;
    TextureAnimator animator;
    std::array<TextureSet*, 1> lenders{&lender};

    explicit Fixture(std::string_view name) {
        const auto dir = test::sampleLevel(name);
        REQUIRE(models.load(dir));
        REQUIRE(textures.load(dir));
        REQUIRE(layout.load(dir));
        REQUIRE(lender.load(test::sampleLender(std::string(name) + "-lender")));
        REQUIRE(scene.build(layout, models, textures, device, WorldLighting{}, lenders));
    }
};

struct DigestTextures {
    std::vector<std::pair<const Texture*, u32>> slots;

    explicit DigestTextures(Fixture& fixture) {
        for (u32 slot = 0; slot < 3; ++slot) {
            slots.emplace_back(&fixture.textures.texture(fixture.device, slot), slot);
        }
        for (u32 slot = 1; slot < 3; ++slot) {
            slots.emplace_back(&fixture.lender.texture(fixture.device, slot), 100 + slot);
        }
    }
    NativeStateDigest::Resources resources() {
        return {this, [](NativeStateDigest& digest, std::type_index type, const void* pointer,
                         void* context) {
                    if (type != typeid(Texture)) {
                        return false;
                    }
                    const auto& bindings = *static_cast<DigestTextures*>(context);
                    for (const auto& [texture, identity] : bindings.slots) {
                        if (texture == pointer) {
                            digest.word(identity);
                            return true;
                        }
                    }
                    return false;
                }};
    }
};

std::optional<NativeStateDigest::Digest>
bindingDigest(const TextureAnimator& animator, NativeStateDigest::Resources resources = {},
              std::optional<TextureAnimator::Clock> clock = {}) {
    NativeStateDigest digest(resources);
    animator.appendBindingDigest(digest);
    if (clock) {
        digest.fields<3>(*clock);
    }
    return digest.checkedFinish();
}

TEST_CASE("texture animations cycle frames and slide coordinates once a game frame",
          "[world][animation]") {
    Fixture f("texture-animator");
    const std::vector<TextureAnimationInfo> animations{
        // The torch shows the lender's two flame frames every second game frame, from the
        // second; the stone shows the set's next two entries; the glass scrolls back along
        // u over four steps.
        cycle("TORCHB", 3, TextureAnimationInfo::kByName, 2, 1, 2, "TORCH00"),
        cycle("STONE", 0, 1, 2, 0, 1),
        cycle("GLASS", 1, TextureAnimationInfo::kScrollU, -4, 0, 0),
        cycle("LOST", 0, TextureAnimationInfo::kByName, 3, 0, 1, "NOPE"),
        cycle("NOWHERE", -1, 1, 2, 0, 1),
        cycle("STILL", 0, 1, 0, 0, 1),
    };
    f.animator.bind(animations, f.textures, f.device, f.lenders);
    REQUIRE(f.animator.size() == 3);
    REQUIRE(f.animator.slot(0) == 3);
    REQUIRE(f.animator.counter(0) == 1);
    REQUIRE(f.animator.frame() == 0);

    CHECK(f.animator.cycleFrame(0, 0) == &f.lender.texture(f.device, 1));
    CHECK(f.animator.cycleFrame(0, 1) == &f.lender.texture(f.device, 2));
    CHECK(f.animator.cycleFrame(0, 2) == nullptr);
    CHECK(f.animator.cycleFrame(2, 0) == nullptr); // Scrolls have no cycle frames.
    CHECK(f.animator.cycleFrame(3, 0) == nullptr);
    CHECK(f.animator.counter(0) == 1); // Sampling another frame cannot advance the clock.

    f.animator.apply(f.scene);
    REQUIRE(f.scene.textureOf(3) == &f.lender.texture(f.device, 2));
    REQUIRE(f.scene.textureOf(0) == &f.textures.texture(f.device, 1));
    REQUIRE(f.scene.textureOffset(1) == Vec2{0.0f, 0.0f});

    f.animator.step(f.scene);
    REQUIRE(f.animator.frame() == 1);
    REQUIRE(f.scene.textureOf(3) == &f.lender.texture(f.device, 2)); // not its frame yet
    REQUIRE(f.scene.textureOf(0) == &f.textures.texture(f.device, 2));
    REQUIRE(f.scene.textureOffset(1) == Vec2{-0.25f, 0.0f});

    f.animator.step(f.scene);
    REQUIRE(f.scene.textureOf(3) == &f.lender.texture(f.device, 1));
    REQUIRE(f.animator.counter(0) == 0);
    REQUIRE(f.scene.textureOf(0) == &f.textures.texture(f.device, 1));
    REQUIRE(f.scene.textureOffset(1) == Vec2{-0.5f, 0.0f});
    // The same stands can be read as motions, and stepped without a scene.
    REQUIRE(f.animator.motion(0).slot == 3);
    REQUIRE(f.animator.motion(0).frame == &f.lender.texture(f.device, 1));
    REQUIRE(f.animator.motion(0).offset == Vec2{0.0f, 0.0f});
    bool scrolled = false;
    for (usize i = 0; i < f.animator.size(); ++i) {
        const TextureMotion motion = f.animator.motion(i);
        if (motion.slot == 1) {
            scrolled = true;
            REQUIRE(motion.frame == nullptr);
            REQUIRE(motion.offset == Vec2{-0.5f, 0.0f});
        }
    }
    REQUIRE(scrolled);

    f.animator.step(f.scene, 2);
    REQUIRE(f.animator.frame() == 4);
    REQUIRE(f.scene.textureOffset(1) == Vec2{0.0f, 0.0f});
    REQUIRE(f.scene.textureOf(3) == &f.lender.texture(f.device, 2));

    // Without a scene the animations still move on, to be shown elsewhere.
    f.animator.step(2);
    REQUIRE(f.animator.frame() == 6);
    REQUIRE(f.animator.motion(0).frame == &f.lender.texture(f.device, 1));
    f.animator.clear();
    REQUIRE(f.animator.size() == 0);
    REQUIRE(f.animator.frame() == 0);
    CHECK(f.animator.cycleFrame(0, 0) == nullptr);
}

TEST_CASE("an external animation frame name resolves to its owning lender",
          "[world][animation][texture-lending]") {
    Fixture f("texture-animator-external-name");
    // The level has a TORCHB reference, but only the lender owns its pixels.
    REQUIRE(f.textures.entry(*f.textures.find("TORCHB")).external());
    const auto owned = f.lender.find("TORCHB");
    REQUIRE(owned.has_value());
    REQUIRE_FALSE(f.lender.entry(*owned).external());
    const std::array animations{
        cycle("TORCHB", 3, TextureAnimationInfo::kByName, 1, 0, 1, "TORCHB")};
    const std::array<TextureSet*, 3> lenders{nullptr, &f.textures, &f.lender};
    f.animator.bind(animations, f.textures, f.device, lenders);
    REQUIRE(f.animator.size() == 1);
    REQUIRE(f.animator.motion(0).frame == &f.lender.texture(f.device, *owned));
}

TEST_CASE("rain composes both scrolling coordinates without accumulating old offsets",
          "[world][animation]") {
    Fixture f("texture-animator-two-axis");
    const std::array animations{cycle("RAIN", 1, TextureAnimationInfo::kScrollV, -10, 0, 0),
                                cycle("RAIN", 1, TextureAnimationInfo::kScrollU, -300, 0, 0)};
    f.animator.bind(animations, f.textures, f.device);
    f.animator.step(f.scene, 3);
    CHECK(f.scene.textureOffset(1).x == Approx(-0.01f));
    CHECK(f.scene.textureOffset(1).y == Approx(-0.3f));
    f.animator.apply(f.scene);
    CHECK(f.scene.textureOffset(1).y == Approx(-0.3f));
    f.animator.step(2);
    f.animator.apply(f.scene);
    CHECK(f.scene.textureOffset(1).x == Approx(-5.0f / 300.0f));
    CHECK(f.scene.textureOffset(1).y == Approx(-0.5f));
    f.animator.step(f.scene, 5);
    CHECK(f.scene.textureOffset(1).y == 0);
    CHECK(f.scene.textureOffset(1).x == Approx(-10.0f / 300.0f));
}

TEST_CASE("scroll presentation samples both axes across wraps without stepping native clocks",
          "[world][animation][presentation][rain]") {
    Fixture f("texture-animator-scroll-presentation");
    const std::array animations{
        cycle("RAIN", 1, TextureAnimationInfo::kScrollV, -10, 0, 0),
        cycle("RAIN", 1, TextureAnimationInfo::kScrollU, -300, 0, 0),
        cycle("SLOW", 0, TextureAnimationInfo::kScrollU, 4, 0, 3),
        cycle("TORCHB", 3, TextureAnimationInfo::kByName, 2, 0, 2, "TORCH00")};
    f.animator.bind(animations, f.textures, f.device, f.lenders);
    f.animator.step(f.scene, 10);
    const Texture* rain = f.scene.textureOf(1);
    const Texture* slow = f.scene.textureOf(0);
    const Texture* torch = f.scene.textureOf(3);
    REQUIRE(rain != nullptr);
    const auto sample = [&](f32 offset) {
        f.device.draws.clear();
        f.scene.draw(f.device, Mat4{1}, CameraFrame{}, 0.5f, offset);
        usize rainDraws = 0;
        usize slowDraws = 0;
        for (const auto& draw : f.device.draws) {
            if (draw.texture == rain) {
                ++rainDraws;
                CHECK(draw.state.uvOffset.x == Approx(-(10 + offset) / 300));
                CHECK(draw.state.uvOffset.y == Approx(-glm::fract((10 + offset) / 10)));
            }
            if (draw.texture == slow) {
                ++slowDraws;
                CHECK(draw.state.uvOffset.x == Approx((10 + offset) / 12));
            }
        }
        CHECK(rainDraws > 0);
        CHECK(slowDraws > 1); // both a static batch and an individually placed unit
        CHECK(f.animator.frame() == 10);
        CHECK(f.animator.counter(0) == 0);
        CHECK(f.scene.textureOffset(1).y == 0);
        CHECK(f.scene.textureOf(3) == torch); // flipbooks keep their authored cadence
    };
    for (const f32 offset : {-0.5f, -0.375f, -0.25f, -0.125f, 0.0f, 0.25f, -0.5f}) {
        CAPTURE(offset);
        sample(offset);
    }
}

TEST_CASE("an animation keyed to a sequence is never stepped, but read off at a frame",
          "[world][animation]") {
    Fixture f("texture-animator");
    // A stone cycle of two frames from the sequence's fourth frame, a frame each two; and a
    // glass scroll that eases in over four frames from the sixth and runs eight.
    TextureAnimationInfo keyedCycle = cycle("STONE", 0, 1, 2, 0, 2);
    keyedCycle.flag = 2;
    keyedCycle.offset = 4;
    TextureAnimationInfo keyedScroll = cycle("GLASS", 1, TextureAnimationInfo::kScrollU, 8, 0, 4);
    keyedScroll.flag = 7;
    keyedScroll.offset = 6;
    const std::vector<TextureAnimationInfo> animations{
        cycle("TORCHB", 3, TextureAnimationInfo::kByName, 2, 1, 2, "TORCH00"), keyedCycle,
        keyedScroll};
    f.animator.bind(animations, f.textures, f.device, f.lenders);
    REQUIRE(f.animator.size() == 3);
    REQUIRE_FALSE(f.animator.keyed(0));
    REQUIRE(f.animator.keyed(1));
    REQUIRE(f.animator.keyed(2));
    // Stepping and applying leave the keyed ones alone.
    const Texture* own = f.scene.textureOf(0);
    f.animator.step(f.scene, 4);
    f.animator.apply(f.scene);
    REQUIRE(f.animator.counter(1) == 0);
    REQUIRE(f.scene.textureOf(0) == own);
    REQUIRE(f.scene.textureOffset(1) == Vec2{0.0f, 0.0f});
    // Before its first frame the cycle shows its first, then a frame each two, and holds
    // its last.
    REQUIRE(f.animator.motionAt(1, 0)->frame == &f.textures.texture(f.device, 1));
    REQUIRE(f.animator.motionAt(1, 4)->frame == &f.textures.texture(f.device, 1));
    REQUIRE(f.animator.motionAt(1, 6)->frame == &f.textures.texture(f.device, 2));
    REQUIRE(f.animator.motionAt(1, 40)->frame == &f.textures.texture(f.device, 2));
    REQUIRE(f.animator.motionAt(1, 6)->slot == 0);
    // The scroll: still before its frame, easing over its rate, steady to its end, then held.
    REQUIRE(f.animator.motionAt(2, 6)->offset == Vec2{0.0f, 0.0f});
    REQUIRE(f.animator.motionAt(2, 6)->frame == nullptr);
    REQUIRE(f.animator.motionAt(2, 8)->offset.x < 0.0f);
    REQUIRE(f.animator.motionAt(2, 8)->offset.y == 0.0f);
    REQUIRE(f.animator.motionAt(2, 6 + 10)->offset.x == Approx(0.5f));
    REQUIRE(f.animator.motionAt(2, 6 + 10)->offset.x == TextureAnimator::scrollAt(10, 4, 8));
    REQUIRE(f.animator.motionAt(2, 100)->offset.x == 1.0f);
    // Nothing for what was not bound.
    REQUIRE_FALSE(f.animator.motionAt(-1, 3).has_value());
    REQUIRE_FALSE(f.animator.motionAt(9, 3).has_value());
    // Without a rate the scroll runs a step a frame round its cycle.
    REQUIRE(TextureAnimator::scrollAt(3, 0, 8) == Approx(3.0f / 8.0f));
    REQUIRE(TextureAnimator::scrollAt(11, 0, 8) == Approx(3.0f / 8.0f));
    // The stretch it puts on the coordinate it slides: nothing of the picture before it
    // starts, growing as it eases in, twice (its frames over its rate) from its end, and
    // always what it reaches less the slide; the other coordinate is left alone.
    REQUIRE(TextureAnimator::scrollStateAt(0, 4, 8).scale == 0.0f);
    REQUIRE(f.animator.motionAt(2, 6)->scale == Vec2{0.0f, 1.0f});
    REQUIRE(f.animator.motionAt(2, 0)->scale == Vec2{0.0f, 1.0f});
    const ScrollState easing = TextureAnimator::scrollStateAt(2, 4, 8);
    REQUIRE(easing.scale == Approx(1.0f - easing.along));
    REQUIRE(easing.scale > 1.0f);
    const ScrollState steady = TextureAnimator::scrollStateAt(6, 4, 8);
    REQUIRE(steady.scale == Approx(1.5f - steady.along));
    REQUIRE(TextureAnimator::scrollStateAt(9, 4, 8).scale == Approx(2.0f - 0.25f));
    REQUIRE(TextureAnimator::scrollStateAt(100, 4, 8).scale == Approx(1.0f));
    REQUIRE(f.animator.motionAt(2, 100)->scale == Vec2{1.0f, 1.0f});
    REQUIRE(TextureAnimator::scrollStateAt(3, 0, 8).scale == 0.0f);
    // A rate past the frames holds still, stretched by one.
    REQUIRE(TextureAnimator::scrollStateAt(10, 9, 8).along == 0.0f);
    REQUIRE(TextureAnimator::scrollStateAt(10, 9, 8).scale == 1.0f);
}

TEST_CASE("keyed subtree fades clamp at their offset and end without resolving a texture",
          "[world][animation]") {
    Fixture f("texture-animator-fades");
    auto fade = cycle("OVERLAY", -1, TextureAnimationInfo::kFadeOut, 20, 0, 0);
    fade.flag = 2;
    fade.offset = 18;
    auto appear = fade;
    appear.source = TextureAnimationInfo::kFadeIn;
    auto empty = appear;
    empty.frames = 0;
    const std::array animations{fade, appear, empty};
    f.animator.bind(animations, f.textures, f.device);
    REQUIRE(f.animator.size() == 3);
    REQUIRE(f.animator.motionAt(0, 0)->alpha == 1.0f);
    REQUIRE(f.animator.motionAt(0, 18)->alpha == 1.0f);
    REQUIRE(*f.animator.motionAt(0, 28)->alpha == Approx(128.0f / 255));
    REQUIRE(f.animator.motionAt(0, 38)->alpha == 0.0f);
    REQUIRE(f.animator.motionAt(0, 500)->alpha == 0.0f);
    REQUIRE(f.animator.motionAt(1, 0)->alpha == 0.0f);
    REQUIRE(*f.animator.motionAt(1, 28)->alpha == Approx(128.0f / 255));
    REQUIRE(f.animator.motionAt(1, 38)->alpha == 1.0f);
    REQUIRE(f.animator.motionAt(2, 500)->alpha == 0.0f);
    f.animator.step(f.scene, 100);
    REQUIRE(f.animator.counter(0) == 0);
    REQUIRE(f.animator.motionAt(0, 18)->alpha == 1.0f);
}

TEST_CASE("fractional texture sampling preserves native clocks and signed scroll wraps",
          "[world][animation][texture-presentation]") {
    Fixture f("texture-render-clock");
    auto keyed = cycle("KEY", 0, TextureAnimationInfo::kScrollU, 8, 0, 4);
    keyed.flag = 0;
    keyed.offset = 2;
    const std::array animations{cycle("RAIN", 1, TextureAnimationInfo::kScrollV, -4, 0, 0),
                                cycle("CYCLE", 0, 0, 2, 0, 2), keyed};
    for (const s32 fps : {30, 60, 120, 144}) {
        f.animator.bind(animations, f.textures, f.device);
        s32 ticks = 0;
        for (s32 render = 1; render <= fps; ++render) {
            const f64 time = static_cast<f64>(render) / static_cast<f64>(fps);
            const auto desired = static_cast<s32>(std::floor(time * 60.0 + 0.000001));
            while (ticks < desired) {
                f.animator.advance(1.0f / 60.0f);
                ++ticks;
            }
            const auto alpha = static_cast<f32>(time * 60.0 - static_cast<f64>(ticks));
            const auto offset = f.animator.presentationOffset(alpha);
            const auto frame = f.animator.frame();
            const f32 sampleFrame = ticks == 0 ? 0.0f : static_cast<f32>(time * 30.0 - 0.5);
            const auto scroll = f.animator.motion(0, offset);
            CHECK(scroll.offset.y == Approx(-glm::fract(sampleFrame / 4)).margin(0.00001));
            const auto cycleFrame = f.animator.motion(1, offset);
            const f32 sample = glm::fract(sampleFrame / 4) * 2;
            CHECK(cycleFrame.frame == &f.textures.texture(f.device, static_cast<u32>(sample)));
            CHECK(cycleFrame.frameBlend == Approx(glm::fract(sample)).margin(0.00001));
            CHECK(f.animator.frame() == frame);
        }
        CHECK(f.animator.frame() == 30);
        CHECK(f.animator.counter(0) == 2);
    }
    const auto before = f.animator.motion(0);
    f.animator.advance(0);
    CHECK(f.animator.presentationOffset(0) == f.animator.presentationOffset(1));
    CHECK_FALSE(f.animator.presentationOffset(-1));
    CHECK(f.animator.motion(0).offset == before.offset);
    const auto start = f.animator.motionAt(2, 2.0f);
    const auto half = f.animator.motionAt(2, 2.5f);
    REQUIRE(start);
    REQUIRE(half);
    CHECK(start->scale.x == 0);
    CHECK(half->offset.x == Approx(-3.625f));
    CHECK(half->scale.x == Approx(4.625f));
}

TEST_CASE("a cycle ends where its frames cannot be read", "[world][animation]") {
    Fixture f("texture-animator-short");
    const std::vector<TextureAnimationInfo> animations{cycle("STONE", 0, 1, 5, 0, 1)};
    f.animator.bind(animations, f.textures, f.device, f.lenders);
    REQUIRE(f.animator.size() == 1);
    // Entries 1 and 2 follow the source and the external 3 has no image, so the cycle is
    // two long.
    f.animator.step(f.scene, 2);
    REQUIRE(f.animator.counter(0) == 0);
    REQUIRE(f.scene.textureOf(0) == &f.textures.texture(f.device, 1));
    f.animator.step(f.scene);
    REQUIRE(f.scene.textureOf(0) == &f.textures.texture(f.device, 2));
}

TEST_CASE("local texture clocks rewind fractional cycles and scrolling without rebinding",
          "[world][animation][local-rewind]") {
    Fixture f("texture-local-clock");
    const std::vector<TextureAnimationInfo> animations{
        cycle("TORCHB", 3, TextureAnimationInfo::kByName, 2, 1, 2, "TORCH00"),
        cycle("STONE", 0, 1, 2, 0, 1),
        cycle("GLASS", 1, TextureAnimationInfo::kScrollU, -4, 2, 0),
    };
    f.animator.bind(animations, f.textures, f.device, f.lenders);
    REQUIRE(f.animator.size() == 3);
    const auto uploads = f.device.texturesCreated;
    f.animator.advance(1.0f / 60);
    const auto saved = f.animator.clock();
    std::array<TextureAnimator::Clock, 8> expected;
    std::array<std::array<s32, 3>, 8> counters{};
    for (usize tick = 0; tick < expected.size(); ++tick) {
        f.animator.advance(1.0f / 60);
        expected[tick] = f.animator.clock();
        for (usize entry = 0; entry < counters[tick].size(); ++entry) {
            counters[tick][entry] = f.animator.counter(entry);
        }
    }
    for (s32 repeat = 0; repeat < 2; ++repeat) {
        f.animator.restoreClock(saved);
        CHECK(f.animator.clock() == saved);
        for (usize tick = 0; tick < expected.size(); ++tick) {
            f.animator.advance(1.0f / 60);
            CHECK(f.animator.clock() == expected[tick]);
            for (usize entry = 0; entry < counters[tick].size(); ++entry) {
                CHECK(f.animator.counter(entry) == counters[tick][entry]);
            }
        }
    }
    f.animator.restoreClock({});
    CHECK(f.animator.counter(0) == 1);
    CHECK(f.animator.counter(1) == 0);
    CHECK(f.animator.counter(2) == 2);
    CHECK(f.device.texturesCreated == uploads);
}

TEST_CASE("texture rewind preserves signed and out of range native start counters",
          "[world][animation][local-rewind][texture-signed-start]") {
    Fixture f("texture-signed-start");
    for (const s32 source : {TextureAnimationInfo::kScrollU, TextureAnimationInfo::kScrollV, 0}) {
        for (const s32 start : {-9, -1, 0, 3, 4, 9}) {
            for (const s32 rate : {-2, 0, 1, 3}) {
                for (const bool keyed : {false, true}) {
                    CAPTURE(source, start, rate, keyed);
                    auto info = cycle("STONE", 0, source, source >= 0 ? 2 : 4, start, rate);
                    info.flag = keyed ? 0 : TextureAnimationInfo::kFreeRunning;
                    const std::array animations{info};
                    TextureAnimator reference;
                    TextureAnimator replay;
                    reference.bind(animations, f.textures, f.device);
                    replay.bind(animations, f.textures, f.device);
                    REQUIRE(reference.size() == 1);
                    REQUIRE(replay.size() == 1);
                    std::array<s32, 48> counters{};
                    std::array<Vec2, 48> offsets{};
                    std::array<TextureAnimator::Clock, 48> clocks;
                    for (usize frame = 0; frame < counters.size(); ++frame) {
                        counters[frame] = reference.counter(0);
                        clocks[frame] = reference.clock();
                        if (source < 0) {
                            offsets[frame] = reference.motion(0).offset;
                        }
                        reference.step(); // Independent oracle: unchanged native increment.
                    }
                    for (const u32 frame : {0U, 1U, 2U, 3U, 8U, 9U, 40U, 19U, 0U, 39U, 4U, 1U}) {
                        CAPTURE(frame);
                        replay.seek(frame);
                        for (u32 follow = 0; follow < 4; ++follow) {
                            const usize index = frame + follow;
                            CHECK(replay.counter(0) == counters[index]);
                            CHECK(replay.clock() == clocks[index]);
                            if (source < 0) {
                                CHECK(replay.motion(0).offset == offsets[index]);
                            }
                            replay.step();
                        }
                        replay.restoreClock(clocks[frame]);
                        CHECK(replay.counter(0) == counters[frame]);
                        if (source < 0) {
                            CHECK(replay.motion(0).offset == offsets[frame]);
                        }
                    }
                }
            }
        }
    }

    const std::array negative{cycle("GLASS", 1, TextureAnimationInfo::kScrollU, 4, -1, 1)};
    f.animator.bind(negative, f.textures, f.device);
    const auto birth = f.animator.clock();
    CHECK(f.animator.motion(0).offset.x == Approx(-0.25f));
    f.animator.step();
    CHECK(f.animator.counter(0) == 0);
    f.animator.restoreClock(birth);
    CHECK(f.animator.counter(0) == -1);
    CHECK(f.animator.motion(0).offset.x == Approx(-0.25f));
}

TEST_CASE("texture binding fingerprints retain authored values and resolve current resources",
          "[world][animation][native-immutable-digest][texture-binding-digest]") {
    Fixture first("texture-binding-digest-first");
    Fixture second("texture-binding-digest-second");
    DigestTextures firstTextures(first);
    DigestTextures secondTextures(second);
    std::vector<TextureAnimationInfo> definitions{
        cycle("STONE", 0, 1, 2, 0, 2),
        cycle("TORCHB", 3, TextureAnimationInfo::kByName, 2, 1, 2, "TORCH00"),
        cycle("GLASS", 1, TextureAnimationInfo::kScrollU, -4, -1, 0),
        cycle("LOST", 0, TextureAnimationInfo::kByName, 3, 0, 1, "NOPE"),
    };
    const auto independentDefinitions = definitions;
    REQUIRE(definitions.data() != independentDefinitions.data());
    first.animator.bind(definitions, first.textures, first.device, first.lenders);
    second.animator.bind(independentDefinitions, second.textures, second.device, second.lenders);
    REQUIRE(first.animator.size() == 3);
    REQUIRE(first.animator.cycleFrame(0, 0) != second.animator.cycleFrame(0, 0));
    const auto initial = bindingDigest(first.animator, firstTextures.resources());
    REQUIRE(initial);
    CHECK(bindingDigest(second.animator, secondTextures.resources()) == initial);
    CHECK(bindingDigest(first.animator, firstTextures.resources()) == initial);

    // A warmed scalar fingerprint does not cache pointer lookup success or the
    // semantic IDs supplied by a particular stage registry.
    CHECK_FALSE(bindingDigest(first.animator));
    auto changedResources = firstTextures;
    changedResources.slots[1].second += 1000;
    REQUIRE(bindingDigest(first.animator, changedResources.resources()));
    CHECK(bindingDigest(first.animator, changedResources.resources()) != initial);
    changedResources = firstTextures;
    changedResources.slots.erase(changedResources.slots.begin() + 1);
    CHECK_FALSE(bindingDigest(first.animator, changedResources.resources()));
    CHECK(bindingDigest(first.animator, firstTextures.resources()) == initial);

    // The caller's input vector is not the animator's immutable authored bank.
    definitions[0].name = "CHANGED";
    CHECK(bindingDigest(first.animator, firstTextures.resources()) == initial);
    for (u32 field = 0; field < 9; ++field) {
        CAPTURE(field);
        definitions = independentDefinitions;
        auto& value = definitions[0];
        switch (field) {
        case 0: value.name = "OTHER"; break;
        case 1: value.frameName = "AUTHORED"; break;
        case 2: value.texture = 1; break;
        case 3: value.source = 0; break;
        case 4: value.frames = 1; break;
        case 5: value.start = 1; break;
        case 6: value.rate = 3; break;
        case 7: value.offset = 4; break;
        case 8: value.flag = 0; break;
        default: FAIL("unexpected authored field");
        }
        first.animator.bind(definitions, first.textures, first.device, first.lenders);
        REQUIRE(bindingDigest(first.animator, firstTextures.resources()));
        CHECK(bindingDigest(first.animator, firstTextures.resources()) != initial);
        first.animator.bind(independentDefinitions, first.textures, first.device, first.lenders);
        CHECK(bindingDigest(first.animator, firstTextures.resources()) == initial);
    }
    // A dropped unresolved record is still authored binding data, not omitted
    // merely because no drawable entry was created for it.
    definitions = independentDefinitions;
    definitions.back().name = "OTHER_MISSING";
    first.animator.bind(definitions, first.textures, first.device, first.lenders);
    REQUIRE(first.animator.size() == 3);
    CHECK(bindingDigest(first.animator, firstTextures.resources()) != initial);

    first.animator.clear();
    const TextureAnimator empty;
    REQUIRE(bindingDigest(first.animator));
    CHECK(bindingDigest(first.animator) == bindingDigest(empty));
    CHECK(bindingDigest(first.animator) != initial);
    first.animator.bind(independentDefinitions, first.textures, first.device, first.lenders);
    CHECK(bindingDigest(first.animator, firstTextures.resources()) == initial);
}

TEST_CASE("texture binding cache never substitutes for mutable clocks or counter validation",
          "[world][animation][local-rewind][native-immutable-digest][texture-binding-digest]") {
    Fixture fixture("texture-binding-clock");
    DigestTextures textures(fixture);
    const std::array definitions{
        cycle("STONE", 0, 1, 2, 0, 2),
        cycle("GLASS", 1, TextureAnimationInfo::kScrollU, 5, -1, 0),
    };
    fixture.animator.bind(definitions, fixture.textures, fixture.device);
    const auto immutable = bindingDigest(fixture.animator, textures.resources());
    REQUIRE(immutable);
    fixture.animator.advance(1.0f / 60);
    const auto saved = fixture.animator.clock();
    const auto captured = bindingDigest(fixture.animator, textures.resources(), saved);
    REQUIRE(captured);
    std::array<NativeStateDigest::Digest, 8> expected{};
    for (auto& tick : expected) {
        fixture.animator.advance(1.0f / 60);
        const auto next =
            bindingDigest(fixture.animator, textures.resources(), fixture.animator.clock());
        REQUIRE(next);
        tick = *next;
        CHECK(bindingDigest(fixture.animator, textures.resources()) == immutable);
        CHECK(bindingDigest(fixture.animator, textures.resources(), saved) == captured);
    }
    const auto uploads = fixture.device.texturesCreated;
    for (usize replay = 1; replay <= expected.size(); ++replay) {
        fixture.animator.advance(0.25f);
        CHECK(bindingDigest(fixture.animator, textures.resources(), fixture.animator.clock()) !=
              captured);
        fixture.animator.restoreClock(saved);
        CHECK(bindingDigest(fixture.animator, textures.resources(), fixture.animator.clock()) ==
              captured);
        for (usize tick = 0; tick < replay; ++tick) {
            fixture.animator.advance(1.0f / 60);
        }
        CHECK(bindingDigest(fixture.animator, textures.resources(), fixture.animator.clock()) ==
              expected[replay - 1]);
        for (const f32 alpha : {0.0f, 0.5f, 1.0f}) {
            fixture.animator.motion(0, fixture.animator.presentationOffset(alpha));
            CHECK(bindingDigest(fixture.animator, textures.resources()) == immutable);
        }
    }
    CHECK(fixture.device.texturesCreated == uploads);

    // Absolute seeks can reach the u32 boundary even though admitted gameplay
    // clocks cannot. Wrapping the unchanged native increment contradicts the
    // original seed; the warmed binding cache must still reject that phase.
    const std::array wrap{cycle("GLASS", 1, TextureAnimationInfo::kScrollU, 5, 1, 0)};
    fixture.animator.bind(wrap, fixture.textures, fixture.device);
    fixture.animator.seek(std::numeric_limits<u32>::max());
    REQUIRE(bindingDigest(fixture.animator));
    fixture.animator.step();
    CHECK(fixture.animator.frame() == 0);
    CHECK(fixture.animator.counter(0) == 2);
    CHECK_FALSE(bindingDigest(fixture.animator));
    fixture.animator.seek(0);
    CHECK(bindingDigest(fixture.animator));
}
} // namespace
