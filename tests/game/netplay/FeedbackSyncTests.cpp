#include <array>
#include <limits>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "game/netplay/FeedbackSync.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Admission = FeedbackSync::Admission;

struct FeedbackPeers {
    static constexpr std::array<u8, 4> kOwners{1, 2, 3, 2};
    static constexpr std::array<u32, 4> kGrants{11, 22, 33, 44};
    FeedbackSync host;
    FeedbackSync guest;
    FeedbackSync other;
    FeedbackPeers() {
        host.begin(7, 1, kOwners, kGrants);
        guest.begin(7, 2, kOwners, kGrants);
        other.begin(7, 3, kOwners, kGrants);
    }
    static void relay(FeedbackSync& from, FeedbackSync& to, u8 source, u8 target) {
        while (!from.outgoing().empty()) {
            REQUIRE(from.outgoing().front().peer == target);
            REQUIRE(to.receive(source, from.outgoing().front().bytes) == Admission::Accepted);
            from.sent();
        }
    }
};

TEST_CASE("host feedback routes every cue only to the owning peer and room seat",
          "[netplay][feedback-sync][rumble]") {
    FeedbackPeers peers;
    for (const auto cue : {ControlFeedback::Damage, ControlFeedback::MeleeHit,
                           ControlFeedback::GeneratorDestroyed, ControlFeedback::ThunderHammer}) {
        REQUIRE(peers.host.emit(0, 30, cue));
        REQUIRE(peers.host.take() == FeedbackEvent{0, 30, cue});
        CHECK(peers.host.outgoing().empty());
        REQUIRE(peers.host.emit(3, 10, cue));
        REQUIRE(peers.host.outgoing().size() == 1);
        const auto bytes = peers.host.outgoing().front().bytes;
        CHECK(peers.host.outgoing().front().peer == 2);
        CHECK(peers.other.receive(1, bytes) == Admission::WrongPeer);
        CHECK(peers.guest.receive(3, bytes) == Admission::WrongPeer);
        CHECK(peers.guest.receive(1, bytes) == Admission::Accepted);
        CHECK(peers.guest.receive(1, bytes) == Admission::Stale);
        REQUIRE(peers.guest.take() == FeedbackEvent{3, 10, cue});
        CHECK_FALSE(peers.guest.take());
        CHECK_FALSE(peers.other.take());
        CHECK_FALSE(peers.host.take());
        CHECK_FALSE(peers.guest.emit(3, 10, cue));
        CHECK(peers.host.receive(2, bytes) == Admission::WrongPeer);
        peers.host.sent();
    }
    CHECK_FALSE(peers.host.setEnabled(1, false));
    CHECK_FALSE(peers.guest.setEnabled(2, false));
    CHECK_FALSE(peers.guest.setEnabled(0, false));
}

TEST_CASE("feedback packets reject invalid bounds and spoofed ownership before mutation",
          "[netplay][feedback-sync][rumble]") {
    FeedbackPeers peers;
    for (const s32 seat : {-1, 4, std::numeric_limits<s32>::max()}) {
        CHECK_FALSE(peers.host.emit(seat, 10, ControlFeedback::Damage));
        CHECK_FALSE(peers.guest.setEnabled(seat, false));
    }
    for (const s32 frames : {-1, 31, std::numeric_limits<s32>::max()}) {
        CHECK_FALSE(peers.host.emit(1, frames, ControlFeedback::Damage));
    }
    CHECK_FALSE(peers.host.emit(1, 10, static_cast<ControlFeedback>(255)));
    CHECK(peers.host.outgoing().empty());
    REQUIRE(peers.host.emit(1, 10, ControlFeedback::Damage));
    const auto valid = peers.host.outgoing().front().bytes;
    REQUIRE(valid.size() == FeedbackSync::kPacketBytes);
    CHECK(FeedbackSync::recognizes(valid));
    for (usize size = 0; size < valid.size(); ++size) {
        CHECK(peers.guest.receive(1, std::span{valid}.first(size)) == Admission::Invalid);
    }
    auto oversized = valid;
    oversized.push_back(0);
    CHECK(peers.guest.receive(1, oversized) == Admission::Invalid);
    for (const auto& [offset, value] : std::array<std::pair<usize, u8>, 9>{
             {{0, 0}, {4, 2}, {5, 255}, {6, 1}, {16, 4}, {17, 4}, {18, 31}, {19, 1}, {32, 0}}}) {
        auto invalid = valid;
        invalid[offset] = value;
        CHECK(peers.guest.receive(1, invalid) == Admission::Invalid);
    }
    auto invalid = valid;
    invalid[24] = 0; // A zero generation is never valid.
    CHECK(peers.guest.receive(1, invalid) == Admission::Invalid);
    invalid = valid;
    invalid[20] ^= 1; // Correct seat but a different match grant.
    CHECK(peers.guest.receive(1, invalid) == Admission::WrongPeer);
    invalid = valid;
    invalid[16] = 2; // Cannot target another guest's seat.
    CHECK(peers.guest.receive(1, invalid) == Admission::WrongPeer);
    CHECK(peers.guest.receive(0, valid) == Admission::WrongPeer);
    CHECK_FALSE(peers.guest.take());
    CHECK(peers.guest.receive(1, valid) == Admission::Accepted);
    REQUIRE(peers.guest.take() == FeedbackEvent{1, 10, ControlFeedback::Damage});

    REQUIRE(peers.guest.setEnabled(1, false));
    const auto gate = peers.guest.outgoing().front().bytes;
    invalid = gate;
    invalid[16] = 2;
    CHECK(peers.host.receive(2, invalid) == Admission::WrongPeer);
    invalid = gate;
    invalid[20] ^= 1;
    CHECK(peers.host.receive(2, invalid) == Admission::WrongPeer);
    for (const usize offset : {usize{18}, usize{32}}) {
        invalid = gate;
        invalid[offset] = 1;
        CHECK(peers.host.receive(2, invalid) == Admission::Invalid);
    }
    invalid = gate;
    invalid[17] = 2;
    CHECK(peers.host.receive(2, invalid) == Admission::Invalid);
    CHECK(peers.other.receive(2, gate) == Admission::WrongPeer);
    CHECK(peers.host.receive(2, gate) == Admission::Accepted);
    CHECK(peers.host.receive(2, gate) == Admission::Stale);
}

