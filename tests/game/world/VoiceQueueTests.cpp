#include <algorithm>
#include <array>
#include <stdexcept>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundClip.h"
#include "engine/core/Types.h"

#include "game/world/VoiceQueue.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("voice queues serialize clips and reject excess waiting without silencing the first",
          "[game][voice-queue]") {
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    SoundClip clip;
    clip.sampleRate = 48000;
    clip.channels = 1;
    clip.samples.assign(48000, 0.25f);
    SoundSequence sequence;
    sequence.steps.push_back({&clip, false, false});
    VoiceQueue queue;
    queue.bind(&player);
    const auto first = queue.queue(sequence, 0.5f);
    REQUIRE(first != kNoSound);
    CHECK(queue.room(1));
    const auto second = queue.queue(sequence, 0.25f);
    REQUIRE(second != kNoSound);
    CHECK(player.voiceCount() == 1);
    CHECK(queue.backlog() == Approx(2));
    CHECK_FALSE(queue.room(1));
    queue.hold(true);
    CHECK_FALSE(queue.room(-1));
    CHECK(queue.queue(sequence, 1) == kNoSound);
    queue.hold(false);
    queue.update(-1);
    CHECK(queue.backlog() == Approx(2));
    std::vector<f32> samples(96000);
    mixer.mix(samples);
    CHECK(samples.back() == Approx(0.125f));
    player.update();
    queue.update(1);
    CHECK(queue.backlog() == Approx(1));
    mixer.mix(samples);
    CHECK(samples.back() == Approx(0.0625f));
    player.update();
    queue.update(1);
    CHECK(queue.backlog() == Approx(0));
    CHECK(queue.room(0));
}

TEST_CASE("voice queues cap pending clips until simulation completion or explicit clear",
          "[game][voice-queue]") {
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    SoundClip clip;
    clip.sampleRate = 48000;
    clip.channels = 1;
    clip.samples.assign(48000, 0.1f);
    SoundSequence sequence;
    sequence.steps.push_back({&clip, false, false});
    VoiceQueue queue;
    CHECK(queue.queue(sequence, 1) == kNoSound);
    CHECK(queue.backlog() == 1);
    queue.clear();
    queue.bind(&player);
    SoundHandle tail = kNoSound;
    for (usize i = 0; i < VoiceQueue::kMost; ++i) {
        tail = queue.queue(sequence, 1);
        REQUIRE(tail != kNoSound);
    }
    CHECK_FALSE(queue.room(-1));
    CHECK(queue.queue(sequence, 1) == kNoSound);
    player.stop(tail);
    CHECK(queue.backlog() == VoiceQueue::kMost);
    CHECK(queue.queue(sequence, 1) == kNoSound);
    queue.update(static_cast<f32>(VoiceQueue::kMost));
    CHECK(queue.backlog() == 0);
    CHECK(queue.queue(sequence, 1) != kNoSound);
    queue.hold(true);
    queue.clear();
    CHECK_FALSE(queue.held());
    CHECK(queue.backlog() == 0);
}

