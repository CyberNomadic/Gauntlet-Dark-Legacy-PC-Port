#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"

#include "TestSupport.h"
#include "fixtures/NativeSoundBank.h"
#include "game/world/LevelSoundscape.h"
#include "game/world/SpeechTiming.h"
#include "game/world/VoiceQueue.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("speech timing uses decoded frames and simulation time, including authored loops",
          "[game][speech-timing][determinism]") {
    const SoundClip first{24000, 2, std::vector<f32>(12000, 0.1f)};
    const SoundClip second{48000, 1, std::vector<f32>(36000, 0.1f)};
    SoundSequence sequence;
    sequence.steps = {{&first, false, false}, {&second, false, false}};
    auto timing = SpeechTiming::of(sequence);
    REQUIRE(timing.remainingSeconds == 1.0);
    REQUIRE_FALSE(timing.looping);
    timing.advance(-1);
    timing.advance(std::numeric_limits<f64>::quiet_NaN());
    timing.advance(std::numeric_limits<f64>::infinity());
    CHECK(timing.remainingSeconds == 1.0);
    timing.advance(0.25);
    CHECK(timing.remainingSeconds == 0.75);
    timing.advance(0.75);
    CHECK_FALSE(timing.playing());
    sequence.steps.front().loopStart = true;
    sequence.steps.back().loopBack = true;
    timing = SpeechTiming::of(sequence);
    timing.advance(100);
    CHECK(timing.playing());
    CHECK(timing.looping);
    timing = {};
    CHECK_FALSE(timing.playing());
}

TEST_CASE("voice admission and backlog ignore device progress, absence and external stop",
          "[game][speech-timing][voice-queue][determinism]") {
    AudioMixer fastMixer(48000);
    AudioMixer stalledMixer(48000);
    SoundPlayer fast(fastMixer);
    SoundPlayer stalled(stalledMixer);
    std::array<VoiceQueue, 3> queues;
    queues[0].bind(&fast);
    queues[1].bind(&stalled);
    const SoundClip clip{48000, 1, std::vector<f32>(12000, 0.2f)};
    SoundSequence sequence;
    sequence.steps.push_back({&clip, false, false});
    for (auto& queue : queues) {
        for (usize line = 0; line < VoiceQueue::kMost; ++line) {
            queue.queue(sequence, 1);
        }
        REQUIRE(queue.backlog() == 4);
        REQUIRE_FALSE(queue.room(VoiceQueue::kAlwaysRoom));
    }
    std::vector<f32> output(96000);
    for (usize line = 0; line < VoiceQueue::kMost; ++line) {
        fastMixer.mix(output);
        fast.update();
    }
    stalled.stopAll();
    for (s32 tick = 0; tick < 240; ++tick) {
        for (auto& queue : queues) {
            queue.update(1.0f / 60.0f);
        }
        for (usize i = 1; i < queues.size(); ++i) {
            CHECK(queues[i].backlog() == queues[0].backlog());
            CHECK(queues[i].room(1) == queues[0].room(1));
            CHECK(std::ranges::equal(queues[i].timing(), queues[0].timing()));
        }
    }
    CHECK(queues[0].backlog() == 0);
    for (auto& queue : queues) {
        queue.hold(true);
        CHECK(queue.queue(sequence, 1) == kNoSound);
        CHECK(queue.backlog() == 0);
        queue.clear();
        CHECK_FALSE(queue.held());
    }
}

TEST_CASE("timed ceremony bank precedence and chained duration are independent of playback",
          "[game][speech-timing][soundscape][determinism]") {
    const auto root = test::scratchDirectory("speech-timing");
    const std::array<test::NativeSoundSample, 2> samples{
        {{48000, std::vector<s16>(12000, 8192)}, {48000, std::vector<s16>(36000, 8192)}}};
    test::writeNativeSoundBank(root / "audio/CHARACTER", R"({"sounds":[
        {"index":0,"name":"NAME","sequence":[{"sample":0}]}]})",
                               samples);
    test::writeNativeSoundBank(root / "audio/WIZTOWER", R"({"sounds":[
        {"index":0,"name":"SPEECH","sequence":[{"sample":1}]}]})",
                               samples);
    test::writeNativeSoundBank(root / "audio/COMMON", R"({"sounds":[
        {"index":0,"name":"SPEECH","sequence":[{"sample":0}]},
        {"index":1,"name":"COMMON_LINE","sequence":[{"sample":1}]}]})",
                               samples);
    test::writeNativeSoundBank(root / "audio/VOICE1", R"({"sounds":[
        {"index":0,"name":"NARRATOR","sequence":[{"sample":0}]}]})",
                               samples);
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    LevelSoundscape audible;
    LevelSoundscape silent;
    audible.open(root, &output, nullptr);
    silent.open(root, nullptr, nullptr);
    SoundSet character;
    REQUIRE(character.load(root / "audio/CHARACTER"));
    auto spoken = audible.speechPromotion("SPEECH", audible.speechFrom(character, "NAME"));
    auto unplayed = silent.speechPromotion("SPEECH", silent.speechFrom(character, "NAME"));
    REQUIRE(spoken.handle != kNoSound);
    REQUIRE(unplayed.handle == kNoSound);
    REQUIRE(spoken.timing.remainingSeconds == Approx(1));
    REQUIRE(unplayed.timing == spoken.timing);
    CHECK(silent.speechPromotion("COMMON_LINE").timing.remainingSeconds == Approx(0.75));
    CHECK(silent.speechPromotion("NARRATOR").timing.remainingSeconds == Approx(0.25));
    CHECK(silent.speechNamed("SPEECH").timing.remainingSeconds == Approx(0.25));
    CHECK(silent.speechNarration("NARRATOR").timing.remainingSeconds == Approx(0.25));
    CHECK_FALSE(silent.speechPromotion("MISSING").timing.playing());
    CHECK(silent.speechPromotion("MISSING", unplayed).timing == unplayed.timing);
    output.stopAll();
    for (s32 tick = 0; tick < 60; ++tick) {
        spoken.timing.advance(1.0f / 60.0f);
        unplayed.timing.advance(1.0f / 60.0f);
        REQUIRE(spoken.timing == unplayed.timing);
    }
    CHECK_FALSE(spoken.timing.playing());
    audible.close();
    silent.close();
}
} // namespace
