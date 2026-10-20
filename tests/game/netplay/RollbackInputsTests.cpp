#include <algorithm>
#include <array>
#include <limits>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "game/netplay/RollbackInputs.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Admission = RollbackInputs::Admission;
constexpr u64 kEpoch = 37;

struct Fixture {
    RollbackInputs inputs;
    RollbackInputs::Owners owners;
    RollbackInputs::Grants grants{};

    explicit Fixture(RollbackInputs::Owners roster = {1, 2, 3, 4}, u8 delay = 0) : owners(roster) {
        for (usize seat = 0; seat < owners.size(); ++seat) {
            grants[seat] = owners[seat] == 0 ? 0 : static_cast<u32>(10 + seat);
        }
        REQUIRE(inputs.open(1, kEpoch, owners, grants, delay));
    }

    std::vector<InputCommand> frame(u8 peer, u64 tick, bool actions = false) const {
        std::vector<InputCommand> result;
        // NOLINTNEXTLINE(bugprone-too-small-loop-variable): seats and peers number at most five
        for (u8 seat = 0; seat < static_cast<u8>(owners.size()); ++seat) {
            if (owners[seat] != peer) {
                continue;
            }
            InputCommand command;
            command.epoch = kEpoch;
            command.tick = tick;
            command.grant = grants[seat];
            command.seat = seat;
            if (actions) {
                command.direction = {1, 0};
                command.magnitude = 1;
                command.aimDirection = Vec3{0, 0, 1};
                command.heldButtons = static_cast<u32>(CommandHeld::Attack) |
                                      static_cast<u32>(CommandHeld::ThrowPotion);
                command.pressedButtons = static_cast<u32>(CommandPress::Attack) |
                                         static_cast<u32>(CommandPress::UsePotion);
                command.hintTopic = 2;
                command.hintRevision = 7;
            }
            REQUIRE(command.valid());
            result.push_back(command);
        }
        return result;
    }

    void sample(bool actions = false) {
        REQUIRE(inputs.sampleReady());
        REQUIRE(inputs.sample(frame(1, inputs.sampleTick(), actions)));
    }

    void remote(u8 peer, u64 tick, bool actions = false) {
        REQUIRE(inputs.submit(peer, frame(peer, tick, actions)) == Admission::Accepted);
    }
};

bool same(const InputCommand& a, const InputCommand& b) {
    return InputPacket::encode(std::span{&a, 1}) == InputPacket::encode(std::span{&b, 1});
}
} // namespace

TEST_CASE("rollback local buffering is negotiated and never predicts a local seat",
          "[rollback-inputs]") {
    const u8 delay = GENERATE(u8{0}, u8{1}, u8{2});
    Fixture fixture({1, 2, 0, 0}, delay);
    auto& inputs = fixture.inputs;
    CHECK(inputs.sampleTick() == 1 + delay);
    CHECK(inputs.confirmedTick() == 0); // Known inputs alone are not executed state.
    for (u64 tick = 1; tick <= 20; ++tick) {
        CAPTURE(delay, tick);
        fixture.remote(2, inputs.sampleTick(), true);
        fixture.sample(true);
        CHECK_FALSE(inputs.sampleReady());
        const auto used = inputs.advance();
        REQUIRE(used);
        CHECK(used->predictedSeats == 0);
        CHECK(used->commands[0].pressed(CommandPress::UsePotion) == (tick > delay));
        CHECK(inputs.confirmedTick() == tick);
    }
    for (u8 buffered = 0; buffered < delay; ++buffered) {
        REQUIRE(inputs.advance());
    }
    CHECK_FALSE(inputs.ready());
    CHECK_FALSE(inputs.advance());
}

