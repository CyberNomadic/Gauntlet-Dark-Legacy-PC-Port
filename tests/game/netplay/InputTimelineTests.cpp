#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "game/netplay/InputTimeline.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Admission = InputTimeline::Admission;

InputCommand commandFor(const InputTimeline& timeline, u8 seat = 0) {
    InputCommand command;
    command.epoch = timeline.epoch();
    command.tick = timeline.tick();
    command.grant = timeline.grant(seat);
    command.seat = seat;
    command.direction = {1, 0};
    command.magnitude = 1;
    command.heldButtons = InputCommand::kHeldMask;
    command.pressedButtons = InputCommand::kPressMask;
    command.aimDirection = Vec3{1, 0, 0};
    return command;
}

TEST_CASE("only a seat's authenticated owner and current grant can submit inputs",
          "[netplay][ownership]") {
    InputTimeline timeline;
    REQUIRE(timeline.assign(0, 17));
    REQUIRE(timeline.assign(3, 17)); // two local controllers on one remote machine
    auto command = commandFor(timeline);
    CHECK(timeline.submit(0, command) == Admission::WrongOwner);
    CHECK(timeline.submit(18, command) == Admission::WrongOwner);
    REQUIRE(timeline.submit(17, command) == Admission::Accepted);
    REQUIRE(timeline.submit(17, commandFor(timeline, 3)) == Admission::Accepted);
    REQUIRE(timeline.assign(0, 18));
    CHECK(timeline.submit(18, command) == Admission::WrongGrant);
    CHECK(timeline.submit(17, command) == Admission::WrongOwner);
    const auto frame = timeline.advance();
    CHECK(frame[0].magnitude == 0); // discarded old owner's pending movement
    CHECK(frame[3].magnitude == 1);
    CHECK_FALSE(timeline.assign(4, 17));
    CHECK(timeline.owner(4) == 0);
    CHECK(timeline.grant(4) == 0);
    command.seat = 255;
    CHECK(timeline.submit(17, command) == Admission::Invalid);
}

TEST_CASE("duplicate reordered and late packets cannot replay or rewrite actions",
          "[netplay][input-timeline]") {
    InputTimeline timeline;
    REQUIRE(timeline.assign(0, 2));
    auto first = commandFor(timeline);
    auto second = first;
    second.tick = 1;
    second.magnitude = 0;
    second.pressedButtons = 0;
    REQUIRE(timeline.submit(2, second) == Admission::Accepted);
    REQUIRE(timeline.submit(2, first) == Admission::Accepted);
    auto altered = first;
    altered.direction = {-1, 0};
    CHECK(timeline.submit(2, altered) == Admission::Duplicate);
    const auto consumed = timeline.advance();
    CHECK(consumed[0].direction == first.direction);
    CHECK(consumed[0].pressedButtons == InputCommand::kPressMask);
    CHECK(timeline.submit(2, first) == Admission::TooLate);
    CHECK(timeline.advance()[0].magnitude == 0);
    CHECK(timeline.submit(2, second) == Admission::TooLate);
}

TEST_CASE("lost inputs repeat held state briefly but never potion-use selectors or press edges",
          "[netplay][input-timeline]") {
    InputTimeline timeline;
    REQUIRE(timeline.assign(0, 2));
    REQUIRE(timeline.submit(2, commandFor(timeline)) == Admission::Accepted);
    CHECK(timeline.advance()[0].pressedButtons == InputCommand::kPressMask);
    for (u64 tick = 1; tick <= InputTimeline::kHoldTicks; ++tick) {
        const auto frame = timeline.advance();
        CHECK(frame[0].tick == tick);
        CHECK(frame[0].magnitude == 1);
        CHECK(frame[0].heldButtons == InputCommand::kHeldMask);
        CHECK(frame[0].pressedButtons == 0);
        CHECK(frame[0].aimDirection == Vec3{1, 0, 0});
    }
    for (s32 i = 0; i < 20; ++i) {
        const auto frame = timeline.advance();
        CHECK(frame[0].magnitude == 0);
        CHECK(frame[0].heldButtons == 0);
        CHECK(frame[0].pressedButtons == 0);
        CHECK_FALSE(frame[0].aimDirection);
    }
}

