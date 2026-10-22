#include <array>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/SoundSet.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/party/PortalDeparture.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("native departure history can undo lazy start and replay its skin without rebinding",
          "[local-rewind][portal-departure-local-rewind]") {
    test::FakeRenderDevice device;
    TextureSet textures;
    PortalDeparture native;
    PortalDeparture replay;
    std::array<PortalDeparture::LocalSnapshot, 16> history;
    const Mat4 body = glm::translate(Mat4{1}, Vec3{3, 10, -2});
    const auto compare = [&] {
        CHECK(replay.started() == native.started());
        CHECK(replay.finished() == native.finished());
        CHECK(replay.phase() == native.phase());
        CHECK(replay.displacement() == native.displacement());
        CHECK(replay.transform(body) == native.transform(body));
        CHECK(replay.skin() == native.skin());
    };
    const auto step = [&](PortalDeparture& departure, u32 tick, bool wrong) {
        if (wrong || tick == 15 || tick == 90) {
            departure.begin(device, textures);
        }
        departure.update(wrong ? 14 : 1);
    };
    u32 tick = 0;
    for (u32 window = 0; window < 48; ++window) {
        CAPTURE(window, tick);
        auto& saved = history[window % history.size()];
        replay.saveLocal(saved);
        const u32 distance = window % 8 + 1;
        for (u32 i = 0; i < distance; ++i) {
            step(replay, tick + i, true);
        }
        REQUIRE(replay.restoreLocal(saved));
        compare();
        for (u32 i = 0; i < distance; ++i) {
            step(native, tick + i, false);
            step(replay, tick + i, false);
            compare();
        }
        tick += distance;
    }
    CHECK_FALSE(native.restoreLocal(history[0]));
    TextureSet other;
    replay.loadSkin(device, other);
    CHECK_FALSE(replay.restoreLocal(history[0]));
    replay.saveLocal(history[0]);
    replay.clear();
    CHECK_FALSE(replay.restoreLocal(history[0]));
    CHECK(device.textureUpdates == 0);
    CHECK(device.texturesCreated == 0);
}

TEST_CASE("native departure retains its skin resources when a start is corrected",
          "[local-rewind][portal-departure-local-rewind][assets]") {
    const auto path = test::assetOrSkip("WEAPONS/textures.ngc");
    test::FakeRenderDevice device;
    TextureSet textures;
    REQUIRE(textures.load(path.parent_path()));
    PortalDeparture departure;
    PortalDeparture::LocalSnapshot before;
    departure.saveLocal(before);
    departure.begin(device, textures);
    const Texture* first = departure.skin();
    REQUIRE(first);
    const auto uploads = device.texturesCreated;
    for (s32 distance = 1; distance <= 8; ++distance) {
        departure.update(distance);
        REQUIRE(departure.restoreLocal(before));
        CHECK_FALSE(departure.started());
        CHECK(departure.skin() == nullptr);
        departure.begin(device, textures);
        CHECK(departure.skin() == first);
        CHECK(device.texturesCreated == uploads);
    }
    CHECK(device.textureUpdates == 0);
}

TEST_CASE("portal departure sinks and spins for fifty ticks even without artwork",
          "[portal-departure]") {
    test::FakeRenderDevice device;
    TextureSet empty;
    PortalDeparture departure;
    const Mat4 body = glm::translate(Mat4{1}, Vec3{20, 8, -5});
    CHECK(departure.transform(body) == body);
    departure.begin(device, empty);
    departure.update(25);
    const auto middle = departure.transform(body);
    CHECK(middle[3].x == 20);
    CHECK(middle[3].y == Approx(5));
    CHECK(middle[3].z == -5);
    CHECK(middle[0].x == Approx(std::cos(PortalDeparture::kSpinPerSecond * 25 / 60)));
    CHECK_FALSE(departure.finished());
    departure.update(25);
    CHECK(departure.finished());
    CHECK(departure.transform(body)[3].y == Approx(2));
    departure.update(1000);
    CHECK(departure.transform(body)[3].y == Approx(2));
    departure.clear();
    CHECK_FALSE(departure.started());
    CHECK(departure.transform(body) == body);
}

TEST_CASE("portal departure uses the animated lightning skin, not spawn flames",
          "[portal-departure][assets]") {
    const auto path = test::assetOrSkip("WEAPONS/textures.ngc");
    test::FakeRenderDevice device;
    TextureSet textures;
    REQUIRE(textures.load(path.parent_path()));
    PortalDeparture departure;
    departure.loadSkin(device, textures);
    CHECK_FALSE(departure.started());
    CHECK(departure.skin() == nullptr);
    departure.begin(device, textures);
    const auto first = textures.find("DTH_LIGHT00");
    const auto last = textures.find("DTH_LIGHT00+9");
    REQUIRE(first);
    REQUIRE(last);
    CHECK(departure.skin() == &textures.texture(device, *first));
    for (s32 tick = 0; tick < 45; ++tick) {
        CHECK(departure.phase() == Approx(static_cast<f32>(tick) / PortalDeparture::kTicks));
        const auto frame = tick / 5;
        const auto index =
            textures.find(frame == 0 ? "DTH_LIGHT00" : "DTH_LIGHT00+" + std::to_string(frame));
        REQUIRE(index);
        CHECK(departure.skin() == &textures.texture(device, *index));
        departure.update(1);
    }
    CHECK(departure.skin() == &textures.texture(device, *last));
    departure.update(5);
    CHECK(departure.skin() == nullptr);
    departure.clear();
}

TEST_CASE("fractional portal presentation samples the same authored spin without advancing time",
          "[portal-departure][online-departure]") {
    const Mat4 body = glm::translate(Mat4{1}, Vec3{10, 7, 2});
    for (s32 frame = 0; frame < 100; ++frame) {
        const f32 ticks = static_cast<f32>(frame) * 0.5f;
        const auto shown = PortalDeparture::transformAt(body, ticks / PortalDeparture::kTicks);
        CHECK(shown[3].y == Approx(7 - ticks * PortalDeparture::kSinkPerTick));
        CHECK(shown[0].x ==
              Approx(std::cos(PortalDeparture::kSpinPerSecond * ticks / 60)).margin(0.00001f));
        CHECK(shown[3].x == 10);
        CHECK(shown[3].z == 2);
    }
}

TEST_CASE("transport uses the tunnel one-shot rather than the looping portal flame",
          "[portal-departure][assets]") {
    const auto path = test::assetOrSkip("audio/COMMON.vbk");
    SoundSet sounds;
    REQUIRE(sounds.load(path));
    const auto tunnel = sounds.find(PortalDeparture::kSound);
    REQUIRE(tunnel);
    CHECK(sounds.entry(*tunnel).id == 4);
    CHECK(sounds.entry(*tunnel).duration > 0);
    REQUIRE_FALSE(sounds.entry(*tunnel).sequence.empty());
    for (const auto& step : sounds.entry(*tunnel).sequence) {
        CHECK_FALSE(step.loopBack);
        CHECK_FALSE(step.loopStart);
    }
    CHECK_FALSE(sounds.sequence(*tunnel).steps.empty());
}
} // namespace
