#include <cmath>
#include <filesystem>
#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "game/camera/LegendsCamera.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

formats::LegendsN64CameraData markers() {
    formats::LegendsN64CameraData data;
    data.markers = {{{0, 0, 0}, 2, .2f}, {{0, 0, 0}, 0, .6f}, {{100, 0, 0}, 1, .7f}};
    return data;
}

formats::LegendsN64CameraData tower() {
    auto data = markers();
    data.tower = true;
    data.towerExits = {{90, 2, -52}, {-90, 5, -52}, {-90, 5, 52}, {90, 5, 52}, {0, -5, 104}};
    return data;
}

void settle(LegendsCamera& camera, const Vec3& feet, s32 frames = 300) {
    for (s32 frame = 0; frame < frames; ++frame) {
        camera.update(feet, 1.0f / 30);
    }
}

TEST_CASE("Legends follow markers exclude the opening view and retain native hysteresis",
          "[legends][camera]") {
    LegendsCamera camera;
    camera.reset(markers(), {0, 0, 0});
    CHECK(camera.marker() == 1);
    CHECK(camera.camera().yaw == 0);
    CHECK(camera.camera().pitch == Approx(.6f));
    CHECK(camera.distance() == 36);
    settle(camera, {55, 0, 0});
    CHECK(camera.marker() == 1);
    settle(camera, {90, 0, 0});
    CHECK(camera.marker() == 2);
    CHECK(camera.camera().yaw == Approx(1));
    CHECK(camera.camera().pitch == Approx(.7f));
    CHECK(glm::distance(camera.camera().position,
                        camera.attention() - camera.camera().forward() * camera.distance()) <
          .001f);
}

TEST_CASE("N64 tower camera uses native radial rings and return rays instead of IST angles",
          "[legends][camera]") {
    LegendsCamera camera;
    camera.reset(tower(), {0, 0, -20});
    CHECK(camera.mode() == LegendsCamera::Mode::TowerInner);
    CHECK(camera.camera().pitch == Approx(.05f));
    CHECK(camera.attention().y == 7);
    CHECK(std::hypot(camera.attention().x, camera.attention().z) == Approx(20));
    CHECK(camera.distance() == 55);
    camera.update({0, 0, 46}, 1.0f / 30);
    CHECK(camera.mode() == LegendsCamera::Mode::TowerInner);
    camera.update({0, 0, 47}, 1.0f / 30);
    CHECK(camera.mode() == LegendsCamera::Mode::TowerMiddle);
    settle(camera, {0, 0, 60});
    CHECK(camera.camera().pitch == Approx(.1f));
    CHECK(camera.distance() == 40);
    CHECK(camera.attention().x == Approx(0).margin(.001f));
    camera.update({0, 0, 43}, 1.0f / 30);
    CHECK(camera.mode() == LegendsCamera::Mode::TowerMiddle);
    camera.update({0, 0, 42}, 1.0f / 30);
    CHECK(camera.mode() == LegendsCamera::Mode::TowerInner);
    camera.update({0, 0, 91}, 1.0f / 30);
    CHECK(camera.mode() == LegendsCamera::Mode::TowerOuter);
    camera.update({0, 0, 88}, 1.0f / 30);
    CHECK(camera.mode() == LegendsCamera::Mode::TowerOuter);
    camera.update({0, 0, 87}, 1.0f / 30);
    CHECK(camera.mode() == LegendsCamera::Mode::TowerMiddle);
    settle(camera, {90, 5, -52});
    CHECK(camera.mode() == LegendsCamera::Mode::TowerOuter);
    CHECK(camera.camera().pitch == Approx(.7f));
    CHECK(camera.distance() == 40);
    CHECK(camera.attention().x / camera.attention().z == Approx(-90.0f / 52));
}

