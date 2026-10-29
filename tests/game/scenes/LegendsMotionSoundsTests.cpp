#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "formats/LegendsN64Texture.h"
#include "game/scenes/LegendsLevel.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct MotionFixture {
    test::FakeRenderDevice device;
    std::unique_ptr<LegendsLevel> level;
    LegendsSceneAdapter* adapter = nullptr;
    Vec3 platePosition{0};

    MotionFixture() {
        const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
        const auto directory = root / "levels/mount1";
        if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
            SKIP("Native N64 Legends extraction is not installed");
        }
        const auto objects = readFile(directory / "objects.wad");
        auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"), objects);
        const auto dynamic = std::filesystem::is_regular_file(directory / "texdyn.rom")
                                 ? readFile(directory / "texdyn.rom")
                                 : std::vector<u8>{};
        for (auto& texture : formats::decodeLegendsN64Textures(objects, dynamic)) {
            map.textures.push_back({std::move(texture.image), texture.clampU, texture.clampV,
                                    texture.hasTransparency, std::move(texture.name),
                                    texture.external, texture.width, texture.height});
        }
        const auto definitions = formats::decodeLegendsN64Switches(
            readFile(directory / "ipr.rom"), readFile(root / "items/generic/ptrap.rom"));
        const LegendsWorldMotion motion{map, definitions.switches};
        REQUIRE(motion.platePosition(2));
        platePosition = *motion.platePosition(2);
        auto owned = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
        adapter = owned.get();
        level = std::make_unique<LegendsLevel>(device, map, std::move(owned), directory,
                                               test::assetOrSkip("PLAYERS").parent_path());
    }

    Vec3 begin() {
        REQUIRE(adapter->soundLoops().empty());
        adapter->touchSwitches(std::array{LegendsPortalVisitor{0, platePosition, true, .5f, 3}});
        // A plate press is a different native cue from the target's retained
        // movement handle. Isolate the latter's lifetime in this regression.
        adapter->takeSounds();
        level->update(1.0f / 60, false);
        const auto loops = adapter->soundLoops();
        REQUIRE(loops.size() == 1);
        CHECK(loops[0].token == 0x1fe);
        return loops[0].position;
    }
};

f64 mixTicks(AudioMixer& mixer, SoundPlayer& output, usize count) {
    std::array<f32, 1600> samples{};
    f64 energy = 0;
    for (usize tick = 0; tick < count; ++tick) {
        mixer.mix(samples);
        for (const f32 value : samples) {
            energy += static_cast<f64>(value) * value;
        }
        output.update();
    }
    return energy;
}
} // namespace

TEST_CASE("Native moving platform audio pauses resumes and follows positional audibility",
          "[legends][motion-sounds][audio][assets]") {
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    MotionFixture fixture;
    const Vec3 listener = fixture.begin();
    fixture.level->playSounds(output, listener);
    REQUIRE(output.voiceCount() == 1);
    for (usize tick = 0; tick < 12; ++tick) {
        fixture.level->playSounds(output, listener);
    }
    CHECK(output.voiceCount() == 1); // No start per simulation/render tick.
    CHECK(mixTicks(mixer, output, 12) > .000001);

    fixture.level->pauseSounds();
    mixTicks(mixer, output, 2); // Shared mixer anti-click stop ramp.
    CHECK(output.voiceCount() == 0);
    CHECK(mixTicks(mixer, output, 12) == 0);
    CHECK(fixture.adapter->soundLoops().size() == 1);
    CHECK(fixture.adapter->takeSounds().empty()); // Pause is not an endpoint.
    fixture.level->playSounds(output, listener);
    REQUIRE(output.voiceCount() == 1);
    CHECK(mixTicks(mixer, output, 12) > .000001);

    // A shared output reset invalidates the old handle, not the native motion.
    output.stopAll();
    mixTicks(mixer, output, 2);
    REQUIRE(output.voiceCount() == 0);
    fixture.level->playSounds(output, listener);
    REQUIRE(output.voiceCount() == 1);
    CHECK(mixTicks(mixer, output, 12) > .000001);

    fixture.level->playSounds(output, listener + Vec3{1000, 0, 0});
    mixTicks(mixer, output, 2);
    CHECK(output.voiceCount() == 0);
    CHECK(mixTicks(mixer, output, 12) == 0);
    fixture.level->playSounds(output, listener);
    REQUIRE(output.voiceCount() == 1);
    fixture.level->stopSounds(); // Leaving while still moving stops the retained handle.
    mixTicks(mixer, output, 2);
    CHECK(output.voiceCount() == 0);
    CHECK(mixTicks(mixer, output, 12) == 0);
}

TEST_CASE("Native movement completion replaces the loop with one endpoint effect",
          "[legends][motion-sounds][audio][assets]") {
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    MotionFixture fixture;
    const Vec3 listener = fixture.begin();
    fixture.level->playSounds(output, listener);
    REQUIRE(output.voiceCount() == 1);
    fixture.level->update(3, true); // Stop Time leaves ordinary scenery in motion.
    REQUIRE(fixture.adapter->soundLoops().empty());
    fixture.level->playSounds(output, listener);
    mixTicks(mixer, output, 2);
    REQUIRE(output.voiceCount() == 1); // End cue remains, moving handle is gone.
    fixture.level->pauseSounds();
    CHECK(output.voiceCount() == 1); // One-shot tails remain audible under pause.
    CHECK(mixTicks(mixer, output, 30) > .000001);
    fixture.level->stopSounds();
    mixTicks(mixer, output, 2);
    REQUIRE(output.voiceCount() == 0);
    fixture.level->update(1, false);
    fixture.level->playSounds(output, listener);
    CHECK(output.voiceCount() == 0);
    CHECK(mixTicks(mixer, output, 12) == 0);
    fixture.level->stopSounds();
}
