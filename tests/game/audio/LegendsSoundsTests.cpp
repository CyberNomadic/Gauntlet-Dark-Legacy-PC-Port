#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "game/audio/LegendsSounds.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::filesystem::path nativeRoot() {
    return test::dataDirectory().parent_path() / "assets/n64/extracted";
}
} // namespace

TEST_CASE("Native Legends sound tokens retain authored aliases gain and pitch",
          "[legends][audio][native-effects][assets]") {
    const auto root = nativeRoot();
    if (!std::filesystem::is_regular_file(root / "_program/game.bin")) {
        SKIP("Native N64 audio extraction is not installed");
    }
    LegendsSounds sounds;
    REQUIRE(sounds.openN64(root));
    REQUIRE(sounds.opened());
    const auto obelisk = sounds.resolve(0x78);
    REQUIRE(obelisk);
    CHECK(obelisk->effect == 0x78);
    CHECK(obelisk->volume == 1);
    CHECK(obelisk->pitch == 1);
    const auto alias = sounds.resolve(0x1f8);
    REQUIRE(alias);
    CHECK(alias->effect == 0x51);
    CHECK(alias->pitch == .5f);
    const auto movement = sounds.resolve(0x1fe);
    REQUIRE(movement);
    CHECK(movement->effect == 5);
    CHECK(movement->volume == 1);
    CHECK(movement->pitch == 1);
    const auto movementEnd = sounds.resolve(0x1ff);
    REQUIRE(movementEnd);
    CHECK(movementEnd->effect == 8);
    CHECK(movementEnd->volume == 1);
    CHECK(movementEnd->pitch == 1);
    const auto indexed = sounds.resolve(0x10008);
    REQUIRE(indexed);
    CHECK(indexed->effect == 0x11a);
    CHECK(indexed->volume == Catch::Approx(89.0f / 127));
    CHECK(indexed->pitch == 1);
    CHECK_FALSE(sounds.resolve(0xffffffff));
    CHECK_FALSE(sounds.resolve(0xffff));
    CHECK_FALSE(sounds.resolve(309));
    CHECK_FALSE(sounds.resolve(0x1007c));
    CHECK_FALSE(sounds.resolve(0x11000));
    CHECK_FALSE(sounds.resolve(0x10068)); // narration alias, not effect43
    CHECK_FALSE(sounds.resolve(0x10011)); // streamed speech, not effect267
    CHECK_FALSE(sounds.resolve(0x2000));
    CHECK_FALSE(sounds.resolve(0x4000));
    CHECK_FALSE(sounds.resolve(0x100000));
}

TEST_CASE("Native Legends cue routing reaches the effects mixer without name substitutes",
          "[legends][audio][native-effects][assets]") {
    const auto root = nativeRoot();
    if (!std::filesystem::is_regular_file(root / "_program/game.bin")) {
        SKIP("Native N64 audio extraction is not installed");
    }
    LegendsSounds sounds;
    REQUIRE(sounds.openN64(root));
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    const auto handle = sounds.play(output, 0x78);
    REQUIRE(handle != kNoSound);
    std::array<f32, 1600> samples{};
    f64 energy = 0;
    for (usize i = 0; i < 120; ++i) {
        mixer.mix(samples);
        for (const auto value : samples) {
            energy += static_cast<f64>(value) * value;
        }
        output.update();
    }
    CHECK(energy > .001);
    CHECK(sounds.play(output, 0xffff) == kNoSound);
    CHECK(sounds.play(output, 0x78, 0) == kNoSound);
    CHECK(sounds.play(output, 0x78, std::numeric_limits<f32>::quiet_NaN()) == kNoSound);
    CHECK_FALSE(sounds.openN64(root / "missing"));
    CHECK_FALSE(sounds.opened());
    CHECK_FALSE(sounds.resolve(0x78));
}

TEST_CASE("Native Legends sound routing requires the verified original program",
          "[legends][audio][native-effects]") {
    const auto directory = test::scratchDirectory("legends-sound-routing-invalid");
    LegendsSounds sounds;
    CHECK_FALSE(sounds.opened());
    CHECK_FALSE(sounds.openN64(directory));
    std::filesystem::create_directories(directory / "_program");
    writeFile(directory / "_program/game.bin", std::vector<u8>(0x80000, 0));
    CHECK_FALSE(sounds.openN64(directory));
    CHECK_FALSE(sounds.opened());
    CHECK_FALSE(sounds.resolve(0));
}
