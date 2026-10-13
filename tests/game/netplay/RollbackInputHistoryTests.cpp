#include <array>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/netplay/InputTimeline.h"
#include "game/netplay/RollbackInputHistory.h"

namespace {
using namespace gdl;
using namespace gdl::game;

InputCommand inputAt(u64 tick, u8 seat = 0, u64 epoch = 7, u32 grant = 3) {
    InputCommand input;
    input.epoch = epoch;
    input.tick = tick;
    input.grant = grant;
    input.seat = seat;
    input.direction = {1, 0};
    input.magnitude = 1;
    input.aimDirection = Vec3{1, 0, 0};
    input.heldButtons =
        static_cast<u32>(CommandHeld::Attack) | static_cast<u32>(CommandHeld::ThrowPotion);
    input.pressedButtons = static_cast<u32>(CommandPress::UsePotion);
    return input;
}
bool record(RollbackInputHistory& history, const InputCommand& input, u64 sampled = 0) {
    return history.record(std::span{&input, 1}, sampled);
}

TEST_CASE("rollback input history retires post-step ticks rather than receipt watermarks",
          "[netplay][rollback-history]") {
    RollbackInputHistory history;
    REQUIRE(history.begin(7, {3, 0, 0, 0}));
    REQUIRE(record(history, inputAt(11), 201));
    REQUIRE(record(history, inputAt(13), 203));
    CHECK(history.replay(0).empty());
    REQUIRE(history.resolve(7, 10));
    auto replay = history.replay(0);
    REQUIRE(replay.size() == 3);
    CHECK(replay[0].input.tick == 11);
    CHECK(replay[0].sampled == 201);
    CHECK_FALSE(replay[0].predicted);
    CHECK(replay[1].input.tick == 12);
    CHECK(replay[1].predicted);
    CHECK(replay[1].input.pressedButtons == 0);
    CHECK(replay[2].input.tick == 13);
    CHECK(replay[2].input.pressedButtons != 0);
    CHECK(history.pending() == 2);
    // A newer command arriving does not retire the gap or any older command.
    REQUIRE(record(history, inputAt(20)));
    CHECK(history.pending() == 3);
    CHECK_FALSE(history.resolve(6, 20));
    CHECK_FALSE(history.resolve(7, 9));
    CHECK_FALSE(history.resolve(7, 10));
    CHECK(history.pending() == 3);
    REQUIRE(history.resolve(7, 13));
    CHECK(history.pending() == 1);
    replay = history.replay(0, inputAt(13));
    REQUIRE(replay.size() == 7);
    CHECK(replay.front().input.tick == 14);
    CHECK(replay.back().input.tick == 20);
    REQUIRE(history.resolve(7, 20));
    CHECK(history.pending() == 0);
    CHECK(history.replay(0).empty());
}

TEST_CASE("rollback gaps use exactly the authoritative missing-input policy",
          "[netplay][rollback-history]") {
    InputTimeline timeline;
    REQUIRE(timeline.assign(0, 2));
    RollbackInputHistory history;
    REQUIRE(history.begin(timeline.epoch(), {timeline.grant(0), 0, 0, 0}));
    const auto first = inputAt(0, 0, timeline.epoch(), timeline.grant(0));
    REQUIRE(timeline.submit(2, first) == InputTimeline::Admission::Accepted);
    timeline.advance();
    REQUIRE(timeline.previous(0));
    CHECK(timeline.previous(0)->tick == 0);
    for (u64 tick = 1; tick <= 4; ++tick) {
        timeline.advance();
    }
    REQUIRE(history.resolve(timeline.epoch(), 4));
    const auto future = inputAt(13, 0, timeline.epoch(), timeline.grant(0));
    REQUIRE(timeline.submit(2, future) == InputTimeline::Admission::Accepted);
    REQUIRE(record(history, future));
    const auto replay = history.replay(0, timeline.previous(0));
    REQUIRE(replay.size() == 9);
    for (const auto& entry : replay) {
        const auto authority = timeline.advance()[0];
        CHECK(entry.input.tick == authority.tick);
        CHECK(entry.input.direction == authority.direction);
        CHECK(entry.input.magnitude == authority.magnitude);
        CHECK(entry.input.heldButtons == authority.heldButtons);
        CHECK(entry.input.pressedButtons == authority.pressedButtons);
        CHECK(entry.input.aimDirection == authority.aimDirection);
    }
    CHECK(replay[0].input.heldButtons != 0); // original tick 0 held through tick 6
    CHECK(replay[1].input.heldButtons != 0);
    CHECK(replay[2].input.heldButtons == 0); // not six more ticks after checkpoint 4
    CHECK(replay.back().input.pressedButtons != 0);
    CHECK_FALSE(replay.back().predicted);
}

TEST_CASE("rollback has no inferred press or movement before its first actual input",
          "[netplay][rollback-history]") {
    RollbackInputHistory history;
    REQUIRE(history.begin(7, {3, 0, 0, 0}));
    REQUIRE(history.resolve(7, 0));
    REQUIRE(record(history, inputAt(3)));
    const auto replay = history.replay(0);
    REQUIRE(replay.size() == 3);
    for (usize i = 0; i < 2; ++i) {
        CHECK(replay[i].input.tick == i + 1);
        CHECK(replay[i].input.magnitude == 0);
        CHECK(replay[i].input.heldButtons == 0);
        CHECK(replay[i].input.pressedButtons == 0);
        CHECK(replay[i].predicted);
    }
    CHECK(replay[2].input.magnitude == 1);
    CHECK_FALSE(replay[2].predicted);
}

TEST_CASE("rollback respects an explicit potion release before its concealment deadline",
          "[netplay][rollback-history][potion-input]") {
    InputTimeline timeline;
    REQUIRE(timeline.assign(0, 2));
    RollbackInputHistory history;
    REQUIRE(history.begin(timeline.epoch(), {timeline.grant(0), 0, 0, 0}));
    const auto held = inputAt(0, 0, timeline.epoch(), timeline.grant(0));
    REQUIRE(timeline.submit(2, held) == InputTimeline::Admission::Accepted);
    timeline.advance();
    REQUIRE(history.resolve(timeline.epoch(), 0));
    auto released = held;
    released.tick = 4;
    released.heldButtons = 0;
    released.pressedButtons = 0;
    auto later = released;
    later.tick = 14;
    // Out-of-order arrival must not revive the earlier held state after release.
    REQUIRE(record(history, later));
    REQUIRE(record(history, released));
    REQUIRE(timeline.submit(2, later) == InputTimeline::Admission::Accepted);
    REQUIRE(timeline.submit(2, released) == InputTimeline::Admission::Accepted);
    const auto replay = history.replay(0, timeline.previous(0));
    REQUIRE(replay.size() == 14);
    for (const auto& entry : replay) {
        CAPTURE(entry.input.tick);
        const auto authority = timeline.advance()[0];
        CHECK(entry.input.held(CommandHeld::ThrowPotion) == (entry.input.tick < 4));
        CHECK(entry.input.heldButtons == authority.heldButtons);
        CHECK(entry.input.pressedButtons == 0);
        CHECK(entry.input.pressedButtons == authority.pressedButtons);
    }
}

TEST_CASE("rollback histories isolate local seats and revoke old epochs and grants",
          "[netplay][rollback-history]") {
    RollbackInputHistory history;
    REQUIRE(history.begin(7, {3, 0, 0, 8}));
    REQUIRE(history.resolve(7, 40));
    const std::array frame{inputAt(41), inputAt(41, 3, 7, 8)};
    REQUIRE(history.record(frame, 99));
    CHECK(history.pending() == 2);
    CHECK(history.count(0) == 1);
    CHECK(history.count(3) == 1);
    CHECK(history.count(4) == 0);
    CHECK(history.replay(4).empty());
    CHECK(history.replay(1).empty());
    history.invalidate(0);
    CHECK(history.count(0) == 0);
    REQUIRE(history.replay(3).size() == 1);
    CHECK(history.replay(3).front().input.grant == 8);
    REQUIRE(history.begin(8, {4, 0, 0, 0}));
    CHECK_FALSE(history.resolved());
    CHECK(history.pending() == 0);
    CHECK_FALSE(history.record(frame));
    CHECK_FALSE(record(history, inputAt(0, 0, 8, 3)));
    CHECK_FALSE(record(history, inputAt(0, 3, 8, 8)));
    REQUIRE(record(history, inputAt(0, 0, 8, 4)));
    history.clear();
    CHECK(history.epoch() == 0);
    CHECK(history.pending() == 0);
    CHECK_FALSE(history.resolve(8, 0));
    CHECK_FALSE(history.begin(0, {1, 0, 0, 0}));
    CHECK_FALSE(history.begin(8, {}));
}

TEST_CASE("rollback input admission is atomic and duplicate samples cannot rewrite history",
          "[netplay][rollback-history]") {
    RollbackInputHistory history;
    REQUIRE(history.begin(7, {3, 4, 0, 0}));
    REQUIRE(history.resolve(7, 0));
    REQUIRE(record(history, inputAt(3), 25));
    REQUIRE(record(history, inputAt(3), 26));
    REQUIRE(record(history, inputAt(2), 24)); // reordering is harmless before acknowledgement
    CHECK(history.count(0) == 2);
    auto changed = inputAt(3);
    changed.pressedButtons = 0;
    CHECK_FALSE(record(history, changed));
    auto invalid = inputAt(4, 1, 7, 99);
    const std::array invalidFrame{inputAt(4), invalid};
    CHECK_FALSE(history.record(invalidFrame));
    CHECK(history.count(0) == 2);
    CHECK(history.count(1) == 0);
    const std::array repeatedSeat{inputAt(4), inputAt(4)};
    CHECK_FALSE(history.record(repeatedSeat));
    const std::array mixedTicks{inputAt(4), inputAt(5, 1, 7, 4)};
    CHECK_FALSE(history.record(mixedTicks));
    const auto replay = history.replay(0);
    REQUIRE(replay.size() == 3);
    CHECK(replay.back().sampled == 25);
    CHECK(replay.back().input.pressedButtons != 0);
    REQUIRE(history.resolve(7, 3));
    CHECK_FALSE(record(history, inputAt(3)));
    CHECK_FALSE(record(history, inputAt(2)));
    CHECK(history.pending() == 0);
}

TEST_CASE("rollback history exhausts closed without dropping unresolved input or growing unbounded",
          "[netplay][rollback-history]") {
    RollbackInputHistory history;
    REQUIRE(history.begin(7, {3, 4, 0, 0}));
    REQUIRE(history.resolve(7, 10));
    for (u64 i = 1; i <= RollbackInputHistory::kHistory; ++i) {
        REQUIRE(record(history, inputAt(10 + i)));
    }
    CHECK(history.count(0) == RollbackInputHistory::kHistory);
    REQUIRE(history.replay(0).size() == RollbackInputHistory::kHistory);
    const std::array overflow{inputAt(11 + RollbackInputHistory::kHistory),
                              inputAt(11 + RollbackInputHistory::kHistory, 1, 7, 4)};
    CHECK_FALSE(history.record(overflow));
    CHECK(history.resyncRequired());
    CHECK(history.resyncRequired(0));
    CHECK(history.resyncRequired(1));
    CHECK_FALSE(history.resyncRequired(4));
    CHECK(history.count(0) == RollbackInputHistory::kHistory);
    CHECK(history.count(1) == 0);
    CHECK(history.replay(0).empty());
    REQUIRE(history.resolve(7, 10 + RollbackInputHistory::kHistory));
    CHECK(history.pending() == 0);
    CHECK(history.resyncRequired()); // only an explicit fresh-state reset releases the latch
    CHECK_FALSE(record(history, overflow[0]));
    history.invalidate(0);
    REQUIRE(record(history, overflow[0]));
    CHECK_FALSE(history.resyncRequired(0));
    CHECK(history.resyncRequired(1));
    CHECK(history.replay(0).size() == 1);
}

TEST_CASE("rollback pre-checkpoint history and future lead gaps are bounded by ticks",
          "[netplay][rollback-history]") {
    RollbackInputHistory history;
    REQUIRE(history.begin(7, {3, 0, 0, 0}));
    REQUIRE(record(history, inputAt(500)));
    REQUIRE(record(history, inputAt(500 + RollbackInputHistory::kHistory - 1)));
    CHECK_FALSE(record(history, inputAt(500 + RollbackInputHistory::kHistory)));
    CHECK(history.count(0) == 2);
    CHECK(history.resyncRequired(0));
    REQUIRE(history.begin(7, {3, 0, 0, 0}));
    REQUIRE(record(history, inputAt(500)));
    REQUIRE(history.resolve(7, 0));
    CHECK(history.resyncRequired(0));
    CHECK(history.replay(0).empty());
}

TEST_CASE("rollback rejects an invalid checkpoint held seed instead of leaking another seat",
          "[netplay][rollback-history]") {
    for (const auto& seed : {inputAt(0, 1), inputAt(0, 0, 6), inputAt(0, 0, 7, 4), inputAt(4)}) {
        RollbackInputHistory history;
        REQUIRE(history.begin(7, {3, 0, 0, 0}));
        REQUIRE(history.resolve(7, 0));
        REQUIRE(record(history, inputAt(3)));
        CHECK(history.replay(0, seed).empty());
        CHECK(history.resyncRequired(0));
    }
}

TEST_CASE("rollback tick arithmetic remains bounded near the integer limit",
          "[netplay][rollback-history]") {
    constexpr u64 kLast = std::numeric_limits<u64>::max();
    RollbackInputHistory history;
    REQUIRE(history.begin(7, {3, 0, 0, 0}));
    REQUIRE(history.resolve(7, kLast - 3));
    REQUIRE(record(history, inputAt(kLast)));
    const auto replay = history.replay(0, inputAt(kLast - 3));
    REQUIRE(replay.size() == 3);
    CHECK(replay.front().input.tick == kLast - 2);
    CHECK(replay.back().input.tick == kLast);
    REQUIRE(history.resolve(7, kLast));
    CHECK(history.replay(0).empty());
    CHECK_FALSE(record(history, inputAt(0)));
}
} // namespace