TEST_CASE("native voice queue replay restores admission with audible silent and failing output",
          "[voice-queue-local-rewind]") {
    const s32 mode = GENERATE(0, 1, 2);
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    VoiceQueue reference;
    VoiceQueue predicted;
    predicted.bind(mode == 0 ? nullptr : &output);
    SoundClip clip;
    clip.sampleRate = 48000;
    clip.channels = 1;
    clip.samples.assign(48000, 0.25f);
    SoundSequence sequence;
    sequence.bank = "TEST";
    sequence.steps.push_back({&clip, false, false});
    usize commands = 0;
    const SoundPlayer::Capture capture(&output, [&](const SoundPlayer::Event& event) {
        ++commands;
        if (mode == 2 && event.kind == SoundPlayer::Event::Kind::Play) {
            throw std::runtime_error("test optional voice output");
        }
    });
    const auto advance = [&](VoiceQueue& queue, s32 tick) {
        queue.hold(tick % 23 >= 16);
        if (tick % 9 == 0 && queue.room(0.5f)) {
            queue.queue(sequence, 1);
        }
        queue.update(1.0f / 60);
    };
    VoiceQueue::LocalSnapshot saved;
    s32 tick = 0;
    for (s32 window = 0; window < 40; ++window) {
        CAPTURE(mode, window, tick);
        predicted.saveLocal(saved);
        const auto before = std::vector(predicted.timing().begin(), predicted.timing().end());
        const bool held = predicted.held();
        predicted.hold(false);
        for (s32 step = 0; step < 1 + window % 8; ++step) {
            predicted.queue(sequence, 1);
            predicted.update(0.1f);
        }
        predicted.hold(true);
        const auto count = commands;
        REQUIRE(predicted.restoreLocal(saved));
        CHECK(commands == count);
        CHECK(predicted.held() == held);
        CHECK(std::ranges::equal(predicted.timing(), before));
        for (s32 step = 0; step < 1 + window % 8; ++step, ++tick) {
            advance(reference, tick);
            advance(predicted, tick);
            CHECK(std::ranges::equal(predicted.timing(), reference.timing()));
            CHECK(predicted.held() == reference.held());
            CHECK(predicted.room(0.5f) == reference.room(0.5f));
            CHECK(predicted.backlog() == reference.backlog());
        }
        REQUIRE(predicted.canRestoreLocal(saved));
    }
    CHECK((commands > 0) == (mode != 0));
}

TEST_CASE("native voice queue history is owner and output binding specific",
          "[voice-queue-local-rewind]") {
    VoiceQueue owner;
    VoiceQueue foreign;
    VoiceQueue::LocalSnapshot saved;
    CHECK_FALSE(owner.restoreLocal(saved));
    owner.saveLocal(saved);
    CHECK_FALSE(foreign.restoreLocal(saved));
    REQUIRE(owner.restoreLocal(saved));
    REQUIRE(owner.restoreLocal(saved));
    owner.clear();
    CHECK_FALSE(owner.restoreLocal(saved));
    owner.saveLocal(saved);
    owner.bind(nullptr);
    CHECK_FALSE(owner.restoreLocal(saved));
}

TEST_CASE("native voice equality distinguishes admission and logical sound predecessors",
          "[voice-queue-local-rewind][native-equality]") {
    VoiceQueue queue;
    const VoiceQueue foreign{};
    VoiceQueue::LocalSnapshot base;
    VoiceQueue::LocalSnapshot same;
    VoiceQueue::LocalSnapshot changed;
    CHECK_FALSE(queue.equalLocal(base, base));
    SceneSoundEvents events;
    events.open(91, nullptr);
    queue.recordTo(&events);
    queue.saveLocal(base);
    queue.saveLocal(same);
    REQUIRE(queue.equalLocal(base, same));
    CHECK_FALSE(foreign.equalLocal(base, same));
    queue.hold(true);
    queue.saveLocal(changed);
    CHECK_FALSE(queue.equalLocal(base, changed));
    REQUIRE(queue.restoreLocal(base));
    const SoundClip clip{48000, 1, std::vector<f32>(48000, 0.25f)};
    SoundSequence sequence{{{&clip, false, false}}, 1, SoundGainCurve::Linear, "VOICE1", 1};
    const auto capture = [&](u64 tick) {
        events.beginTick(tick);
        {
            auto scope = events.scoped({5, 12, 1, FrameEventEmitter::kNoSeat}, 1);
            queue.queue(sequence, 1);
        }
        events.endTick();
        queue.update(1);
        CHECK(queue.backlog() == 0);
    };
    capture(1);
    queue.saveLocal(same);
    CHECK_FALSE(queue.equalLocal(base, same)); // Drained timing still retains its predecessor.
    REQUIRE(queue.restoreLocal(base));
    capture(2);
    queue.saveLocal(changed);
    CHECK_FALSE(queue.equalLocal(same, changed));
    REQUIRE(queue.restoreLocal(same));
    queue.saveLocal(changed);
    CHECK(queue.equalLocal(same, changed));
    events.close();
    CHECK_FALSE(queue.equalLocal(same, changed));
}

