#include <array>
#include <limits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/netplay/PredictedShotLedger.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Status = PredictedShotLedger::Status;
ShotIdentity shot(u64 tick, u64 generation = 1, u8 seat = 1, u32 grant = 11, u64 epoch = 7) {
    return {epoch, tick, generation, grant, seat, false};
}
PredictedShotLedger ledgerAt(u64 tick = 10) {
    PredictedShotLedger ledger;
    REQUIRE(ledger.begin(7, {0, 11, 22, 0}));
    REQUIRE(ledger.observe(7, tick, {}));
    return ledger;
}

TEST_CASE("predicted releases reconcile each native shot once across all visual components",
          "[netplay][predicted-shot-ledger]") {
    auto ledger = ledgerAt();
    const auto first = shot(14, 8);
    const auto second = shot(17, 12);
    const std::array releases{first, second};
    REQUIRE(ledger.replace(1, 10, 20, releases));
    REQUIRE(ledger.entries().size() == 2);
    const auto firstId = ledger.entries().at(first).instance;
    CHECK((firstId & PredictedShotLedger::kPrivateInstance) != 0);
    CHECK(ledger.entries().at(second).instance != firstId);
    CHECK(ledger.entries().at(first).status == Status::Pending);
    CHECK(ledger.suppress(first));
    REQUIRE(ledger.replace(1, 10, 21, releases));
    CHECK(ledger.entries().at(first).instance == firstId);
    const std::array components{first, first, first}; // body, streak, elemental visual
    REQUIRE(ledger.observe(7, 14, components));
    CHECK(ledger.entries().size() == 2);
    CHECK(ledger.entries().at(first).status == Status::Confirmed);
    CHECK(ledger.entries().at(first).instance == firstId);
    CHECK(ledger.entries().at(second).status == Status::Pending);
    REQUIRE(ledger.replace(1, 14, 21, std::span{releases}.last(1)));
    CHECK(ledger.entries().at(first).status == Status::Confirmed);
    CHECK(ledger.suppress(first));
    REQUIRE(ledger.observe(7, 17, releases));
    CHECK(ledger.entries().at(second).status == Status::Confirmed);
    CHECK(ledger.entries().size() == 2);
}

TEST_CASE("missing authoritative release deadlines withdraw even unsampled short lived shots",
          "[netplay][predicted-shot-ledger]") {
    auto ledger = ledgerAt();
    const auto release = shot(14);
    REQUIRE(ledger.replace(1, 10, 20, std::array{release}));
    REQUIRE(ledger.observe(7, 13, {}));
    CHECK(ledger.entries().at(release).status == Status::Pending);
    REQUIRE(ledger.observe(7, 16, {})); // release and possible wall impact between snapshots
    CHECK(ledger.entries().at(release).status == Status::Retired);
    CHECK(ledger.entries().at(release).retiredAt == 16);
    CHECK(ledger.suppress(release));
    CHECK_FALSE(ledger.replace(1, 16, 20, std::array{release}));
    CHECK_FALSE(ledger.observe(7, 14, std::array{release}));
    CHECK_FALSE(ledger.observe(7, 16, std::array{release}));
    CHECK(ledger.entries().at(release).status == Status::Retired);
    REQUIRE(ledger.advanceDisplayed(7, 13));
    CHECK(ledger.suppress(release));
    REQUIRE(ledger.advanceDisplayed(7, 16));
    CHECK(ledger.suppress(release));
    REQUIRE(ledger.observe(7, 17, {}));
    REQUIRE(ledger.advanceDisplayed(7, 17));
    CHECK_FALSE(ledger.suppress(release));
    CHECK(ledger.entries().empty());
}