TEST_CASE("missing remote inputs retain held aim and throw charging but never replay an edge",
          "[rollback-inputs]") {
    Fixture fixture({1, 2, 0, 0});
    fixture.sample();
    fixture.remote(2, 1, true);
    REQUIRE(fixture.inputs.advance());
    CHECK(fixture.inputs.confirmedTick() == 1);
    for (u64 tick = 2; tick <= 9; ++tick) {
        fixture.sample();
        const auto predicted = fixture.inputs.advance();
        REQUIRE(predicted);
        CHECK(predicted->predictedSeats == 2);
        const auto& command = predicted->commands[1];
        CHECK(command.tick == tick);
        CHECK(command.magnitude == 1);
        CHECK(command.direction == Vec2{1, 0});
        CHECK(command.aimDirection == std::optional{Vec3{0, 0, 1}});
        CHECK(command.held(CommandHeld::Attack));
        CHECK(command.held(CommandHeld::ThrowPotion));
        CHECK(command.pressedButtons == 0);
        CHECK(command.hintTopic == 0);
        CHECK(command.hintRevision == 0);
    }
    CHECK(fixture.inputs.tick() == 10);
    CHECK_FALSE(fixture.inputs.ready());
    CHECK_FALSE(fixture.inputs.advance());
}

TEST_CASE("confirmation requires every seat including multiple seats owned by one peer",
          "[rollback-inputs]") {
    Fixture fixture({1, 2, 2, 3});
    fixture.sample();
    auto partial = fixture.frame(2, 1);
    partial.pop_back();
    CHECK(fixture.inputs.submit(2, partial) == Admission::Invalid);
    fixture.remote(2, 2);
    fixture.remote(3, 2);
    const auto used = fixture.inputs.advance();
    REQUIRE(used);
    CHECK(used->predictedSeats == 14);
    fixture.remote(2, 1);
    CHECK(fixture.inputs.confirmedTick() == 0);
    CHECK_FALSE(fixture.inputs.mismatchTick()); // Actual neutral agrees with guessed neutral.
    fixture.remote(3, 1);
    CHECK(fixture.inputs.confirmedTick() == 1);
    fixture.sample();
    REQUIRE(fixture.inputs.advance());
    CHECK(fixture.inputs.confirmedTick() == 2);
}

TEST_CASE(
    "three and four peer corrections replay one through eight frames with immutable local inputs",
    "[rollback-inputs]") {
    const u8 peers = GENERATE(u8{3}, u8{4});
    const u64 distance = GENERATE(u64{1}, u64{2}, u64{3}, u64{4}, u64{5}, u64{6}, u64{7}, u64{8});
    CAPTURE(peers, distance);
    Fixture fixture({1, 2, 3, static_cast<u8>(peers == 4 ? 4 : 0)});
    for (u64 tick = 1; tick <= distance; ++tick) {
        fixture.sample(tick % 2 != 0);
        REQUIRE(fixture.inputs.advance());
    }
    CHECK(fixture.inputs.confirmedTick() == 0);
    const u64 nextSample = fixture.inputs.sampleTick();
    const auto localEvidence = std::vector<InputCommand>{fixture.inputs.redundantInputs().begin(),
                                                         fixture.inputs.redundantInputs().end()};
    // Reordered/lost packets arrive in a single eight-tick redundant window.
    // Each seat's actual inputs still exist; no late edge is discarded.
    for (u8 peer = 2; peer <= peers; ++peer) {
        std::vector<InputCommand> recovered;
        for (u64 tick = distance; tick > 0; --tick) {
            const auto commands = fixture.frame(peer, tick, tick % 2 != 0);
            recovered.insert(recovered.end(), commands.begin(), commands.end());
        }
        REQUIRE(fixture.inputs.submit(peer, recovered) == Admission::Accepted);
        CHECK(fixture.inputs.submit(peer, recovered) == Admission::Duplicate);
    }
    REQUIRE(fixture.inputs.mismatchTick() == 1);
    CHECK(fixture.inputs.confirmedTick() == 0); // Actual complete, but wrong state not corrected.
    CHECK_FALSE(fixture.inputs.ready());
    CHECK_FALSE(fixture.inputs.rewind(2));
    REQUIRE(fixture.inputs.rewind(1));
    CHECK(fixture.inputs.replaying());
    CHECK_FALSE(fixture.inputs.sampleReady());
    CHECK(fixture.inputs.sampleTick() == nextSample);
    for (u64 tick = 1; tick <= distance; ++tick) {
        const auto corrected = fixture.inputs.advance();
        REQUIRE(corrected);
        CHECK(corrected->predictedSeats == 0);
        for (u8 peer = 1; peer <= peers; ++peer) {
            for (const auto& actual : fixture.frame(peer, tick, tick % 2 != 0)) {
                CHECK(same(corrected->commands[actual.seat], actual));
            }
        }
        CHECK(fixture.inputs.confirmedTick() == tick);
    }
    CHECK_FALSE(fixture.inputs.replaying());
    CHECK_FALSE(fixture.inputs.mismatchTick());
    CHECK(fixture.inputs.sampleTick() == nextSample);
    REQUIRE(fixture.inputs.redundantInputs().size() == localEvidence.size());
    for (usize i = 0; i < localEvidence.size(); ++i) {
        CHECK(same(fixture.inputs.redundantInputs()[i], localEvidence[i]));
    }
}