TEST_CASE("recorded voice queues restore their logical predecessor before corrected narration",
          "[voice-queue-local-rewind][scene-sound-events]") {
    const bool audible = GENERATE(false, true);
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    SceneSoundEvents events;
    events.open(31, audible ? &player : nullptr);
    VoiceQueue queue;
    queue.bind(&player);
    queue.recordTo(&events);
    const SoundClip clip{48000, 1, std::vector<f32>(48000, 0.25f)};
    SoundSequence sequence{{{&clip, false, false}}, 1, SoundGainCurve::Linear, "VOICE1", 1};
    constexpr FrameEventEmitter kSpeaker{5, 12, 1, FrameEventEmitter::kNoSeat};
    std::vector<SoundPlayer::Event> output;
    const SoundPlayer::Capture observe(&player, [&](const auto& event) {
        if (event.kind == SoundPlayer::Event::Kind::Play) {
            output.push_back(event);
        }
    });
    const auto capture = [&](u64 tick, u32 cue) {
        events.beginTick(tick);
        {
            auto scope = events.scoped(kSpeaker, cue);
            CHECK(queue.queue(sequence, 1) == kNoSound);
        }
        return events.endTick();
    };
    const auto publish = [&](u64 tick, const auto& journal) {
        const std::array journals{journal};
        auto result = events.reconcile(tick, journals);
        REQUIRE(result);
        events.dispatch(result);
        return result;
    };
    const auto first = capture(1, 1);
    CHECK(output.empty());
    const auto firstCue = queue.logicalTail();
    REQUIRE(firstCue);
    CHECK(queue.backlog() == 1);
    publish(1, first);
    VoiceQueue::LocalSnapshot saved;
    queue.saveLocal(saved);
    const auto wrong = capture(2, 99);
    const auto wrongCue = queue.logicalTail();
    CHECK(wrongCue != firstCue);
    publish(2, wrong);
    const auto count = output.size();
    REQUIRE(queue.restoreLocal(saved));
    CHECK(output.size() == count);
    CHECK(queue.backlog() == 1);
    CHECK(queue.logicalTail() == firstCue);
    const auto corrected = capture(2, 2);
    REQUIRE(corrected.events.size() == 1);
    CHECK(corrected.events.front().payload.after == firstCue);
    CHECK(corrected.events.front().payload.after != wrongCue);
    const auto nextCue = queue.logicalTail();
    publish(2, corrected);
    if (audible) {
        REQUIRE(output.size() == 3);
        CHECK(output[2].after == output[0].handle);
        CHECK(output[2].after != output[1].handle);
    } else {
        CHECK(output.empty());
    }
    REQUIRE(queue.restoreLocal(saved));
    const auto replayed = capture(2, 2);
    CHECK(replayed == corrected);
    CHECK(queue.logicalTail() == nextCue);
    CHECK(publish(2, replayed).actions.empty());
    CHECK(queue.backlog() == 2);
    CHECK(events.unsupportedCount() == 0);
    events.close();
}

TEST_CASE("recorded voice admission agrees for absent stalled and progressing output",
          "[voice-queue-local-rewind][scene-sound-events]") {
    AudioMixer mixer(48000);
    AudioMixer stalledMixer(48000);
    SoundPlayer player(mixer);
    SoundPlayer stalledPlayer(stalledMixer);
    SceneSoundEvents silent;
    SceneSoundEvents running;
    SceneSoundEvents stalled;
    silent.open(14, nullptr);
    running.open(14, &player);
    stalled.open(14, &stalledPlayer);
    VoiceQueue a;
    VoiceQueue b;
    VoiceQueue c;
    a.recordTo(&silent);
    b.recordTo(&running);
    c.recordTo(&stalled);
    const SoundClip clip{48000, 1, std::vector<f32>(4800, 0.25f)};
    const SoundSequence sequence{
        {{&clip, false, false}}, 1, SoundGainCurve::Linear, "CHARACTER", 2};
    constexpr FrameEventEmitter kEmitter{2, 4, 1, 1};
    std::array<f32, 1600> samples{};
    for (u64 tick = 1; tick <= 90; ++tick) {
        std::array<SceneSoundEvents::Journal, 3> journals;
        usize i = 0;
        for (const auto& [queue, events] :
             {std::pair{&a, &silent}, std::pair{&b, &running}, std::pair{&c, &stalled}}) {
            events->beginTick(tick);
            {
                auto scope = events->scoped(kEmitter, 1);
                if (tick % 3 == 0 && queue->room(0.5f)) {
                    queue->queue(sequence, 1);
                }
                queue->update(1.0f / 60);
            }
            journals[i] = events->endTick();
            const auto result = events->reconcile(tick, std::span(&journals[i], 1));
            REQUIRE(result);
            events->dispatch(result);
            REQUIRE(events->confirm(tick));
            ++i;
        }
        mixer.mix(samples);
        player.update();
        CHECK(journals[0] == journals[1]);
        CHECK(journals[0] == journals[2]);
        CHECK(a.backlog() == b.backlog());
        CHECK(a.backlog() == c.backlog());
        CHECK(a.logicalTail() == b.logicalTail());
        CHECK(a.logicalTail() == c.logicalTail());
    }
    silent.close();
    running.close();
    stalled.close();
}

