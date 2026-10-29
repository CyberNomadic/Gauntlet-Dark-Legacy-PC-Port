#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/audio/N64LegendsStream.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/io/File.h"

#include "TestSupport.h"

namespace {
using namespace gdl;

std::filesystem::path nativeAudio() {
    return test::dataDirectory().parent_path() / "assets/n64/extracted/audio";
}

std::vector<f32> render(N64LegendsStream& stream, usize frames, usize piece) {
    std::vector<f32> pcm;
    while (pcm.size() / 2 < frames) {
        REQUIRE(stream.read(pcm, std::min(piece, frames - pcm.size() / 2)));
    }
    return pcm;
}
} // namespace

TEST_CASE("N64 music refuses unaudited input before native sequencing",
          "[audio][stream][legends]") {
    const auto root = test::scratchDirectory("n64-music-invalid");
    N64LegendsStream stream;
    std::vector<f32> output{0.25f};
    CHECK_FALSE(stream.opened());
    CHECK(stream.desc().channels == 0);
    CHECK_FALSE(stream.read(output, 160));
    stream.rewind();
    CHECK_FALSE(stream.open(root, 39));
    CHECK_FALSE(stream.open(root, 0));
    writeTextFile(root / "gauntlet.proj", "not a native bank");
    CHECK_FALSE(stream.open(root, 0));
    CHECK(output == std::vector<f32>{0.25f});
    CHECK_FALSE(stream.opened());
}

TEST_CASE("N64 native music rewinds and renders independently of read chunk size",
          "[audio][stream][legends][assets]") {
    const auto root = nativeAudio();
    if (!std::filesystem::exists(root)) {
        SKIP("Native N64 music extraction is not installed");
    }
    N64LegendsStream stream;
    REQUIRE(stream.open(root, 0));
    CHECK(stream.desc().sampleRate == 32000);
    CHECK(stream.desc().channels == 2);
    constexpr usize frames = 64000;
    const auto expected = render(stream, frames, 4096);
    REQUIRE(expected.size() == frames * 2);
    CHECK(std::any_of(expected.begin(), expected.end(), [](f32 value) { return value != 0.0f; }));
    for (const usize piece : {1U, 997U, 8191U}) {
        stream.rewind();
        REQUIRE(stream.opened());
        const auto actual = render(stream, frames, piece);
        CHECK(std::equal(expected.begin(), expected.end(), actual.begin(), actual.end()));
    }
    std::vector<f32> output{0.25f};
    CHECK_FALSE(stream.read(output, 0));
    REQUIRE(stream.read(output, std::numeric_limits<usize>::max()));
    CHECK(output.size() == 1 + 32768 * 2);
    CHECK(output.front() == 0.25f);
    CHECK_FALSE(stream.open(root, 0xffff));
    CHECK_FALSE(stream.opened());
    CHECK_FALSE(stream.read(output, 160));
}

TEST_CASE("N64 music sources do not share sequencer names or playback state",
          "[audio][stream][legends][assets]") {
    const auto root = nativeAudio();
    if (!std::filesystem::exists(root)) {
        SKIP("Native N64 music extraction is not installed");
    }
    constexpr usize frames = 64000;
    N64LegendsStream first;
    REQUIRE(first.open(root, 0));
    const auto expectedFirst = render(first, frames, 997);
    N64LegendsStream second;
    REQUIRE(second.open(root, 17));
    const auto expectedSecond = render(second, frames, 997);
    first.rewind();
    second.rewind();
    std::vector<f32> actualFirst;
    std::vector<f32> actualSecond;
    while (actualFirst.size() / 2 < frames) {
        const usize count = std::min<usize>(997, frames - actualFirst.size() / 2);
        REQUIRE(first.read(actualFirst, count));
        REQUIRE(second.read(actualSecond, count));
    }
    CHECK(std::equal(expectedFirst.begin(), expectedFirst.end(), actualFirst.begin(),
                     actualFirst.end()));
    CHECK(std::equal(expectedSecond.begin(), expectedSecond.end(), actualSecond.begin(),
                     actualSecond.end()));
    CHECK_FALSE(first.open(root, 100));
    second.rewind();
    const auto surviving = render(second, frames, 4096);
    CHECK(std::equal(surviving.begin(), surviving.end(), expectedSecond.begin(),
                     expectedSecond.end()));
}

