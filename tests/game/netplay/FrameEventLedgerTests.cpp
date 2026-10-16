#include <array>
#include <limits>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "game/netplay/FrameEventLedger.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct Payload {
    std::string name;
    s32 amount = 0;
    std::optional<FrameEventIdentity> predecessor;
    bool operator==(const Payload&) const = default;
};
using Ledger = FrameEventLedger<Payload>;
using Policy = Ledger::Policy;
using Kind = Ledger::ActionKind;
using Error = Ledger::Error;

Ledger::Event event(u64 tick, u32 cue = 1, Policy policy = Policy::OneShot) {
    return {{1, tick, {1, 42, 1, 0}, cue, 0}, policy, {"cue", 1, {}}, {}};
}
Ledger::Event loop(u64 tick, Policy policy = Policy::LoopStart, u32 channel = 1) {
    auto result = event(tick, 10, policy);
    result.loop = FrameLoopKey{result.id.emitter, channel, 0};
    return result;
}
Ledger::Result replace(Ledger& ledger, u64 tick, std::vector<Ledger::Event> events) {
    const std::array journals{Ledger::Journal{std::move(events)}};
    return ledger.replace(ledger.epoch(), tick, journals);
}
void kinds(const Ledger::Result& result, std::vector<Kind> expected) {
    REQUIRE(result);
    std::vector<Kind> actual;
    for (const auto& action : result.actions) {
        actual.push_back(action.kind);
    }
    CHECK(actual == expected);
}

TEST_CASE("frame event identities distinguish owners generations seats cues and occurrences",
          "[netplay][frame-event-ledger]") {
    CHECK_FALSE(FrameEventEmitter{}.valid());
    CHECK_FALSE(FrameEventIdentity{}.valid());
    CHECK_FALSE(FrameLoopKey{}.valid());
    Ledger ledger;
    REQUIRE(ledger.reset(1));
    const auto first = event(1);
    std::vector<Ledger::Event> events{first};
    auto other = first;
    ++other.id.emitter.nameSpace;
    events.push_back(other);
    other = first;
    ++other.id.emitter.entity;
    events.push_back(other);
    other = first;
    ++other.id.emitter.generation;
    events.push_back(other);
    other = first;
    ++other.id.emitter.seat;
    events.push_back(other);
    other = first;
    ++other.id.cue;
    events.push_back(other);
    other = first;
    ++other.id.occurrence;
    events.push_back(other);
    other = first;
    other.id.emitter.seat = FrameEventEmitter::kNoSeat;
    events.push_back(other);
    const auto result = replace(ledger, 1, events);
    REQUIRE(result);
    CHECK(result.actions.size() == events.size());
    kinds(replace(ledger, 1, events), {});
    other.id.emitter.seat = FrameEventEmitter::kNoSeat + 1;
    CHECK_FALSE(other.id.valid());
}

TEST_CASE("replayed one shots emit once and disclose unretractable corrections",
          "[netplay][frame-event-ledger]") {
    Ledger ledger;
    REQUIRE(ledger.reset(1));
    const auto first = event(1);
    kinds(replace(ledger, 1, {first}), {Kind::Present});
    kinds(replace(ledger, 1, {first}), {});
    auto corrected = first;
    corrected.payload.amount = 2;
    auto result = replace(ledger, 1, {corrected});
    kinds(result, {});
    CHECK(result.unretractable == std::vector{first.id});
    result = replace(ledger, 1, {corrected});
    kinds(result, {});
    CHECK(result.unretractable.empty());
    result = replace(ledger, 1, {});
    kinds(result, {});
    CHECK(result.unretractable == std::vector{first.id});
    kinds(replace(ledger, 1, {}), {});
    result = replace(ledger, 1, {corrected, event(1, 2)});
    kinds(result, {Kind::Present});
    CHECK(result.actions.front().event.id.cue == 2);
    CHECK(result.unretractable == std::vector{first.id});
    kinds(ledger.confirm(1, 1), {});
    kinds(replace(ledger, 1, {corrected, event(1, 2)}), {});
    CHECK(replace(ledger, 1, {first}).error == Error::ConfirmedConflict);
}