TEST_CASE(
    "multiple late corrections select the earliest disagreement and preserve later actual evidence",
    "[rollback-inputs]") {
    Fixture fixture({1, 2, 0, 0});
    for (u64 tick = 1; tick <= 8; ++tick) {
        fixture.sample();
        REQUIRE(fixture.inputs.advance());
    }
    fixture.remote(2, 6, true);
    CHECK(fixture.inputs.mismatchTick() == 6);
    fixture.remote(2, 3, true);
    CHECK(fixture.inputs.mismatchTick() == 3);
    REQUIRE(fixture.inputs.rewind(3));
    for (u64 tick = 3; tick <= 8; ++tick) {
        const auto used = fixture.inputs.advance();
        REQUIRE(used);
        CHECK(used->commands[1].held(CommandHeld::Attack));
        CHECK(used->commands[1].pressed(CommandPress::UsePotion) == (tick == 3 || tick == 6));
    }
    CHECK_FALSE(fixture.inputs.ready()); // Ticks 1/2 are still not confirmed.
}

TEST_CASE(
    "rollback admission checks complete batches atomically and detects conflicting actual input",
    "[rollback-inputs]") {
    Fixture fixture({1, 2, 0, 0});
    fixture.remote(2, 1);
    auto conflict = fixture.frame(2, 1, true);
    const auto later = fixture.frame(2, 2);
    conflict.insert(conflict.begin(), later.begin(), later.end());
    CHECK(fixture.inputs.submit(2, conflict) == Admission::Conflict);
    fixture.remote(2, 2); // The rejected batch did not admit its earlier new command.
    CHECK(fixture.inputs.submit(2, fixture.frame(2, 1)) == Admission::Duplicate);
    auto invalid = fixture.frame(2, 3);
    invalid[0].epoch++;
    CHECK(fixture.inputs.submit(2, invalid) == Admission::WrongEpoch);
    invalid = fixture.frame(2, 3);
    invalid[0].grant++;
    CHECK(fixture.inputs.submit(2, invalid) == Admission::WrongGrant);
    CHECK(fixture.inputs.submit(3, fixture.frame(2, 3)) == Admission::WrongPeer);
    CHECK(fixture.inputs.submit(2, fixture.frame(1, 3)) == Admission::WrongPeer);
    CHECK(fixture.inputs.submit(1, fixture.frame(1, 3)) == Admission::WrongPeer);
    invalid = fixture.frame(2, 3);
    invalid[0].direction.x = std::numeric_limits<f32>::quiet_NaN();
    CHECK(fixture.inputs.submit(2, invalid) == Admission::Invalid);
    CHECK(fixture.inputs.submit(2, fixture.frame(2, 0)) == Admission::Invalid);
    CHECK(fixture.inputs.submit(2, fixture.frame(2, std::numeric_limits<u64>::max())) ==
          Admission::TooEarly);
    CHECK(fixture.inputs.submit(2, fixture.frame(2, fixture.inputs.tick() + 121)) ==
          Admission::TooEarly);
    invalid = fixture.frame(2, 3);
    invalid.push_back(invalid.front());
    CHECK(fixture.inputs.submit(2, invalid) == Admission::Invalid);
    auto negativeZero = fixture.frame(2, 1);
    negativeZero[0].direction.x = -0.0f;
    CHECK(fixture.inputs.submit(2, negativeZero) == Admission::Conflict);
}

