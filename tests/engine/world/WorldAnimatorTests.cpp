#include <cmath>
#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ModelSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/world/WorldAnimator.h"
#include "engine/world/WorldScene.h"

#include "FakeRenderDevice.h"
#include "SampleLevel.h"
#include "TestSupport.h"

namespace {

using namespace gdl;
using Catch::Approx;

constexpr f32 kStep = 1.0f / 30.0f;

struct Fixture {
    test::FakeRenderDevice device;
    ModelSet models;
    TextureSet textures;
    WorldLayout layout;
    WorldScene scene;
    WorldAnimator animator;

    explicit Fixture(std::string_view name) {
        const auto dir = test::sampleLevel(name);
        REQUIRE(models.load(dir));
        REQUIRE(textures.load(dir));
        REQUIRE(layout.load(dir));
        REQUIRE(scene.build(layout, models, textures, device));
    }

    /** Where the blade's first vertex lands when the scene is drawn. */
    Vec3 bladeAt() {
        device.draws.clear();
        scene.draw(device, Mat4{1.0f}, Vec3{10.0f, 0.0f, 0.0f});
        return device.draws[3].vertices[0].position;
    }
};

/** A layout of one keyed object with the given level flags. */
WorldLayout layoutWithFlags(std::string_view name, u32 flags) {
    const auto dir = test::scratchDirectory(name);
    writeTextFile(dir / "world.json", R"({
  "objects": [{"name": "SPIN", "position": [0, 0, 0], "flags": )" +
                                          std::to_string(flags) + R"(}],
  "animations": [{"object": 0, "frames": 4, "state": 257, "start": 0,
                  "track": {"flags": 2, "frames": [0, 3], "values": [0, 1]}}]
})");
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    return layout;
}

TEST_CASE("world animations turn their objects thirty frames a second and loop",
          "[world][animation]") {
    Fixture f("world-animator");
    f.animator.bind(f.layout);
    REQUIRE(f.animator.size() == 1);
    REQUIRE(f.animator.object(0) == 8);
    REQUIRE(f.animator.frame(0) == 0.0f);

    f.animator.apply(f.scene);
    REQUIRE(f.bladeAt() == Vec3{11.0f, 0.0f, 50.0f});
    f.animator.step(kStep, f.scene);
    REQUIRE(f.animator.frame(0) == Approx(1.0f));
    REQUIRE(f.bladeAt() == Vec3{11.0f, 0.0f, 50.0f}); // posed before moving on
    f.animator.step(kStep, f.scene);
    // A third of the way to one radian about y swings the blade off +x.
    const Vec3 blade = f.bladeAt();
    REQUIRE(blade.x == Approx(10.0f + std::cos(1.0f / 3.0f)).margin(1e-4f));
    REQUIRE(blade.z == Approx(50.0f - std::sin(1.0f / 3.0f)).margin(1e-4f));
    f.animator.step(kStep, f.scene);
    REQUIRE(f.animator.frame(0) == 0.0f); // the last frame wraps to the first
    REQUIRE_FALSE(f.animator.finished(0));
    f.animator.step(2.0f * kStep, f.scene);
    REQUIRE(f.animator.frame(0) == Approx(2.0f));

    f.animator.clear();
    REQUIRE(f.animator.size() == 0);
}

TEST_CASE("world animation jumps and loop resets cut the presentation without extra clock steps",
          "[world][animation][presentation]") {
    Fixture f("world-animator-presentation-cuts");
    f.animator.bind(f.layout);
    f.animator.apply(f.scene);
    const auto at = [&](f32 alpha) {
        f.device.draws.clear();
        f.scene.draw(f.device, Mat4{1}, CameraFrame::at(Vec3{0}), alpha);
        REQUIRE(f.device.draws.size() >= 4);
        return f.device.draws[3].vertices[0].position;
    };
    f.scene.capturePresentation();
    f.animator.fire(8, true, true);
    f.animator.apply(f.scene);
    const Vec3 endpoint = f.bladeAt();
    CHECK(at(0.0f) == endpoint);
    CHECK(at(0.5f) == endpoint);
    CHECK(f.animator.frame(0) == 3);
    CHECK(f.animator.finished(0));

    f.scene.capturePresentation();
    f.animator.hold(8);
    f.animator.apply(f.scene);
    const Vec3 start = f.bladeAt();
    CHECK(at(0.0f) == start);
    CHECK(at(0.5f) == start);
    f.animator.cycle(8, true);
    for (s32 i = 0; i < 3; ++i) {
        f.scene.capturePresentation();
        f.animator.step(kStep, f.scene);
    }
    CHECK(f.animator.frame(0) == 0);
    f.scene.capturePresentation();
    f.animator.step(kStep, f.scene);
    CHECK(at(0.0f) == start);
    CHECK(at(0.75f) == start);
    CHECK(f.animator.frame(0) == 1);
}

TEST_CASE("a held switch cycles its track and releases at the forward endpoint",
          "[world][animation]") {
    WorldScene scene;
    WorldAnimator animator;
    animator.bind(layoutWithFlags("world-animator-cycle", 0));
    animator.hold(0);
    animator.cycle(0, true);
    for (s32 i = 0; i < 10; ++i) {
        animator.step(kStep, scene);
    }
    CHECK_FALSE(animator.finished(0));
    animator.cycle(0, false);
    for (s32 i = 0; i < 10; ++i) {
        animator.step(kStep, scene);
    }
    CHECK(animator.finished(0));
    CHECK(animator.frame(0) == 3);
}

TEST_CASE("one-shot animations stop at their last frame, backwards ones at their first",
          "[world][animation]") {
    WorldScene scene;
    WorldAnimator animator;
    animator.bind(layoutWithFlags("world-animator-once", WorldObject::kOnce));
    REQUIRE(animator.size() == 1);
    for (s32 i = 0; i < 10; ++i) {
        animator.step(kStep, scene);
    }
    REQUIRE(animator.frame(0) == 3.0f);
    REQUIRE(animator.finished(0));

    animator.bind(layoutWithFlags("world-animator-reverse", WorldObject::kReverse));
    REQUIRE(animator.frame(0) == 3.0f);
    animator.step(kStep, scene);
    REQUIRE(animator.frame(0) == Approx(2.0f));
    for (s32 i = 0; i < 10; ++i) {
        animator.step(kStep, scene);
    }
    REQUIRE(animator.frame(0) == 0.0f);
    REQUIRE(animator.finished(0));

    // Both flags together switch the animation off.
    animator.bind(
        layoutWithFlags("world-animator-off", WorldObject::kOnce | WorldObject::kReverse));
    REQUIRE(animator.size() == 0);
}

TEST_CASE("pausing looping world tracks does not hold one-shot trigger movement",
          "[world][animation][stop-time]") {
    WorldScene scene;
    WorldAnimator animator;
    animator.bind(layoutWithFlags("world-time-stop", 0));
    animator.step(kStep, scene);
    REQUIRE(animator.frame(0) == Approx(1));
    animator.step(5, scene, true);
    CHECK(animator.frame(0) == Approx(1));
    animator.step(kStep, scene);
    CHECK(animator.frame(0) == Approx(2));
    animator.fire(0, true);
    animator.step(1, scene, true);
    CHECK(animator.finished(0));
    CHECK(animator.frame(0) == Approx(3));
    animator.fire(0, false);
    animator.step(1, scene, true);
    CHECK(animator.finished(0));
    CHECK(animator.frame(0) == Approx(0));
}

TEST_CASE("world track loops retain time at their final-key endpoint across cadences",
          "[world][animation][cadence]") {
    const WorldLayout layout = layoutWithFlags("world-cadence", 0);
    for (const s32 rate : {30, 60, 144, 240}) {
        CAPTURE(rate);
        WorldScene scene;
        WorldAnimator animator;
        animator.bind(layout);
        for (s32 update = 0; update < rate; ++update) {
            animator.step(1.0f / static_cast<f32>(rate), scene);
        }
        REQUIRE(animator.frame(0) == Approx(0.0f).margin(0.0001f));
        // This track has keys 0..3, so its authored period is three 30 Hz intervals.
        animator.step(8.0f / 30.0f, scene);
        REQUIRE(animator.frame(0) == Approx(2.0f).margin(0.0001f));
    }
}

TEST_CASE("world animation checkpoints replay exact poses and cycle boundaries",
          "[world][animation][world-rollback]") {
    for (const s32 mode : {0, 1, 2, 3, 4}) {
        for (const s32 rate : {30, 60, 120}) {
            CAPTURE(mode, rate);
            Fixture host("world-checkpoint-host");
            Fixture replay("world-checkpoint-replay");
            host.animator.bind(host.layout);
            replay.animator.bind(replay.layout);
            if (mode == 1) {
                host.animator.fire(8, true);
            } else if (mode == 2) {
                host.animator.fire(8, true, true);
                host.animator.fire(8, false);
            } else if (mode == 3) {
                host.animator.hold(8);
            } else if (mode == 4) {
                host.animator.fire(8, true, true);
            }
            const f32 seconds = 1.0f / static_cast<f32>(rate);
            host.animator.step(seconds, host.scene);
            const auto saved = host.animator.checkpoint();
            REQUIRE(saved.size() == 1);
            REQUIRE(saved.front().valid());
            REQUIRE(replay.animator.restore(saved));
            CHECK(replay.animator.cycleEvents().empty());
            for (s32 tick = 0; tick < 30; ++tick) {
                const bool paused = tick >= 4 && tick < 8;
                host.scene.capturePresentation();
                replay.scene.capturePresentation();
                host.animator.step(seconds, host.scene, paused);
                replay.animator.step(seconds, replay.scene, paused);
                CHECK(replay.animator.checkpoint() == host.animator.checkpoint());
                CHECK(replay.bladeAt() == host.bladeAt());
                const auto expected = host.animator.cycleEvents();
                const auto actual = replay.animator.cycleEvents();
                REQUIRE(actual.size() == expected.size());
                for (usize event = 0; event < expected.size(); ++event) {
                    CHECK(actual[event].object == expected[event].object);
                    CHECK(actual[event].wrapped == expected[event].wrapped);
                }
            }
            REQUIRE(replay.animator.restore(saved));
            CHECK(replay.animator.checkpoint() == saved);
            CHECK(replay.animator.cycleEvents().empty());
        }
    }
}

TEST_CASE("world animation cursor restoration validates the whole bound roster before mutation",
          "[world][animation][world-rollback]") {
    Fixture fixture("world-checkpoint-reject");
    fixture.animator.bind(fixture.layout);
    fixture.animator.step(kStep, fixture.scene);
    const auto saved = fixture.animator.checkpoint();
    const auto eventCount = fixture.animator.cycleEvents().size();
    const auto reject = [&](const auto& changed) {
        CHECK_FALSE(fixture.animator.restore(changed));
        CHECK(fixture.animator.checkpoint() == saved);
        CHECK(fixture.animator.cycleEvents().size() == eventCount);
    };
    reject(std::vector<WorldAnimator::Cursor>{});
    auto changed = saved;
    changed.push_back(changed.front());
    reject(changed);
    for (const f32 invalid : {-1.0f, 4.0f, std::numeric_limits<f32>::infinity(),
                              std::numeric_limits<f32>::quiet_NaN()}) {
        changed = saved;
        changed.front().frame = invalid;
        reject(changed);
    }
    changed = saved;
    ++changed.front().object;
    reject(changed);
    changed = saved;
    ++changed.front().frames;
    reject(changed);
    changed = saved;
    changed.front().finished = true;
    reject(changed);
    changed = saved;
    changed.front().held = true;
    reject(changed);
    REQUIRE(fixture.animator.restore(saved));
    CHECK(fixture.animator.cycleEvents().empty());
}
} // namespace