TEST_CASE("recorded voice history rejects a changed sound resource epoch",
          "[voice-queue-local-rewind][scene-sound-events]") {
    SceneSoundEvents events;
    events.open(8, nullptr);
    VoiceQueue queue;
    queue.recordTo(&events);
    VoiceQueue::LocalSnapshot saved;
    queue.saveLocal(saved);
    REQUIRE(queue.canRestoreLocal(saved));
    events.open(9, nullptr);
    CHECK_FALSE(queue.restoreLocal(saved));
    queue.saveLocal(saved);
    REQUIRE(queue.canRestoreLocal(saved));
    events.close();
    CHECK_FALSE(queue.restoreLocal(saved));
}

TEST_CASE("drained narration starts a new epoch without its previous logical predecessor",
          "[voice-queue-local-rewind][scene-sound-events]") {
    const bool detach = GENERATE(false, true);
    SceneSoundEvents events;
    events.open(81, nullptr);
    VoiceQueue queue;
    queue.recordTo(&events);
    const SoundClip clip{48000, 1, std::vector<f32>(48000, 0.25f)};
    SoundSequence sequence{{{&clip, false, false}}, 1, SoundGainCurve::Linear, "VOICE1", 1};
    constexpr FrameEventEmitter kEmitter{5, 12, 1, FrameEventEmitter::kNoSeat};
    const auto capture = [&] {
        events.beginTick(1);
        {
            auto scope = events.scoped(kEmitter, 1);
            queue.queue(sequence, 1);
        }
        return events.endTick();
    };
    const auto old = capture();
    REQUIRE(old.events.size() == 1);
    REQUIRE(queue.logicalTail());
    queue.update(1);
    queue.hold(true);
    VoiceQueue::LocalSnapshot oldSnapshot;
    queue.saveLocal(oldSnapshot);
    if (detach) {
        queue.recordTo(nullptr);
        CHECK_FALSE(queue.logicalTail());
        CHECK_FALSE(queue.canRestoreLocal(oldSnapshot));
    }
    events.open(82, nullptr);
    queue.recordTo(&events);
    CHECK_FALSE(queue.logicalTail());
    CHECK_FALSE(queue.canRestoreLocal(oldSnapshot));
    CHECK(queue.held());
    CHECK(queue.backlog() == 0);
    queue.hold(false);
    const auto current = capture();
    REQUIRE(current.events.size() == 1);
    CHECK(current.events[0].id.epoch == 82);
    CHECK_FALSE(current.events[0].payload.after);
    CHECK(queue.logicalTail().event == current.events[0].id);
    CHECK(events.reconcile(1, std::span(&current, 1)));
    CHECK(events.unsupportedCount() == 0);
    events.close();
}