TEST_CASE("loop publications update stable lifetimes without restarting every tick",
          "[netplay][frame-event-ledger]") {
    Ledger ledger;
    REQUIRE(ledger.reset(1));
    kinds(replace(ledger, 1, {loop(1)}), {Kind::StartLoop});
    kinds(replace(ledger, 2, {loop(2)}), {});
    auto moved = loop(3);
    moved.payload.amount = 2;
    kinds(replace(ledger, 3, {moved}), {Kind::UpdateLoop});
    CHECK(ledger.activeLoops() == 1);
    kinds(replace(ledger, 3, {moved}), {});
    auto restarted = loop(4);
    restarted.loop->instance = 1;
    const auto result = replace(ledger, 4, {loop(4, Policy::LoopStop), restarted});
    // Distinct per-tick occurrences are required, even for a stop and start.
    CHECK(result.error == Error::DuplicateIdentity);
    restarted.id.occurrence = 1;
    kinds(replace(ledger, 4, {loop(4, Policy::LoopStop), restarted}),
          {Kind::StopLoop, Kind::StartLoop});
    auto stop = restarted;
    stop.id.tick = 5;
    stop.policy = Policy::LoopStop;
    kinds(replace(ledger, 5, {stop}), {Kind::StopLoop});
    CHECK(ledger.activeLoops() == 0);
}

TEST_CASE("atomic replay reconciles loops only after replacing the whole suffix",
          "[netplay][frame-event-ledger]") {
    Ledger ledger;
    REQUIRE(ledger.reset(1));
    kinds(replace(ledger, 1, {loop(1)}), {Kind::StartLoop});
    kinds(replace(ledger, 2, {}), {});
    kinds(replace(ledger, 3, {}), {});
    // A stop on an intermediate replay frame followed by the same desired loop
    // does not audibly stop/restart it. The older unaffected start also remains.
    std::array replay{Ledger::Journal{{loop(2, Policy::LoopStop)}}, Ledger::Journal{{loop(3)}}};
    kinds(ledger.replace(1, 2, replay), {});
    replay[1].events.clear();
    kinds(ledger.replace(1, 2, replay), {Kind::StopLoop});
    replay[0].events.clear();
    kinds(ledger.replace(1, 2, replay), {Kind::StartLoop});
    std::array<Ledger::Journal, 3> removed{};
    kinds(ledger.replace(1, 1, removed), {Kind::StopLoop});
    kinds(ledger.replace(1, 1, removed), {});
    CHECK(ledger.activeLoops() == 0);
}

TEST_CASE("confirmed payloads use the corrected branch and commit exactly once",
          "[netplay][frame-event-ledger]") {
    Ledger ledger;
    REQUIRE(ledger.reset(1));
    auto save = event(1, 20, Policy::Confirmed);
    save.payload = {"save", 5, {}};
    auto unlock = event(2, 21, Policy::Confirmed);
    unlock.payload = {"unlock", 2, save.id};
    kinds(replace(ledger, 1, {save, event(1)}), {Kind::Present});
    kinds(replace(ledger, 2, {unlock}), {});
    save.payload.amount = 8;
    std::array corrected{Ledger::Journal{{save, event(1)}}, Ledger::Journal{}};
    kinds(ledger.replace(1, 1, corrected), {});
    auto result = ledger.confirm(1, 1);
    kinds(result, {Kind::Commit});
    CHECK(result.actions.front().event.payload.amount == 8);
    kinds(ledger.confirm(1, 1), {});
    kinds(ledger.confirm(1, 2), {}); // The withdrawn unlock never reaches persistence.
    CHECK(ledger.confirmed() == 2);
    CHECK(ledger.confirm(1, 1).error == Error::ConfirmationRegression);
    CHECK(ledger.confirm(1, 3).error == Error::UnsimulatedConfirmation);
    kinds(ledger.replace(1, 1, corrected), {});
    corrected[0].events.front().payload.amount = 9;
    CHECK(ledger.replace(1, 1, corrected).error == Error::ConfirmedConflict);
    CHECK(ledger.confirmed() == 2);
}