TEST_CASE("history wraps without losing eight redundant ticks or accepting stale revisions",
          "[rollback-inputs]") {
    Fixture fixture({1, 1, 1, 1});
    for (u64 tick = 1; tick <= 300; ++tick) {
        fixture.sample(tick % 3 == 0);
        REQUIRE(fixture.inputs.advance());
        CHECK(fixture.inputs.confirmedTick() == tick);
    }
    REQUIRE(fixture.inputs.redundantInputs().size() == 32);
    CHECK(fixture.inputs.redundantInputs().front().tick == 293);
    CHECK(fixture.inputs.redundantInputs().back().tick == 300);
    CHECK_FALSE(fixture.inputs.rewind(293)); // Actual all-local frames have no mismatch.

    Fixture remote({1, 2, 0, 0});
    for (u64 tick = 1; tick <= 300; ++tick) {
        remote.sample();
        remote.remote(2, tick);
        REQUIRE(remote.inputs.advance());
    }
    CHECK(remote.inputs.submit(2, remote.frame(2, 1)) == Admission::Stale);
    CHECK(remote.inputs.submit(2, remote.frame(2, 300)) == Admission::Duplicate);
    CHECK(remote.inputs.submit(2, remote.frame(2, 300, true)) == Admission::Conflict);
    CHECK(remote.inputs.confirmedTick() == 300);
}

TEST_CASE("time synchronization follows authenticated simulated progress not future input lead",
          "[rollback-inputs]") {
    Fixture fixture({1, 2, 3, 0});
    // These are received zero-latency reports from peers that remain at tick
    // one, not unknown peers awaiting their first network report.
    REQUIRE(fixture.inputs.observePeerTick(2, kEpoch, 1) == Admission::Duplicate);
    REQUIRE(fixture.inputs.observePeerTick(3, kEpoch, 1) == Admission::Duplicate);
    for (u64 tick = 1; tick <= 4; ++tick) {
        fixture.sample();
        REQUIRE(fixture.inputs.advance());
    }
    REQUIRE(fixture.inputs.timeSyncIdle());
    fixture.remote(2, 100);
    fixture.remote(3, 100);
    CHECK(fixture.inputs.timeSyncIdle());
    CHECK(fixture.inputs.observePeerTick(2, kEpoch, 5) == Admission::Accepted);
    CHECK(fixture.inputs.timeSyncIdle()); // The other peer is still behind.
    CHECK(fixture.inputs.observePeerTick(3, kEpoch, 5) == Admission::Accepted);
    CHECK_FALSE(fixture.inputs.timeSyncIdle());
    CHECK(fixture.inputs.observePeerTick(3, kEpoch, 5) == Admission::Duplicate);
    CHECK(fixture.inputs.observePeerTick(3, kEpoch, 1) == Admission::Stale);
    CHECK(fixture.inputs.observePeerTick(3, kEpoch + 1, 6) == Admission::WrongEpoch);
    CHECK(fixture.inputs.observePeerTick(4, kEpoch, 6) == Admission::WrongPeer);
    CHECK(fixture.inputs.observePeerTick(1, kEpoch, 6) == Admission::WrongPeer);
    CHECK(fixture.inputs.observePeerTick(2, kEpoch, 0) == Admission::Invalid);
    CHECK(fixture.inputs.observePeerTick(2, kEpoch, std::numeric_limits<u64>::max()) ==
          Admission::TooEarly);
    fixture.remote(2, 1, true);
    REQUIRE(fixture.inputs.rewind(1));
    CHECK_FALSE(fixture.inputs.timeSyncIdle()); // Replay does not intentionally idle.
}

TEST_CASE("unknown startup progress permits only the prediction window then a measured peer paces",
          "[rollback-inputs][rollback-pacing]") {
    Fixture fixture({1, 2, 0, 0});
    auto& inputs = fixture.inputs;
    for (u64 tick = 1; tick <= RollbackInputs::kPredictionTicks; ++tick) {
        CHECK_FALSE(inputs.timeSyncIdle());
        fixture.sample();
        REQUIRE(inputs.advance());
    }
    CHECK(inputs.tick() == 9);
    CHECK(inputs.confirmedTick() == 0);
    CHECK_FALSE(inputs.timeSyncIdle());
    fixture.sample();
    CHECK_FALSE(inputs.ready()); // Unknown latency never extends speculation.
    CHECK_FALSE(inputs.advance());

    REQUIRE(inputs.observePeerTick(2, kEpoch, 2, 1) == Admission::Accepted);
    CHECK(inputs.timeSyncIdle()); // The first measured report tightens the allowance.
    for (u64 tick = 1; tick <= 8; ++tick) {
        fixture.remote(2, tick);
    }
    CHECK(inputs.confirmedTick() == 8);
    CHECK(inputs.ready());
    CHECK(inputs.timeSyncIdle()); // Buffered input does not mean a slow peer simulated it.
    REQUIRE(inputs.observePeerTick(2, kEpoch, 9, 1) == Admission::Accepted);
    CHECK_FALSE(inputs.timeSyncIdle());
    REQUIRE(inputs.advance());
    CHECK(inputs.tick() == 10);
}

