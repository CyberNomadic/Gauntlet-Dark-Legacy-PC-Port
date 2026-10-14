#include <algorithm>
#include <chrono>
#include <limits>
#include <memory>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/platform/Input.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/netplay/RollbackInputHistory.h"
#include "game/players/ClassData.h"
#include "game/screens/MatchInputs.h"
#include "game/screens/PartyMotion.h"
#include "game/world/CameraMovementLimit.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Phase = MatchSession::Phase;
using Admission = MatchSession::Admission;

struct Wire final : PacketTransport {
    struct Packet {
        Connection connection;
        std::vector<u8> bytes;
        Delivery delivery;
    };
    SendResult result = SendResult::Sent;
    bool feedbackCongested = false;
    usize budget = std::numeric_limits<usize>::max();
    std::vector<Packet> sent;
    std::optional<Statistics> stats;
    mutable usize statisticsQueries = 0;
    SendResult send(Connection connection, std::span<const u8> bytes, Delivery delivery) override {
        if (feedbackCongested && FeedbackSync::recognizes(bytes)) {
            return SendResult::Congested;
        }
        if (result == SendResult::Sent) {
            if (budget == 0) {
                return SendResult::Congested;
            }
            --budget;
            sent.push_back({connection, {bytes.begin(), bytes.end()}, delivery});
        }
        return result;
    }
    std::vector<Event> poll() override { return {}; }
    std::optional<Statistics> statistics(Connection /*connection*/) const override {
        ++statisticsQueries;
        return stats;
    }
    void close(Connection /*connection*/) override {}
};

struct Room {
    MatchOwners owners;
    std::array<MatchSession, 5> sessions;
    std::array<Wire, 5> wires;
    explicit Room(MatchOwners roster = {1, 2, 3, 4}) : owners(roster) {
        std::vector<MatchLink> links;
        for (u8 peer = 2; peer <= 4; ++peer) {
            if (present(peer)) {
                links.push_back({peer, static_cast<u64>(peer - 1)});
                const std::array server{MatchLink{1, 99}};
                REQUIRE(sessions[peer].open(peer, owners, server));
            }
        }
        REQUIRE(sessions[1].open(1, owners, links));
    }
    bool present(u8 peer) const { return std::ranges::find(owners, peer) != owners.end(); }
    MatchParty party() const {
        MatchParty result;
        for (usize seat = 0; seat < owners.size(); ++seat) {
            if (owners[seat] != 0) {
                CharacterProfile profile;
                profile.name = "TEST";
                profile.character = static_cast<s32>(seat);
                profile.gold = static_cast<s32>(seat) * 100;
                result[seat] = profile;
            }
        }
        return result;
    }
    void pump(MatchSession::Time now = MatchSession::Clock::now()) {
        for (u8 pass = 0; pass < 5; ++pass) {
            for (u8 peer = 1; peer <= 4; ++peer) {
                if (!present(peer)) {
                    continue;
                }
                sessions[peer].flush(wires[peer], now);
                const auto sent = std::move(wires[peer].sent);
                wires[peer].sent.clear();
                for (const auto& packet : sent) {
                    const auto target = peer == 1 ? packet.connection + 1 : 1;
                    const auto sender = peer == 1 ? 99 : peer - 1;
                    const auto admitted = sessions[target].receive(sender, packet.bytes, now);
                    REQUIRE((admitted == Admission::Accepted || admitted == Admission::Stale));
                }
            }
        }
    }
    void ready(MatchTransition transition = MatchTransition::Start, u32 scene = 7,
               bool challengeWon = false) {
        const auto profiles = party();
        const bool checkpointed = transition == MatchTransition::Travel ||
                                  transition == MatchTransition::SecretEnter ||
                                  transition == MatchTransition::SecretReturn;
        REQUIRE(sessions[1].prepare(scene, transition, checkpointed ? &profiles : nullptr, nullptr,
                                    challengeWon));
        pump();
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (present(peer)) {
                REQUIRE(sessions[peer].phase() == Phase::Loading);
                REQUIRE(sessions[peer].loaded());
            }
        }
        pump();
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (present(peer)) {
                REQUIRE(sessions[peer].phase() == Phase::Running);
            }
        }
    }
};
std::vector<u8> message(const MatchSession& session, MatchControlKind kind,
                        MatchStop stop = MatchStop::None, u8 votes = 0) {
    const auto bytes = MatchControlPacket::encode({kind, stop, session.context(), votes});
    REQUIRE(bytes);
    return *bytes;
}
CombatSnapshot snapshot(const MatchSession& session) {
    CombatSnapshot result;
    result.motion.epoch = session.context().epoch;
    result.motion.tick = session.tick() - 1;
    result.motion.cameraContinuity = 1;
    for (usize seat = 0; seat < 4; ++seat) {
        if (session.context().owners[seat] != 0) {
            result.motion.players[seat] = SeatMotion{session.context().grants[seat], 1, {}, 0};
            result.players[seat] =
                PlayerCombatState{1000, ReplicaPlayerLife::Standing, false, true, {4, 1, 1, 0, 1}};
        }
    }
    return result;
}

TEST_CASE("match control has an exact bounded versioned encoding", "[netplay][match-session]") {
    Room room;
    REQUIRE(room.sessions[1].prepare(7));
    for (u8 kind = 0; kind <= 6; ++kind) {
        const auto stop = kind == 5 ? MatchStop::Left : MatchStop::None;
        const u8 votes = kind == 6 ? 1 : 0;
        const auto bytes =
            message(room.sessions[1], static_cast<MatchControlKind>(kind), stop, votes);
        REQUIRE(bytes.size() == MatchControlPacket::kBytes);
        const auto decoded = MatchControlPacket::decode(bytes);
        REQUIRE(decoded);
        CHECK(decoded->context == room.sessions[1].context());
        CHECK(decoded->kind == static_cast<MatchControlKind>(kind));
        CHECK(decoded->stop == stop);
        CHECK(decoded->movieSkipVotes == votes);
        CHECK(bytes[4] == 4);
        for (usize size = 0; size < bytes.size(); ++size) {
            CHECK_FALSE(MatchControlPacket::decode(std::span(bytes).first(size)));
        }
        auto trailing = bytes;
        trailing.push_back(0);
        CHECK_FALSE(MatchControlPacket::decode(trailing));
        for (const usize index : {0U, 4U, 5U, 6U, 7U, 20U, 22U, 23U, 24U, 44U}) {
            auto corrupt = bytes;
            corrupt[index] = 255;
            CHECK_FALSE(MatchControlPacket::decode(corrupt));
        }
    }
    auto context = room.sessions[1].context();
    SECTION("missing seat grant") {
        context.grants[0] = 0;
    }
    SECTION("grant on empty seat") {
        context.owners[0] = 0;
    }
    SECTION("no host") {
        context.owners.fill(2);
    }
    SECTION("invalid scene") {
        context.scene = 65536;
    }
    SECTION("zero epoch") {
        context.epoch = 0;
    }
    CHECK_FALSE(MatchControlPacket::encode({MatchControlKind::Prepare, MatchStop::None, context}));
}

TEST_CASE("secret result flags are bounded host context and secret entries have no movie votes",
          "[netplay][match-session][secret-transition]") {
    Room room({1, 2, 0, 0});
    REQUIRE(room.sessions[1].prepare(7));
    auto context = room.sessions[1].context();
    for (const auto transition :
         {MatchTransition::Start, MatchTransition::Travel, MatchTransition::Resume,
          MatchTransition::Shop, MatchTransition::SecretEnter, MatchTransition::SecretReturn}) {
        context.transition = transition;
        context.challengeWon = false;
        CHECK(MatchControlPacket::encode({MatchControlKind::Prepare, MatchStop::None, context}));
        context.challengeWon = true;
        const auto encoded =
            MatchControlPacket::encode({MatchControlKind::Prepare, MatchStop::None, context});
        CHECK(encoded.has_value() == (transition == MatchTransition::SecretReturn));
        context.challengeWon = false;
        const auto vote = MatchControlPacket::encode(
            {MatchControlKind::MovieSkipVote, MatchStop::None, context, 1});
        CHECK(vote.has_value() ==
              (transition == MatchTransition::Start || transition == MatchTransition::Travel));
    }
    context.transition = MatchTransition::SecretReturn;
    context.challengeWon = true;
    const auto encoded =
        MatchControlPacket::encode({MatchControlKind::Prepare, MatchStop::None, context});
    REQUIRE(encoded);
    REQUIRE(encoded->size() == 48);
    CHECK((*encoded)[44] == 1);
    REQUIRE(MatchControlPacket::decode(*encoded));
    CHECK(MatchControlPacket::decode(*encoded)->context.challengeWon);
    auto invalid = *encoded;
    invalid[4] = 3;
    CHECK_FALSE(MatchControlPacket::decode(invalid));
    invalid = *encoded;
    invalid[44] = 2;
    CHECK_FALSE(MatchControlPacket::decode(invalid));
    invalid = *encoded;
    invalid[45] = 1;
    CHECK_FALSE(MatchControlPacket::decode(invalid));
}

TEST_CASE("secret enter and return form fresh authenticated checkpoint epochs without movies",
          "[netplay][match-session][secret-transition]") {
    Room room({1, 2, 2, 1});
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    const auto startEpoch = host.context().epoch;
    const auto party = room.party();
    CHECK_FALSE(host.prepare(8, MatchTransition::SecretEnter, &party, nullptr, true));
    CHECK_FALSE(host.prepare(8, MatchTransition::Travel, &party, nullptr, true));
    CHECK(host.context().epoch == startEpoch);
    room.ready(MatchTransition::SecretEnter, 8);
    CHECK(host.context().epoch == startEpoch + 1);
    CHECK(guest.context() == host.context());
    CHECK_FALSE(host.context().challengeWon);
    CHECK(host.travelParty());
    CHECK(guest.travelParty());
    const bool won = GENERATE(false, true);
    REQUIRE(host.prepare(7, MatchTransition::SecretReturn, &party, nullptr, won));
    room.pump();
    CHECK_FALSE(host.requestMovieSkip(0));
    CHECK_FALSE(guest.requestMovieSkip(1));
    CHECK_FALSE(host.movieSkipReady());
    CHECK_FALSE(guest.movieSkipReady());
    CHECK(guest.context().challengeWon == won);
    CHECK(guest.context().epoch == startEpoch + 2);
    auto forged = guest.context();
    forged.challengeWon = !won;
    const auto forgedPrepare =
        MatchControlPacket::encode({MatchControlKind::Prepare, MatchStop::None, forged});
    REQUIRE(forgedPrepare);
    CHECK(host.receive(1, *forgedPrepare) == Admission::WrongPeer);
    const auto forgedReady =
        MatchControlPacket::encode({MatchControlKind::Ready, MatchStop::None, forged});
    REQUIRE(forgedReady);
    CHECK(host.receive(1, *forgedReady) == Admission::Stale);
    CHECK(host.context().challengeWon == won);
    REQUIRE(host.loaded());
    REQUIRE(guest.loaded());
    room.pump();
    CHECK(host.phase() == Phase::Running);
    CHECK(guest.phase() == Phase::Running);
}

TEST_CASE("secret transitions require running gameplay and cannot leave a shop",
          "[netplay][match-session][secret-transition]") {
    Room room({1, 2, 0, 0});
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    const auto party = room.party();
    CHECK_FALSE(host.prepare(8, MatchTransition::SecretEnter, &party));
    CHECK_FALSE(host.prepare(7, MatchTransition::SecretReturn, &party));
    room.ready();
    REQUIRE(host.requestPause());
    room.pump();
    for (const auto transition : {MatchTransition::SecretEnter, MatchTransition::SecretReturn}) {
        CHECK_FALSE(host.prepare(8, transition, &party));
        auto context = host.context();
        ++context.epoch;
        context.transition = transition;
        const auto packet =
            MatchControlPacket::encode({MatchControlKind::Prepare, MatchStop::None, context});
        REQUIRE(packet);
        CHECK(guest.receive(99, *packet) == Admission::WrongPhase);
    }
    room.ready(MatchTransition::Resume);
    ShopSetup setup;
    setup.source = 7;
    setup.destination = 8;
    setup.maxima = {1000, 100, 1000};
    REQUIRE(host.prepare(7, MatchTransition::Shop, &party, &setup));
    room.pump();
    REQUIRE(host.loaded());
    REQUIRE(guest.loaded());
    room.pump();
    for (const auto transition : {MatchTransition::SecretEnter, MatchTransition::SecretReturn}) {
        CHECK_FALSE(host.prepare(8, transition, &party));
        auto context = host.context();
        ++context.epoch;
        context.transition = transition;
        const auto packet =
            MatchControlPacket::encode({MatchControlKind::Prepare, MatchStop::None, context});
        REQUIRE(packet);
        CHECK(guest.receive(99, *packet) == Admission::WrongPhase);
    }
    CHECK(guest.context().transition == MatchTransition::Shop);
}

TEST_CASE("match feedback uses authenticated reliable owner connections only",
          "[netplay][match-session][feedback-sync][rumble]") {
    Room room({1, 2, 3, 2});
    room.ready();
    auto& host = room.sessions[1];
    REQUIRE(host.feedback().emit(0, 15, ControlFeedback::Damage));
    REQUIRE(host.feedback().take() == FeedbackEvent{0, 15, ControlFeedback::Damage});
    for (s32 seat = 1; seat < 4; ++seat) {
        REQUIRE(host.feedback().emit(seat, 0, ControlFeedback::ThunderHammer));
    }
    auto& wire = room.wires[1];
    host.flush(wire);
    REQUIRE(wire.sent.size() == 3);
    for (usize i = 0; i < wire.sent.size(); ++i) {
        const auto& packet = wire.sent[i];
        CHECK(packet.delivery == PacketTransport::Delivery::Reliable);
        const auto owner = room.owners[i + 1];
        CHECK(packet.connection == owner - 1U);
        auto& guest = room.sessions[owner];
        CHECK(guest.receive(42, packet.bytes) == Admission::WrongPeer);
        CHECK(host.receive(1, packet.bytes) == Admission::WrongPeer);
        REQUIRE(guest.receive(99, packet.bytes) == Admission::Accepted);
        CHECK(guest.receive(99, packet.bytes) == Admission::Stale);
        REQUIRE(guest.feedback().take() ==
                FeedbackEvent{static_cast<u8>(i + 1), 0, ControlFeedback::ThunderHammer});
    }
    CHECK_FALSE(room.sessions[2].feedback().take());
    CHECK_FALSE(room.sessions[3].feedback().take());
}

TEST_CASE("congested feedback retains reliable cues without blocking gameplay packets",
          "[netplay][match-session][feedback-sync][rumble]") {
    Room room({1, 2, 0, 0});
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    REQUIRE(host.feedback().emit(1, 10, ControlFeedback::Damage));
    room.wires[1].feedbackCongested = true;
    REQUIRE(host.advance());
    REQUIRE(host.publish(snapshot(host)));
    host.flush(room.wires[1]);
    CHECK(host.feedback().outgoing().size() == 1);
    REQUIRE_FALSE(room.wires[1].sent.empty());
    for (const auto& packet : room.wires[1].sent) {
        CHECK(packet.delivery == PacketTransport::Delivery::Unreliable);
    }
    REQUIRE(guest.feedback().setEnabled(1, false));
    room.wires[2].feedbackCongested = true;
    REQUIRE(MatchInputs::sample(guest, {}));
    guest.flush(room.wires[2]);
    CHECK(guest.feedback().outgoing().size() == 1);
    REQUIRE(room.wires[2].sent.size() == 1);
    CHECK(room.wires[2].sent.front().delivery == PacketTransport::Delivery::Unreliable);
    room.wires[1].feedbackCongested = false;
    room.wires[2].feedbackCongested = false;
    room.pump();
    CHECK(host.feedback().outgoing().empty());
    CHECK(guest.feedback().outgoing().empty());
    CHECK_FALSE(guest.feedback().take()); // Old cue arrived after the local pause.
    REQUIRE(guest.feedback().setEnabled(1, true));
    room.pump();
    REQUIRE(host.feedback().emit(1, 0, ControlFeedback::MeleeHit));
    room.pump();
    REQUIRE(guest.feedback().take() == FeedbackEvent{1, 0, ControlFeedback::MeleeHit});
}