TEST_CASE("an earlier terminal retracts future intents but preserves heard one shot identities",
          "[netplay][frame-event-ledger]") {
    Ledger ledger;
    REQUIRE(ledger.reset(1));
    kinds(replace(ledger, 1, {loop(1)}), {Kind::StartLoop});
    kinds(ledger.confirm(1, 1), {});
    kinds(replace(ledger, 2, {event(2)}), {Kind::Present});
    kinds(replace(ledger, 3, {loop(3, Policy::LoopStop), event(3),
                              event(3, 20, Policy::Confirmed)}),
          {Kind::Present, Kind::StopLoop});
    kinds(replace(ledger, 4, {event(4), event(4, 20, Policy::Confirmed)}), {Kind::Present});
    const std::array corrected{Ledger::Journal{{event(2)}}};
    auto result = ledger.replaceSuffix(1, 2, corrected, 2);
    kinds(result, {Kind::StartLoop});
    CHECK(result.unretractable == std::vector{event(3).id, event(4).id});
    CHECK(ledger.head() == 2);
    CHECK(ledger.retainedTicks() == 4); // Future tombstones, not live simulation.
    CHECK(ledger.confirm(1, 3).error == Error::UnsimulatedConfirmation);
    kinds(ledger.replaceSuffix(1, 2, corrected, 2), {});
    // Another actual input removes the early terminal. Re-executing its abandoned
    // tail must not repeat already-heard effects or commit abandoned unlocks.
    kinds(replace(ledger, 3, {event(3)}), {});
    kinds(replace(ledger, 4, {event(4), event(4, 2), event(4, 20, Policy::Confirmed)}),
          {Kind::Present});
    result = ledger.confirm(1, 4);
    kinds(result, {Kind::Commit});
    CHECK(result.actions.front().event.id.tick == 4);
    kinds(ledger.confirm(1, 4), {});
}

TEST_CASE("empty terminal suffixes may retract only unconfirmed native ticks",
          "[netplay][frame-event-ledger]") {
    Ledger ledger;
    REQUIRE(ledger.reset(1));
    kinds(replace(ledger, 1, {loop(1), event(1)}), {Kind::Present, Kind::StartLoop});
    kinds(replace(ledger, 2, {event(2)}), {Kind::Present});
    auto result = ledger.replaceSuffix(1, 1, {}, 0);
    kinds(result, {Kind::StopLoop});
    CHECK(result.unretractable == std::vector{event(1).id, event(2).id});
    CHECK_FALSE(ledger.head());
    CHECK_FALSE(ledger.confirmed());
    CHECK(ledger.confirm(1, 1).error == Error::UnsimulatedConfirmation);
    kinds(replace(ledger, 1, {event(1)}), {});
    kinds(ledger.confirm(1, 1), {});
    kinds(replace(ledger, 2, {event(2)}), {});
    CHECK(ledger.replaceSuffix(1, 1, {}, 0).error == Error::ConfirmedConflict);
    const std::array identical{Ledger::Journal{{event(1)}}};
    CHECK(ledger.replaceSuffix(1, 1, identical, 1).error == Error::ConfirmedConflict);
    CHECK(ledger.head() == 2);
    kinds(ledger.replaceSuffix(1, 2, {}, 1), {});
    CHECK(ledger.head() == 1);
    CHECK(ledger.confirmed() == 1);
    kinds(replace(ledger, 2, {event(2)}), {});
}