TEST_CASE("a stalled peer can retransmit oldest missing input beyond the latest redundant window",
          "[rollback-inputs]") {
    Fixture sender({1, 2, 0, 0}, 2);
    for (u64 tick = 1; tick <= 10; ++tick) {
        sender.sample(true);
        REQUIRE(sender.inputs.advance());
    }
    CHECK(sender.inputs.confirmedTick() == 2);
    CHECK_FALSE(sender.inputs.ready());
    REQUIRE(sender.inputs.redundantInputs().size() == 8);
    CHECK(sender.inputs.redundantInputs().front().tick == 5);

    RollbackInputs receiver;
    REQUIRE(receiver.open(2, kEpoch, sender.owners, sender.grants, 2));
    const auto recovery = sender.inputs.resendInputs(3);
    REQUIRE(recovery.size() == 8);
    CHECK(recovery.front().tick == 3);
    CHECK(recovery.back().tick == 10);
    REQUIRE(receiver.submit(1, recovery) == Admission::Accepted);
    CHECK(receiver.submit(1, recovery) == Admission::Duplicate);
    for (u64 tick = 1; tick <= 10; ++tick) {
        REQUIRE(receiver.sample(sender.frame(2, receiver.sampleTick())));
        const auto used = receiver.advance();
        REQUIRE(used);
        CHECK(used->predictedSeats == 0);
        CHECK(used->commands[0].pressed(CommandPress::UsePotion) == (tick > 2));
        CHECK(receiver.confirmedTick() == tick);
    }
    const auto following = sender.inputs.resendInputs(11);
    REQUIRE(following.size() == 2);
    CHECK(following.back().tick == 12);
    CHECK(sender.inputs.resendInputs(0).empty());
    CHECK(sender.inputs.resendInputs(13).empty());
}

TEST_CASE("equal rate peers allow in-flight progress age instead of throttling one another",
          "[rollback-inputs]") {
    // Eighty milliseconds is roughly three 30 Hz or five 60 Hz simulation ticks.
    // Both inputs and progress are delayed, without fabricating future input lead.
    const u8 transitTicks = GENERATE(u8{3}, u8{5});
    Fixture first({1, 2, 0, 0});
    RollbackInputs second;
    REQUIRE(second.open(2, kEpoch, first.owners, first.grants, 0));
    for (u64 tick = 1; tick <= 30; ++tick) {
        CAPTURE(transitTicks, tick);
        const u64 delivered = tick > transitTicks ? tick - transitTicks : 1;
        const auto firstProgress = first.inputs.observePeerTick(2, kEpoch, delivered, transitTicks);
        const auto secondProgress = second.observePeerTick(1, kEpoch, delivered, transitTicks);
        REQUIRE((firstProgress == Admission::Accepted || firstProgress == Admission::Duplicate));
        REQUIRE(secondProgress == firstProgress);
        if (tick > transitTicks) {
            first.remote(2, delivered);
            REQUIRE(second.submit(1, first.frame(1, delivered)) == Admission::Accepted);
        }
        CHECK_FALSE(first.inputs.timeSyncIdle());
        CHECK_FALSE(second.timeSyncIdle());
        first.sample();
        REQUIRE(second.sample(first.frame(2, second.sampleTick())));
        REQUIRE(first.inputs.advance());
        REQUIRE(second.advance());
    }
    // Fresh transport latency is not a license to run arbitrarily ahead.
    for (u64 extra = 0; extra < 2; ++extra) {
        first.sample();
        REQUIRE(first.inputs.advance());
    }
    CHECK(first.inputs.timeSyncIdle());
    CHECK(first.inputs.observePeerTick(2, kEpoch, first.inputs.tick(), 9) == Admission::Invalid);
    CHECK(first.inputs.timeSyncIdle());
    CHECK(first.inputs.observePeerTick(2, kEpoch, 1, 8) == Admission::Stale);
    CHECK(first.inputs.timeSyncIdle());
    CHECK(first.inputs.observePeerTick(2, kEpoch, first.inputs.tick() - transitTicks,
                                       transitTicks) == Admission::Accepted);
    CHECK_FALSE(first.inputs.timeSyncIdle());
}