TEST_CASE("match phase barriers discard feedback before pause travel and disconnect",
          "[netplay][match-session][feedback-sync][rumble]") {
    Room room({1, 2, 0, 0});
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    REQUIRE(host.feedback().emit(1, 20, ControlFeedback::Damage));
    const auto stale = host.feedback().outgoing().front().bytes;
    REQUIRE(guest.receive(99, stale) == Admission::Accepted);
    REQUIRE(host.requestPause());
    CHECK(host.feedback().outgoing().empty());
    room.pump();
    CHECK_FALSE(guest.feedback().take());
    CHECK(guest.receive(99, stale) == Admission::Stale);
    room.ready(MatchTransition::Resume);
    CHECK(guest.receive(99, stale) == Admission::Stale);
    REQUIRE(host.feedback().emit(1, 20, ControlFeedback::Damage));
    room.ready(MatchTransition::SecretEnter, 8);
    CHECK(host.feedback().outgoing().empty());
    CHECK_FALSE(guest.feedback().take());
    CHECK(guest.receive(99, stale) == Admission::Stale);
    REQUIRE(host.feedback().emit(1, 20, ControlFeedback::Damage));
    room.wires[1].result = PacketTransport::SendResult::Disconnected;
    host.flush(room.wires[1]);
    CHECK(host.phase() == Phase::Stopped);
    CHECK(host.feedback().outgoing().empty());
    CHECK_FALSE(host.feedback().take());
    guest.disconnected(99);
    CHECK_FALSE(guest.feedback().take());
    CHECK(guest.receive(99, stale) == Admission::WrongPeer);
}

TEST_CASE("movie skipping requires every occupied seat not just every machine",
          "[netplay][match-session][movie-skip]") {
    Room room({1, 1, 2, 2});
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    CHECK_FALSE(host.requestMovieSkip(0));
    REQUIRE(host.prepare(7));
    room.pump();
    CHECK_FALSE(guest.requestMovieSkip(0));
    CHECK_FALSE(host.requestMovieSkip(4));
    REQUIRE(host.requestMovieSkip(0));
    REQUIRE(guest.requestMovieSkip(2));
    room.pump();
    CHECK(host.movieSkipVotes() == 5);
    CHECK(guest.movieSkipVotes() == 5);
    CHECK_FALSE(host.movieSkipReady());
    CHECK_FALSE(guest.movieSkipReady());
    REQUIRE(host.requestMovieSkip(1));
    room.pump();
    for (s32 duplicate = 0; duplicate < 30; ++duplicate) {
        REQUIRE(guest.requestMovieSkip(2));
        REQUIRE(host.requestMovieSkip(0));
    }
    CHECK(host.queuedControls() == 0);
    CHECK(guest.queuedControls() == 0);
    CHECK_FALSE(host.movieSkipReady());
    REQUIRE(guest.requestMovieSkip(3));
    CHECK_FALSE(guest.movieSkipReady()); // A local vote is not host approval.
    room.wires[1].result = PacketTransport::SendResult::Congested;
    room.pump();
    CHECK(host.movieSkipReady());
    CHECK_FALSE(guest.movieSkipReady());
    room.wires[1].result = PacketTransport::SendResult::Sent;
    room.pump();
    CHECK(host.movieSkipReady());
    CHECK(guest.movieSkipReady());
    CHECK(host.tick() == 0);
    REQUIRE(host.loaded());
    REQUIRE(guest.loaded());
    room.pump();
    const auto oldVote = message(guest, MatchControlKind::MovieSkipVote, MatchStop::None, 8);
    CHECK(host.receive(1, oldVote) == Admission::Stale);
    REQUIRE(host.requestPause());
    room.pump();
    room.ready(MatchTransition::Resume);
    CHECK(host.movieSkipVotes() == 0);
    CHECK(guest.movieSkipVotes() == 0);
    CHECK_FALSE(host.requestMovieSkip(0));
    const auto profiles = room.party();
    REQUIRE(host.prepare(8, MatchTransition::Travel, &profiles));
    room.pump();
    CHECK_FALSE(host.movieSkipReady());
    CHECK_FALSE(guest.movieSkipReady());
    CHECK(host.receive(1, oldVote) == Admission::Stale);
    CHECK(host.movieSkipVotes() == 0);
    REQUIRE(guest.requestMovieSkip(2));
    room.pump();
    CHECK(host.movieSkipVotes() == 4);
}

TEST_CASE("movie skip masks cannot vote for empty seats or another peer's players",
          "[netplay][match-session][movie-skip]") {
    Room room({1, 2, 0, 0});
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    REQUIRE(host.prepare(7));
    room.pump();
    const auto forged = message(guest, MatchControlKind::MovieSkipVote, MatchStop::None, 3);
    CHECK(host.receive(1, forged) == Admission::WrongPeer);
    CHECK(host.movieSkipVotes() == 0);
    CHECK_FALSE(host.requestMovieSkip(2));
    for (const u8 mask : std::array<u8, 4>{0, 4, 16, 255}) {
        CHECK_FALSE(MatchControlPacket::encode(
            {MatchControlKind::MovieSkipVote, MatchStop::None, host.context(), mask}));
    }
    CHECK_FALSE(
        MatchControlPacket::encode({MatchControlKind::Ready, MatchStop::None, host.context(), 1}));
    REQUIRE(host.requestMovieSkip(0));
    REQUIRE(guest.requestMovieSkip(1));
    room.pump();
    CHECK(host.movieSkipReady());
    CHECK(guest.movieSkipReady());
}

TEST_CASE("match admission separates room peer identities from connection handles",
          "[netplay][match-session]") {
    MatchSession session;
    const MatchOwners owners{1, 2, 3, 0};
    const std::array duplicate{MatchLink{2, 1}, MatchLink{3, 1}};
    CHECK_FALSE(session.open(1, owners, duplicate));
    const std::array missing{MatchLink{2, 1}};
    CHECK_FALSE(session.open(1, owners, missing));
    CHECK_FALSE(session.open(4, owners, missing));
    CHECK(session.phase() == Phase::Offline);
    const std::array valid{MatchLink{2, 1}, MatchLink{3, 2}};
    REQUIRE(session.open(1, owners, valid));
    CHECK_FALSE(session.open(1, owners, valid));
    CHECK_FALSE(session.advance());
    CHECK_FALSE(session.loaded());
    CHECK_FALSE(session.requestPause());
    REQUIRE(session.prepare(1));
    const auto control = message(session, MatchControlKind::Commit);
    CHECK(session.receive(1, control) == Admission::WrongPeer);
    CHECK(session.receive(99, control) == Admission::WrongPeer);
    session.leave();
    CHECK_FALSE(session.prepare(1));
    session.clear();
    CHECK(session.phase() == Phase::Offline);
    CHECK(session.open(1, owners, valid));
}

TEST_CASE("travel and secret transitions wait for every profile and reject forged progression",
          "[netplay][match-session][party-checkpoint][secret-transition]") {
    const auto transition = GENERATE(MatchTransition::Travel, MatchTransition::SecretEnter,
                                     MatchTransition::SecretReturn);
    const bool won = transition == MatchTransition::SecretReturn;
    Room room({1, 2, 2, 0});
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    const auto oldContext = host.context();
    auto party = room.party();
    party[1]->gold = 7654;
    party[1]->progress.inventory.keys = 7;
    CHECK_FALSE(host.prepare(8, transition));
    auto missing = party;
    missing[2].reset();
    CHECK_FALSE(host.prepare(8, transition, &missing, nullptr, won));
    CHECK(host.context() == oldContext);
    CHECK(host.phase() == Phase::Running);
    REQUIRE(host.prepare(8, transition, &party, nullptr, won));
    REQUIRE(host.travelParty());
    REQUIRE(host.loaded());
    auto& wire = room.wires[1];
    wire.budget = 1; // Prepare arrives, then each profile in a separate batch.
    host.flush(wire);
    REQUIRE(wire.sent.size() == 1);
    REQUIRE(guest.receive(99, wire.sent.back().bytes) == Admission::Accepted);
    CHECK(guest.context().transition == transition);
    CHECK(guest.context().challengeWon == won);
    wire.sent.clear();
    CHECK_FALSE(guest.travelParty());
    CHECK_FALSE(guest.loaded());
    CHECK_FALSE(host.advance());

    PartyCheckpoint forged{host.context().epoch, 0, *party[0]};
    auto encoded = PartyCheckpointPacket::encode(forged);
    REQUIRE(encoded);
    CHECK(host.receive(1, *encoded) == Admission::WrongPeer);
    CHECK(guest.receive(1, *encoded) == Admission::WrongPeer);
    ++forged.epoch;
    CHECK(guest.receive(99, *PartyCheckpointPacket::encode(forged)) == Admission::Invalid);
    forged.epoch = oldContext.epoch;
    CHECK(guest.receive(99, *PartyCheckpointPacket::encode(forged)) == Admission::Stale);
    forged.epoch = host.context().epoch;
    forged.seat = 3;
    CHECK(guest.receive(99, *PartyCheckpointPacket::encode(forged)) == Admission::Invalid);

    for (usize seat = 0; seat < 3; ++seat) {
        wire.budget = 1;
        host.flush(wire);
        REQUIRE(wire.sent.size() == 1);
        const auto packet = wire.sent.back();
        wire.sent.clear();
        CHECK(packet.delivery == PacketTransport::Delivery::Reliable);
        REQUIRE(guest.receive(99, packet.bytes) == Admission::Accepted);
        CHECK(guest.receive(99, packet.bytes) == Admission::Accepted); // exact replay
        if (seat < 2) {
            CHECK_FALSE(guest.travelParty());
            CHECK_FALSE(guest.loaded());
        }
        auto changed = *PartyCheckpointPacket::decode(packet.bytes);
        ++changed.profile.gold;
        CHECK(guest.receive(99, *PartyCheckpointPacket::encode(changed)) == Admission::Invalid);
    }
    REQUIRE(guest.travelParty());
    CHECK((*guest.travelParty())[1]->gold == 7654);
    CHECK((*guest.travelParty())[1]->progress.inventory.keys == 7);
    REQUIRE(guest.loaded());
    wire.budget = 100;
    room.pump();
    CHECK(host.phase() == Phase::Running);
    CHECK(guest.phase() == Phase::Running);
    REQUIRE(host.requestPause());
    room.pump();
    REQUIRE(host.prepare(8, MatchTransition::Resume));
    room.pump();
    CHECK(guest.receive(99, *encoded) == Admission::Stale);
    CHECK_FALSE(guest.travelParty());
}

TEST_CASE("travel preparation is bounded under congestion and missing profiles time out",
          "[netplay][match-session][party-checkpoint]") {
    Room room;
    room.ready();
    auto& host = room.sessions[1];
    const auto party = room.party();
    REQUIRE(host.prepare(8, MatchTransition::Travel, &party));
    CHECK(host.queuedControls() == 15); // 3 peers * (Prepare + 4 profiles)
    room.wires[1].result = PacketTransport::SendResult::Congested;
    host.flush(room.wires[1]);
    CHECK(host.queuedControls() == 15);
    CHECK_FALSE(host.prepare(9, MatchTransition::Travel, &party));
    CHECK(host.queuedControls() == 15);
    const auto prepare = message(host, MatchControlKind::Prepare);
    auto& guest = room.sessions[2];
    REQUIRE(guest.receive(99, prepare) == Admission::Accepted);
    guest.update(MatchSession::kLoadTimeout);
    CHECK(guest.phase() == Phase::Stopped);
    CHECK(guest.stopReason() == MatchStop::LoadTimeout);
    CHECK_FALSE(guest.travelParty());
    CHECK_FALSE(guest.loaded());
}

TEST_CASE("simulation waits for every machine to load including two local seats",
          "[netplay][match-session][multiplayer]") {
    MatchOwners owners{1, 2, 3, 4};
    SECTION("four machines") {}
    SECTION("two machines two seats each") {
        owners = {1, 1, 4, 4};
    }
    Room room(owners);
    auto& host = room.sessions[1];
    REQUIRE(host.prepare(7));
    REQUIRE(host.loaded());
    room.pump();
    for (u8 peer = 2; peer < 4; ++peer) {
        if (room.present(peer)) {
            REQUIRE(room.sessions[peer].loaded());
            CHECK_FALSE(room.sessions[peer].loaded());
        }
    }
    room.pump();
    CHECK_FALSE(host.advance());
    CHECK_FALSE(MatchInputs::sample(host, {}));
    CHECK(host.tick() == 0);
    auto& slow = room.sessions[4];
    CHECK(slow.receive(99, message(host, MatchControlKind::Commit)) == Admission::WrongPhase);
    REQUIRE(slow.loaded());
    room.pump();
    REQUIRE(host.phase() == Phase::Running);
    REQUIRE(host.advance());
    CHECK(host.tick() == 1);
    CHECK_FALSE(slow.advance());
    CHECK_FALSE(slow.prepare(8));
    CHECK_FALSE(slow.publish(snapshot(host)));
}

TEST_CASE("unreliable gameplay may overtake Commit or trail Pause without bypassing either",
          "[netplay][match-session]") {
    Room room({1, 2, 0, 0});
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    REQUIRE(host.prepare(7));
    room.pump();
    REQUIRE(host.loaded());
    REQUIRE(guest.loaded());
    REQUIRE(host.receive(1, message(guest, MatchControlKind::Ready)) == Admission::Accepted);
    REQUIRE(host.phase() == Phase::Running);
    REQUIRE(host.advance());
    REQUIRE(host.publish(snapshot(host)));
    Wire wire;
    host.flush(wire);
    REQUIRE(wire.sent.size() >= 2);
    for (const auto& packet : wire.sent) {
        if (packet.delivery == PacketTransport::Delivery::Unreliable) {
            CHECK(guest.receive(99, packet.bytes) == Admission::Stale);
            CHECK(guest.receive(123, packet.bytes) == Admission::WrongPeer);
        }
    }
    CHECK(guest.phase() == Phase::Loading);
    CHECK_FALSE(guest.advance());
    CHECK_FALSE(guest.playback().latest());
    for (const auto& packet : wire.sent) {
        if (packet.delivery == PacketTransport::Delivery::Reliable) {
            REQUIRE(guest.receive(99, packet.bytes) == Admission::Accepted);
        }
    }
    REQUIRE(guest.phase() == Phase::Running);
    wire.sent.clear();
    REQUIRE(host.advance());
    REQUIRE(host.publish(snapshot(host)));
    host.flush(wire);
    for (const auto& packet : wire.sent) {
        REQUIRE(guest.receive(99, packet.bytes) == Admission::Accepted);
    }
    REQUIRE(guest.playback().latest());
    CHECK(guest.playback().latest()->motion.tick == 1);

    REQUIRE(MatchInputs::sample(guest, {}));
    Wire inputs;
    guest.flush(inputs);
    REQUIRE(host.requestPause());
    for (const auto& packet : inputs.sent) {
        if (packet.delivery == PacketTransport::Delivery::Unreliable) {
            CHECK(host.receive(1, packet.bytes) == Admission::Stale);
        }
    }
    REQUIRE(guest.receive(99, message(host, MatchControlKind::Pause)) == Admission::Accepted);
    for (const auto& packet : wire.sent) {
        CHECK(guest.receive(99, packet.bytes) == Admission::Stale);
    }
    CHECK(host.phase() == Phase::Paused);
    CHECK(guest.phase() == Phase::Paused);
    CHECK_FALSE(host.advance());
    CHECK(guest.playback().latest()->motion.tick == 1);
}