TEST_CASE("disconnect and reconnect revoke held actions queued inputs and the old grant",
          "[netplay][ownership]") {
    InputTimeline timeline;
    REQUIRE(timeline.assign(0, 2));
    REQUIRE(timeline.assign(1, 2));
    REQUIRE(timeline.assign(2, 3));
    REQUIRE(timeline.submit(2, commandFor(timeline)) == Admission::Accepted);
    timeline.advance();
    const auto queued = commandFor(timeline);
    REQUIRE(timeline.submit(2, queued) == Admission::Accepted);
    REQUIRE(timeline.submit(2, commandFor(timeline, 1)) == Admission::Accepted);
    REQUIRE(timeline.submit(3, commandFor(timeline, 2)) == Admission::Accepted);
    timeline.disconnect(2);
    timeline.disconnect(0);
    CHECK(timeline.owner(0) == 0);
    CHECK(timeline.owner(1) == 0);
    CHECK(timeline.owner(2) == 3);
    const auto frame = timeline.advance();
    CHECK(frame[0].heldButtons == 0);
    CHECK(frame[1].pressedButtons == 0);
    CHECK(frame[2].pressedButtons == InputCommand::kPressMask);
    REQUIRE(timeline.assign(0, 2));
    CHECK(timeline.submit(2, queued) == Admission::WrongGrant);
}

TEST_CASE("epochs isolate loading and pause transitions without changing seat ownership",
          "[netplay][input-timeline]") {
    InputTimeline timeline;
    REQUIRE(timeline.assign(0, 2));
    const auto old = commandFor(timeline);
    REQUIRE(timeline.submit(2, old) == Admission::Accepted);
    timeline.advance();
    REQUIRE(timeline.submit(2, commandFor(timeline)) == Admission::Accepted);
    timeline.beginEpoch();
    CHECK(timeline.tick() == 0);
    CHECK(timeline.owner(0) == 2);
    CHECK(timeline.submit(2, old) == Admission::WrongEpoch);
    const auto frame = timeline.advance();
    CHECK(frame[0].magnitude == 0);
    CHECK(frame[0].heldButtons == 0);
    CHECK(frame[0].pressedButtons == 0);
    CHECK(timeline.submit(2, commandFor(timeline)) == Admission::Accepted);
}

TEST_CASE("future input storage is bounded by seats and the allowed tick window",
          "[netplay][input-timeline]") {
    InputTimeline timeline;
    REQUIRE(timeline.assign(0, 2));
    auto command = commandFor(timeline);
    for (u64 tick = 0; tick <= InputTimeline::kMaxAhead; ++tick) {
        command.tick = tick;
        REQUIRE(timeline.submit(2, command) == Admission::Accepted);
    }
    command.tick = InputTimeline::kMaxAhead + 1;
    CHECK(timeline.submit(2, command) == Admission::TooEarly);
    command.tick = std::numeric_limits<u64>::max();
    CHECK(timeline.submit(2, command) == Admission::TooEarly);
    for (u64 tick = 0; tick <= InputTimeline::kMaxAhead; ++tick) {
        CHECK(timeline.advance()[0].tick == tick);
    }
    CHECK(timeline.advance()[0].pressedButtons == 0);
}
TEST_CASE("input admission telemetry ignores duplicate late commands and resets with the epoch",
          "[netplay][prediction][input-timeline]") {
    InputTimeline timeline;
    REQUIRE(timeline.assign(0, 2));
    auto command = commandFor(timeline);
    timeline.advance();
    CHECK(timeline.submit(2, command) == Admission::TooLate);
    CHECK(timeline.diagnostics(0).late == 1);
    CHECK(timeline.diagnostics(0).arrivalMargin == -1);
    CHECK(timeline.submit(2, command) == Admission::TooLate);
    CHECK(timeline.diagnostics(0).late == 1);
    command.tick = timeline.tick() + 3;
    CHECK(timeline.submit(2, command) == Admission::Accepted);
    CHECK(timeline.diagnostics(0).arrivalMargin == 3);
    timeline.beginEpoch();
    CHECK_FALSE(timeline.diagnostics(0).received);
    CHECK(timeline.diagnostics(0).late == 0);
}