TEST_CASE("turbo volley emissions in one native tick reconcile independently",
          "[netplay][predicted-shot-ledger][turbo-volley-identity]") {
    auto ledger = ledgerAt();
    auto ordinary = shot(14, 8);
    auto first = ordinary;
    first.emission = 0x00010000;
    auto second = first;
    second.emission = 0x00010001;
    auto otherSlot = first;
    otherSlot.emission = 0x00020000;
    const std::array releases{ordinary, first, second, otherSlot};
    REQUIRE(ledger.replace(1, 10, 20, releases));
    REQUIRE(ledger.entries().size() == 4);
    for (usize i = 0; i < releases.size(); ++i) {
        for (usize j = 0; j < releases.size(); ++j) {
            CHECK((ledger.entries().at(releases[i]).instance ==
                   ledger.entries().at(releases[j]).instance) == (i == j));
        }
    }
    REQUIRE(ledger.observe(7, 14, std::array{first, first, otherSlot}));
    CHECK(ledger.entries().at(first).status == Status::Confirmed);
    CHECK(ledger.entries().at(otherSlot).status == Status::Confirmed);
    CHECK(ledger.entries().at(second).status == Status::Retired);
    CHECK(ledger.entries().at(ordinary).status == Status::Retired);
    REQUIRE(ledger.observe(7, 15, std::array{otherSlot}));
    CHECK(ledger.entries().at(first).status == Status::Retired);
    CHECK(ledger.suppress(first));
    CHECK(ledger.suppress(second));
    CHECK_FALSE(ledger.observe(7, 14, releases));
    REQUIRE(ledger.advanceDisplayed(7, 15));
    CHECK(ledger.suppress(otherSlot));
    CHECK_FALSE(ledger.suppress(second));
}

TEST_CASE("latest host projectile removal cannot be resurrected by the delayed render buffer",
          "[netplay][predicted-shot-ledger]") {
    auto ledger = ledgerAt();
    const auto release = shot(14);
    REQUIRE(ledger.observe(7, 16, std::array{release})); // first seen on host, no prediction
    REQUIRE(ledger.entries().size() == 1);
    CHECK(ledger.entries().at(release).status == Status::Confirmed);
    CHECK(ledger.suppress(release));
    REQUIRE(ledger.observe(7, 20, {}));
    CHECK(ledger.entries().at(release).status == Status::Retired);
    REQUIRE(ledger.advanceDisplayed(7, 16));
    CHECK(ledger.suppress(release)); // root must not redraw the old still-live host copy
    CHECK_FALSE(ledger.observe(7, 19, std::array{release}));
    CHECK_FALSE(ledger.advanceDisplayed(7, 15));
    CHECK_FALSE(ledger.advanceDisplayed(7, 21));
    CHECK_FALSE(ledger.advanceDisplayed(8, 20));
    CHECK(ledger.suppress(release));
}

TEST_CASE("replaying cancelled and retimed attacks replaces keys rather than duplicating shots",
          "[netplay][predicted-shot-ledger]") {
    auto ledger = ledgerAt();
    const auto original = shot(14, 9);
    const auto retimed = shot(15, 9);
    REQUIRE(ledger.replace(1, 10, 20, std::array{original}));
    const auto originalId = ledger.entries().at(original).instance;
    REQUIRE(ledger.replace(1, 10, 20, std::array{retimed}));
    CHECK(ledger.entries().at(original).status == Status::Retired);
    CHECK(ledger.entries().at(retimed).status == Status::Pending);
    CHECK(ledger.entries().at(retimed).instance != originalId);
    // A later replay can restore the same unresolved native release, without new IDs.
    REQUIRE(ledger.replace(1, 10, 20, std::array{original}));
    CHECK(ledger.entries().at(original).status == Status::Pending);
    CHECK(ledger.entries().at(original).instance == originalId);
    CHECK(ledger.entries().at(retimed).status == Status::Retired);
    REQUIRE(ledger.replace(1, 10, 20, {}));
    CHECK(ledger.entries().at(original).status == Status::Retired);
    REQUIRE(ledger.observe(7, 14, std::array{original}));
    CHECK(ledger.entries().at(original).status == Status::Confirmed); // host wins the race
    CHECK(ledger.entries().at(retimed).status == Status::Retired);
}