TEST_CASE("local devices map to frozen global seats without UI pointers",
          "[netplay][match-session][controls][multiplayer]") {
    Room room({1, 2, 1, 2});
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    SessionInputs::Frame local;
    const Input devices;
    local[0].move = {{1, 0}, 1};
    local[0].throwPotion = true;
    local[0].menu.devices = &devices;
    local[0].menu.typed = "PRIVATE";
    local[0].menu.pointer = Vec2{1};
    local[1].move = {{-1, 0}, 1};
    local[1].aimDirection = Vec3{0.6f, 0, 0.8f};
    REQUIRE(MatchInputs::sample(guest, local));
    REQUIRE(MatchInputs::sample(host, local));
    CHECK_FALSE(MatchInputs::sample(host, local));
    room.pump();
    auto frame = MatchInputs::advance(host);
    REQUIRE(frame);
    CHECK((*frame)[0].throwPotion);
    CHECK((*frame)[2].move.direction == Vec2{-1, 0});
    CHECK((*frame)[2].aimDirection == local[1].aimDirection);
    CHECK((*frame)[1].move.magnitude == 0);
    for (u8 tick = 1; tick <= guest.context().inputLead; ++tick) {
        frame = MatchInputs::advance(host);
        REQUIRE(frame);
    }
    CHECK((*frame)[1].throwPotion);
    CHECK((*frame)[3].move.direction == Vec2{-1, 0});
    CHECK((*frame)[3].aimDirection == local[1].aimDirection);
    for (const auto& input : *frame) {
        CHECK(input.menu.devices == nullptr);
        CHECK(input.menu.typed.empty());
        CHECK_FALSE(input.menu.pointer);
    }
    CHECK((*MatchInputs::advance(host))[1].throwPotion);
}

TEST_CASE("spoofed trailing input cannot partially apply an otherwise valid command",
          "[netplay][match-session]") {
    Room room({1, 2, 0, 0});
    room.ready();
    auto& host = room.sessions[1];
    InputCommand input;
    input.epoch = host.context().epoch;
    input.grant = host.context().grants[1];
    input.seat = 1;
    input.pressedButtons = static_cast<u32>(CommandPress::UsePotion);
    auto spoof = input;
    spoof.seat = 0;
    const std::array commands{input, spoof};
    const auto packet = InputPacket::encode(commands);
    REQUIRE(packet);
    CHECK(host.receive(1, *packet) == Admission::WrongPeer);
    const auto frame = host.advance();
    REQUIRE(frame);
    CHECK_FALSE((*frame)[1].pressed(CommandPress::UsePotion));
    CHECK_FALSE((*frame)[0].pressed(CommandPress::UsePotion));
    input.tick = host.tick() + InputTimeline::kMaxAhead + 1;
    const auto future = InputPacket::encode(std::span(&input, 1));
    REQUIRE(future);
    CHECK(host.receive(1, *future) == Admission::Invalid);
}

TEST_CASE("simultaneous guest pause requests do not terminate the shared match",
          "[netplay][match-session][multiplayer]") {
    Room room;
    room.ready();
    for (u8 peer = 2; peer <= 4; ++peer) {
        REQUIRE(room.sessions[peer].requestPause());
    }
    room.pump();
    for (u8 peer = 1; peer <= 4; ++peer) {
        CHECK(room.sessions[peer].phase() == Phase::Paused);
        CHECK(room.sessions[peer].stopReason() == MatchStop::None);
        CHECK_FALSE(room.sessions[peer].advance());
    }
    room.ready(MatchTransition::Resume);
    CHECK(room.sessions[1].advance());
}

TEST_CASE("a stalled host bounds guest input lead and coalesces pending actions",
          "[netplay][match-session][input-pacing]") {
    Room room({1, 2, 1, 2});
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    SessionInputs::Frame input;
    for (u32 tick = 0; tick < 600; ++tick) {
        input[0].usePotion = tick == 200;
        input[1].selector.up = tick == 300;
        input[0].attack = tick < 500;
        REQUIRE(MatchInputs::sample(guest, input));
        room.pump(); // These packets used to exceed kMaxAhead and fail admission.
    }
    CHECK_FALSE(guest.inputReady());
    CHECK(guest.inputTick() == guest.context().inputLead + MatchSession::kInputSlack + 1);
    const u64 pendingTick = guest.inputTick();
    CHECK(host.tick() == 0);
    CHECK(host.phase() == Phase::Running);

    // New snapshots let the guest continue. The stalled period's discrete presses
    // survive, but its old held attack and movement do not replay after release.
    input = {};
    input[1].move = {{-1, 0}, 1};
    std::array<u32, 4> potions{};
    std::array<u32, 4> selections{};
    for (u32 tick = 0; tick < 100; ++tick) {
        REQUIRE(MatchInputs::sample(guest, input));
        room.pump();
        const auto frame = host.advance();
        REQUIRE(frame);
        for (usize seat = 0; seat < 4; ++seat) {
            potions[seat] += (*frame)[seat].pressed(CommandPress::UsePotion) ? 1U : 0U;
            selections[seat] += (*frame)[seat].pressed(CommandPress::SelectorUp) ? 1U : 0U;
        }
        if (tick >= pendingTick) {
            CHECK_FALSE((*frame)[1].held(CommandHeld::Attack));
            CHECK((*frame)[3].direction == Vec2{-1, 0});
        }
        if (tick % 3 == 0) {
            REQUIRE(host.publish(snapshot(host)));
        }
        room.pump();
    }
    CHECK(potions == std::array<u32, 4>{0, 1, 0, 0});
    CHECK(selections == std::array<u32, 4>{0, 0, 0, 1});
    CHECK(guest.inputTick() > pendingTick);
}

TEST_CASE("pending guest presses are atomic and cannot leak across pause or travel",
          "[netplay][match-session][input-pacing]") {
    Room room({1, 2, 0, 0});
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    while (guest.inputReady()) {
        REQUIRE(MatchInputs::sample(guest, {}));
    }
    SessionInputs::Frame input;
    input[0].usePotion = true;
    REQUIRE(MatchInputs::sample(guest, input));
    SECTION("pause discards pending gameplay") {
        REQUIRE(host.requestPause());
        room.pump();
        room.ready(MatchTransition::Resume);
    }
    SECTION("travel discards pending gameplay") {
        room.ready(MatchTransition::Travel, 8);
    }
    SECTION("invalid frame cannot poison pending input") {
        // This section retains the valid pending potion, but rejects a spoofed
        // attack on another seat before changing any of the buffered presses.
        auto command = SessionInputs::command(input[0], guest.context().epoch, guest.inputTick(),
                                              guest.context().grants[1], 1);
        command.pressedButtons = static_cast<u32>(CommandPress::Attack);
        auto invalid = command;
        invalid.seat = 0;
        const std::array frame{command, invalid};
        CHECK_FALSE(guest.sample(frame));
        REQUIRE(host.advance());
        REQUIRE(host.publish(snapshot(host)));
        room.pump();
        REQUIRE(host.advance());
        REQUIRE(host.publish(snapshot(host)));
        room.pump();
        REQUIRE(guest.inputReady());
        REQUIRE(MatchInputs::sample(guest, {}));
        room.pump();
        u32 potions = 0;
        for (u32 tick = 0; tick < 30; ++tick) {
            const auto received = host.advance();
            REQUIRE(received);
            CHECK_FALSE((*received)[1].pressed(CommandPress::Attack));
            potions += (*received)[1].pressed(CommandPress::UsePotion) ? 1U : 0U;
        }
        CHECK(potions == 1);
        return;
    }
    REQUIRE(guest.inputReady());
    for (u32 tick = 0; tick < 30; ++tick) {
        REQUIRE(MatchInputs::sample(guest, {}));
        room.pump();
        const auto frame = host.advance();
        REQUIRE(frame);
        CHECK_FALSE((*frame)[1].pressed(CommandPress::UsePotion));
        REQUIRE(host.publish(snapshot(host)));
        room.pump();
    }
}

TEST_CASE("sampling backpressure preserves one ordinary attack tap per seat without stale levels",
          "[netplay][match-session][input-pacing][queued-attack]") {
    const usize local = GENERATE(0U, 1U);
    Room room({1, 2, 1, 2});
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    const usize seat = local == 0 ? 1 : 3;
    for (s32 round = 0; round < 2; ++round) {
        CAPTURE(local, round);
        for (u32 step = 0; step < 100 && guest.inputReady(); ++step) {
            REQUIRE(MatchInputs::sample(guest, {}));
            room.pump();
        }
        REQUIRE_FALSE(guest.inputReady());
        const u64 pendingTick = guest.inputTick();
        for (u32 step = 0; step < 120; ++step) {
            SessionInputs::Frame input{};
            if (step == 10 || step == 70) {
                // Multiple discrete edges still coalesce once; their stale
                // movement, aim and unrelated held levels must not return.
                auto& tap = input[local];
                tap.attack = tap.attackPressed = true;
                tap.strongAttack = tap.combo = tap.throwPotion = true;
                tap.move = {{1, 0}, 1};
                tap.aimDirection = Vec3{1, 0, 0};
            }
            REQUIRE(MatchInputs::sample(guest, input));
            CHECK(guest.sampledInputs().empty());
        }
        std::array<u32, 4> sampled{};
        std::array<u32, 4> received{};
        for (u32 step = 0; step < 80; ++step) {
            REQUIRE(MatchInputs::sample(guest, {}));
            for (const auto& command : guest.sampledInputs()) {
                if (command.pressed(CommandPress::Attack)) {
                    ++sampled[command.seat];
                    CHECK(command.tick == pendingTick);
                    CHECK(command.seat == seat);
                    CHECK(command.heldButtons == static_cast<u32>(CommandHeld::Attack));
                    CHECK(command.pressedButtons == static_cast<u32>(CommandPress::Attack));
                    CHECK(command.magnitude == 0);
                    CHECK_FALSE(command.aimDirection);
                } else {
                    CHECK(command.heldButtons == 0);
                }
            }
            room.pump();
            const auto frame = host.advance();
            REQUIRE(frame);
            for (usize index = 0; index < frame->size(); ++index) {
                const auto& command = (*frame)[index];
                if (command.pressed(CommandPress::Attack)) {
                    ++received[index];
                    CHECK(index == seat);
                    CHECK(command.tick == pendingTick);
                    CHECK(command.held(CommandHeld::Attack));
                } else {
                    CHECK_FALSE(command.held(CommandHeld::Attack));
                }
                CHECK_FALSE(command.held(CommandHeld::StrongAttack));
                CHECK_FALSE(command.held(CommandHeld::Combo));
                CHECK_FALSE(command.held(CommandHeld::ThrowPotion));
            }
            REQUIRE(host.publish(snapshot(host)));
            room.pump();
        }
        std::array<u32, 4> expected{};
        expected[seat] = 1;
        CHECK(sampled == expected);
        CHECK(received == expected);
    }
}

TEST_CASE("queued attack edges cannot survive reset or poison an invalid frame",
          "[netplay][match-session][input-pacing][queued-attack]") {
    Room room({1, 2, 1, 2});
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    while (guest.inputReady()) {
        REQUIRE(MatchInputs::sample(guest, {}));
        room.pump();
    }
    SessionInputs::Frame input{};
    input[0].attack = input[0].attackPressed = true;
    REQUIRE(MatchInputs::sample(guest, input));
    bool kept = false;
    SECTION("pause resume discards the old epoch tap") {
        REQUIRE(host.requestPause());
        room.pump();
        room.ready(MatchTransition::Resume);
    }
    SECTION("travel discards the old epoch tap") {
        room.ready(MatchTransition::Travel, 8);
    }
    SECTION("a trailing invalid grant cannot poison another seat") {
        // Seat 1's valid earlier tap remains. The invalid batch must not add
        // its valid-leading seat 3 tap before rejecting the trailing grant.
        input = {};
        input[1].attack = input[1].attackPressed = true;
        auto commands = MatchInputs::commands(guest, input);
        REQUIRE(commands.size() == 2);
        std::ranges::reverse(commands);
        ++commands.back().grant;
        CHECK_FALSE(guest.sample(commands));
        CHECK(guest.sampledInputs().empty());
        kept = true;
    }
    std::array<u32, 4> presses{};
    for (u32 tick = 0; tick < 80; ++tick) {
        REQUIRE(MatchInputs::sample(guest, {}));
        room.pump();
        const auto frame = host.advance();
        REQUIRE(frame);
        for (usize index = 0; index < frame->size(); ++index) {
            const auto& command = (*frame)[index];
            if (command.pressed(CommandPress::Attack)) {
                ++presses[index];
                CHECK(command.held(CommandHeld::Attack));
            } else {
                CHECK_FALSE(command.held(CommandHeld::Attack));
            }
        }
        REQUIRE(host.publish(snapshot(host)));
        room.pump();
    }
    CHECK(presses == std::array<u32, 4>{0, kept ? 1U : 0U, 0, 0});
}

TEST_CASE("ready input sampling leaves the caller's attack levels unchanged",
          "[netplay][match-session][input-pacing][queued-attack]") {
    const bool held = GENERATE(false, true);
    const bool pressed = GENERATE(false, true);
    Room room({1, 2, 0, 0});
    room.ready();
    auto& guest = room.sessions[2];
    SessionInputs::Frame input{};
    input[0].attack = held;
    input[0].attackPressed = pressed;
    REQUIRE(guest.inputReady());
    REQUIRE(MatchInputs::sample(guest, input));
    REQUIRE(guest.sampledInputs().size() == 1);
    CHECK(guest.sampledInputs().front().held(CommandHeld::Attack) == held);
    CHECK(guest.sampledInputs().front().pressed(CommandPress::Attack) == pressed);
}