TEST_CASE("N64 tower camera uses its own distance limits when entering each radial zone",
          "[legends][camera]") {
    LegendsCamera camera;
    camera.reset(tower(), {0, 0, 60});
    CHECK(camera.mode() == LegendsCamera::Mode::TowerMiddle);
    CHECK(camera.distance() == 40);
    settle(camera, {0, 0, 60});
    CHECK(camera.distance() == 40);

    camera.reset(tower(), {90, 5, -52});
    CHECK(camera.mode() == LegendsCamera::Mode::TowerOuter);
    CHECK(camera.distance() == 40);
    settle(camera, {90, 5, -52});
    CHECK(camera.distance() == 40);

    settle(camera, {0, 0, 20});
    CHECK(camera.mode() == LegendsCamera::Mode::TowerInner);
    CHECK(camera.distance() == 55);
}

TEST_CASE("N64 tower radial transitions use full actor height and stay finite at the origin",
          "[legends][camera]") {
    LegendsCamera camera;
    camera.reset(tower(), {0, 0, 0});
    settle(camera, {0, 0, 0});
    CHECK(std::isfinite(camera.camera().position.x));
    CHECK(std::isfinite(camera.camera().position.y));
    CHECK(std::isfinite(camera.camera().position.z));
    camera.update({0, 48, 0}, 1.0f / 30);
    CHECK(camera.mode() == LegendsCamera::Mode::TowerMiddle);
    const auto before = camera.camera();
    camera.update({std::numeric_limits<f32>::quiet_NaN(), 0, 0}, 1.0f / 30);
    camera.update({0, 0, 0}, std::numeric_limits<f32>::infinity());
    CHECK(camera.camera() == before);
    CHECK_THROWS_AS(camera.reset({}, {0, 0, 0}), FormatError);
}

TEST_CASE("Legends camera native ticks and render samples are independent of render rate",
          "[legends][camera]") {
    LegendsCamera thirty;
    LegendsCamera sixty;
    LegendsCamera oneTwenty;
    thirty.reset(tower(), {0, 0, -20});
    sixty.reset(tower(), {0, 0, -20});
    oneTwenty.reset(tower(), {0, 0, -20});
    for (s32 tick = 0; tick < 180; ++tick) {
        const f32 rotation = static_cast<f32>(tick) * .01f;
        const Vec3 feet{std::sin(rotation) * 25, 0, std::cos(rotation) * 25};
        thirty.update(feet, 1.0f / 30);
        for (s32 i = 0; i < 2; ++i) {
            sixty.update(feet, 1.0f / 60);
        }
        for (s32 i = 0; i < 4; ++i) {
            oneTwenty.update(feet, 1.0f / 120);
        }
        CHECK(sixty.camera() == thirty.camera());
        CHECK(oneTwenty.camera() == thirty.camera());
        const auto native = sixty.camera();
        const auto half = sixty.sample(1.0f / 60);
        CHECK(std::isfinite(half.yaw));
        CHECK(sixty.camera() == native);
        CHECK(sixty.sample(1.0f / 30) == native);
    }
}

TEST_CASE("Native tower entrance and realm arms produce finite moving cameras",
          "[legends][camera][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_directory(root)) {
        SKIP("Native N64 extraction is not installed");
    }
    auto data = formats::decodeLegendsN64Camera(readFile(root / "levels/tower2/ist.rom"), "tower2",
                                                readFile(root / "_program/game.bin"));
    LegendsCamera camera;
    camera.reset(data, {-4.5f, 0, -15.5f}); // tower2's authored IPR entrance
    CHECK(camera.mode() == LegendsCamera::Mode::TowerInner);
    CHECK(camera.camera().pitch == Approx(.05f));
    CHECK(camera.distance() == 55);
    for (const auto& exit : data.towerExits) {
        INFO("Exit " << exit.x << "," << exit.y << "," << exit.z);
        for (s32 step = 0; step < 300; ++step) {
            const Vec3 feet = exit * (static_cast<f32>(step) / 299);
            camera.update(feet, 1.0f / 30);
            const auto render = camera.sample(1.0f / 60);
            CHECK(std::isfinite(render.position.x));
            CHECK(std::isfinite(render.position.y));
            CHECK(std::isfinite(render.position.z));
            CHECK(glm::distance(camera.camera().position, camera.attention()) ==
                  Approx(camera.distance()).margin(.001f));
        }
        CHECK(camera.mode() == LegendsCamera::Mode::TowerOuter);
        settle(camera, exit);
        CHECK(camera.camera().pitch == Approx(.7f));
    }
}
} // namespace