TEST_CASE("N64 music bank and song fingerprints reject modified assets",
          "[audio][stream][legends][assets]") {
    const auto source = nativeAudio();
    if (!std::filesystem::exists(source)) {
        SKIP("Native N64 music extraction is not installed");
    }
    const auto destination = test::scratchDirectory("n64-music-modified");
    constexpr std::array<std::string_view, 5> names{
        "gauntlet.proj", "gauntlet.pool", "gauntlet.sdir", "gauntlet.samp", "mount1.song"};
    for (const auto name : names) {
        writeFile(destination / name, readFile(source / name));
    }
    for (const auto name : names) {
        INFO(name);
        auto bytes = readFile(source / name);
        REQUIRE_FALSE(bytes.empty());
        bytes[bytes.size() / 2] ^= 1;
        writeFile(destination / name, bytes);
        N64LegendsStream stream;
        CHECK_FALSE(stream.open(destination, 0));
        CHECK_FALSE(stream.opened());
        writeFile(destination / name, readFile(source / name));
    }
}

TEST_CASE("Native N64 music follows SoundPlayer music volume and lifetime",
          "[audio][stream][legends][assets]") {
    const auto root = nativeAudio();
    if (!std::filesystem::exists(root)) {
        SKIP("Native N64 music extraction is not installed");
    }
    auto source = std::make_shared<N64LegendsStream>();
    REQUIRE(source->open(root, 0));
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    const auto handle = player.playStream(source, true, 1.0f, SoundCategory::Music);
    REQUIRE(handle != kNoSound);
    std::array<f32, 1600> output{};
    f64 energy = 0;
    for (usize frame = 0; frame < 120; ++frame) {
        mixer.mix(output);
        for (f32 sample : output) {
            energy += static_cast<f64>(sample) * sample;
        }
        player.update();
    }
    CHECK(energy > 0.001);
    CHECK(player.isPlaying(handle));
    player.setCategoryVolume(SoundCategory::Music, 0.0f);
    // The mixer intentionally slews gains; allow one frame for the ramp.
    mixer.mix(output);
    player.update();
    mixer.mix(output);
    CHECK(std::all_of(output.begin(), output.end(), [](f32 sample) { return sample == 0.0f; }));
    player.setCategoryVolume(SoundCategory::Music, 1.0f);
    player.update();
    mixer.mix(output);
    CHECK(std::any_of(output.begin(), output.end(), [](f32 sample) { return sample != 0.0f; }));
    player.stop(handle);
    player.update();
    CHECK_FALSE(player.isPlaying(handle));
    // Stops use the same short anti-click gain ramp as other SoundPlayer voices.
    mixer.mix(output);
    mixer.mix(output);
    CHECK(std::all_of(output.begin(), output.end(), [](f32 sample) { return sample == 0.0f; }));
}

TEST_CASE("Every native N64 song sequences finite non-silent stereo audio",
          "[audio][stream][legends][assets]") {
    const auto root = nativeAudio();
    if (!std::filesystem::exists(root)) {
        SKIP("Native N64 music extraction is not installed");
    }
    for (u16 id = 0; id < 39; ++id) {
        INFO(id);
        N64LegendsStream stream;
        REQUIRE(stream.open(root, id));
        const auto pcm = render(stream, 32000 * 4, 4096);
        CHECK(std::all_of(pcm.begin(), pcm.end(), [](f32 sample) {
            return std::isfinite(sample) && sample >= -1.0f && sample <= 1.0f;
        }));
        CHECK(std::any_of(pcm.begin(), pcm.end(),
                          [](f32 sample) { return std::abs(sample) > 0.001f; }));
    }
}

TEST_CASE("Native N64 songs remain bounded through long playback and looping",
          "[audio][stream][legends][assets][.long]") {
    const auto root = nativeAudio();
    if (!std::filesystem::exists(root)) {
        SKIP("Native N64 music extraction is not installed");
    }
    for (u16 id = 0; id < 39; ++id) {
        INFO(id);
        N64LegendsStream stream;
        REQUIRE(stream.open(root, id));
        f64 tailEnergy = 0;
        for (usize second = 0; second < 600; ++second) {
            const auto pcm = render(stream, 32000, 4096);
            if (second >= 570) {
                for (const auto sample : pcm) {
                    tailEnergy += static_cast<f64>(sample) * sample;
                }
            }
        }
        // Title and movie cues have authored end markers. Do not turn those
        // one-shots into loops; these ordinary level tracks have native loops.
        if (id == 0 || id == 17 || id == 34) {
            CHECK(tailEnergy > 0.001);
        }
    }
}