TEST_CASE("one backpressured guest tap reaches native PartyMotion and releases exactly once",
          "[netplay][match-session][input-pacing][queued-attack][assets]") {
    const auto root = test::assetOrSkip("PDATA/WAR.WAD").parent_path().parent_path();
    test::FakeRenderDevice device;
    ClassDataSet classes;
    REQUIRE(classes.load(root / "PDATA"));
    REQUIRE(classes.stats(0));
    CharacterSave save;
    save.name = "TEST";
    save.autoAttack = false;
    save.progress().health = 1000;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(3, save, classes.stats(0), {}, 0);
    players[0].figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(players[0].figure);
    WorldCollision collision;
    collision.build({{{0, 1, 0}, {{{-100, 0, -100}, {100, 0, -100}, {100, 0, 100}}}, -1, 4},
                     {{0, 1, 0}, {{{-100, 0, -100}, {100, 0, 100}, {-100, 0, 100}}}, -1, 4}});
    u32 attackDeeds = 0;
    u32 releases = 0;
    PartyMotion::Events events;
    events.perform = [&](usize index, PartyMotion::Action action) {
        CHECK(index == 0);
        REQUIRE(action == PartyMotion::Action::ThrowWeapon);
        ++releases;
    };
    events.select = [](usize, const SelectorInput&, s32) {};
    events.advanceTurbo = [](usize, s32, f32) {};
    events.attackDeed = [&](usize index, bool strong, bool moved, const Vec3&) {
        CHECK(index == 0);
        CHECK_FALSE(strong);
        CHECK_FALSE(moved);
        ++attackDeeds;
        return PlayerDeed::Attack;
    };
    Room room({1, 0, 0, 2});
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    while (guest.inputReady()) {
        REQUIRE(MatchInputs::sample(guest, {}));
        room.pump();
    }
    SessionInputs::Frame tap{};
    tap[0].attack = tap[0].attackPressed = true;
    REQUIRE(MatchInputs::sample(guest, tap));
    CHECK(guest.sampledInputs().empty());
    const auto uploads = device.texturesCreated;
    bool observedThrow = false;
    for (u32 tick = 0; tick < 120; ++tick) {
        REQUIRE(MatchInputs::sample(guest, {}));
        room.pump();
        const auto frame = MatchInputs::advance(host);
        REQUIRE(frame);
        PartyMotion::step(players, *frame, false, 0, 1, 1.0f / 60, collision, events);
        observedThrow |= players[0].figure->animator().action() == PlayerAnimator::Action::Throw;
        REQUIRE(host.publish(snapshot(host)));
        room.pump();
    }
    CHECK(attackDeeds == 1);
    CHECK(observedThrow);
    CHECK(releases == 1);
    CHECK(PlayerAnimator::locomotion(players[0].figure->animator().action()));
    CHECK(device.texturesCreated == uploads);
}

TEST_CASE("guest clock skew stays bounded during a long running match",
          "[netplay][match-session][input-pacing]") {
    Room room({1, 2, 0, 0});
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    u32 moving = 0;
    SessionInputs::Frame input;
    input[0].move = {{1, 0}, 1};
    for (u32 tick = 0; tick < 3600; ++tick) {
        // A deliberately exaggerated 10% faster guest clock, including a long
        // burst with no completed snapshots and a guest that later falls behind.
        if (tick % 10 != 0 || tick < 2400) {
            REQUIRE(MatchInputs::sample(guest, input));
        }
        if (tick % 10 == 0 && tick < 2400) {
            REQUIRE(MatchInputs::sample(guest, input));
        }
        room.pump();
        const auto frame = host.advance();
        REQUIRE(frame);
        moving += (*frame)[1].magnitude > 0 ? 1U : 0U;
        if (tick % 3 == 0 && (tick < 1200 || tick > 1500)) {
            REQUIRE(host.publish(snapshot(host)));
        }
        room.pump();
        CHECK(guest.inputTick() < host.tick() + InputTimeline::kMaxAhead);
    }
    CHECK(moving > 3200);
    CHECK(guest.phase() == Phase::Running);
}

TEST_CASE("prepared publications keep session admission and the original captured bytes",
          "[netplay][match-session][prepared-publication]") {
    Room room({1, 2, 0, 0});
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    REQUIRE(host.advance());
    auto source = snapshot(host);
    const auto prepared = CombatPacket::prepare(source);
    REQUIRE(prepared);
    const auto bytes = CombatPacket::encode(source);
    REQUIRE(bytes);
    for (usize fault = 0; fault < 4; ++fault) {
        CAPTURE(fault);
        auto invalid = source;
        switch (fault) {
        case 0: ++invalid.motion.epoch; break;
        case 1: ++invalid.motion.tick; break;
        case 2: ++invalid.motion.players[1]->grant; break;
        default:
            invalid.motion.players[1].reset();
            invalid.players[1].reset();
            break;
        }
        const auto rejected = CombatPacket::prepare(invalid);
        REQUIRE(rejected); // Valid wire data is not sufficient session authority.
        CHECK_FALSE(host.publish(*rejected));
    }
    CHECK_FALSE(guest.publish(*prepared));
    source.motion.epoch = 0;
    source.players[0]->health = 7;
    REQUIRE(host.publish(*prepared));
    CHECK_FALSE(host.publish(*prepared));
    room.pump();
    REQUIRE(guest.playback().latest());
    CHECK(CombatPacket::encode(*guest.playback().latest()) == bytes);
    REQUIRE(host.advance());
    CHECK_FALSE(host.publish(*prepared)); // Previous tick, even though bytes remain valid.
    const auto next = CombatPacket::prepare(snapshot(host));
    REQUIRE(next);
    REQUIRE(host.requestPause());
    CHECK_FALSE(host.publish(*next));
    room.pump();
    room.ready(MatchTransition::Resume);
    REQUIRE(host.advance());
    CHECK_FALSE(host.publish(*prepared)); // Same tick, different admitted epoch.
}

TEST_CASE("pause resume and travel isolate queued actions and replicated state by epoch",
          "[netplay][match-session]") {
    Room room({1, 2, 0, 0});
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    const auto oldReady = message(guest, MatchControlKind::Ready);
    SessionInputs::Frame input;
    input[0].usePotion = true;
    input[0].attack = true;
    REQUIRE(MatchInputs::sample(guest, input));
    room.pump();
    REQUIRE(host.advance());
    const auto oldState = snapshot(host);
    REQUIRE(host.publish(oldState));
    room.pump();
    REQUIRE(guest.playback().latest());
    REQUIRE(guest.requestPause());
    REQUIRE(guest.requestPause());
    CHECK(guest.queuedControls() == 1);
    room.pump();
    CHECK(host.phase() == Phase::Paused);
    CHECK(guest.phase() == Phase::Paused);
    const auto before = host.tick();
    CHECK_FALSE(host.advance());
    CHECK_FALSE(host.publish(oldState));
    CHECK_FALSE(MatchInputs::sample(guest, input));
    host.update(100);
    CHECK(host.tick() == before);
    CHECK_FALSE(host.prepare(8, MatchTransition::Resume));
    room.ready(MatchTransition::Resume);
    CHECK(host.context().epoch == oldState.motion.epoch + 1);
    CHECK_FALSE(guest.playback().latest());
    CHECK(host.receive(1, oldReady) == Admission::Stale);
    const auto packets = CombatReplica::packets(oldState);
    REQUIRE(packets);
    CHECK(guest.receive(99, packets->front()) == Admission::Stale);
    for (u8 tick = 0; tick < 20; ++tick) {
        const auto frame = host.advance();
        REQUIRE(frame);
        CHECK_FALSE((*frame)[1].held(CommandHeld::Attack));
        CHECK_FALSE((*frame)[1].pressed(CommandPress::UsePotion));
    }
    room.ready(MatchTransition::Travel, 8);
    CHECK(host.tick() == 0);
    CHECK(guest.context().scene == 8);
    CHECK(guest.context().transition == MatchTransition::Travel);
}

TEST_CASE("pause and resume preserve reliable order when the control channel is congested",
          "[netplay][match-session]") {
    Room room({1, 2, 0, 0});
    room.ready();
    auto& host = room.sessions[1];
    room.wires[1].result = PacketTransport::SendResult::Congested;
    REQUIRE(host.requestPause());
    REQUIRE(host.prepare(7, MatchTransition::Resume));
    host.flush(room.wires[1]);
    CHECK(host.queuedControls() == 2);
    CHECK(room.sessions[2].phase() == Phase::Running);
    room.wires[1].result = PacketTransport::SendResult::Sent;
    room.pump();
    CHECK(host.queuedControls() == 0);
    CHECK(room.sessions[2].phase() == Phase::Loading);
    CHECK(room.sessions[2].context() == host.context());
}

TEST_CASE("unsent snapshots and input are bounded latest state not a reliable backlog",
          "[netplay][match-session]") {
    Room room({1, 2, 0, 0});
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    room.wires[1].result = PacketTransport::SendResult::Congested;
    room.wires[2].result = PacketTransport::SendResult::Congested;
    for (usize tick = 0; tick < 100; ++tick) {
        REQUIRE(MatchInputs::sample(guest, {}));
        REQUIRE(host.advance());
        REQUIRE(host.publish(snapshot(host)));
        host.flush(room.wires[1]);
        guest.flush(room.wires[2]);
    }
    room.wires[1].result = PacketTransport::SendResult::Sent;
    room.wires[2].result = PacketTransport::SendResult::Sent;
    guest.flush(room.wires[2]);
    REQUIRE(room.wires[2].sent.size() == 1);
    CHECK(room.wires[2].sent.front().delivery == PacketTransport::Delivery::Unreliable);
    const auto commands = InputPacket::decode(room.wires[2].sent.front().bytes);
    REQUIRE(commands);
    CHECK(commands->size() == InputHistory::kTicks);
    host.flush(room.wires[1]);
    const auto latest = CombatReplica::packets(snapshot(host));
    REQUIRE(latest);
    REQUIRE(room.wires[1].sent.size() == latest->size());
    for (usize i = 0; i < latest->size(); ++i) {
        CHECK(room.wires[1].sent[i].bytes == (*latest)[i]);
        CHECK(room.wires[1].sent[i].delivery == PacketTransport::Delivery::Unreliable);
    }
}

TEST_CASE("a congested partial snapshot completes while newer captures coalesce",
          "[netplay][match-session][prepared-publication]") {
    const bool usePrepared = GENERATE(false, true);
    Room room({1, 2, 0, 0});
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    auto& wire = room.wires[1];
    const auto dense = [&] {
        auto state = snapshot(host);
        state.geometry = SceneGeometry{1, 806, 0, {}};
        for (u32 i = 0; i < 806; ++i) {
            const u32 row = i / 31;
            const Vec3 position{static_cast<f32>(i % 31), static_cast<f32>(host.tick()) / 60,
                                static_cast<f32>(row)};
            state.geometry->objects.push_back({i, 1, glm::translate(Mat4{1}, position), 1, true});
        }
        return state;
    };
    const auto publish = [&](const CombatSnapshot& state) {
        if (!usePrepared) {
            return host.publish(state);
        }
        const auto prepared = CombatPacket::prepare(state);
        return prepared && host.publish(*prepared);
    };
    REQUIRE(host.advance());
    const auto first = dense();
    const auto packets = CombatReplica::packets(first, SnapshotBlock::Compression::Automatic,
                                                CombatReplica::Recovery::SingleLoss);
    REQUIRE(packets);
    REQUIRE(packets->size() > 2);
    REQUIRE(publish(first));
    const auto drain = [&] {
        host.flush(wire);
        for (const auto& packet : wire.sent) {
            const auto admission = guest.receive(99, packet.bytes);
            REQUIRE((admission == Admission::Accepted || admission == Admission::Stale));
        }
        wire.sent.clear();
        guest.flush(room.wires[2]);
        for (const auto& packet : room.wires[2].sent) {
            CHECK(host.receive(1, packet.bytes) == Admission::Accepted);
        }
        room.wires[2].sent.clear();
    };
    // Only one fragment fits per capture interval: replacing unfinished batches
    // here used to send first fragments forever without one complete checkpoint.
    for (usize tick = 0; tick < packets->size() * 2 + 5; ++tick) {
        REQUIRE(host.advance());
        if (host.publicationReady()) {
            REQUIRE(publish(dense()));
        }
        wire.budget = 1;
        drain();
    }
    REQUIRE(guest.playback().latest());
    CHECK(guest.playback().latest()->motion.tick > 0);
    const auto before = guest.playback().latest()->motion.tick;
    // Hundreds of further captures replace one pending batch, not the in-flight
    // batch and not a growing list of stale snapshots.
    wire.budget = 0;
    for (usize tick = 0; tick < 200; ++tick) {
        REQUIRE(host.advance());
        if (host.publicationReady()) {
            REQUIRE(publish(dense()));
        }
        drain();
    }
    wire.budget = std::numeric_limits<usize>::max();
    host.flush(wire);
    const auto last = CombatReplica::packets(dense(), SnapshotBlock::Compression::Automatic,
                                             CombatReplica::Recovery::SingleLoss);
    REQUIRE(last);
    CHECK(wire.sent.size() <= packets->size() + last->size());
    drain();
    REQUIRE(guest.playback().latest());
    CHECK(guest.playback().latest()->motion.tick > before);
    CHECK(CombatPacket::encode(*guest.playback().latest()) == CombatPacket::encode(dense()));
}

TEST_CASE("completed snapshot receipts bound traffic independently for each peer",
          "[netplay][match-session][snapshot-receipts]") {
    auto storage = std::make_unique<Room>(MatchOwners{1, 2, 3, 0});
    auto& room = *storage;
    room.ready();
    auto& host = room.sessions[1];
    auto& fast = room.sessions[2];
    std::vector<Wire::Packet> slow;
    usize fastPublications = 0;
    for (u64 tick = 0; tick < 20; ++tick) {
        REQUIRE(host.advance());
        REQUIRE(host.publicationReady());
        REQUIRE(host.publish(snapshot(host)));
        CHECK_FALSE(host.publicationReady()); // No second publication of this tick.
        host.flush(room.wires[1]);
        bool sentFast = false;
        bool sentSlow = false;
        for (const auto& packet : room.wires[1].sent) {
            if (packet.connection == 1) {
                sentFast = true;
                const auto accepted = fast.receive(99, packet.bytes);
                REQUIRE((accepted == Admission::Accepted || accepted == Admission::Stale));
            } else {
                sentSlow = true;
                slow.push_back(packet);
            }
        }
        room.wires[1].sent.clear();
        CHECK(sentFast);
        CHECK(sentSlow == (tick < MatchSession::kPublicationWindow));
        fast.flush(room.wires[2]);
        REQUIRE(room.wires[2].sent.size() == 1);
        const auto& receipt = room.wires[2].sent.front();
        CHECK(receipt.delivery == PacketTransport::Delivery::Reliable);
        REQUIRE(CombatReplica::decodeReceipt(receipt.bytes));
        REQUIRE(host.receive(1, receipt.bytes) == Admission::Accepted);
        CHECK(host.receive(1, receipt.bytes) == Admission::Stale);
        room.wires[2].sent.clear();
        ++fastPublications;
    }
    CHECK(fastPublications == 20);
    auto& delayed = room.sessions[3];
    for (const auto& packet : slow) {
        const auto accepted = delayed.receive(99, packet.bytes);
        REQUIRE((accepted == Admission::Accepted || accepted == Admission::Stale));
    }
    delayed.flush(room.wires[3]);
    REQUIRE(room.wires[3].sent.size() == 1); // Latest completion subsumes the first.
    REQUIRE(host.receive(2, room.wires[3].sent.front().bytes) == Admission::Accepted);
    room.wires[3].sent.clear();
    REQUIRE(host.advance());
    REQUIRE(host.publish(snapshot(host)));
    room.pump();
    REQUIRE(delayed.playback().latest());
    CHECK(delayed.playback().latest()->motion.tick == host.tick() - 1);
}