TEST_CASE("terminal suffix validation is atomic and discarded tombstones retire boundedly",
          "[netplay][frame-event-ledger]") {
    Ledger ledger;
    REQUIRE(ledger.reset(1));
    kinds(replace(ledger, 1, {loop(1)}), {Kind::StartLoop});
    for (u64 tick = 2; tick <= Ledger::kHistory; ++tick) {
        kinds(replace(ledger, tick, {event(tick)}), {Kind::Present});
    }
    auto bad = event(2);
    bad.id.tick = 3;
    const std::array invalid{Ledger::Journal{{bad}}};
    CHECK(ledger.replaceSuffix(1, 2, invalid, 2).error == Error::InvalidEvent);
    CHECK(ledger.head() == Ledger::kHistory);
    CHECK(ledger.activeLoops() == 1);
    CHECK(ledger.replaceSuffix(1, 2, {}, 2).error == Error::InvalidRange);
    CHECK(ledger.replaceSuffix(1, 0, {}, 0).error == Error::InvalidRange);
    const std::array corrected{Ledger::Journal{{event(2)}}};
    REQUIRE(ledger.replaceSuffix(1, 2, corrected, 2));
    CHECK(ledger.retainedTicks() == Ledger::kHistory);
    for (u64 tick = 3; tick <= Ledger::kHistory; ++tick) {
        kinds(replace(ledger, tick, {event(tick)}), {});
    }
    CHECK(replace(ledger, 17, {event(17)}).error == Error::HistoryFull);
    kinds(ledger.confirm(1, 16), {});
    kinds(replace(ledger, 17, {event(17)}), {Kind::Present});
    CHECK(ledger.retainedTicks() == Ledger::kHistory);
    CHECK(ledger.replaceSuffix(1, 1, {}, 0).error == Error::TooOld);
    kinds(ledger.reset(2), {Kind::StopLoop});
    CHECK(ledger.retainedTicks() == 0);
}

TEST_CASE("one to eight replayed ticks replace intents without repeating unchanged sounds",
          "[netplay][frame-event-ledger]") {
    for (u64 distance = 1; distance <= 8; ++distance) {
        CAPTURE(distance);
        Ledger ledger;
        REQUIRE(ledger.reset(1));
        kinds(replace(ledger, 1, {loop(1)}), {Kind::StartLoop});
        kinds(ledger.confirm(1, 1), {});
        std::vector<Ledger::Journal> corrected;
        usize newSounds = 0;
        for (u64 index = 0; index < distance; ++index) {
            const u64 tick = index + 2;
            auto save = event(tick, 20, Policy::Confirmed);
            save.payload.amount = 1;
            auto predicted = event(tick);
            kinds(replace(ledger, tick, {predicted, save}), {Kind::Present});
            save.payload.amount = 2;
            if (index % 2 == 0) {
                ++predicted.id.cue;
                ++newSounds;
            }
            corrected.push_back({{predicted, save}});
        }
        // Replaying final desired starts should not restart the already-active
        // loop, even though the event's completed-tick identity is new.
        corrected.back().events.push_back(loop(distance + 1));
        const auto result = ledger.replace(1, 2, corrected);
        REQUIRE(result);
        REQUIRE(result.actions.size() == newSounds);
        REQUIRE(result.unretractable.size() == newSounds);
        for (const auto& action : result.actions) {
            CHECK(action.kind == Kind::Present);
            CHECK(action.event.id.cue == 2);
        }
        kinds(ledger.replace(1, 2, corrected), {});
        const auto committed = ledger.confirm(1, distance + 1);
        REQUIRE(committed);
        REQUIRE(committed.actions.size() == distance);
        for (const auto& action : committed.actions) {
            CHECK(action.kind == Kind::Commit);
            CHECK(action.event.payload.amount == 2);
        }
        kinds(ledger.confirm(1, distance + 1), {});
    }
}

TEST_CASE("three and four seats must all confirm contiguous actual inputs before committing",
          "[netplay][frame-event-ledger]") {
    for (usize seats : {usize{3}, usize{4}}) {
        CAPTURE(seats);
        Ledger ledger;
        REQUIRE(ledger.reset(1));
        for (u64 tick = 1; tick <= 8; ++tick) {
            kinds(replace(ledger, tick, {event(tick, 20, Policy::Confirmed)}), {});
        }
        // The session supplies contiguous frontiers, not highest packet ticks:
        // the last seat has received tick 8 but still lacks ticks 3 and 4.
        std::array<u64, 4> contiguous{8, 8, 8, 8};
        contiguous[seats - 1] = 2;
        const auto frontier = [&] {
            return *std::min_element(contiguous.begin(), contiguous.begin() + seats);
        };
        auto result = ledger.confirm(1, frontier());
        REQUIRE(result);
        REQUIRE(result.actions.size() == 2);
        CHECK(result.actions.back().event.id.tick == 2);
        kinds(ledger.confirm(1, frontier()), {});
        contiguous[seats - 1] = 4;
        result = ledger.confirm(1, frontier());
        REQUIRE(result);
        REQUIRE(result.actions.size() == 2);
        CHECK(result.actions.front().event.id.tick == 3);
        contiguous[seats - 1] = 8;
        result = ledger.confirm(1, frontier());
        REQUIRE(result);
        REQUIRE(result.actions.size() == 4);
        CHECK(result.actions.back().event.id.tick == 8);
        kinds(ledger.confirm(1, frontier()), {});
    }
}