TEST_CASE("local feedback pauses discard pending and in-flight cues across resume",
          "[netplay][feedback-sync][rumble]") {
    FeedbackPeers peers;
    REQUIRE(peers.host.emit(1, 30, ControlFeedback::Damage));
    const auto inFlight = peers.host.outgoing().front().bytes;
    CHECK(peers.guest.receive(1, inFlight) == Admission::Accepted);
    REQUIRE(peers.guest.setEnabled(1, false));
    CHECK_FALSE(peers.guest.take()); // Already received but not yet presented.
    const auto pauseGate = peers.guest.outgoing().front().bytes;
    FeedbackPeers::relay(peers.guest, peers.host, 2, 1);
    CHECK(peers.host.outgoing().empty()); // Not-yet-sent pulse was discarded too.
    REQUIRE(peers.host.emit(1, 30, ControlFeedback::ThunderHammer));
    CHECK(peers.host.outgoing().empty());
    CHECK(peers.guest.receive(1, inFlight) == Admission::Stale);
    REQUIRE(peers.guest.setEnabled(1, true));
    CHECK(peers.guest.receive(1, inFlight) == Admission::Stale); // Late after resume.
    FeedbackPeers::relay(peers.guest, peers.host, 2, 1);
    CHECK(peers.host.receive(2, pauseGate) == Admission::Stale);
    REQUIRE(peers.host.emit(1, 30, ControlFeedback::ThunderHammer));
    FeedbackPeers::relay(peers.host, peers.guest, 1, 2);
    REQUIRE(peers.guest.take() == FeedbackEvent{1, 30, ControlFeedback::ThunderHammer});
    CHECK_FALSE(peers.guest.take());

    REQUIRE(peers.host.emit(0, 15, ControlFeedback::Damage));
    REQUIRE(peers.host.setEnabled(0, false));
    CHECK_FALSE(peers.host.take());
    REQUIRE(peers.host.emit(0, 15, ControlFeedback::Damage));
    CHECK_FALSE(peers.host.take());
    REQUIRE(peers.host.setEnabled(0, true));
    CHECK_FALSE(peers.host.take());
    REQUIRE(peers.host.emit(0, 0, ControlFeedback::MeleeHit));
    REQUIRE(peers.host.take() == FeedbackEvent{0, 0, ControlFeedback::MeleeHit});
}