TEST_CASE("local seats grants and match epochs never reconcile each others weapon releases",
          "[netplay][predicted-shot-ledger]") {
    auto ledger = ledgerAt();
    const auto first = shot(14, 9);
    const auto second = shot(14, 9, 2, 22);
    const auto remote = shot(14, 9, 0, 1);
    REQUIRE(ledger.replace(1, 10, 20, std::array{first}));
    REQUIRE(ledger.replace(2, 10, 20, std::array{second}));
    ledger.withdraw(1); // player one opens a menu or loses prediction eligibility
    CHECK(ledger.entries().at(first).status == Status::Retired);
    CHECK(ledger.entries().at(second).status == Status::Pending);
    REQUIRE(ledger.observe(7, 15, std::array{first, second, remote}));
    ledger.withdraw(1);
    CHECK(ledger.entries().at(first).status == Status::Confirmed);
    CHECK(ledger.entries().at(second).status == Status::Confirmed);
    CHECK_FALSE(ledger.suppress(remote));
    const auto secondId = ledger.entries().at(second).instance;
    REQUIRE(ledger.begin(7, {0, 12, 22, 0}));
    CHECK_FALSE(ledger.suppress(first));
    CHECK(ledger.entries().at(second).instance == secondId);
    REQUIRE(ledger.observe(7, 16, std::array{first, second, remote}));
    CHECK_FALSE(ledger.suppress(first));
    REQUIRE(ledger.begin(8, {0, 12, 22, 0}));
    CHECK(ledger.entries().empty());
    CHECK_FALSE(ledger.resolved());
    CHECK_FALSE(ledger.observe(7, 100, std::array{first}));
    CHECK_FALSE(ledger.begin(0, {}));
    CHECK(ledger.epoch() == 8);
    ledger.clear();
    CHECK(ledger.epoch() == 0);
    CHECK_FALSE(ledger.observe(0, 100, {}));
}

TEST_CASE("malformed partial or duplicate replay frames do not change existing prediction",
          "[netplay][predicted-shot-ledger]") {
    auto ledger = ledgerAt();
    const auto release = shot(14);
    REQUIRE(ledger.replace(1, 10, 20, std::array{release}));
    const auto id = ledger.entries().at(release).instance;
    CHECK_FALSE(ledger.replace(1, 9, 20, {}));
    CHECK_FALSE(ledger.replace(1, 10, 9, {}));
    CHECK_FALSE(ledger.replace(4, 10, 20, {}));
    CHECK_FALSE(ledger.replace(0, 10, 20, {}));
    CHECK_FALSE(ledger.replace(1, 10, 20, std::array{release, release}));
    CHECK_FALSE(ledger.replace(1, 10, 20, std::array{shot(10)}));
    CHECK_FALSE(ledger.replace(1, 10, 20, std::array{shot(21)}));
    CHECK_FALSE(ledger.replace(1, 10, 20, std::array{shot(14, 1, 2, 22)}));
    CHECK_FALSE(ledger.replace(1, 10, 20, std::array{shot(14, 1, 1, 12)}));
    CHECK_FALSE(ledger.observe(7, 13, std::array{release}));
    CHECK_FALSE(ledger.observe(7, 15, std::array{shot(14, 0)}));
    CHECK_FALSE(ledger.observe(7, 15, std::array{shot(14, 1, 1, 11, 8)}));
    CHECK(ledger.entries().size() == 1);
    CHECK(ledger.entries().at(release).instance == id);
    CHECK(ledger.entries().at(release).status == Status::Pending);
    CHECK(ledger.resolved() == 10);
    CHECK_FALSE(ledger.failed());
}

TEST_CASE("long loss gaps retain pending releases until authority or control revocation",
          "[netplay][predicted-shot-ledger]") {
    auto ledger = ledgerAt();
    const auto release = shot(14);
    for (u64 endTick = 15; endTick <= 130; ++endTick) {
        REQUIRE(ledger.replace(1, 10, endTick, std::array{release}));
        CHECK(ledger.entries().size() == 1);
        CHECK(ledger.entries().at(release).status == Status::Pending);
    }
    ledger.withdraw(1); // caller's stale-authority deadline expires
    CHECK(ledger.entries().at(release).status == Status::Retired);
    CHECK(ledger.entries().at(release).retiredAt == 14);
    REQUIRE(ledger.observe(7, 100, {}));
    CHECK(ledger.entries().at(release).status == Status::Retired);
    REQUIRE(ledger.advanceDisplayed(7, 100));
    CHECK(ledger.entries().empty());
}