TEST_CASE("missing snapshot chunks or receipts recover only with a newer timed publication",
          "[netplay][match-session][snapshot-receipts]") {
    const bool lostReceipt = GENERATE(false, true);
    auto storage = std::make_unique<Room>(MatchOwners{1, 2, 0, 0});
    auto& room = *storage;
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    for (usize i = 0; i < MatchSession::kPublicationWindow; ++i) {
        REQUIRE(host.advance());
        REQUIRE(host.publish(snapshot(host)));
        host.flush(room.wires[1]);
        if (lostReceipt) {
            for (const auto& packet : room.wires[1].sent) {
                const auto accepted = guest.receive(99, packet.bytes);
                REQUIRE((accepted == Admission::Accepted || accepted == Admission::Stale));
            }
            guest.flush(room.wires[2]);
            REQUIRE(room.wires[2].sent.size() == 1);
            room.wires[2].sent.clear(); // Lost feedback must not permanently close the window.
        }
        room.wires[1].sent.clear(); // Or lose the complete unreliable batch.
    }
    REQUIRE(host.advance());
    CHECK_FALSE(host.publicationReady());
    CHECK_FALSE(host.publish(snapshot(host)));
    for (const f64 seconds :
         {-1.0, 0.0, std::numeric_limits<f64>::infinity(), std::numeric_limits<f64>::quiet_NaN()}) {
        host.update(seconds);
        CHECK_FALSE(host.publicationReady());
    }
    host.update(MatchSession::kPublicationTimeout - 0.125);
    CHECK_FALSE(host.publicationReady());
    host.flush(room.wires[1]);
    CHECK(room.wires[1].sent.empty()); // No retransmission of either old tick.
    host.update(0.125);
    REQUIRE(host.publicationReady());
    REQUIRE(host.publish(snapshot(host)));
    CHECK_FALSE(host.publicationReady());
    CHECK_FALSE(host.publish(snapshot(host)));
    room.pump();
    REQUIRE(guest.playback().latest());
    CHECK(guest.playback().latest()->motion.tick == host.tick() - 1);
}

TEST_CASE("snapshot receipts authenticate the peer epoch digest and begun publication",
          "[netplay][match-session][snapshot-receipts]") {
    auto storage = std::make_unique<Room>(MatchOwners{1, 2, 0, 0});
    auto& room = *storage;
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    CombatReplica::Receipt identity;
    REQUIRE(host.advance());
    REQUIRE(CombatReplica::packets(snapshot(host), SnapshotBlock::Compression::Automatic,
                                   CombatReplica::Recovery::SingleLoss, &identity));
    REQUIRE(host.publish(snapshot(host)));
    const auto bytes = CombatReplica::encodeReceipt(identity);
    REQUIRE(bytes);
    CHECK(host.receive(99, *bytes) == Admission::WrongPeer);
    CHECK(guest.receive(99, *bytes) == Admission::WrongPeer);
    CHECK(host.receive(1, *bytes) == Admission::Invalid); // Not sent yet.
    host.flush(room.wires[1]);
    for (usize fault = 0; fault < 3; ++fault) {
        auto forged = identity;
        if (fault == 0) {
            ++forged.epoch;
        } else if (fault == 1) {
            ++forged.tick;
        } else {
            ++forged.checksum;
        }
        const auto bad = CombatReplica::encodeReceipt(forged);
        REQUIRE(bad);
        CHECK(host.receive(1, *bad) == (fault == 0 ? Admission::Stale : Admission::Invalid));
    }
    REQUIRE(host.advance());
    REQUIRE(host.publish(snapshot(host)));
    host.flush(room.wires[1]);
    REQUIRE(host.advance());
    CHECK_FALSE(host.publicationReady());
    REQUIRE(host.receive(1, *bytes) == Admission::Accepted);
    CHECK(host.publicationReady());
    CHECK(host.receive(1, *bytes) == Admission::Stale);
    REQUIRE(host.requestPause());
    CHECK_FALSE(host.publicationReady());
    CHECK(host.receive(1, *bytes) == Admission::Stale);
    room.pump();
    room.ready(MatchTransition::Resume);
    REQUIRE(host.advance());
    CHECK(host.publicationReady());
    CHECK(host.receive(1, *bytes) == Admission::Stale);
    host.disconnected(1);
    CHECK_FALSE(host.publicationReady());
    CHECK(host.receive(1, *bytes) == Admission::WrongPeer);
}

TEST_CASE("snapshot bursts finish their repair tail after an early completion receipt",
          "[netplay][match-session][snapshot-receipts]") {
    auto storage = std::make_unique<Room>(MatchOwners{1, 2, 0, 0});
    auto& room = *storage;
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    auto& wire = room.wires[1];
    REQUIRE(host.advance());
    auto source = std::make_unique<CombatSnapshot>(snapshot(host));
    source->geometry = SceneGeometry{1, static_cast<u32>(SceneGeometry::kMaxStates), 0, {}};
    u32 random = 17;
    for (u32 index = 0; index < SceneGeometry::kMaxStates; ++index) {
        Mat4 matrix{1};
        for (s32 column = 0; column < 4; ++column) {
            for (s32 row = 0; row < 3; ++row) {
                random = random * 1664525U + 1013904223U;
                matrix[column][row] = static_cast<f32>(random >> 8U) / 65536.0f;
            }
        }
        source->geometry->objects.push_back({index, 1, matrix, 1, true});
    }
    const auto expected = CombatReplica::packets(*source, SnapshotBlock::Compression::Automatic,
                                                 CombatReplica::Recovery::SingleLoss);
    REQUIRE(expected);
    REQUIRE(expected->size() > MatchSession::kPublicationBurst);
    REQUIRE(host.publish(*source));
    host.flush(wire);
    REQUIRE(wire.sent.size() == MatchSession::kPublicationBurst);
    usize received = 0;
    const auto consume = [&] {
        for (const auto& packet : wire.sent) {
            REQUIRE(received < expected->size());
            CHECK(packet.bytes == (*expected)[received]);
            const auto accepted = guest.receive(99, packet.bytes);
            REQUIRE((accepted == Admission::Accepted || accepted == Admission::Stale));
            ++received;
        }
        wire.sent.clear();
    };
    consume();
    host.update(1000); // An unfinished batch cannot expire or be replaced.
    while (!guest.playback().latest()) {
        REQUIRE(received < expected->size());
        wire.budget = 1;
        host.flush(wire);
        REQUIRE(wire.sent.size() == 1);
        consume();
    }
    REQUIRE(received < expected->size()); // Data committed, repair tail not yet sent.
    REQUIRE(MatchInputs::sample(guest, {}));
    room.wires[2].budget = 1;
    guest.flush(room.wires[2]);
    REQUIRE(room.wires[2].sent.size() == 1);
    CHECK(InputPacket::decode(room.wires[2].sent.front().bytes)); // Input gets the first slot.
    room.wires[2].sent.clear();
    room.wires[2].budget = 1;
    guest.flush(room.wires[2]);
    REQUIRE(room.wires[2].sent.size() == 1);
    CHECK(CombatReplica::recognizesReceipt(room.wires[2].sent.front().bytes));
    REQUIRE(host.receive(1, room.wires[2].sent.front().bytes) == Admission::Accepted);
    room.wires[2].sent.clear();
    wire.budget = std::numeric_limits<usize>::max();
    while (received < expected->size()) {
        host.flush(wire);
        REQUIRE_FALSE(wire.sent.empty());
        CHECK(wire.sent.size() <= MatchSession::kPublicationBurst);
        consume();
    }
    host.flush(wire);
    CHECK(wire.sent.empty());
    CHECK_FALSE(host.publish(*source));
    REQUIRE(host.advance());
    CHECK(host.publicationReady());
}

TEST_CASE("load timeout aborts even before the prepare message reaches a guest",
          "[netplay][match-session]") {
    Room room({1, 2, 0, 0});
    auto& host = room.sessions[1];
    REQUIRE(host.prepare(7));
    host.update(-1);
    host.update(std::numeric_limits<f64>::quiet_NaN());
    host.update(std::numeric_limits<f64>::infinity());
    host.update(MatchSession::kLoadTimeout - 0.5);
    CHECK(host.phase() == Phase::Loading);
    host.update(0.5);
    CHECK(host.phase() == Phase::Stopped);
    CHECK(host.stopReason() == MatchStop::LoadTimeout);
    room.pump();
    CHECK(room.sessions[2].phase() == Phase::Stopped);
    CHECK(room.sessions[2].stopReason() == MatchStop::LoadTimeout);
}

TEST_CASE("departure stops every remaining machine without stale input or state revival",
          "[netplay][match-session]") {
    Room room;
    room.ready();
    auto& host = room.sessions[1];
    const auto oldPrepare = message(host, MatchControlKind::Prepare);
    SECTION("running") {}
    SECTION("paused") {
        REQUIRE(host.requestPause());
        room.pump();
    }
    SECTION("loading next stage") {
        const auto profiles = room.party();
        REQUIRE(host.prepare(8, MatchTransition::Travel, &profiles));
        room.pump();
    }
    host.disconnected(1); // remote peer 2, NOT host peer 1
    room.sessions[2].disconnected(99);
    room.pump();
    for (u8 peer = 1; peer <= 4; ++peer) {
        CHECK(room.sessions[peer].phase() == Phase::Stopped);
        CHECK(room.sessions[peer].stopReason() == MatchStop::Disconnected);
        CHECK_FALSE(room.sessions[peer].advance());
        CHECK_FALSE(room.sessions[peer].playback().latest());
        CHECK_FALSE(MatchInputs::sample(room.sessions[peer], {}));
    }
    CHECK(room.sessions[3].receive(99, oldPrepare) == Admission::WrongPhase);
}

TEST_CASE("transport send failure terminates loading instead of pretending a start succeeded",
          "[netplay][match-session]") {
    Room room({1, 2, 0, 0});
    auto& host = room.sessions[1];
    REQUIRE(host.prepare(7));
    room.wires[1].result = PacketTransport::SendResult::Disconnected;
    host.flush(room.wires[1]);
    CHECK(host.phase() == Phase::Stopped);
    CHECK_FALSE(host.advance());
    CHECK_FALSE(host.loaded());
    CHECK(host.queuedControls() == 0);
}

TEST_CASE("match lifecycle drives real four player movement events and one shared camera",
          "[netplay][match-session][multiplayer][party-motion]") {
    Room room({1, 2, 2, 1});
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    WorldCollision collision;
    CollisionTriangle first;
    first.vertices = {Vec3{-200, 0, -200}, Vec3{200, 0, -200}, Vec3{200, 0, 200}};
    first.normal = {0, 1, 0};
    first.object = 0;
    auto second = first;
    second.vertices = {Vec3{-200, 0, -200}, Vec3{200, 0, 200}, Vec3{-200, 0, 200}};
    collision.build({first, second});
    std::array<PlayerRuntime, 4> players;
    std::array<Vec3, 4> starts{};
    std::array<CameraSubject, 4> subjects;
    for (usize seat = 0; seat < players.size(); ++seat) {
        starts[seat] = {static_cast<f32>(seat) * 2 - 3, 0, 0};
        players[seat].actor.spawn(static_cast<s32>(seat), {}, nullptr, starts[seat], 0);
        subjects[seat] = {players[seat].actor.position(), players[seat].actor.followPoint()};
    }
    TowerCamera camera;
    const CameraRange range;
    CameraView view;
    camera.reset(subjects, {}, range, view);
    std::array<u32, 4> potions{};
    std::array<u32, 4> toggles{};
    PartyMotion::Events events;
    events.perform = [&](usize player, PartyMotion::Action action) {
        REQUIRE(action == PartyMotion::Action::NoPotion);
        ++potions[player];
    };
    events.select = [&](usize player, const SelectorInput& input, s32 /*ticks*/) {
        if (input.up) {
            ++toggles[player];
        }
    };
    events.limitMovement = [&](usize player, const Vec3& from, const Vec3& to) {
        const auto& actor = players[player].actor;
        return CameraMovementLimit::constrain(from, to, camera.attention(), camera.camera(), view,
                                              actor.followPoint() - actor.position());
    };
    for (u32 tick = 0; tick < 180; ++tick) {
        if (tick == 90) {
            REQUIRE(guest.requestPause());
            room.pump();
            const auto held = CombatPacket::encode(*guest.playback().latest());
            for (u32 paused = 0; paused < 100; ++paused) {
                CHECK_FALSE(MatchInputs::advance(host));
                CHECK_FALSE(MatchInputs::sample(guest, {}));
                CHECK(CombatPacket::encode(*guest.playback().latest()) == held);
            }
            room.ready(MatchTransition::Resume);
        }
        SessionInputs::Frame local;
        for (auto& input : local) {
            input.move = {{1, 0}, 1};
            input.usePotion = tick == 0 || tick == 90;
            input.selector.up = input.usePotion;
        }
        REQUIRE(MatchInputs::sample(host, local));
        REQUIRE(MatchInputs::sample(guest, local));
        room.pump();
        const auto inputs = MatchInputs::advance(host);
        REQUIRE(inputs);
        CHECK_FALSE(MatchInputs::advance(guest));
        const auto moved = PartyMotion::step(players, *inputs, false, camera.yaw(), 1, 1.0f / 60,
                                             collision, events);
        camera.update(moved, {}, range, view, 1.0f / 60);
        auto state = snapshot(host);
        state.motion.camera = camera.camera();
        state.motion.aspect = view.aspect;
        state.motion.horizontalFov = view.horizontalFov;
        for (usize seat = 0; seat < players.size(); ++seat) {
            state.motion.players[seat]->position = players[seat].actor.position();
            state.motion.players[seat]->yaw = players[seat].actor.yaw();
        }
        REQUIRE(host.publish(state));
        room.pump();
        REQUIRE(guest.playback().latest());
        CHECK(CombatPacket::encode(*guest.playback().latest()) == CombatPacket::encode(state));
    }
    for (usize seat = 0; seat < players.size(); ++seat) {
        CHECK(potions[seat] == 2);
        CHECK(toggles[seat] == 2);
        CHECK(glm::length(players[seat].actor.position() - starts[seat]) > 1);
    }
}
TEST_CASE("input scheduling adapts to delayed lossy links without repeating late edges",
          "[netplay][prediction][match-session]") {
    const u64 latency = GENERATE(0, 3, 6, 9);
    const bool measuredPath = GENERATE(false, true);
    Room room({1, 2, 0, 0});
    if (measuredPath) {
        room.wires[2].stats = PacketTransport::Statistics{static_cast<s32>(latency * 1000 / 30)};
    }
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    struct Delayed {
        u64 due;
        bool toHost;
        std::vector<u8> bytes;
    };
    std::vector<Delayed> pending;
    usize serial = 0;
    u32 lateAtWarmup = 0;
    usize actions = 0;
    std::vector<std::array<u64, 7>> timingChanges;
    for (u64 tick = 0; tick < 900; ++tick) {
        const auto now = MatchSession::Time{std::chrono::seconds{1}} +
                         std::chrono::duration_cast<MatchSession::Clock::duration>(
                             std::chrono::duration<f64>{static_cast<f64>(tick) / 60});
        host.update(1.0 / 60, now);
        const auto previousLead = guest.inputLead();
        const auto previousLate = host.inputDiagnostics(1).late;
        guest.update(1.0 / 60, now);
        std::ranges::stable_sort(pending, {}, &Delayed::due);
        for (const auto& packet : pending) {
            if (packet.due > tick) {
                break;
            }
            const auto admitted = packet.toHost ? host.receive(1, packet.bytes, now)
                                                : guest.receive(99, packet.bytes, now);
            REQUIRE((admitted == Admission::Accepted || admitted == Admission::Stale));
        }
        std::erase_if(pending, [tick](const auto& packet) { return packet.due <= tick; });
        SessionInputs::Frame local;
        local[0].move = {{0, 1}, 1};
        local[0].usePotion = tick % 60 == 0 && (tick >= 400 || measuredPath);
        REQUIRE(guest.sample(MatchInputs::commands(guest, local), now));
        const auto frame = host.advance();
        REQUIRE(frame);
        actions += (*frame)[1].pressed(CommandPress::UsePotion) ? 1U : 0U;
        if (tick % 3 == 0 && host.publicationReady()) {
            auto state = snapshot(host);
            const auto& diagnostics = host.inputDiagnostics(1);
            state.players[1]->movement.receivedInput = diagnostics.received.value_or(0);
            state.players[1]->movement.arrivalMargin = diagnostics.arrivalMargin;
            state.players[1]->movement.lateInputs = diagnostics.late;
            REQUIRE(host.publish(state));
        }
        for (const u8 peer : {u8{1}, u8{2}}) {
            room.sessions[peer].flush(room.wires[peer], now);
            for (const auto& packet : room.wires[peer].sent) {
                ++serial;
                if (serial % 11 == 0) {
                    continue;
                }
                const Delayed delivery{tick + latency + serial % 3, peer == 2, packet.bytes};
                pending.push_back(delivery);
                if (serial % 7 == 0) {
                    auto duplicate = delivery;
                    duplicate.due += 4;
                    pending.push_back(std::move(duplicate));
                }
            }
            room.wires[peer].sent.clear();
        }
        if (tick == 400) {
            lateAtWarmup = host.inputDiagnostics(1).late;
        }
        if (tick >= 400 &&
            (guest.inputLead() != previousLead || host.inputDiagnostics(1).late != previousLate)) {
            const auto* latest = guest.playback().latest();
            timingChanges.push_back({tick, host.tick(), guest.inputTick(), guest.inputLead(),
                                     latest ? latest->motion.tick : 0,
                                     host.inputDiagnostics(1).received.value_or(0),
                                     host.inputDiagnostics(1).late});
        }
        CHECK(guest.inputLead() <= 30);
    }
    CAPTURE(latency, guest.inputLead(), lateAtWarmup, host.inputDiagnostics(1).late);
    if (host.inputDiagnostics(1).late != lateAtWarmup) {
        for (const auto& change : timingChanges) {
            UNSCOPED_INFO("timing tick/host/next/lead/checkpoint/received/late: "
                          << change[0] << '/' << change[1] << '/' << change[2] << '/' << change[3]
                          << '/' << change[4] << '/' << change[5] << '/' << change[6]);
        }
    }
    CHECK(host.inputDiagnostics(1).late == lateAtWarmup);
    CHECK(actions == (measuredPath ? 15 : 8));
    CHECK(guest.inputTick() < host.tick() + 36);
}