TEST_CASE("feedback gates coalesce while preserving other local seats",
          "[netplay][feedback-sync][rumble]") {
    FeedbackPeers peers;
    REQUIRE(peers.host.emit(1, 10, ControlFeedback::Damage));
    const auto oldPulse = peers.host.outgoing().front().bytes;
    peers.host.sent();
    REQUIRE(peers.host.emit(3, 0, ControlFeedback::GeneratorDestroyed));
    FeedbackPeers::relay(peers.host, peers.guest, 1, 2);
    for (s32 repeat = 0; repeat < 1000; ++repeat) {
        REQUIRE(peers.guest.setEnabled(1, false));
        REQUIRE(peers.guest.setEnabled(1, true));
        CHECK(peers.guest.outgoing().size() == 1);
    }
    CHECK(peers.guest.receive(1, oldPulse) == Admission::Stale);
    REQUIRE(peers.guest.take() == FeedbackEvent{3, 0, ControlFeedback::GeneratorDestroyed});
    FeedbackPeers::relay(peers.guest, peers.host, 2, 1);
    REQUIRE(peers.host.emit(1, 10, ControlFeedback::Damage));
    FeedbackPeers::relay(peers.host, peers.guest, 1, 2);
    REQUIRE(peers.guest.take() == FeedbackEvent{1, 10, ControlFeedback::Damage});
}

TEST_CASE("feedback congestion retains bounded recent cues without requiring skipped sequences",
          "[netplay][feedback-sync][rumble]") {
    FeedbackPeers peers;
    for (usize i = 0; i < FeedbackSync::kBudget * 2; ++i) {
        REQUIRE(peers.host.emit(1, static_cast<s32>(i % 31), ControlFeedback::Damage));
        REQUIRE(peers.host.emit(0, static_cast<s32>(i % 31), ControlFeedback::Damage));
        CHECK(peers.host.outgoing().size() <= FeedbackSync::kBudget);
    }
    REQUIRE(peers.host.outgoing().size() == FeedbackSync::kBudget);
    FeedbackPeers::relay(peers.host, peers.guest, 1, 2);
    for (usize i = FeedbackSync::kBudget; i < FeedbackSync::kBudget * 2; ++i) {
        REQUIRE(peers.host.take() ==
                FeedbackEvent{0, static_cast<s32>(i % 31), ControlFeedback::Damage});
        REQUIRE(peers.guest.take() ==
                FeedbackEvent{1, static_cast<s32>(i % 31), ControlFeedback::Damage});
    }
    CHECK_FALSE(peers.host.take());
    CHECK_FALSE(peers.guest.take());
    // A slow consumer is bounded even when transport itself never congests.
    for (usize i = 0; i < FeedbackSync::kBudget * 2; ++i) {
        REQUIRE(peers.host.emit(1, 0, ControlFeedback::MeleeHit));
        FeedbackPeers::relay(peers.host, peers.guest, 1, 2);
    }
    usize pending = 0;
    while (peers.guest.take()) {
        ++pending;
    }
    CHECK(pending == FeedbackSync::kBudget);
}

TEST_CASE("travel and disconnect reset all feedback queues gates and replay history",
          "[netplay][feedback-sync][rumble]") {
    FeedbackPeers peers;
    REQUIRE(peers.host.emit(1, 30, ControlFeedback::Damage));
    const auto oldPulse = peers.host.outgoing().front().bytes;
    FeedbackPeers::relay(peers.host, peers.guest, 1, 2);
    REQUIRE(peers.guest.setEnabled(3, false));
    peers.host.begin(8, 1, FeedbackPeers::kOwners, FeedbackPeers::kGrants);
    peers.guest.begin(8, 2, FeedbackPeers::kOwners, FeedbackPeers::kGrants);
    CHECK(peers.host.outgoing().empty());
    CHECK(peers.guest.outgoing().empty());
    CHECK_FALSE(peers.guest.take());
    CHECK(peers.guest.receive(1, oldPulse) == Admission::Stale);
    REQUIRE(peers.host.emit(3, 0, ControlFeedback::GeneratorDestroyed));
    FeedbackPeers::relay(peers.host, peers.guest, 1, 2);
    REQUIRE(peers.guest.take() == FeedbackEvent{3, 0, ControlFeedback::GeneratorDestroyed});
    REQUIRE(peers.host.emit(0, 30, ControlFeedback::Damage));
    REQUIRE(peers.host.emit(1, 30, ControlFeedback::Damage));
    peers.host.clear();
    peers.guest.clear();
    CHECK_FALSE(peers.host.take());
    CHECK_FALSE(peers.guest.take());
    CHECK(peers.host.outgoing().empty());
    CHECK(peers.guest.outgoing().empty());
    CHECK_FALSE(peers.host.emit(0, 30, ControlFeedback::Damage));
    CHECK_FALSE(peers.guest.setEnabled(1, true));
    CHECK(peers.guest.receive(1, oldPulse) != Admission::Accepted);
    peers.host.begin(0, 1, FeedbackPeers::kOwners, FeedbackPeers::kGrants);
    CHECK_FALSE(peers.host.emit(0, 30, ControlFeedback::Damage));
}
} // namespace