TEST_CASE("prediction history overflow fails closed instead of silently evicting unresolved shots",
          "[netplay][predicted-shot-ledger]") {
    auto ledger = ledgerAt();
    std::vector<ShotIdentity> releases;
    for (u64 index = 0; index < PredictedShotLedger::kCapacity; ++index) {
        releases.push_back(shot(14, index + 1));
    }
    REQUIRE(ledger.replace(1, 10, 20, releases));
    REQUIRE(ledger.entries().size() == PredictedShotLedger::kCapacity);
    CHECK_FALSE(ledger.replace(2, 10, 20, std::array{shot(14, 1, 2, 22)}));
    CHECK(ledger.failed());
    CHECK(ledger.entries().empty());
    CHECK_FALSE(ledger.suppress(releases.front()));
    CHECK_FALSE(ledger.observe(7, 20, releases));
    CHECK_FALSE(ledger.begin(7, {0, 11, 22, 0}));
    REQUIRE(ledger.begin(8, {0, 11, 22, 0}));
    CHECK_FALSE(ledger.failed());
    REQUIRE(ledger.observe(8, 10, {}));
    CHECK_FALSE(ledger.replace(1, 10, 10 + PredictedShotLedger::kHorizon + 1, {}));
    CHECK(ledger.failed());
}

TEST_CASE("four full-horizon fans retain corrected identities until the render cursor passes",
          "[netplay][predicted-shot-ledger][spread-prediction]") {
    constexpr u64 kBaseline = 10;
    constexpr u64 kEnd = kBaseline + PredictedShotLedger::kHorizon;
    constexpr PredictedShotLedger::Grants kGrants{11, 22, 33, 44};
    PredictedShotLedger ledger;
    REQUIRE(ledger.begin(7, kGrants));
    REQUIRE(ledger.observe(7, kBaseline, {}));
    std::array<std::vector<ShotIdentity>, InputCommand::kSeats> original;
    std::array<std::vector<ShotIdentity>, InputCommand::kSeats> corrected;
    for (u8 seat = 0; seat < InputCommand::kSeats; ++seat) {
        for (u64 tick = kBaseline + 1; tick <= kEnd; ++tick) {
            for (u8 ordinal = 0; ordinal < ShotIdentity::kMaxShots; ++ordinal) {
                auto identity = shot(tick, tick, seat, kGrants[seat]);
                identity.ordinal = ordinal;
                original[seat].push_back(identity);
                identity.generation += PredictedShotLedger::kHorizon;
                corrected[seat].push_back(identity);
            }
        }
        REQUIRE(ledger.replace(seat, kBaseline, kEnd, original[seat]));
    }
    REQUIRE(ledger.entries().size() == PredictedShotLedger::kCapacity / 2);
    for (u8 seat = 0; seat < InputCommand::kSeats; ++seat) {
        REQUIRE(ledger.replace(seat, kBaseline, kEnd, corrected[seat]));
    }
    REQUIRE(ledger.entries().size() == PredictedShotLedger::kCapacity);
    CHECK_FALSE(ledger.failed());
    for (u8 seat = 0; seat < InputCommand::kSeats; ++seat) {
        for (const auto& identity : original[seat]) {
            CHECK(ledger.entries().at(identity).status == Status::Retired);
            CHECK(ledger.suppress(identity));
        }
        for (const auto& identity : corrected[seat]) {
            CHECK(ledger.entries().at(identity).status == Status::Pending);
        }
    }
    std::vector<ShotIdentity> surviving;
    for (const auto& fan : corrected) {
        surviving.insert(surviving.end(), fan.end() - ShotIdentity::kMaxShots, fan.end());
    }
    REQUIRE(ledger.observe(7, kEnd, surviving));
    for (const auto& identity : surviving) {
        CHECK(ledger.entries().at(identity).status == Status::Confirmed);
    }
    REQUIRE(ledger.advanceDisplayed(7, kBaseline));
    CHECK(ledger.entries().size() == PredictedShotLedger::kCapacity);
    REQUIRE(ledger.observe(7, kEnd + 1, surviving));
    REQUIRE(ledger.advanceDisplayed(7, kEnd + 1));
    CHECK(ledger.entries().size() == surviving.size());
    CHECK_FALSE(ledger.failed());
    REQUIRE(ledger.observe(7, kEnd + 2, {}));
    REQUIRE(ledger.advanceDisplayed(7, kEnd + 2));
    CHECK(ledger.entries().size() == surviving.size()); // still on the retirement fence
    REQUIRE(ledger.observe(7, kEnd + 3, {}));
    REQUIRE(ledger.advanceDisplayed(7, kEnd + 3));
    CHECK(ledger.entries().empty());
    CHECK_FALSE(ledger.failed());
}