TEST_CASE("new scenes seed their first input deadline from measured path RTT",
          "[netplay][latency][match-session]") {
    const s32 rtt = GENERATE(-1, 0, 1, 50, 150, 300, 2000);
    Room room({1, 2, 0, 0});
    room.wires[2].stats = PacketTransport::Statistics{rtt};
    room.ready();
    auto& guest = room.sessions[2];
    const auto expected =
        static_cast<u8>(rtt < 0 ? 3 : std::min(30, 3 + (std::min(rtt, 500) * 60 + 999) / 1000));
    CHECK(guest.inputLead() == expected);
    CHECK(guest.inputTick() == expected);
    REQUIRE(MatchInputs::sample(guest, {}));
    CHECK(guest.sampledInputs().front().tick == expected);
    // Travel remeasures the path instead of keeping the old scene's high lead.
    room.wires[2].stats = PacketTransport::Statistics{0};
    room.ready(MatchTransition::Travel, 8);
    CHECK(guest.inputLead() == 3);
    CHECK(guest.inputTick() == 3);
}

TEST_CASE("healthy delivery recovers inflated input lead without rewinding commands or losing taps",
          "[netplay][latency][match-session][lead-recovery]") {
    const bool twoLocalSeats = GENERATE(false, true);
    Room room(twoLocalSeats ? MatchOwners{1, 2, 2, 0} : MatchOwners{1, 2, 0, 0});
    room.wires[2].stats = PacketTransport::Statistics{600};
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    REQUIRE(guest.inputLead() == 30);
    std::array<u64, 4> lastInput{};
    std::array<usize, 4> actions{};
    usize reductions = 0;
    u8 previousLead = guest.inputLead();
    for (u64 tick = 0; tick < 1200; ++tick) {
        const auto now = MatchSession::Time{std::chrono::seconds{1}} +
                         std::chrono::duration_cast<MatchSession::Clock::duration>(
                             std::chrono::duration<f64>{static_cast<f64>(tick) / 60});
        host.update(1.0 / 60, now);
        guest.update(1.0 / 60, now);
        SessionInputs::Frame local;
        for (auto& seat : local) {
            seat.usePotion = tick < 1100 && tick % 40 == 0;
        }
        REQUIRE(guest.sample(MatchInputs::commands(guest, local), now));
        for (const auto& command : guest.sampledInputs()) {
            CHECK(command.tick > lastInput[command.seat]);
            lastInput[command.seat] = command.tick;
        }
        room.pump(now);
        const auto frame = host.advance();
        REQUIRE(frame);
        for (usize seat = 1; seat < frame->size(); ++seat) {
            actions[seat] += (*frame)[seat].pressed(CommandPress::UsePotion) ? 1U : 0U;
        }
        if (tick % 3 == 0) {
            auto state = snapshot(host);
            for (usize seat = 1; seat < state.players.size(); ++seat) {
                if (!state.players[seat]) {
                    continue;
                }
                const auto& feedback = host.inputDiagnostics(seat);
                state.players[seat]->movement.receivedInput = feedback.received.value_or(0);
                state.players[seat]->movement.arrivalMargin = feedback.arrivalMargin;
                state.players[seat]->movement.lateInputs = feedback.late;
            }
            REQUIRE(host.publish(state));
            room.pump(now);
        }
        if (guest.inputLead() < previousLead) {
            // The first exact admission pairs the host clock to a successful
            // send, replacing the old RTT-sized bootstrap margin at once.
            CHECK((reductions == 0 || previousLead - guest.inputLead() <= 3));
            ++reductions;
        }
        previousLead = guest.inputLead();
    }
    CHECK(reductions >= 1);
    CHECK(guest.inputLead() == guest.context().inputLead);
    for (usize seat = 1; seat <= (twoLocalSeats ? 2U : 1U); ++seat) {
        CHECK(host.inputDiagnostics(seat).late == 0);
        CHECK(actions[seat] == 28);
    }
}

TEST_CASE("lead recovery requires fresh safe feedback from every local seat",
          "[netplay][latency][match-session][lead-recovery]") {
    const s32 obstruction = GENERATE(0, 1, 2, 3);
    Room room({1, 2, 2, 0});
    room.wires[2].stats = PacketTransport::Statistics{600};
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    for (u64 tick = 0; tick < 240; ++tick) {
        REQUIRE(host.advance());
        if (tick % 3 == 0) {
            auto state = snapshot(host);
            for (usize seat = 1; seat <= 2; ++seat) {
                state.players[seat]->movement.receivedInput = tick + 30;
                state.players[seat]->movement.arrivalMargin = 30;
            }
            // One healthy seat cannot mask a second seat's stalled or marginal
            // delivery. These are bounded authenticated host diagnostics.
            if (obstruction == 0) {
                state.players[2]->movement.receivedInput = 1;
            } else if (obstruction == 1) {
                state.players[2]->movement.arrivalMargin = 3;
            } else if (obstruction == 2) {
                state.players[2].reset();
                state.motion.players[2].reset();
            } else if (tick >= 21 && tick < 210) {
                // Seven healthy samples, a long stale interval, then fresh
                // feedback must start a new recovery window, not finish it.
                state.players[2]->movement.receivedInput = 48;
            }
            if (obstruction == 2) {
                // A sparse valid packet can arrive from the authenticated host
                // even though its current publisher enforces a complete roster.
                const auto packets = CombatReplica::packets(state);
                REQUIRE(packets);
                for (const auto& packet : *packets) {
                    REQUIRE(guest.receive(99, packet) == Admission::Accepted);
                }
            } else {
                REQUIRE(host.publish(state));
                room.pump();
            }
        }
        CHECK(guest.inputLead() == 30);
    }
}

TEST_CASE("input lead increases use the worst local seat independent of seat order",
          "[netplay][latency][match-session][lead-recovery]") {
    const bool reversed = GENERATE(false, true);
    Room room({1, 2, 2, 0});
    room.ready();
    auto& host = room.sessions[1];
    auto& guest = room.sessions[2];
    REQUIRE(host.advance());
    auto state = snapshot(host);
    state.players[1]->movement.receivedInput = 1;
    state.players[2]->movement.receivedInput = 1;
    state.players[1]->movement.arrivalMargin = reversed ? -7 : 0;
    state.players[2]->movement.arrivalMargin = reversed ? 0 : -7;
    REQUIRE(host.publish(state));
    room.pump();
    CHECK(guest.inputLead() == 12);
}

TEST_CASE("clean links do not impose a six-tick input floor on guests",
          "[netplay][latency][match-session]") {
    const bool sampleAfterHost = GENERATE(false, true);
    // Two local guest seats and a separate remote seat must share neither input
    // counters nor pressed edges. Both independently paced update orders work.
    Room room({1, 2, 2, 3});
    room.ready();
    auto& host = room.sessions[1];
    std::array<s32, 4> actions{};
    for (u64 tick = 0; tick < 180; ++tick) {
        const auto sample = [&] {
            for (u8 peer = 2; peer <= 3; ++peer) {
                auto& guest = room.sessions[peer];
                CHECK(guest.inputLead() == 3);
                CHECK(guest.inputTick() <= host.tick() + 3);
                SessionInputs::Frame input;
                for (auto& seat : input) {
                    seat.usePotion = tick < 160 && tick % 20 == 0;
                }
                REQUIRE(MatchInputs::sample(guest, input));
            }
            room.pump();
        };
        if (!sampleAfterHost) {
            sample();
        }
        const auto frame = host.advance();
        REQUIRE(frame);
        for (usize seat = 1; seat < frame->size(); ++seat) {
            actions[seat] += (*frame)[seat].pressed(CommandPress::UsePotion) ? 1 : 0;
        }
        if (tick % 3 == 0) {
            auto state = snapshot(host);
            for (usize seat = 1; seat < actions.size(); ++seat) {
                const auto& feedback = host.inputDiagnostics(seat);
                state.players[seat]->movement.receivedInput = feedback.received.value_or(0);
                state.players[seat]->movement.arrivalMargin = feedback.arrivalMargin;
                state.players[seat]->movement.lateInputs = feedback.late;
            }
            REQUIRE(host.publish(state));
            room.pump();
        }
        if (sampleAfterHost) {
            sample();
        }
    }
    for (usize seat = 1; seat < actions.size(); ++seat) {
        CHECK(host.inputDiagnostics(seat).late == 0);
        CHECK(actions[seat] == 8);
    }
}

TEST_CASE("a stalled guest advances only its new attack deadline using monotonic time",
          "[netplay][latency][match-session][input-admission-trace][asymmetric-input-clock]") {
    const bool maximumLead = GENERATE(false, true);
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 0, 0});
    room->wires[2].stats = PacketTransport::Statistics{maximumLead ? 500 : 0};
    room->ready();
    auto& host = room->sessions[1];
    auto& guest = room->sessions[2];
    const u64 lead = maximumLead ? 30 : 3;
    REQUIRE(guest.inputLead() == lead);
    const MatchSession::Time start{std::chrono::seconds{1}};

    // Deliver a real snapshot after its capture. At maximum lead, the delayed
    // snapshot consumes 27 ticks of that lead, as a slow complete seed can do.
    REQUIRE(host.advance());
    const auto baseline = std::make_unique<CombatSnapshot>(snapshot(host));
    REQUIRE(host.publish(*baseline));
    for (u64 tick = 0; tick < lead - 3; ++tick) {
        REQUIRE(host.advance());
    }
    room->pump(start);
    REQUIRE(guest.playback().latest());
    CHECK(guest.playback().latest()->motion.tick == 0);
    CHECK(host.tick() == lead - 2);
    REQUIRE(guest.sample(MatchInputs::commands(guest, {}), start));
    REQUIRE(guest.sampledInputs().size() == 1);
    const auto prior = guest.sampledInputs().front();
    REQUIRE(prior.tick == lead);
    room->pump(start); // neutral sample arrives two host ticks before its deadline

    // No sleeps or benchmark: the host runs twelve 60 Hz ticks while the guest
    // performs no update, exactly 200 ms of asymmetric simulation progress.
    for (u64 tick = 0; tick < 12; ++tick) {
        host.update(1.0 / 60);
        REQUIRE(host.advance());
    }
    REQUIRE(host.resolvedInput(1));
    CHECK(host.resolvedInput(1)->tick == prior.tick);
    CHECK(host.tick() == lead + 10);
    CHECK(guest.inputTick() == prior.tick + 1);
    REQUIRE(guest.inputReady());
    const auto resumed = start + std::chrono::milliseconds{200};
    guest.update(1.0 / 60, resumed);
    CHECK(guest.inputTick() == lead + 12);
    REQUIRE(guest.inputReady());
    SessionInputs::Frame attack;
    attack[0].attack = attack[0].attackPressed = true;
    REQUIRE(guest.sample(MatchInputs::commands(guest, attack), resumed));
    REQUIRE(guest.sampledInputs().size() == 1);
    const auto current = guest.sampledInputs().front();
    REQUIRE(current.tick == lead + 12);
    REQUIRE(current.held(CommandHeld::Attack));
    REQUIRE(current.pressed(CommandPress::Attack));

    // Replay retains the original neutral sample and the newly stamped attack.
    // Filling the skipped interval cannot fabricate another attack edge.
    RollbackInputHistory history;
    REQUIRE(history.begin(guest.context().epoch, guest.context().grants));
    REQUIRE(history.resolve(guest.context().epoch, baseline->motion.tick));
    REQUIRE(history.record(std::span{&prior, 1}, 0));
    REQUIRE(history.record(std::span{&current, 1}, 1));
    const auto replay = history.replay(1);
    REQUIRE_FALSE(replay.empty());
    CHECK(replay.back().input.epoch == current.epoch);
    CHECK(replay.back().input.grant == current.grant);
    CHECK(replay.back().input.seat == current.seat);
    CHECK(replay.back().input.tick == current.tick);
    CHECK(replay.back().input.heldButtons == current.heldButtons);
    CHECK(replay.back().input.pressedButtons == current.pressedButtons);
    CHECK_FALSE(replay.back().predicted);
    for (const auto& entry : replay.first(replay.size() - 1)) {
        CHECK_FALSE(entry.input.pressed(CommandPress::Attack));
        if (entry.input.tick == prior.tick) {
            CHECK_FALSE(entry.predicted);
        }
    }

    room->pump(resumed);
    REQUIRE(host.commandTrace().size() == 1);
    const auto& accepted = host.commandTrace().front();
    CHECK(accepted.command.epoch == current.epoch);
    CHECK(accepted.command.grant == current.grant);
    CHECK(accepted.command.seat == current.seat);
    CHECK(accepted.command.tick == current.tick);
    CHECK(accepted.sender == guest.localPeer());
    CHECK(accepted.receivedTick == lead + 10);
    CHECK(accepted.admission == InputTimeline::Admission::Accepted);
    CHECK_FALSE(accepted.executedTick);
    CHECK(accepted.concealedTicks == 0);
    CHECK(host.inputDiagnostics(1).arrivalMargin == 2);
    while (host.tick() <= current.tick) {
        const auto frame = host.advance();
        REQUIRE(frame);
        CHECK((*frame)[1].pressed(CommandPress::Attack) == ((*frame)[1].tick == current.tick));
    }
    REQUIRE(accepted.executedTick);
    CHECK(*accepted.executedTick == current.tick);
    CHECK(host.droppedCommandTraces() == 0);
}

