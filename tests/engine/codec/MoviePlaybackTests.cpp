#include <algorithm>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/codec/MoviePlayback.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "TestSupport.h"

namespace {

using namespace gdl;

TEST_CASE("an unopened playback is inert", "[codec][movie]") {
    MoviePlayback playback;
    REQUIRE_FALSE(playback.isOpen());
    REQUIRE_FALSE(playback.update(0.1));
    REQUIRE_FALSE(playback.frameChanged());
    REQUIRE(playback.frame().pixels.empty());
    REQUIRE_FALSE(playback.open(test::scratchDirectory("movie-missing") / "none.avi"));
}

TEST_CASE("the Midway logo movie plays through", "[codec][movie][assets]") {
    const auto file = test::assetOrSkip("VQMOVIES/midway.avi");
    MoviePlayback playback;
    REQUIRE(playback.open(file));

    const MovieInfo& info = playback.info();
    REQUIRE(info.width == 512);
    REQUIRE(info.height == 384);
    CHECK_THAT(info.framesPerSecond, Catch::Matchers::WithinAbs(30.0, 0.01));
    REQUIRE(info.frameCount == 240);
    REQUIRE(info.hasAudio);
    REQUIRE(info.audioReady);
    REQUIRE(info.audioChannels == 2);
    REQUIRE(info.audioSampleRate == 48042);

    std::vector<f32> audio;
    bool sawColour = false;
    s32 updates = 0;
    while (playback.update(1.0 / 30.0) && updates < 1000) {
        ++updates;
        playback.takeAudio(audio);
        if (playback.frameChanged()) {
            const Image& frame = playback.frame();
            const Color centre = frame.pixel(frame.width / 2, frame.height / 2);
            sawColour = sawColour || centre != Color::black();
        }
    }
    playback.takeAudio(audio);

    REQUIRE(playback.decodedFrames() == 240);
    REQUIRE(sawColour);
    REQUIRE(audio.size() > 700000);
    REQUIRE(audio.size() < 800000);
    REQUIRE(std::ranges::max(audio) > 0.1f);
    REQUIRE(updates >= 240);
}

TEST_CASE("the story and title movies play through with picture and sound",
          "[codec][movie][assets]") {
    const auto* name = GENERATE("VQMOVIES/OPENING.avi", "VQMOVIES/TITLE2.avi");
    CAPTURE(name);
    const auto file = test::assetOrSkip(name);
    MoviePlayback playback;
    REQUIRE(playback.open(file));
    REQUIRE(playback.info().frameCount > 0);
    REQUIRE(playback.info().hasAudio);
    const u32 expectedFrames = playback.info().frameCount;
    const f64 step = 1.0 / playback.info().framesPerSecond;
    std::vector<f32> audio;
    bool sawColour = false;
    bool finished = false;
    for (u32 update = 0; update < expectedFrames + 10; ++update) {
        const bool playing = playback.update(step);
        playback.takeAudio(audio);
        if (playback.frameChanged()) {
            const Image& frame = playback.frame();
            sawColour =
                sawColour || frame.pixel(frame.width / 2, frame.height / 2) != Color::black();
        }
        if (!playing) {
            finished = true;
            break;
        }
    }
    REQUIRE(finished);
    REQUIRE(playback.decodedFrames() == expectedFrames);
    REQUIRE(sawColour);
    REQUIRE_FALSE(audio.empty());
    REQUIRE(std::ranges::max(audio) > 0.1f);
}

TEST_CASE("a movie's clock starts on its first update and keeps to its sound",
          "[codec][movie][assets][movie-sync]") {
    const auto file = test::assetOrSkip("VQMOVIES/midway.avi");
    MoviePlayback playback;
    REQUIRE(playback.open(file));
    // The first update only starts the clock: the time a level took to load is not play.
    REQUIRE(playback.update(0.5));
    CHECK(playback.playTime() == 0.0);
    CHECK(playback.decodedFrames() == 0);
    // Then it runs on the frame's time: the first frame shows once a frame's time has passed.
    REQUIRE(playback.update(1.0 / 60.0));
    CHECK(playback.decodedFrames() == 0);
    REQUIRE(playback.update(1.0 / 60.0));
    CHECK(playback.decodedFrames() == 1);
    // While the sound advances the picture keeps to it, whatever the frame's time says.
    REQUIRE(playback.update(1.0 / 60.0, 0.5));
    CHECK(playback.playTime() == Catch::Approx(0.5));
    REQUIRE(playback.decodedFrames() == 9); // up to eight frames a call catch up
    // A sound that has not moved on (no device, or finished) leaves the frame's time to run.
    REQUIRE(playback.update(1.0 / 60.0, 0.5));
    CHECK(playback.playTime() == Catch::Approx(0.5 + 1.0 / 60.0));
    // Closing and reopening starts over.
    playback.close();
    REQUIRE(playback.open(file));
    REQUIRE(playback.update(1.0));
    CHECK(playback.playTime() == 0.0);
}

} // namespace