TEST_CASE("action traces distinguish exact execution from redundancy and held concealment",
          "[netplay][input-timeline][input-admission-trace]") {
    InputTimeline timeline;
    REQUIRE(timeline.assign(0, 2));
    auto command = commandFor(timeline);
    command.tick = 3;
    command.heldButtons = static_cast<u32>(CommandHeld::Attack);
    command.pressedButtons = static_cast<u32>(CommandPress::Attack);
    REQUIRE(timeline.submit(2, command) == Admission::Accepted);
    auto conflicting = command;
    conflicting.direction = {-1, 0};
    conflicting.heldButtons = static_cast<u32>(CommandHeld::Strafe);
    conflicting.pressedButtons = static_cast<u32>(CommandPress::UsePotion);
    REQUIRE(timeline.submit(2, conflicting) == Admission::Duplicate);
    REQUIRE(timeline.commandTrace().size() == 1);
    const auto& trace = timeline.commandTrace().front();
    CHECK(trace.command.epoch == command.epoch);
    CHECK(trace.command.grant == command.grant);
    CHECK(trace.command.seat == command.seat);
    CHECK(trace.command.tick == command.tick);
    CHECK(trace.command.direction == command.direction);
    CHECK(trace.command.heldButtons == command.heldButtons);
    CHECK(trace.command.pressedButtons == command.pressedButtons);
    CHECK(trace.sender == 2);
    CHECK(trace.receivedTick == 0);
    CHECK(trace.admission == Admission::Accepted);
    CHECK(trace.repeats == 1);
    CHECK_FALSE(trace.executedTick);
    for (u64 tick = 0; tick < command.tick; ++tick) {
        CHECK(timeline.advance()[0].heldButtons == 0);
        CHECK_FALSE(trace.executedTick);
    }
    const auto exact = timeline.advance()[0];
    CHECK(exact.pressedButtons == command.pressedButtons);
    REQUIRE(trace.executedTick);
    CHECK(*trace.executedTick == command.tick);
    CHECK(timeline.submit(2, conflicting) == Admission::TooLate);
    CHECK(trace.admission == Admission::Accepted);
    CHECK(trace.repeats == 2);
    for (u64 tick = 1; tick <= InputTimeline::kHoldTicks; ++tick) {
        const auto concealed = timeline.advance()[0];
        CHECK(concealed.heldButtons == command.heldButtons);
        CHECK(concealed.pressedButtons == 0);
        CHECK(trace.concealedTicks == tick);
        CHECK(*trace.executedTick == command.tick);
    }
    CHECK(timeline.advance()[0].heldButtons == 0);
    CHECK(trace.concealedTicks == InputTimeline::kHoldTicks);
    CHECK(timeline.droppedCommandTraces() == 0);
}