TEST_CASE("a stalled host bounds the monotonic estimate and preserves coalesced edges",
          "[netplay][match-session][input-pacing][asymmetric-input-clock]") {
    const bool maximumLead = GENERATE(false, true);
    const bool minimumTime = GENERATE(false, true);
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 2, 0});
    room->wires[2].stats = PacketTransport::Statistics{maximumLead ? 500 : 0};
    room->ready();
    auto& host = room->sessions[1];
    auto& guest = room->sessions[2];
    const u64 lead = maximumLead ? 30 : 3;
    const auto start =
        minimumTime ? MatchSession::Time::min() : MatchSession::Time{std::chrono::seconds{1}};
    REQUIRE(host.advance());
    REQUIRE(host.publish(snapshot(host)));
    room->pump(start);
    guest.update(1.0 / 60, MatchSession::Time::max());
    CHECK(guest.inputTick() == MatchSession::kMaxDeadlineAdvance + lead);
    usize samples = 0;
    while (guest.inputReady() && samples < InputTimeline::kMaxAhead) {
        REQUIRE(guest.sample(MatchInputs::commands(guest, {}), MatchSession::Time::max()));
        room->pump(MatchSession::Time::max());
        ++samples;
    }
    CHECK(samples == MatchSession::kInputSlack + 1);
    REQUIRE_FALSE(guest.inputReady());
    const u64 blockedTick = guest.inputTick();
    CHECK(blockedTick == MatchSession::kMaxDeadlineAdvance + lead + MatchSession::kInputSlack + 1);
    CHECK(blockedTick <= MatchSession::kMaxUnconfirmedTicks + 1);
    CHECK(blockedTick <= RollbackInputHistory::kHistory);
    CHECK(host.tick() == 1);

    SessionInputs::Frame edge;
    edge[0].usePotion = true;
    edge[1].attack = edge[1].attackPressed = true;
    for (usize repeat = 0; repeat < 20; ++repeat) {
        guest.update(1.0 / 60, MatchSession::Time::max());
        REQUIRE(guest.sample(MatchInputs::commands(guest, edge), MatchSession::Time::max()));
        CHECK(guest.sampledInputs().empty());
        CHECK(guest.inputTick() == blockedTick);
    }

    // Only actual host progress releases the bound; the queued tap is emitted
    // once, with the current controls, not once for every blocked local update.
    while (host.tick() <= blockedTick) {
        REQUIRE(host.advance());
    }
    REQUIRE(host.publish(snapshot(host)));
    room->pump(MatchSession::Time::max());
    REQUIRE(guest.inputReady());
    REQUIRE(guest.sample(MatchInputs::commands(guest, {}), MatchSession::Time::max()));
    REQUIRE(guest.sampledInputs().size() == 2);
    const u64 releasedTick = guest.sampledInputs().front().tick;
    CHECK(guest.sampledInputs()[0].pressed(CommandPress::UsePotion));
    CHECK(guest.sampledInputs()[1].pressed(CommandPress::Attack));
    CHECK(guest.sampledInputs()[1].held(CommandHeld::Attack));
    room->pump(MatchSession::Time::max());
    REQUIRE(guest.sample(MatchInputs::commands(guest, {}), MatchSession::Time::max()));
    room->pump(MatchSession::Time::max());
    usize potions = 0;
    usize attacks = 0;
    while (host.tick() <= releasedTick + InputTimeline::kHoldTicks + 2) {
        const auto frame = host.advance();
        REQUIRE(frame);
        potions += (*frame)[1].pressed(CommandPress::UsePotion) ? 1U : 0U;
        attacks += (*frame)[2].pressed(CommandPress::Attack) ? 1U : 0U;
        if ((*frame)[2].tick > releasedTick) {
            CHECK_FALSE((*frame)[2].held(CommandHeld::Attack));
        }
    }
    CHECK(potions == 1);
    CHECK(attacks == 1);
}

TEST_CASE("deadline anchors ignore stale peers and backwards time and reset at a new epoch",
          "[netplay][match-session][input-pacing][asymmetric-input-clock]") {
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 0, 0});
    room->ready();
    auto& host = room->sessions[1];
    auto& guest = room->sessions[2];
    const MatchSession::Time start{std::chrono::seconds{1}};
    REQUIRE(host.advance());
    auto state = std::make_unique<CombatSnapshot>(snapshot(host));
    const auto packets = CombatReplica::packets(*state);
    REQUIRE(packets);
    REQUIRE(host.publish(*state));
    room->pump(start);
    guest.update(1.0 / 60, start + std::chrono::milliseconds{100});
    CHECK(guest.inputTick() == 9);
    guest.update(1.0 / 60, MatchSession::Time::min());
    CHECK(guest.inputTick() == 9);
    auto revoked = MatchInputs::commands(guest, {});
    REQUIRE(revoked.size() == 1);
    ++revoked.front().grant;
    CHECK_FALSE(guest.sample(revoked, MatchSession::Time::max()));
    for (const auto& bytes : *packets) {
        CHECK(guest.receive(1234, bytes, MatchSession::Time::max()) == Admission::WrongPeer);
        const auto admission = guest.receive(99, bytes, MatchSession::Time::max());
        CHECK((admission == Admission::Accepted || admission == Admission::Stale));
    }
    // Duplicate/stale snapshots cannot reset the clock or consume the time jump.
    guest.update(1.0 / 60, start + std::chrono::milliseconds{200});
    CHECK(guest.inputTick() == 15);
    guest.update(1.0 / 60, MatchSession::Time::max());
    CHECK(guest.inputTick() == MatchSession::kMaxDeadlineAdvance + guest.inputLead());

    SessionInputs::Frame edge;
    edge[0].usePotion = true;
    for (usize step = 0; step < InputTimeline::kMaxAhead && guest.inputReady(); ++step) {
        REQUIRE(guest.sample(MatchInputs::commands(guest, {}), MatchSession::Time::max()));
    }
    REQUIRE_FALSE(guest.inputReady());
    REQUIRE(guest.sample(MatchInputs::commands(guest, edge), MatchSession::Time::max()));
    CHECK(guest.sampledInputs().empty());
    const auto oldEpoch = guest.context().epoch;
    REQUIRE(host.requestPause());
    room->pump(MatchSession::Time::max());
    room->ready(MatchTransition::Resume);
    REQUIRE(guest.context().epoch != oldEpoch);
    CHECK(guest.inputTick() == guest.context().inputLead);
    REQUIRE(guest.inputReady());
    for (const auto& bytes : *packets) {
        CHECK(guest.receive(99, bytes, MatchSession::Time::max()) == Admission::Stale);
    }
    REQUIRE(host.advance());
    REQUIRE(host.publish(snapshot(host)));
    room->pump(start);
    guest.update(1.0 / 60, start + std::chrono::milliseconds{200});
    CHECK(guest.inputTick() == 15);
    REQUIRE(guest.sample(MatchInputs::commands(guest, {}), start + std::chrono::milliseconds{200}));
    REQUIRE(guest.sampledInputs().size() == 1);
    CHECK_FALSE(guest.sampledInputs().front().pressed(CommandPress::UsePotion));
    CHECK(guest.sampledInputs().front().epoch == guest.context().epoch);
    CHECK(guest.sampledInputs().front().grant == guest.context().grants[1]);
}

TEST_CASE("congested retries keep their original input identity when the clock advances",
          "[netplay][match-session][input-pacing][asymmetric-input-clock]") {
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 0, 0});
    room->ready();
    auto& host = room->sessions[1];
    auto& guest = room->sessions[2];
    auto& wire = room->wires[2];
    const MatchSession::Time start{std::chrono::seconds{1}};
    REQUIRE(host.advance());
    REQUIRE(host.publish(snapshot(host)));
    room->pump(start);
    SessionInputs::Frame attack;
    attack[0].attack = attack[0].attackPressed = true;
    REQUIRE(guest.sample(MatchInputs::commands(guest, attack), start));
    const auto original = guest.sampledInputs().front();
    const auto originalPacket = InputPacket::encode(std::span{&original, 1});
    REQUIRE(originalPacket);
    wire.result = PacketTransport::SendResult::Congested;
    guest.flush(wire);
    CHECK(wire.sent.empty());
    for (usize tick = 0; tick < 12; ++tick) {
        REQUIRE(host.advance());
    }
    const auto resumed = start + std::chrono::milliseconds{200};
    guest.update(1.0 / 60, resumed);
    CHECK(guest.inputTick() == 15);
    CHECK(guest.sampledInputs().front().tick == original.tick);
    wire.result = PacketTransport::SendResult::Sent;
    guest.flush(wire);
    REQUIRE(wire.sent.size() == 1);
    CHECK(wire.sent.front().bytes == *originalPacket);
    REQUIRE(host.receive(1, wire.sent.front().bytes, resumed) == Admission::Accepted);
    wire.sent.clear();
    guest.flush(wire);
    CHECK(wire.sent.empty());
    REQUIRE(host.commandTrace().size() == 1);
    CHECK(host.commandTrace().front().command.tick == original.tick);
    CHECK(host.commandTrace().front().admission == InputTimeline::Admission::TooLate);
    CHECK_FALSE(host.commandTrace().front().executedTick);

    // A new neutral sample must not relabel/reissue the old attack as a new tap.
    REQUIRE(guest.sample(MatchInputs::commands(guest, {}), resumed));
    REQUIRE(guest.sampledInputs().size() == 1);
    const auto current = guest.sampledInputs().front();
    CHECK(current.tick == 15);
    CHECK_FALSE(current.held(CommandHeld::Attack));
    CHECK_FALSE(current.pressed(CommandPress::Attack));
    room->pump(resumed);
    while (host.tick() <= current.tick) {
        const auto frame = host.advance();
        REQUIRE(frame);
        CHECK_FALSE((*frame)[1].held(CommandHeld::Attack));
        CHECK_FALSE((*frame)[1].pressed(CommandPress::Attack));
    }
    CHECK(host.commandTrace().size() == 1);
}

TEST_CASE("exact admission feedback ages the host clock instead of inflating safety lead",
          "[netplay][latency][match-session][asymmetric-input-clock]") {
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 0, 0});
    room->ready();
    auto& host = room->sessions[1];
    auto& guest = room->sessions[2];
    const MatchSession::Time start{std::chrono::seconds{1}};
    REQUIRE(guest.sample(MatchInputs::commands(guest, {}), start));
    for (usize tick = 0; tick < 3; ++tick) {
        REQUIRE(host.advance());
    }
    room->pump(start);
    CHECK(host.inputDiagnostics(1).arrivalMargin == 0);
    REQUIRE(host.advance());
    auto state = std::make_unique<CombatSnapshot>(snapshot(host));
    const auto& feedback = host.inputDiagnostics(1);
    state->players[1]->movement.receivedInput = feedback.received.value_or(0);
    state->players[1]->movement.arrivalMargin = feedback.arrivalMargin;
    REQUIRE(host.publish(*state));
    room->pump(start + std::chrono::milliseconds{200});
    CHECK(guest.inputLead() == guest.context().inputLead);
    CHECK(guest.inputTick() == 3 + 12 + guest.inputLead());
}

TEST_CASE("deadline extrapolation cannot wrap an exhausted authoritative tick counter",
          "[netplay][match-session][asymmetric-input-clock]") {
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 0, 0});
    room->ready();
    auto& host = room->sessions[1];
    auto& guest = room->sessions[2];
    REQUIRE(host.advance());
    auto state = std::make_unique<CombatSnapshot>(snapshot(host));
    state->motion.tick = std::numeric_limits<u64>::max() - 5;
    const auto packets = CombatReplica::packets(*state);
    REQUIRE(packets);
    const MatchSession::Time start{std::chrono::seconds{1}};
    for (const auto& packet : *packets) {
        REQUIRE(guest.receive(99, packet, start) == Admission::Accepted);
    }
    CHECK(guest.inputTick() == std::numeric_limits<u64>::max() - 2);
    guest.update(1.0 / 60, start + std::chrono::milliseconds{200});
    CHECK(guest.inputTick() == std::numeric_limits<u64>::max());
    CHECK_FALSE(guest.inputReady());
    CHECK_FALSE(guest.sample(MatchInputs::commands(guest, {}), start));
    guest.update(1.0 / 60, MatchSession::Time::min());
    CHECK(guest.inputTick() == std::numeric_limits<u64>::max());
}

TEST_CASE("admission calibration starts at successful transmission not a congested sample",
          "[netplay][match-session][admission-clock]") {
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 0, 0});
    room->ready();
    auto& host = room->sessions[1];
    auto& guest = room->sessions[2];
    const MatchSession::Time start{std::chrono::seconds{1}};
    REQUIRE(guest.sample(MatchInputs::commands(guest, {}), start));
    room->wires[2].result = PacketTransport::SendResult::Congested;
    guest.flush(room->wires[2], start + std::chrono::milliseconds{500});
    CHECK(room->wires[2].sent.empty());
    for (usize tick = 0; tick < 3; ++tick) {
        REQUIRE(host.advance());
    }
    room->wires[2].result = PacketTransport::SendResult::Sent;
    room->pump(start + std::chrono::seconds{1});
    REQUIRE(host.inputDiagnostics(1).received == 3);
    REQUIRE(host.advance());
    auto state = std::make_unique<CombatSnapshot>(snapshot(host));
    state->players[1]->movement.receivedInput = 3;
    state->players[1]->movement.arrivalMargin = host.inputDiagnostics(1).arrivalMargin;
    REQUIRE(host.publish(*state));
    room->pump(start + std::chrono::milliseconds{1200});
    CHECK(guest.inputLead() == 3);
    CHECK(guest.inputTick() == 18); // admission 3 + 200 ms + safety 3
    const u64 frozen = state->motion.tick + MatchSession::kMaxDeadlineAdvance + 3;
    guest.update(0, MatchSession::Time::max());
    CHECK(guest.inputTick() == frozen);
    guest.update(0, MatchSession::Time::min());
    CHECK(guest.inputTick() == frozen);
}

TEST_CASE("redundant input transmission cannot refresh the first send used for clock calibration",
          "[netplay][match-session][admission-clock]") {
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 0, 0});
    room->ready();
    auto& host = room->sessions[1];
    auto& guest = room->sessions[2];
    const MatchSession::Time start{std::chrono::seconds{1}};
    REQUIRE(guest.sample(MatchInputs::commands(guest, {}), start));
    for (usize tick = 0; tick < 3; ++tick) {
        REQUIRE(host.advance());
    }
    room->pump(start + std::chrono::milliseconds{100});
    REQUIRE(host.advance());
    auto state = std::make_unique<CombatSnapshot>(snapshot(host));
    state->players[1]->movement.receivedInput = 3;
    state->players[1]->movement.arrivalMargin = host.inputDiagnostics(1).arrivalMargin;
    REQUIRE(guest.sample(MatchInputs::commands(guest, {}), start + std::chrono::milliseconds{300}));
    room->pump(start + std::chrono::milliseconds{300});
    REQUIRE(host.publish(*state));
    room->pump(start + std::chrono::milliseconds{400});
    CHECK(guest.inputLead() == 3);
    CHECK(guest.inputTick() == 24); // original successful send was 300 ms ago
}