TEST_CASE("repeated prediction confirmation and expiry have bounded retained memory",
          "[netplay][predicted-shot-ledger]") {
    auto ledger = ledgerAt();
    for (u64 tick = 11; tick < 4000; ++tick) {
        const auto release = shot(tick, tick);
        REQUIRE(ledger.replace(1, tick - 1, tick, std::array{release}));
        REQUIRE(ledger.observe(7, tick, std::array{release}));
        REQUIRE(ledger.advanceDisplayed(7, tick > 30 ? tick - 20 : 10));
        CHECK(ledger.entries().size() <= 23);
        CHECK(ledger.entries().at(release).status == Status::Confirmed);
        CHECK_FALSE(ledger.failed());
    }
}

TEST_CASE("release deadlines near the maximum simulation tick do not wrap",
          "[netplay][predicted-shot-ledger]") {
    const auto last = std::numeric_limits<u64>::max();
    auto ledger = ledgerAt(last - 10);
    const auto release = shot(last);
    REQUIRE(ledger.replace(1, last - 10, last, std::array{release}));
    REQUIRE(ledger.observe(7, last, std::array{release}));
    CHECK(ledger.entries().at(release).status == Status::Confirmed);
    CHECK_FALSE(ledger.observe(7, 0, {}));
    CHECK_FALSE(ledger.replace(1, last, 0, {}));
    CHECK(ledger.entries().at(release).status == Status::Confirmed);
}

TEST_CASE("spread members reconcile independently without compacting surviving ordinals",
          "[netplay][predicted-shot-ledger][spread-prediction]") {
    auto ledger = ledgerAt();
    std::array<ShotIdentity, ShotIdentity::kMaxShots> fan;
    std::array<u64, ShotIdentity::kMaxShots> instances;
    for (usize ordinal = 0; ordinal < fan.size(); ++ordinal) {
        fan[ordinal] = shot(14, 9);
        fan[ordinal].ordinal = static_cast<u8>(ordinal);
    }
    REQUIRE(ledger.replace(1, 10, 20, fan));
    REQUIRE(ledger.entries().size() == fan.size());
    for (usize ordinal = 0; ordinal < fan.size(); ++ordinal) {
        instances[ordinal] = ledger.entries().at(fan[ordinal]).instance;
        for (usize previous = 0; previous < ordinal; ++previous) {
            CHECK(instances[ordinal] != instances[previous]);
        }
    }
    // A corrected pre-release checkpoint removes the outer pair; a later replay
    // can restore it without changing the three retained members' identities.
    REQUIRE(ledger.replace(1, 10, 20, std::span{fan}.first(3)));
    CHECK(ledger.entries().at(fan[3]).status == Status::Retired);
    CHECK(ledger.entries().at(fan[4]).status == Status::Retired);
    REQUIRE(ledger.replace(1, 10, 20, fan));
    for (usize ordinal = 0; ordinal < fan.size(); ++ordinal) {
        CHECK(ledger.entries().at(fan[ordinal]).instance == instances[ordinal]);
    }
    // Two members hit geometry before the next packet. Duplicate body/streak
    // origins in the complete live roster do not confirm their missing siblings.
    const std::array live{fan[4], fan[0], fan[1], fan[4], fan[1], fan[0]};
    REQUIRE(ledger.observe(7, 14, live));
    for (usize ordinal = 0; ordinal < fan.size(); ++ordinal) {
        CHECK(ledger.entries().at(fan[ordinal]).status ==
              ((ordinal == 2 || ordinal == 3) ? Status::Retired : Status::Confirmed));
        CHECK(ledger.suppress(fan[ordinal]));
        CHECK(ledger.entries().at(fan[ordinal]).instance == instances[ordinal]);
    }
    CHECK_FALSE(ledger.observe(7, 13, fan));
    REQUIRE(ledger.replace(1, 14, 20, {}));
    REQUIRE(ledger.observe(7, 15, std::array{fan[4]}));
    CHECK(ledger.entries().at(fan[4]).status == Status::Confirmed);
    for (usize ordinal = 0; ordinal < 4; ++ordinal) {
        CHECK(ledger.entries().at(fan[ordinal]).status == Status::Retired);
    }
    auto invalid = fan[0];
    invalid.ordinal = ShotIdentity::kMaxShots;
    CHECK_FALSE(ledger.observe(7, 16, std::array{invalid}));
    CHECK(ledger.resolved() == 15);
    CHECK_FALSE(ledger.failed());
}
} // namespace