TEST_CASE(
    "action traces retain authenticated identities without accepting conflicting neutral input",
    "[netplay][input-timeline][input-admission-trace]") {
    InputTimeline timeline;
    REQUIRE(timeline.assign(0, 2));
    REQUIRE(timeline.assign(3, 3));
    auto neutral = commandFor(timeline);
    neutral.heldButtons = neutral.pressedButtons = 0;
    REQUIRE(timeline.submit(2, neutral) == Admission::Accepted);
    REQUIRE(timeline.submit(2, commandFor(timeline)) == Admission::Duplicate);
    CHECK(timeline.commandTrace().empty()); // first actual payload is neutral
    REQUIRE(timeline.submit(3, commandFor(timeline, 3)) == Admission::Accepted);
    CHECK(timeline.advance()[0].heldButtons == 0);
    auto late = commandFor(timeline);
    late.tick = 0;
    REQUIRE(timeline.submit(2, late) == Admission::TooLate);
    REQUIRE(timeline.commandTrace().size() == 2);
    const auto& trace = timeline.commandTrace().back();
    CHECK(trace.command.tick == 0);
    CHECK(trace.command.seat == 0);
    CHECK(trace.receivedTick == 1);
    CHECK(trace.admission == Admission::TooLate);
    CHECK_FALSE(trace.executedTick);
    CHECK(trace.concealedTicks == 0);
    for (usize index = 0; index <= InputTimeline::kCommandTraces; ++index) {
        auto invalid = late;
        invalid.tick = static_cast<u64>(index);
        CHECK(timeline.submit(9, invalid) == Admission::WrongOwner);
        ++invalid.grant;
        CHECK(timeline.submit(2, invalid) == Admission::WrongGrant);
        invalid = late;
        ++invalid.epoch;
        CHECK(timeline.submit(2, invalid) == Admission::WrongEpoch);
        invalid = late;
        invalid.seat = 255;
        CHECK(timeline.submit(2, invalid) == Admission::Invalid);
        invalid = late;
        invalid.tick = timeline.tick() + InputTimeline::kMaxAhead + 1;
        CHECK(timeline.submit(2, invalid) == Admission::TooEarly);
    }
    CHECK(timeline.commandTrace().size() == 2);
    CHECK(timeline.droppedCommandTraces() == 0);
    REQUIRE(timeline.assign(0, 2));
    REQUIRE(timeline.commandTrace().size() == 1);
    CHECK(timeline.commandTrace().front().command.seat == 3);
    timeline.disconnect(3);
    CHECK(timeline.commandTrace().empty());
    REQUIRE(timeline.submit(2, commandFor(timeline)) == Admission::Accepted);
    timeline.beginEpoch();
    CHECK(timeline.commandTrace().empty());
    CHECK(timeline.droppedCommandTraces() == 0);
}

TEST_CASE("action trace retention is bounded and ordinary movement cannot evict it",
          "[netplay][input-timeline][input-admission-trace]") {
    InputTimeline timeline;
    REQUIRE(timeline.assign(0, 2));
    constexpr u64 kExtra = 7;
    for (u64 tick = 0; tick < InputTimeline::kCommandTraces + kExtra; ++tick) {
        REQUIRE(timeline.submit(2, commandFor(timeline)) == Admission::Accepted);
        timeline.advance();
    }
    REQUIRE(timeline.commandTrace().size() == InputTimeline::kCommandTraces);
    CHECK(timeline.commandTrace().front().command.tick == kExtra);
    CHECK(timeline.droppedCommandTraces() == kExtra);
    for (const auto& trace : timeline.commandTrace()) {
        REQUIRE(trace.executedTick);
        CHECK(*trace.executedTick == trace.command.tick);
    }
    for (usize index = 0; index < InputTimeline::kCommandTraces; ++index) {
        auto movement = commandFor(timeline);
        movement.heldButtons = movement.pressedButtons = 0;
        REQUIRE(timeline.submit(2, movement) == Admission::Accepted);
        timeline.advance();
    }
    CHECK(timeline.commandTrace().size() == InputTimeline::kCommandTraces);
    CHECK(timeline.commandTrace().front().command.tick == kExtra);
    CHECK(timeline.droppedCommandTraces() == kExtra);
    timeline.beginEpoch();
    CHECK(timeline.commandTrace().empty());
    CHECK(timeline.droppedCommandTraces() == 0);
}
} // namespace