TEST_CASE("bounded history stalls rather than evicting unconfirmed intent",
          "[netplay][frame-event-ledger]") {
    Ledger ledger;
    REQUIRE(ledger.reset(1));
    kinds(replace(ledger, 1, {loop(1), event(1, 20, Policy::Confirmed)}), {Kind::StartLoop});
    for (u64 tick = 2; tick <= Ledger::kHistory; ++tick) {
        kinds(replace(ledger, tick, {}), {});
    }
    CHECK(replace(ledger, 17, {event(17)}).error == Error::HistoryFull);
    CHECK(ledger.head() == 16);
    CHECK(ledger.retainedTicks() == Ledger::kHistory);
    kinds(ledger.confirm(1, 1), {Kind::Commit});
    kinds(replace(ledger, 17, {event(17)}), {Kind::Present});
    CHECK(ledger.retainedTicks() == Ledger::kHistory);
    CHECK(ledger.activeLoops() == 1);
    // Its birth journal retired, but the confirmed loop is still active.
    kinds(replace(ledger, 17, {}), {});
    kinds(replace(ledger, 17, {loop(17, Policy::LoopStop)}), {Kind::StopLoop});
    CHECK(replace(ledger, 1, {}).error == Error::TooOld);
    kinds(ledger.confirm(1, 17), {});
}

TEST_CASE("invalid replay is rejected atomically before any event is presented",
          "[netplay][frame-event-ledger]") {
    Ledger ledger;
    CHECK(replace(ledger, 1, {}).error == Error::Closed);
    CHECK(ledger.reset(0).error == Error::WrongEpoch);
    CHECK(ledger.reset(1, 0).error == Error::InvalidRange);
    REQUIRE(ledger.reset(1));
    CHECK(replace(ledger, 2, {}).error == Error::Gap);
    kinds(replace(ledger, 1, {loop(1)}), {Kind::StartLoop});
    kinds(replace(ledger, 2, {}), {});
    CHECK(replace(ledger, 1, {}).error == Error::IncompleteReplay);
    CHECK(replace(ledger, 4, {}).error == Error::Gap);
    CHECK(ledger.replace(2, 3, {}).error == Error::WrongEpoch);
    CHECK(ledger.replace(1, 3, {}).error == Error::InvalidRange);
    auto bad = event(3);
    bad.id.emitter.generation = 0;
    std::array journals{Ledger::Journal{{event(2)}}, Ledger::Journal{{bad}}};
    auto result = ledger.replace(1, 2, journals);
    CHECK(result.error == Error::InvalidEvent);
    CHECK(result.actions.empty());
    CHECK(ledger.head() == 2);
    CHECK(ledger.activeLoops() == 1);
    journals[1].events.front() = event(3);
    kinds(ledger.replace(1, 2, journals), {Kind::Present, Kind::Present});
    CHECK(replace(ledger, 3, {event(3), event(3)}).error == Error::DuplicateIdentity);
    bad = event(3);
    bad.policy = Policy::Confirmed;
    CHECK(replace(ledger, 3, {bad}).error == Error::ConflictingIdentity);
    bad = loop(3);
    ++bad.loop->emitter.entity;
    CHECK(replace(ledger, 3, {bad}).error == Error::InvalidEvent);
    bad = event(3);
    bad.policy = static_cast<Policy>(255);
    CHECK(replace(ledger, 3, {bad}).error == Error::InvalidEvent);
    CHECK(ledger.head() == 3);
}