TEST_CASE("rollback startup validates frozen roster and cannot reopen an existing epoch owner",
          "[rollback-inputs]") {
    RollbackInputs inputs;
    CHECK_FALSE(inputs.ready());
    CHECK_FALSE(inputs.sampleReady());
    CHECK_FALSE(inputs.advance());
    CHECK_FALSE(inputs.rewind(1));
    CHECK(inputs.confirmedTick() == 0);
    const RollbackInputs::Owners owners{1, 2, 0, 0};
    const RollbackInputs::Grants grants{1, 1, 0, 0};
    CHECK_FALSE(inputs.open(0, kEpoch, owners, grants));
    CHECK_FALSE(inputs.open(3, kEpoch, owners, grants));
    CHECK_FALSE(inputs.open(1, 0, owners, grants));
    CHECK_FALSE(inputs.open(1, kEpoch, owners, grants, 3));
    CHECK_FALSE(inputs.open(1, kEpoch, owners, {1, 0, 0, 0}));
    CHECK_FALSE(inputs.open(1, kEpoch, {1, 5, 0, 0}, grants));
    REQUIRE(inputs.open(1, kEpoch, owners, grants));
    CHECK_FALSE(inputs.open(1, kEpoch + 1, owners, grants));
}

TEST_CASE("an earlier terminal truncates only active replay and preserves sampled future inputs",
          "[rollback-inputs][rollback-terminal]") {
    Fixture fixture({1, 2, 3, 0});
    auto& inputs = fixture.inputs;
    CHECK_FALSE(inputs.finishReplayAt(1));
    for (u64 tick = 1; tick <= 8; ++tick) {
        fixture.sample(tick % 2 == 0);
        REQUIRE(inputs.advance());
    }
    const auto before = inputs.resendInputs(1);
    const auto sampleCursor = inputs.sampleTick();
    fixture.remote(2, 3, true);
    REQUIRE(inputs.rewind(3));
    CHECK_FALSE(inputs.finishReplayAt(3)); // A terminal needs an actually replayed frame.
    REQUIRE(inputs.advance());
    CHECK_FALSE(inputs.finishReplayAt(3));
    CHECK_FALSE(inputs.finishReplayAt(5));
    REQUIRE(inputs.finishReplayAt(4));
    CHECK_FALSE(inputs.replaying());
    CHECK_FALSE(inputs.finishReplayAt(4));
    CHECK(inputs.tick() == 4);
    CHECK(inputs.sampleTick() == sampleCursor);
    CHECK_FALSE(inputs.sampleReady()); // Already sampled input cannot be polled again.
    const auto after = inputs.resendInputs(1);
    REQUIRE(after.size() == before.size());
    for (usize i = 0; i < before.size(); ++i) {
        CHECK(same(before[i], after[i]));
    }
    fixture.remote(2, 6, true);
    CHECK_FALSE(inputs.mismatchTick()); // Discarded speculative tail is no longer executed.
    fixture.remote(3, 2, true);         // An earlier correction can remove the tentative terminal.
    REQUIRE(inputs.mismatchTick() == 2);
    REQUIRE(inputs.rewind(2));
    for (u64 tick = 2; tick <= 8; ++tick) {
        const auto frame = inputs.advance();
        REQUIRE(frame);
        CHECK(same(frame->commands[0], fixture.frame(1, tick, tick % 2 == 0).front()));
        if (tick == 6) {
            CHECK(frame->commands[1].pressed(CommandPress::UsePotion));
        }
    }
    CHECK(inputs.sampleTick() == sampleCursor);
    CHECK(inputs.sampleReady());
}