TEST_CASE("unknown unsent foreign and saturated feedback cannot calibrate the guest clock",
          "[netplay][match-session][admission-clock]") {
    const s32 invalid = GENERATE(0, 1, 2, 3);
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 0, 0});
    room->wires[2].stats = PacketTransport::Statistics{600};
    room->ready();
    auto& host = room->sessions[1];
    auto& guest = room->sessions[2];
    REQUIRE(guest.inputLead() == 30);
    const MatchSession::Time start{std::chrono::seconds{1}};
    REQUIRE(guest.sample(MatchInputs::commands(guest, {}), start));
    if (invalid != 1) {
        room->pump(start);
    }
    REQUIRE(host.advance());
    auto state = std::make_unique<CombatSnapshot>(snapshot(host));
    auto& feedback = state->players[1]->movement;
    feedback.receivedInput = invalid == 0 ? 29 : 30;
    feedback.arrivalMargin = invalid == 2 ? -static_cast<s32>(InputTimeline::kMaxAhead) : 30;
    if (invalid == 3) {
        ++state->motion.players[1]->grant;
    }
    const auto packets = CombatReplica::packets(*state);
    REQUIRE(packets);
    for (const auto& packet : *packets) {
        REQUIRE(guest.receive(99, packet, start + std::chrono::milliseconds{200}) ==
                Admission::Accepted);
    }
    CHECK(guest.inputLead() == 30);
    if (invalid == 3) {
        // Wrong-seat feedback must not consume the watermark: a subsequent
        // authenticated checkpoint can report the same received command.
        ++state->motion.tick;
        --state->motion.players[1]->grant;
        const auto corrected = CombatReplica::packets(*state);
        REQUIRE(corrected);
        for (const auto& packet : *corrected) {
            REQUIRE(guest.receive(99, packet, start + std::chrono::milliseconds{300}) ==
                    Admission::Accepted);
        }
        CHECK(guest.inputLead() == guest.context().inputLead);
    }
}

TEST_CASE(
    "calibrated two-seat safety lead uses the worst fresh real admission in either seat order",
    "[netplay][match-session][admission-clock]") {
    const usize lateSeat = GENERATE(usize{1}, usize{2});
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 2, 0});
    room->wires[2].stats = PacketTransport::Statistics{600};
    room->ready();
    auto& host = room->sessions[1];
    auto& guest = room->sessions[2];
    const MatchSession::Time start{std::chrono::seconds{1}};
    REQUIRE(guest.inputLead() == 30);
    REQUIRE(guest.sample(MatchInputs::commands(guest, {}), start));
    for (usize tick = 0; tick < 3; ++tick) {
        REQUIRE(host.advance());
    }
    room->pump(start);
    REQUIRE(host.advance());
    auto state = std::make_unique<CombatSnapshot>(snapshot(host));
    for (usize seat : {usize{1}, usize{2}}) {
        state->players[seat]->movement.receivedInput = 30;
        state->players[seat]->movement.arrivalMargin = host.inputDiagnostics(seat).arrivalMargin;
    }
    REQUIRE(host.publish(*state));
    room->pump(start + std::chrono::milliseconds{100});
    REQUIRE(guest.inputLead() == 3);

    const auto sentAt = start + std::chrono::milliseconds{500};
    guest.update(0, sentAt);
    const auto nextTick = guest.inputTick();
    REQUIRE(nextTick > 31);
    const auto inputs = MatchInputs::commands(guest, {});
    REQUIRE(guest.sample(inputs, sentAt));
    guest.flush(room->wires[2], sentAt);
    room->wires[2].sent.clear();
    while (host.tick() < nextTick) {
        REQUIRE(host.advance());
    }
    const auto admit = [&](usize seat) {
        const auto found = std::ranges::find(inputs, seat, &InputCommand::seat);
        REQUIRE(found != inputs.end());
        const auto bytes = InputPacket::encode(std::span{&*found, 1});
        REQUIRE(bytes);
        REQUIRE(host.receive(1, *bytes, sentAt) == Admission::Accepted);
    };
    admit(lateSeat == 1 ? 2 : 1);
    for (usize tick = 0; tick < 6; ++tick) {
        REQUIRE(host.advance());
    }
    admit(lateSeat);
    REQUIRE(host.inputDiagnostics(lateSeat).arrivalMargin == -6);
    REQUIRE(host.advance());
    state = std::make_unique<CombatSnapshot>(snapshot(host));
    for (usize seat : {usize{1}, usize{2}}) {
        state->players[seat]->movement.receivedInput = nextTick;
        state->players[seat]->movement.arrivalMargin = host.inputDiagnostics(seat).arrivalMargin;
    }
    REQUIRE(host.publish(*state));
    room->pump(sentAt + std::chrono::milliseconds{100});
    CHECK(guest.inputLead() == 11);
}

TEST_CASE("transport statistics tolerate extreme and backwards injected clocks",
          "[netplay][match-session][admission-clock]") {
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 0, 0});
    room->ready();
    auto& guest = room->sessions[2];
    auto& wire = room->wires[2];
    const auto before = wire.statisticsQueries;
    guest.flush(wire, MatchSession::Time::min());
    CHECK(wire.statisticsQueries == before);
    guest.flush(wire, MatchSession::Time::max());
    CHECK(wire.statisticsQueries == before + 1);
    guest.flush(wire, MatchSession::Time::min());
    CHECK(wire.statisticsQueries == before + 1);
    guest.flush(wire, MatchSession::Time::max());
    CHECK(wire.statisticsQueries == before + 1);
}

TEST_CASE("an older admission anchor follows renewed authority without widening the stalled bound",
          "[netplay][match-session][admission-clock]") {
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 0, 0});
    room->ready();
    auto& host = room->sessions[1];
    auto& guest = room->sessions[2];
    const MatchSession::Time start{std::chrono::seconds{1}};
    REQUIRE(guest.sample(MatchInputs::commands(guest, {}), start));
    for (usize tick = 0; tick < 3; ++tick) {
        REQUIRE(host.advance());
    }
    room->pump(start);
    REQUIRE(host.advance());
    auto state = std::make_unique<CombatSnapshot>(snapshot(host));
    state->players[1]->movement.receivedInput = 3;
    state->players[1]->movement.arrivalMargin = 0;
    const auto receive = [&](MatchSession::Time now) {
        const auto packets = CombatReplica::packets(*state);
        REQUIRE(packets);
        for (const auto& packet : *packets) {
            REQUIRE(guest.receive(99, packet, now) == Admission::Accepted);
        }
    };
    receive(start + std::chrono::milliseconds{200});
    REQUIRE(guest.inputTick() == 18);
    // The original command is still the latest feedback. Newer host snapshots
    // renew the bounded clock without pretending that command was sent again.
    state->motion.tick = 45;
    receive(start + std::chrono::seconds{1});
    CHECK(guest.inputTick() == 66); // admission3 + elapsed60 + safety3
    guest.update(0, start + std::chrono::milliseconds{1100});
    CHECK(guest.inputTick() == 72);
    guest.update(0, MatchSession::Time::max());
    CHECK(guest.inputTick() == 78); // newest authority45 + cap30 + safety3
}

TEST_CASE("committed action receipts preserve native clocks before replay and render delays",
          "[netplay][match-session][action-receipt]") {
    auto room = std::make_unique<Room>();
    room->ready();
    auto& host = room->sessions[1];
    auto& guest = room->sessions[2];
    for (usize tick = 0; tick < 5; ++tick) {
        REQUIRE(host.advance());
    }
    auto state = std::make_unique<CombatSnapshot>(snapshot(host));
    auto& player = *state->players[1];
    player.animation = {static_cast<u32>(PlayerAnimator::Action::Ready), 0, 3, 0, 1};
    player.rollback.emplace();
    player.rollback->action = PlayerAnimator::Action::Quick1;
    player.rollback->animation.generation = 7;
    player.rollback->animation.playing = true;
    player.rollback->released = true;
    player.rollbackInput = InputCommand{state->motion.epoch, 3, host.context().grants[1], 1};
    player.movement.enabled = player.movement.current = true;
    player.movement.receivedInput = 6;
    player.movement.lateInputs = 2;
    REQUIRE(state->valid());
    const auto packets = CombatReplica::packets(*state, SnapshotBlock::Compression::None);
    REQUIRE(packets);
    REQUIRE(packets->size() > 1);
    const MatchSession::Time pressed{std::chrono::seconds{1}};
    const auto received = pressed + std::chrono::milliseconds{900};
    for (usize index = 0; index + 1 < packets->size(); ++index) {
        REQUIRE(guest.receive(99, (*packets)[index], received) == Admission::Accepted);
        CHECK(guest.actionReceipts().empty());
    }
    REQUIRE(guest.receive(99, packets->back(), received) == Admission::Accepted);
    REQUIRE(guest.actionReceipts().size() == 4);
    const auto receipt = guest.actionReceipts()[1];
    CHECK(receipt.epoch == state->motion.epoch);
    CHECK(receipt.tick == state->motion.tick);
    CHECK(receipt.seat == 1);
    CHECK(receipt.grant == state->motion.players[1]->grant);
    CHECK(receipt.continuity == state->motion.players[1]->continuity);
    CHECK(receipt.present);
    CHECK(receipt.life == player.life);
    CHECK_FALSE(receipt.portalPhase);
    CHECK(receipt.rollback);
    CHECK(receipt.action == static_cast<u32>(player.rollback->action));
    CHECK(receipt.generation == player.rollback->animation.generation);
    CHECK(receipt.action != player.animation.action);
    CHECK(receipt.generation != player.animation.generation);
    CHECK(receipt.actualInput == player.rollbackInput->tick);
    CHECK(receipt.receivedInput == player.movement.receivedInput);
    CHECK(receipt.lateInputs == player.movement.lateInputs);
    CHECK(receipt.reaction == player.reaction);
    CHECK(receipt.enabled);
    CHECK(receipt.current);
    CHECK_FALSE(receipt.ranged);
    CHECK_FALSE(receipt.weapon);
    CHECK_FALSE(receipt.melee);
    CHECK_FALSE(receipt.turbo);
    CHECK(receipt.released);
    CHECK(receipt.receivedAt == received);

    // A delayed consumer can still distinguish a timely authority receipt from
    // its first rendered frame after the unchanged one-second response deadline.
    const auto rendered = pressed + std::chrono::milliseconds{1150};
    guest.update(1.0 / 60, rendered);
    guest.latency().presented(*state, rendered);
    CHECK(guest.actionReceipts()[1] == receipt);
    CHECK(receipt.receivedAt - pressed < std::chrono::seconds{1});
    CHECK(rendered - pressed > std::chrono::seconds{1});
    CHECK(guest.droppedActionReceipts() == 0);

    // More than one committed state in a poll cannot erase an intervening action.
    REQUIRE(host.advance());
    state = std::make_unique<CombatSnapshot>(snapshot(host));
    const auto neutral = CombatReplica::packets(*state);
    REQUIRE(neutral);
    for (const auto& packet : *neutral) {
        REQUIRE(guest.receive(99, packet, rendered) == Admission::Accepted);
    }
    REQUIRE(guest.actionReceipts().size() == 8);
    CHECK(guest.actionReceipts()[1] == receipt);
    CHECK(guest.actionReceipts()[5].receivedAt == rendered);
    CHECK_FALSE(guest.actionReceipts()[5].rollback);
    CHECK(guest.actionReceipts()[5].action == state->players[1]->animation.action);
    CHECK(guest.actionReceipts()[5].generation == state->players[1]->animation.generation);
}

TEST_CASE("action receipt history excludes unauthenticated incomplete and stale checkpoints",
          "[netplay][match-session][action-receipt]") {
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 0, 0});
    room->ready();
    auto& host = room->sessions[1];
    auto& guest = room->sessions[2];
    REQUIRE(host.advance());
    auto state = std::make_unique<CombatSnapshot>(snapshot(host));
    const auto packets = CombatReplica::packets(*state);
    REQUIRE(packets);
    const MatchSession::Time now{std::chrono::seconds{1}};
    for (const auto& packet : *packets) {
        CHECK(guest.receive(98, packet, now) == Admission::WrongPeer);
    }
    CHECK(guest.actionReceipts().empty());
    for (const auto& packet : *packets) {
        REQUIRE(guest.receive(99, packet, now) == Admission::Accepted);
    }
    const auto expected = guest.actionReceipts();
    REQUIRE(expected.size() == 2);
    for (const auto& packet : *packets) {
        const auto result = guest.receive(99, packet, now + std::chrono::seconds{1});
        CHECK((result == Admission::Accepted || result == Admission::Stale));
    }
    auto corrupt = packets->front();
    corrupt[0] ^= 0xff;
    CHECK(guest.receive(99, corrupt, now) == Admission::Invalid);
    CHECK(guest.actionReceipts() == expected);

    ++state->motion.epoch;
    const auto foreign = CombatReplica::packets(*state);
    REQUIRE(foreign);
    for (const auto& packet : *foreign) {
        CHECK(guest.receive(99, packet, now) == Admission::Stale);
    }
    CHECK(guest.actionReceipts() == expected);
    CHECK(guest.droppedActionReceipts() == 0);
}

TEST_CASE("action receipts retain interruptions and bounded history across session epochs",
          "[netplay][match-session][action-receipt]") {
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 0, 0});
    room->ready();
    auto& host = room->sessions[1];
    auto& guest = room->sessions[2];
    const MatchSession::Time start{std::chrono::seconds{1}};
    for (usize index = 0; index <= MatchSession::kActionReceipts / 2; ++index) {
        REQUIRE(host.advance());
        auto state = std::make_unique<CombatSnapshot>(snapshot(host));
        if (index == 0) {
            state->players[1]->portalPhase = 0.0f;
        } else if (index == 1) {
            state->players[1]->life = ReplicaPlayerLife::Dying;
        } else if (index == 2) {
            state->players[1].reset();
            state->motion.players[1].reset();
        }
        const auto packets = CombatReplica::packets(*state);
        REQUIRE(packets);
        const auto now = start + std::chrono::milliseconds{static_cast<s64>(index * 20)};
        for (const auto& packet : *packets) {
            REQUIRE(guest.receive(99, packet, now) == Admission::Accepted);
        }
        const auto& receipt = guest.actionReceipts().back();
        CHECK(receipt.seat == 1);
        CHECK(receipt.tick == index);
        CHECK(receipt.grant == host.context().grants[1]);
        CHECK(receipt.present == (index != 2));
        CHECK(receipt.portalPhase == (index == 0));
        if (index == 1) {
            CHECK(receipt.life == ReplicaPlayerLife::Dying);
        }
    }
    CHECK(guest.actionReceipts().size() == MatchSession::kActionReceipts);
    CHECK(guest.droppedActionReceipts() == 2);
    CHECK(guest.actionReceipts().front().tick == 1);
    REQUIRE(guest.requestPause());
    room->pump();
    REQUIRE(host.phase() == Phase::Paused);
    REQUIRE(host.prepare(host.context().scene, MatchTransition::Resume));
    room->pump();
    CHECK(guest.actionReceipts().empty());
    CHECK(guest.droppedActionReceipts() == 0);
    guest.clear();
    CHECK(guest.actionReceipts().empty());
    CHECK(guest.droppedActionReceipts() == 0);
}

} // namespace