TEST_CASE("voice queue attachment rejects unfinished speech without mutating admission or output",
          "[voice-queue-local-rewind][scene-sound-events]") {
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    VoiceQueue queue;
    queue.bind(&output);
    const SoundClip clip{48000, 1, std::vector<f32>(48000, 0.25f)};
    const SoundSequence sequence{{{&clip, false, false}}, 1, SoundGainCurve::Linear, "VOICE1", 1};
    const auto voice = queue.queue(sequence, 1);
    REQUIRE(voice != kNoSound);
    queue.hold(true);
    VoiceQueue::LocalSnapshot before;
    queue.saveLocal(before);
    SceneSoundEvents events;
    events.open(83, &output);
    usize commands = 0;
    const SoundPlayer::Capture observe(&output, [&](const auto&) { ++commands; });
    CHECK_THROWS_AS(queue.recordTo(&events), SceneSoundEvents::Unsupported);
    CHECK(commands == 0);
    CHECK(queue.canRestoreLocal(before));
    CHECK(queue.held());
    CHECK(queue.backlog() == 1);
    CHECK_FALSE(queue.logicalTail());
    CHECK(output.isPlaying(voice));
    queue.update(1);
    REQUIRE_NOTHROW(queue.recordTo(&events));
    CHECK(queue.held());
    CHECK(queue.backlog() == 0);
    CHECK_FALSE(queue.canRestoreLocal(before));
    events.close();
}

TEST_CASE("voice queue detachment preserves clocks but discards output-only predecessors",
          "[voice-queue-local-rewind][scene-sound-events]") {
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    VoiceQueue queue;
    queue.bind(&output);
    const SoundClip clip{48000, 1, std::vector<f32>(48000, 0.25f)};
    const SoundSequence sequence{{{&clip, false, false}}, 1, SoundGainCurve::Linear, "VOICE1", 1};
    const auto legacy = queue.queue(sequence, 1);
    REQUIRE(legacy != kNoSound);
    queue.update(1);
    SceneSoundEvents events;
    events.open(84, &output);
    queue.recordTo(&events);
    events.beginTick(1);
    {
        auto scope = events.scoped({5, 12, 1, FrameEventEmitter::kNoSeat}, 1);
        queue.queue(sequence, 1);
    }
    events.endTick();
    queue.hold(true);
    queue.update(0.25f);
    queue.recordTo(nullptr);
    CHECK_FALSE(queue.logicalTail());
    CHECK(queue.held());
    CHECK(queue.backlog() == 0.75);
    queue.hold(false);
    queue.update(1);
    std::vector<SoundPlayer::Event> plays;
    const SoundPlayer::Capture observe(&output, [&](const auto& event) {
        if (event.kind == SoundPlayer::Event::Kind::Play) {
            plays.push_back(event);
        }
    });
    REQUIRE(queue.queue(sequence, 1) != kNoSound);
    REQUIRE(plays.size() == 1);
    CHECK(plays[0].after == kNoSound);
    CHECK(output.isPlaying(legacy)); // a stalled old voice is not a new chain's dependency
    events.close();
}

TEST_CASE("voice queue recorder changes during capture fail before changing its binding",
          "[voice-queue-local-rewind][scene-sound-events]") {
    SceneSoundEvents first;
    SceneSoundEvents second;
    SceneSoundEvents closed;
    first.open(85, nullptr);
    second.open(86, nullptr);
    VoiceQueue queue;
    VoiceQueue::LocalSnapshot unbound;
    queue.saveLocal(unbound);
    CHECK_THROWS_AS(queue.recordTo(&closed), SceneSoundEvents::Unsupported);
    CHECK(queue.canRestoreLocal(unbound));
    second.beginTick(1);
    CHECK_THROWS_AS(queue.recordTo(&second), SceneSoundEvents::Unsupported);
    CHECK(queue.canRestoreLocal(unbound));
    second.abortTick();
    queue.recordTo(&first);
    VoiceQueue::LocalSnapshot bound;
    queue.saveLocal(bound);
    first.beginTick(1);
    CHECK_THROWS_AS(queue.recordTo(nullptr), SceneSoundEvents::Unsupported);
    CHECK_THROWS_AS(queue.recordTo(&second), SceneSoundEvents::Unsupported);
    CHECK(queue.canRestoreLocal(bound));
    first.abortTick();
    REQUIRE_NOTHROW(queue.recordTo(&second));
    CHECK_FALSE(queue.canRestoreLocal(bound));
    first.close();
    second.close();
}

} // namespace