TEST_CASE("event and replay tombstone limits reject overflow without partial dispatch",
          "[netplay][frame-event-ledger]") {
    Ledger ledger;
    REQUIRE(ledger.reset(1));
    std::vector<Ledger::Event> events;
    for (u32 index = 0; index <= Ledger::kEventsPerTick; ++index) {
        auto next = event(1);
        next.id.occurrence = index;
        events.push_back(next);
    }
    CHECK(replace(ledger, 1, events).error == Error::EventLimit);
    CHECK_FALSE(ledger.head());
    for (u32 index = 0; index < Ledger::kPresentedPerTick; ++index) {
        auto next = event(1);
        next.id.occurrence = index;
        kinds(replace(ledger, 1, {next}), {Kind::Present});
    }
    auto next = event(1);
    next.id.occurrence = Ledger::kPresentedPerTick;
    const auto result = replace(ledger, 1, {next});
    CHECK(result.error == Error::EventLimit);
    CHECK(result.actions.empty());
    kinds(replace(ledger, 1, {event(1)}), {});
}

TEST_CASE("loop capacity errors preserve every existing playback lifetime",
          "[netplay][frame-event-ledger]") {
    Ledger ledger;
    REQUIRE(ledger.reset(1));
    std::vector<Ledger::Event> events;
    for (u32 index = 0; index < Ledger::kActiveLoops; ++index) {
        auto next = loop(1, Policy::LoopStart, index + 1);
        next.id.occurrence = index;
        events.push_back(next);
    }
    const auto initial = replace(ledger, 1, events);
    REQUIRE(initial);
    REQUIRE(initial.actions.size() == Ledger::kActiveLoops);
    CHECK(ledger.activeLoops() == Ledger::kActiveLoops);
    auto next = loop(2, Policy::LoopStart, Ledger::kActiveLoops + 1);
    const auto overflow = replace(ledger, 2, {next});
    CHECK(overflow.error == Error::EventLimit);
    CHECK(overflow.actions.empty());
    CHECK(ledger.head() == 1);
    CHECK(ledger.activeLoops() == Ledger::kActiveLoops);
    next.id.occurrence = 1;
    kinds(replace(ledger, 2, {loop(2, Policy::LoopStop), next}), {Kind::StopLoop, Kind::StartLoop});
    CHECK(ledger.activeLoops() == Ledger::kActiveLoops);
}

TEST_CASE("epoch reset cancels old loops and discards only explicitly abandoned pending events",
          "[netplay][frame-event-ledger]") {
    Ledger ledger;
    REQUIRE(ledger.reset(1));
    kinds(replace(ledger, 1, {loop(1), event(1), event(1, 20, Policy::Confirmed)}),
          {Kind::Present, Kind::StartLoop});
    CHECK(ledger.reset(1).error == Error::RepeatedEpoch);
    kinds(ledger.reset(2), {Kind::StopLoop});
    CHECK(ledger.retainedTicks() == 0);
    CHECK_FALSE(ledger.head());
    CHECK_FALSE(ledger.confirmed());
    CHECK(ledger.activeLoops() == 0);
    CHECK(ledger.confirm(1, 1).error == Error::WrongEpoch);
    CHECK(replace(ledger, 1, {event(1)}).error == Error::InvalidEvent);
    auto fresh = event(1);
    fresh.id.epoch = 2;
    kinds(replace(ledger, 1, {fresh}), {Kind::Present});
    kinds(ledger.confirm(2, 1), {});
}

TEST_CASE("frame ranges preserve high tick identity without arithmetic wraparound",
          "[netplay][frame-event-ledger]") {
    Ledger ledger;
    constexpr u64 last = std::numeric_limits<u64>::max();
    REQUIRE(ledger.reset(1, last - 1));
    kinds(replace(ledger, last - 1, {event(last - 1)}), {Kind::Present});
    const std::array<Ledger::Journal, 2> overflow{};
    CHECK(ledger.replace(1, last, overflow).error == Error::InvalidRange);
    CHECK(ledger.head() == last - 1);
    kinds(replace(ledger, last, {event(last)}), {Kind::Present});
    kinds(ledger.confirm(1, last), {});
    kinds(replace(ledger, last, {event(last)}), {});
    CHECK(replace(ledger, 0, {}).error == Error::TooOld);
}
} // namespace
