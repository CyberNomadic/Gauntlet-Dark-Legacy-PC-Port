#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "game/netplay/MatchSession.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Phase = MatchSession::Phase;
using Admission = MatchSession::Admission;
constexpr PacketTransport::Connection kHost = 99;

struct Wire final : PacketTransport {
    struct Packet {
        Connection connection = 0;
        std::vector<u8> bytes;
        Delivery delivery = Delivery::Unreliable;
    };
    SendResult result = SendResult::Sent;
    usize budget = std::numeric_limits<usize>::max();
    std::vector<Packet> sent;

    SendResult send(Connection connection, std::span<const u8> bytes, Delivery delivery) override {
        if (result != SendResult::Sent) {
            return result;
        }
        if (budget == 0) {
            return SendResult::Congested;
        }
        --budget;
        sent.push_back({connection, {bytes.begin(), bytes.end()}, delivery});
        return SendResult::Sent;
    }
    std::vector<Event> poll() override { return {}; }
    std::optional<Statistics> statistics(Connection) const override { return std::nullopt; }
    void close(Connection) override {}
};

std::vector<u8> inputBytes(std::span<const InputCommand> frame) {
    const auto encoded = InputPacket::encode(frame);
    REQUIRE(encoded);
    return *encoded;
}

std::vector<u8> controlBytes(MatchControlKind kind, const MatchContext& context) {
    const auto encoded = MatchControlPacket::encode({kind, MatchStop::None, context});
    REQUIRE(encoded);
    return *encoded;
}

struct Room {
    MatchOwners owners;
    std::array<MatchSession, 5> sessions;
    std::array<Wire, 5> wires;

    explicit Room(MatchOwners roster = {1, 2, 0, 0}, u8 delay = 0) : owners(roster) {
        std::vector<MatchLink> links;
        for (u8 peer = 2; peer <= 4; ++peer) {
            if (present(peer)) {
                links.push_back({peer, PacketTransport::Connection{100} + peer});
                const std::array host{MatchLink{1, kHost}};
                REQUIRE(sessions[peer].open(peer, owners, host, delay));
            }
        }
        REQUIRE(sessions[1].open(1, owners, links, delay));
    }

    bool present(u8 peer) const { return std::ranges::find(owners, peer) != owners.end(); }

    void deliver(u8 source) {
        auto packets = std::move(wires[source].sent);
        wires[source].sent.clear();
        for (const auto& packet : packets) {
            const auto target = source == 1 ? static_cast<u8>(packet.connection - 100) : u8{1};
            const auto sender = source == 1 ? kHost : 100 + source;
            CAPTURE(source, target);
            REQUIRE(packet.delivery == PacketTransport::Delivery::Reliable);
            const auto admitted = sessions[target].receive(sender, packet.bytes);
            REQUIRE((admitted == Admission::Accepted || admitted == Admission::Stale));
        }
    }

    void pump() {
        for (u8 pass = 0; pass < 3; ++pass) {
            for (u8 peer = 1; peer <= 4; ++peer) {
                if (present(peer)) {
                    sessions[peer].flush(wires[peer]);
                    deliver(peer);
                }
            }
        }
    }

    void prepare() {
        REQUIRE(sessions[1].prepare(7));
        pump();
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (present(peer)) {
                REQUIRE(sessions[peer].phase() == Phase::Loading);
                CHECK_FALSE(sessions[peer].advance());
                CHECK_FALSE(sessions[peer].inputReady());
            }
        }
    }

    void commit() {
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (present(peer)) {
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

    void ready() {
        prepare();
        commit();
    }

    std::vector<InputCommand> commands(u8 peer, u64 tick, bool actions = true) const {
        std::vector<InputCommand> frame;
        for (u8 seat = 0; seat < owners.size(); ++seat) {
            if (owners[seat] != peer) {
                continue;
            }
            InputCommand command;
            command.epoch = sessions[1].context().epoch;
            command.tick = tick;
            command.grant = sessions[1].context().grants[seat];
            command.seat = seat;
            if (actions) {
                command.direction = {1, 0};
                command.magnitude = 0.5f;
                command.aimDirection = Vec3{0, 0, 1};
                command.heldButtons = static_cast<u32>(CommandHeld::ThrowPotion);
                if (tick % 3 == 0) {
                    command.pressedButtons = static_cast<u32>(CommandPress::UsePotion);
                }
            }
            frame.push_back(command);
        }
        return frame;
    }

    void advanceAll(u64 tick, bool actions) {
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (!present(peer)) {
                continue;
            }
            CAPTURE(peer, tick);
            auto& session = sessions[peer];
            const auto frame = session.advance();
            REQUIRE(frame);
            for (u8 source = 1; source <= 4; ++source) {
                if (!present(source)) {
                    continue;
                }
                for (const auto& expected : commands(source, tick, actions)) {
                    const auto& actual = (*frame)[expected.seat];
                    CHECK(inputBytes(std::span{&actual, 1}) == inputBytes(std::span{&expected, 1}));
                    REQUIRE(session.resolvedInput(expected.seat));
                    CHECK(inputBytes(std::span{&*session.resolvedInput(expected.seat), 1}) ==
                          inputBytes(std::span{&expected, 1}));
                }
            }
            CHECK(session.tick() == tick + 1);
            REQUIRE(session.completedNativeTick(tick));
        }
    }

    MatchParty party() const {
        MatchParty result;
        for (usize seat = 0; seat < owners.size(); ++seat) {
            if (owners[seat] != 0) {
                CharacterProfile profile;
                profile.name = "NATIVE";
                profile.character = static_cast<s32>(seat);
                profile.gold = static_cast<s32>(seat) * 100;
                result[seat] = profile;
            }
        }
        return result;
    }
};
} // namespace

TEST_CASE("match control authenticates the fixed native startup delay", "[match-lockstep]") {
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 0, 0}, 6);
    REQUIRE(room->sessions[1].prepare(7));
    const auto context = room->sessions[1].context();
    REQUIRE(context.inputLead == 6);
    const auto bytes = controlBytes(MatchControlKind::Prepare, context);
    REQUIRE(bytes.size() == 64);
    CHECK(bytes[4] == 6);
    REQUIRE(MatchControlPacket::decode(bytes));
    CHECK(MatchControlPacket::decode(bytes)->context == context);
    for (usize length = 0; length < bytes.size(); ++length) {
        CHECK_FALSE(MatchControlPacket::decode(std::span{bytes}.first(length)));
    }
    auto invalid = bytes;
    invalid.push_back(0);
    CHECK_FALSE(MatchControlPacket::decode(invalid));
    for (const usize offset : {4U, 48U, 49U, 50U, 51U, 52U, 56U, 60U, 63U}) {
        invalid = bytes;
        invalid[offset] = 255;
        CHECK_FALSE(MatchControlPacket::decode(invalid));
    }
    invalid = bytes;
    invalid[4] = 5;
    CHECK_FALSE(MatchControlPacket::decode(invalid));
    invalid = bytes;
    invalid[22] = 7;
    CHECK_FALSE(MatchControlPacket::decode(invalid));
}

TEST_CASE("native match delay must match the authenticated host prepare", "[match-lockstep]") {
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 0, 0}, 2);
    REQUIRE(room->sessions[1].prepare(7));
    const auto offered = room->sessions[1].context();
    const std::array hostLink{MatchLink{1, kHost}};
    MatchSession defaultDelay;
    REQUIRE(defaultDelay.open(2, room->owners, hostLink));
    CHECK(defaultDelay.receive(kHost, controlBytes(MatchControlKind::Prepare, offered)) ==
          Admission::Invalid);
    CHECK(defaultDelay.phase() == Phase::Lobby);
    MatchSession wrongDelay;
    REQUIRE(wrongDelay.open(2, room->owners, hostLink, 3));
    CHECK(wrongDelay.receive(kHost, controlBytes(MatchControlKind::Prepare, offered)) ==
          Admission::Invalid);
    CHECK(wrongDelay.phase() == Phase::Lobby);
    CHECK(room->sessions[2].receive(kHost, controlBytes(MatchControlKind::Prepare, offered)) ==
          Admission::Accepted);
    CHECK(room->sessions[2].context() == offered);
}

TEST_CASE("native match advances identical complete frames on every peer without snapshots",
          "[match-lockstep]") {
    const auto owners = GENERATE(MatchOwners{1, 2, 0, 0}, MatchOwners{1, 2, 3, 0},
                                 MatchOwners{1, 2, 3, 4}, MatchOwners{2, 1, 2, 3});
    const auto delay = GENERATE(u8{0}, u8{6});
    CAPTURE(owners, delay);
    auto room = std::make_unique<Room>(owners, delay);
    room->ready();
    for (u64 tick = 1; tick <= delay; ++tick) {
        room->advanceAll(tick, false);
    }
    for (u64 tick = 1 + delay; tick <= 20 + delay; ++tick) {
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (room->present(peer)) {
                auto& session = room->sessions[peer];
                CHECK(session.inputTick() == tick);
                REQUIRE(session.inputReady());
                const auto commands = room->commands(peer, tick);
                REQUIRE(session.sample(commands));
                CHECK(inputBytes(session.sampledInputs()) == inputBytes(commands));
                CHECK(session.inputTick() == tick + 1);
                CHECK(session.tick() == tick);
            }
        }
        room->pump();
        room->advanceAll(tick, true);
    }
}

TEST_CASE("native match holds current reliable inputs arriving before the local commit poll",
          "[match-lockstep]") {
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 3, 0});
    room->prepare();
    auto& host = room->sessions[1];
    auto& first = room->sessions[2];
    auto& late = room->sessions[3];
    REQUIRE(host.loaded());
    REQUIRE(first.loaded());
    REQUIRE(late.loaded());
    first.flush(room->wires[2]);
    room->deliver(2);
    CHECK(host.phase() == Phase::Loading);
    late.flush(room->wires[3]);
    room->deliver(3);
    REQUIRE(host.phase() == Phase::Running);
    host.flush(room->wires[1]);
    REQUIRE(room->wires[1].sent.size() == 2);
    std::vector<u8> lateCommit;
    for (const auto& packet : room->wires[1].sent) {
        REQUIRE(MatchControlPacket::decode(packet.bytes));
        CHECK(MatchControlPacket::decode(packet.bytes)->kind == MatchControlKind::Commit);
        if (packet.connection == 102) {
            REQUIRE(first.receive(kHost, packet.bytes) == Admission::Accepted);
        } else {
            lateCommit = packet.bytes;
        }
    }
    room->wires[1].sent.clear();
    REQUIRE_FALSE(lateCommit.empty());
    REQUIRE(first.sample(room->commands(2, 1)));
    first.flush(room->wires[2]);
    room->deliver(2);
    REQUIRE(host.sample(room->commands(1, 1)));
    host.flush(room->wires[1]);
    room->deliver(1); // late peer stores both inputs while still Loading
    REQUIRE(late.phase() == Phase::Loading);
    CHECK_FALSE(late.advance());
    CHECK_FALSE(late.inputReady());
    CHECK_FALSE(late.sample(room->commands(3, 1)));
    REQUIRE(late.receive(kHost, lateCommit) == Admission::Accepted);
    REQUIRE(late.sample(room->commands(3, 1)));
    room->pump();
    room->advanceAll(1, true);
}

TEST_CASE("native match sends commit before input and preserves both through congestion",
          "[match-lockstep]") {
    auto room = std::make_unique<Room>();
    room->prepare();
    REQUIRE(room->sessions[1].loaded());
    REQUIRE(room->sessions[2].loaded());
    room->sessions[2].flush(room->wires[2]);
    room->deliver(2);
    REQUIRE(room->sessions[1].phase() == Phase::Running);
    REQUIRE(room->sessions[1].sample(room->commands(1, 1)));
    room->wires[1].result = PacketTransport::SendResult::Congested;
    room->sessions[1].flush(room->wires[1]);
    CHECK(room->wires[1].sent.empty());
    CHECK(room->sessions[1].queuedControls() == 1);
    room->wires[1].result = PacketTransport::SendResult::Sent;
    room->wires[1].budget = 1;
    room->sessions[1].flush(room->wires[1]);
    REQUIRE(room->wires[1].sent.size() == 1);
    REQUIRE(MatchControlPacket::decode(room->wires[1].sent[0].bytes));
    CHECK(MatchControlPacket::decode(room->wires[1].sent[0].bytes)->kind ==
          MatchControlKind::Commit);
    room->deliver(1);
    REQUIRE(room->sessions[2].phase() == Phase::Running);
    CHECK_FALSE(room->sessions[2].advance());
    room->wires[1].budget = 1;
    room->sessions[1].flush(room->wires[1]);
    REQUIRE(room->wires[1].sent.size() == 1);
    CHECK(room->wires[1].sent[0].bytes == inputBytes(room->commands(1, 1)));
    room->deliver(1);
    REQUIRE(room->sessions[2].sample(room->commands(2, 1)));
    room->wires[1].budget = std::numeric_limits<usize>::max();
    room->pump();
    room->advanceAll(1, true);
}

TEST_CASE("native match never estimates missing input from wall clocks or prior held controls",
          "[match-lockstep]") {
    auto room = std::make_unique<Room>();
    room->ready();
    REQUIRE(room->sessions[1].sample(room->commands(1, 1)));
    REQUIRE(room->sessions[2].sample(room->commands(2, 1)));
    room->wires[2].result = PacketTransport::SendResult::Congested;
    room->pump();
    REQUIRE(room->sessions[2].advance());
    REQUIRE(room->sessions[2].completedNativeTick(1));
    for (u32 update = 0; update < 12; ++update) {
        room->sessions[1].update(60, MatchSession::Time::max());
        CHECK_FALSE(room->sessions[1].advance());
        CHECK(room->sessions[1].tick() == 1);
        CHECK(room->sessions[1].inputTick() == 2);
        CHECK(room->sessions[1].inputLead() == 0);
        CHECK_FALSE(room->sessions[1].inputReady());
        CHECK_FALSE(room->sessions[1].sample(room->commands(1, 2)));
    }
    room->wires[2].result = PacketTransport::SendResult::Sent;
    room->pump();
    REQUIRE(room->sessions[1].advance());
    CHECK(room->sessions[1].tick() == 2);
    REQUIRE(room->sessions[1].completedNativeTick(1));
    CHECK_FALSE(room->sessions[1].advance());
    CHECK_FALSE(room->sessions[2].advance());
}

TEST_CASE("native match refuses global pause resume and replicated gameplay streams",
          "[match-lockstep]") {
    auto room = std::make_unique<Room>();
    room->ready();
    for (u8 peer = 1; peer <= 2; ++peer) {
        CHECK_FALSE(room->sessions[peer].requestPause());
        CHECK(room->sessions[peer].phase() == Phase::Running);
    }
    CHECK_FALSE(room->sessions[1].prepare(7, MatchTransition::Resume));
    auto context = room->sessions[1].context();
    CHECK_FALSE(MatchControlPacket::encode({MatchControlKind::Pause, MatchStop::None, context}));
    CHECK_FALSE(
        MatchControlPacket::encode({MatchControlKind::RequestPause, MatchStop::None, context}));
    context.transition = MatchTransition::Resume;
    CHECK_FALSE(context.valid());
    CHECK_FALSE(MatchControlPacket::encode({MatchControlKind::Prepare, MatchStop::None, context}));
    // Retired replica/audio messages are not routed into native gameplay.
    for (const std::array<u8, 4> obsolete :
         {std::array<u8, 4>{'G', 'D', 'L', 'F'}, std::array<u8, 4>{'G', 'D', 'L', 'A'},
          std::array<u8, 4>{'G', 'D', 'L', 'C'}}) {
        CHECK(room->sessions[1].receive(102, obsolete) == Admission::Invalid);
        CHECK(room->sessions[2].receive(kHost, obsolete) == Admission::Invalid);
    }
    room->pump();
}

TEST_CASE("native match capacity conflict and disconnect end the epoch rather than lose input",
          "[match-lockstep]") {
    const u32 failure = GENERATE(0U, 1U, 2U, 3U);
    CAPTURE(failure);
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 3, 4});
    room->ready();
    auto& host = room->sessions[1];
    if (failure == 0) {
        const auto original = room->commands(2, 1, false);
        REQUIRE(host.receive(102, inputBytes(original)) == Admission::Accepted);
        auto changed = original;
        changed.front().direction.x = -0.0f;
        REQUIRE(inputBytes(changed) != inputBytes(original));
        CHECK(host.receive(102, inputBytes(changed)) == Admission::Invalid);
    } else if (failure == 1) {
        REQUIRE(host.sample(room->commands(1, 1)));
        for (u64 tick = 1; tick <= LockstepInputs::kWindow; ++tick) {
            REQUIRE(host.receive(102, inputBytes(room->commands(2, tick))) == Admission::Accepted);
        }
        constexpr u64 remaining = LockstepInputs::kQueuedPackets - LockstepInputs::kWindow - 1;
        for (u64 tick = 1; tick <= remaining; ++tick) {
            REQUIRE(host.receive(103, inputBytes(room->commands(3, tick))) == Admission::Accepted);
        }
        CHECK(host.receive(103, inputBytes(room->commands(3, remaining + 1))) ==
              Admission::Invalid);
    } else if (failure == 2) {
        host.disconnected(102);
    } else {
        REQUIRE(host.sample(room->commands(1, 1)));
        room->wires[1].result = PacketTransport::SendResult::Invalid;
        host.flush(room->wires[1]);
    }
    REQUIRE(host.phase() == Phase::Stopped);
    CHECK(host.stopReason() ==
          (failure == 2 ? MatchStop::Disconnected : MatchStop::TransportFailure));
    CHECK_FALSE(host.inputReady());
    CHECK_FALSE(host.advance());
    CHECK_FALSE(host.sample(room->commands(1, 1)));
}

TEST_CASE("default match opening uses fixed-delay native frames on every peer",
          "[match-lockstep]") {
    const MatchOwners owners{1, 2, 0, 0};
    const std::array hostLinks{MatchLink{2, 102}};
    const std::array guestLinks{MatchLink{1, kHost}};
    MatchSession host, guest;
    REQUIRE(host.open(1, owners, hostLinks));
    REQUIRE(guest.open(2, owners, guestLinks));
    REQUIRE(host.prepare(7));
    REQUIRE(guest.receive(kHost, controlBytes(MatchControlKind::Prepare, host.context())) ==
            Admission::Accepted);
    REQUIRE(host.loaded());
    REQUIRE(guest.loaded());
    REQUIRE(host.receive(102, controlBytes(MatchControlKind::Ready, guest.context())) ==
            Admission::Accepted);
    REQUIRE(guest.receive(kHost, controlBytes(MatchControlKind::Commit, host.context())) ==
            Admission::Accepted);
    CHECK(host.inputLead() == 3);
    CHECK(guest.inputLead() == 3);
    for (u64 tick = 1; tick <= 3; ++tick) {
        REQUIRE(host.advance());
        REQUIRE(guest.advance());
        REQUIRE(host.completedNativeTick(tick));
        REQUIRE(guest.completedNativeTick(tick));
    }
    CHECK_FALSE(host.advance());
    CHECK_FALSE(guest.advance());
    CHECK_FALSE(host.requestPause());
    CHECK_FALSE(guest.requestPause());
}

TEST_CASE("native match distinguishes dequeued input from a completed native tick",
          "[match-lockstep][native-travel]") {
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 0, 0}, 2);
    room->ready();
    auto& host = room->sessions[1];
    const auto party = room->party();
    const auto sourceEpoch = host.context().epoch;
    CHECK_FALSE(host.completedNativeTick(1));
    CHECK_FALSE(host.prepare(8, MatchTransition::Travel, &party));
    REQUIRE(host.advance());
    CHECK(host.tick() == 2);
    CHECK_FALSE(host.advance());
    CHECK_FALSE(host.inputReady());
    CHECK_FALSE(host.completedNativeTick(0));
    CHECK_FALSE(host.completedNativeTick(2));
    CHECK_FALSE(host.prepare(8, MatchTransition::Travel, &party));
    REQUIRE(host.completedNativeTick(1));
    CHECK_FALSE(host.completedNativeTick(1));
    REQUIRE(host.advance());
    CHECK_FALSE(host.advance());
    REQUIRE(host.completedNativeTick(2));
    CHECK(host.tick() == 3);
    REQUIRE(host.prepare(8, MatchTransition::Travel, &party));
    CHECK(host.context().sourceEpoch == sourceEpoch);
    CHECK(host.context().sourceTick == 2);
}

TEST_CASE("native travel drains exact source inputs before profiles shop and new epoch",
          "[match-lockstep][native-travel]") {
    const auto transition = GENERATE(MatchTransition::Travel, MatchTransition::SecretEnter,
                                     MatchTransition::SecretReturn, MatchTransition::Shop);
    // Prepare can arrive before the final input, during its native update, or
    // after that update already completed. None may silently discard the scene.
    const u32 progress = GENERATE(0U, 1U, 2U);
    CAPTURE(transition, progress);
    auto room = std::make_unique<Room>();
    room->ready();
    auto& host = room->sessions[1];
    auto& guest = room->sessions[2];
    const auto source = host.context();
    const auto local = room->commands(1, 1);
    const auto remote = room->commands(2, 1);
    REQUIRE(host.sample(local));
    REQUIRE(guest.sample(remote));
    guest.flush(room->wires[2]);
    room->deliver(2);
    REQUIRE(host.advance());
    REQUIRE(host.completedNativeTick(1));
    if (progress != 0) {
        host.flush(room->wires[1]);
        room->deliver(1);
        REQUIRE(guest.advance());
        if (progress == 2) {
            REQUIRE(guest.completedNativeTick(1));
        }
    }

    const auto profiles = room->party();
    ShopSetup shop;
    shop.source = 8;
    shop.destination = 9;
    REQUIRE(shop.valid());
    REQUIRE(host.prepare(8, transition, &profiles,
                         transition == MatchTransition::Shop ? &shop : nullptr,
                         transition == MatchTransition::SecretReturn));
    const auto next = host.context();
    CHECK(next.epoch == source.epoch + 1);
    CHECK(next.sourceEpoch == source.epoch);
    CHECK(next.sourceTick == 1);
    CHECK(next.grants == source.grants); // frozen ownership; epoch fences the input lifetime
    const auto wireContext =
        MatchControlPacket::decode(controlBytes(MatchControlKind::Prepare, next));
    REQUIRE(wireContext);
    CHECK(wireContext->context == next);
    if (transition == MatchTransition::Travel) {
        REQUIRE(host.requestMovieSkip(0));
    }
    host.flush(room->wires[1]);
    std::vector<Wire::Packet> terminalInputs;
    for (const auto& packet : room->wires[1].sent) {
        if (InputPacket::decode(packet.bytes)) {
            terminalInputs.push_back(packet);
        } else {
            REQUIRE(guest.receive(kHost, packet.bytes) == Admission::Accepted);
        }
    }
    room->wires[1].sent.clear();
    REQUIRE(guest.nativeDraining());
    REQUIRE(guest.nativeEndTick());
    CHECK(*guest.nativeEndTick() == 1);
    REQUIRE(guest.pendingNativeContext());
    CHECK(*guest.pendingNativeContext() == next);
    CHECK(guest.context() == source);
    CHECK(guest.phase() == Phase::Running);
    CHECK(guest.travelParty() == nullptr);
    CHECK(guest.shop().setup() == nullptr);
    CHECK_FALSE(guest.loaded());
    CHECK_FALSE(guest.inputReady());
    CHECK_FALSE(guest.sample(remote));
    CHECK_FALSE(guest.completedNativeTick(2));
    REQUIRE(guest.receive(kHost, controlBytes(MatchControlKind::Prepare, next)) ==
            Admission::Stale);
    // An authenticated duplicate old input remains harmless while the host is
    // loading. Its original relay must still be delivered to the draining guest.
    CHECK(host.receive(102, inputBytes(remote)) == Admission::Accepted);
    auto pastFence = local;
    pastFence.front().tick = 2;
    CHECK(guest.receive(kHost, inputBytes(pastFence)) == Admission::Stale);
    if (progress == 0) {
        CHECK_FALSE(guest.advance());
        REQUIRE_FALSE(terminalInputs.empty());
        for (const auto& packet : terminalInputs) {
            REQUIRE(guest.receive(kHost, packet.bytes) == Admission::Accepted);
        }
        const auto frame = guest.advance();
        REQUIRE(frame);
        CHECK(inputBytes(std::span{&(*frame)[0], 1}) == inputBytes(local));
        CHECK(inputBytes(std::span{&(*frame)[1], 1}) == inputBytes(remote));
    }
    CHECK_FALSE(guest.advance());
    REQUIRE(guest.completedNativeTick(1));
    CHECK_FALSE(guest.nativeDraining());
    CHECK_FALSE(guest.nativeEndTick());
    CHECK(guest.pendingNativeContext() == nullptr);
    CHECK(guest.context() == next);
    CHECK(guest.phase() == Phase::Loading);
    REQUIRE(guest.travelParty());
    for (usize seat = 0; seat < profiles.size(); ++seat) {
        if (profiles[seat]) {
            CHECK(CharacterProfilePacket::encode(*(*guest.travelParty())[seat]) ==
                  CharacterProfilePacket::encode(*profiles[seat]));
        }
    }
    if (transition == MatchTransition::Shop) {
        REQUIRE(guest.shop().setup());
        CHECK(guest.shop().setup()->source == shop.source);
        CHECK(guest.shop().setup()->destination == shop.destination);
    }
    if (transition == MatchTransition::Travel) {
        CHECK(guest.movieSkipVotes() == 1);
        REQUIRE(guest.requestMovieSkip(1));
        room->pump();
        CHECK(host.movieSkipReady());
        CHECK(guest.movieSkipReady());
    }
    room->commit();
    CHECK(guest.receive(kHost, inputBytes(local)) == Admission::Stale);
    CHECK(host.receive(102, inputBytes(remote)) == Admission::Stale);
    if (transition == MatchTransition::Shop) {
        CHECK(host.tick() == 0);
        CHECK(guest.tick() == 0);
        REQUIRE(host.shop().finish());
        REQUIRE(guest.shop().finish());
        room->pump();
        REQUIRE(host.shop().finished());
        REQUIRE(host.prepare(9, MatchTransition::Travel, &profiles));
        CHECK(host.context().sourceEpoch == next.epoch);
        CHECK(host.context().sourceTick == 0);
        room->pump();
        CHECK_FALSE(guest.nativeDraining());
        REQUIRE(guest.phase() == Phase::Loading);
        room->commit();
    }
    CHECK(host.tick() == 1);
    CHECK(guest.tick() == 1);
    REQUIRE(host.sample(room->commands(1, 1)));
    REQUIRE(guest.sample(room->commands(2, 1)));
    room->pump();
    room->advanceAll(1, true);
}

TEST_CASE("native transition fences refuse stale impossible and conflicting boundaries",
          "[match-lockstep][native-travel]") {
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 0, 0}, 3);
    room->ready();
    room->advanceAll(1, false);
    auto& guest = room->sessions[2];
    const auto old = guest.context();
    auto next = old;
    ++next.epoch;
    next.transition = MatchTransition::Travel;
    next.scene = 8;
    next.sourceEpoch = old.epoch;
    next.sourceTick = 2;
    for (auto& grant : next.grants) {
        grant += grant != 0;
    }
    REQUIRE(next.valid());
    auto changed = next;
    changed.sourceEpoch = 0;
    CHECK_FALSE(changed.valid());
    changed = next;
    ++changed.sourceEpoch;
    CHECK_FALSE(changed.valid());
    changed = next;
    changed.sourceTick = 0;
    CHECK(guest.receive(kHost, controlBytes(MatchControlKind::Prepare, changed)) ==
          Admission::Invalid);
    changed = next;
    changed.sourceTick = LockstepInputs::kWindow + 2;
    CHECK(guest.receive(kHost, controlBytes(MatchControlKind::Prepare, changed)) ==
          Admission::Invalid);
    CHECK(guest.context() == old);
    CHECK_FALSE(guest.nativeDraining());
    REQUIRE(guest.receive(kHost, controlBytes(MatchControlKind::Prepare, next)) ==
            Admission::Accepted);
    changed = next;
    ++changed.sourceTick;
    CHECK(guest.receive(kHost, controlBytes(MatchControlKind::Prepare, changed)) ==
          Admission::Invalid);
    auto profiles = room->party();
    auto checkpoint = PartyCheckpointPacket::encode({next.epoch, 0, *profiles[0]});
    REQUIRE(checkpoint);
    REQUIRE(guest.receive(kHost, *checkpoint) == Admission::Accepted);
    REQUIRE(guest.receive(kHost, *checkpoint) == Admission::Accepted);
    ++profiles[0]->gold;
    checkpoint = PartyCheckpointPacket::encode({next.epoch, 0, *profiles[0]});
    REQUIRE(checkpoint);
    CHECK(guest.receive(kHost, *checkpoint) == Admission::Invalid);
    CHECK(guest.context() == old);
    CHECK(guest.travelParty() == nullptr);
    guest.update(MatchSession::kLoadTimeout);
    CHECK(guest.phase() == Phase::Stopped);
    CHECK(guest.stopReason() == MatchStop::LoadTimeout);
    CHECK_FALSE(guest.nativeDraining());
    CHECK_FALSE(guest.advance());
}

namespace {
NativeAgreement::Digest observed(u8 value = 0) {
    NativeAgreement::Digest digest{};
    digest[0] = value;
    digest.back() = static_cast<u8>(value ^ 0xa5);
    return digest;
}
std::vector<u8> observationReport(u64 epoch, const NativeAgreement::Digest& digest) {
    NativeAgreement sender;
    const std::array links{NativeAgreement::Link{1, kHost}};
    REQUIRE(sender.open(2, epoch, {1, 2, 0, 0}, links, MatchSession::kNativeAgreementPeriod,
                        MatchSession::Time{}));
    REQUIRE(sender.submit(MatchSession::kNativeAgreementPeriod, digest, MatchSession::Time{}) ==
            NativeAgreement::Admission::Accepted);
    Wire wire;
    REQUIRE(sender.flush(wire, MatchSession::Time{}));
    REQUIRE(wire.sent.size() == 1);
    return wire.sent.front().bytes;
}
void pumpAgreement(Room& room, MatchSession::Time now = {}) {
    for (u8 pass = 0; pass < 3; ++pass) {
        for (u8 source = 1; source <= 4; ++source) {
            if (!room.present(source)) {
                continue;
            }
            room.sessions[source].flush(room.wires[source], now);
            auto packets = std::move(room.wires[source].sent);
            room.wires[source].sent.clear();
            for (const auto& packet : packets) {
                REQUIRE(packet.delivery == PacketTransport::Delivery::Reliable);
                const auto target = source == 1 ? static_cast<u8>(packet.connection - 100) : u8{1};
                const auto connection = source == 1 ? kHost : 100 + source;
                const bool stopped = room.sessions[target].phase() == Phase::Stopped;
                const auto result = room.sessions[target].receive(connection, packet.bytes, now);
                if (stopped) {
                    // Reliable traffic already in flight can precede the Stop
                    // echo (notably a peer's scheduled observation at timeout).
                    if (NativeAgreement::recognizes(packet.bytes)) {
                        REQUIRE(packet.bytes.size() == NativeAgreement::kPacketBytes);
                    } else {
                        const auto control = MatchControlPacket::decode(packet.bytes);
                        REQUIRE(control);
                        CHECK(control->kind == MatchControlKind::Stop);
                    }
                    CHECK(result == Admission::WrongPhase);
                } else {
                    REQUIRE((result == Admission::Accepted || result == Admission::Stale));
                }
            }
        }
    }
}
void executeThrough(Room& room, u64 last) {
    while (room.sessions[1].tick() <= last) {
        const auto tick = room.sessions[1].tick();
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (room.present(peer)) {
                REQUIRE(room.sessions[peer].sample(
                    room.commands(peer, room.sessions[peer].inputTick(), false)));
            }
        }
        pumpAgreement(room);
        room.advanceAll(tick, false);
    }
}
} // namespace

TEST_CASE("native match observations are lazy and do not bypass startup barriers",
          "[match-lockstep][match-native-agreement]") {
    SECTION("only Commit opens observation receipt and actual completion opens submission") {
        auto room = std::make_unique<Room>(MatchOwners{1, 2, 0, 0}, 6);
        for (u8 peer = 1; peer <= 2; ++peer) {
            CHECK_FALSE(room->sessions[peer].observeNative(60, observed(), MatchSession::Time{}));
        }
        room->prepare();
        const auto report = observationReport(room->sessions[1].context().epoch, observed());
        CHECK(room->sessions[1].receive(102, report, MatchSession::Time{}) == Admission::Stale);
        CHECK(room->sessions[1].nativeAgreement() == nullptr);
        room->commit();
        for (u8 peer = 1; peer <= 2; ++peer) {
            auto& session = room->sessions[peer];
            CHECK_FALSE(session.observeNative(0, observed(), MatchSession::Time{}));
            CHECK_FALSE(session.observeNative(60, observed(), MatchSession::Time{}));
            REQUIRE(session.advance());
            CHECK_FALSE(session.observeNative(60, observed(), MatchSession::Time{}));
            REQUIRE(session.completedNativeTick(1));
            CHECK_FALSE(session.observeNative(1, observed(), MatchSession::Time{}));
            CHECK(session.nativeAgreement() == nullptr);
            session.update(0, MatchSession::Time::max());
            CHECK(session.phase() == Phase::Running); // no idle observer timeout
        }
    }
}

TEST_CASE("native match observations agree without snapshots or changing input progression",
          "[match-lockstep][match-native-agreement]") {
    const auto roster =
        GENERATE((MatchOwners{1, 2, 0, 0}), (MatchOwners{1, 2, 3, 4}), (MatchOwners{2, 1, 2, 4}));
    auto room = std::make_unique<Room>(roster);
    room->ready();
    for (const u64 tick : {u64{60}, u64{120}}) {
        executeThrough(*room, tick);
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (room->present(peer)) {
                auto& session = room->sessions[peer];
                CHECK_FALSE(session.observeNative(tick - 1, observed(), MatchSession::Time{}));
                REQUIRE(session.observeNative(tick, observed(), MatchSession::Time{}));
                REQUIRE(session.observeNative(tick, observed(), MatchSession::Time{}));
                CHECK(session.tick() == tick + 1);
                CHECK(session.inputTick() == tick + 1);
            }
        }
        pumpAgreement(*room);
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (room->present(peer)) {
                REQUIRE(room->sessions[peer].nativeAgreement());
                CHECK(room->sessions[peer].nativeAgreement()->agreedTick() == tick);
                CHECK(room->sessions[peer].phase() == Phase::Running);
            }
        }
    }
}

TEST_CASE("native match observation mismatch stops every peer only after the full roster reports",
          "[match-lockstep][match-native-agreement]") {
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 3, 4});
    room->ready();
    executeThrough(*room, 60);
    REQUIRE(room->sessions[1].observeNative(60, observed(), MatchSession::Time{}));
    REQUIRE(room->sessions[2].observeNative(60, observed(1), MatchSession::Time{}));
    REQUIRE(room->sessions[3].observeNative(60, observed(), MatchSession::Time{}));
    pumpAgreement(*room);
    CHECK(room->sessions[1].phase() == Phase::Running);
    REQUIRE(room->sessions[1].nativeAgreement());
    CHECK(room->sessions[1].nativeAgreement()->agreedTick() == 0);
    REQUIRE(room->sessions[4].observeNative(60, observed(), MatchSession::Time{}));
    pumpAgreement(*room);
    for (u8 peer = 1; peer <= 4; ++peer) {
        CHECK(room->sessions[peer].phase() == Phase::Stopped);
        CHECK(room->sessions[peer].stopReason() == MatchStop::TransportFailure);
        CHECK(room->sessions[peer].nativeAgreement() == nullptr);
        CHECK_FALSE(room->sessions[peer].inputReady());
        CHECK_FALSE(room->sessions[peer].advance());
    }
}

TEST_CASE("native match guest observation failure retains shared stop through congestion",
          "[match-lockstep][match-native-agreement]") {
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 3, 4});
    room->ready();
    executeThrough(*room, 60);
    auto& guest = room->sessions[2];
    REQUIRE(guest.observeNative(60, observed(), MatchSession::Time{}));
    room->wires[2].result = PacketTransport::SendResult::Congested;
    guest.flush(room->wires[2], MatchSession::Time{});
    CHECK_FALSE(guest.observeNative(60, observed(1), MatchSession::Time{}));
    CHECK(guest.phase() == Phase::Stopped);
    CHECK(guest.stopReason() == MatchStop::TransportFailure);
    CHECK(guest.queuedControls() == 1);
    for (u32 attempt = 0; attempt < 10; ++attempt) {
        guest.flush(room->wires[2], MatchSession::Time{});
        CHECK(guest.queuedControls() == 1);
    }
    CHECK(room->wires[2].sent.empty());
    room->wires[2].result = PacketTransport::SendResult::Sent;
    pumpAgreement(*room);
    for (u8 peer = 1; peer <= 4; ++peer) {
        CHECK(room->sessions[peer].phase() == Phase::Stopped);
        CHECK(room->sessions[peer].stopReason() == MatchStop::TransportFailure);
    }
}

TEST_CASE("native match observation timeout uses monotonic time even while simulation is stalled",
          "[match-lockstep][match-native-agreement]") {
    const bool congested = GENERATE(false, true);
    auto room = std::make_unique<Room>();
    room->ready();
    executeThrough(*room, 60);
    auto& guest = room->sessions[2];
    REQUIRE(guest.observeNative(60, observed(), MatchSession::Time{}));
    if (congested) {
        room->wires[2].result = PacketTransport::SendResult::Congested;
    }
    pumpAgreement(*room);
    guest.update(0, MatchSession::Time{} + std::chrono::seconds{9});
    CHECK(guest.phase() == Phase::Running);
    guest.update(0, MatchSession::Time{} + NativeAgreement::kTimeout);
    CHECK(guest.phase() == Phase::Stopped);
    CHECK(guest.stopReason() == MatchStop::TransportFailure);
    room->wires[2].result = PacketTransport::SendResult::Sent;
    pumpAgreement(*room, MatchSession::Time{} + NativeAgreement::kTimeout);
    CHECK(room->sessions[1].phase() == Phase::Stopped);
    CHECK(room->sessions[1].stopReason() == MatchStop::TransportFailure);
}

TEST_CASE("native match observation demux preserves authentication and old epoch isolation",
          "[match-lockstep][match-native-agreement]") {
    auto room = std::make_unique<Room>();
    room->ready();
    auto& host = room->sessions[1];
    // Use a real earlier nonzero epoch. Epoch zero is malformed, not a stale
    // agreement stream, and cannot be produced by the native sender.
    const auto previousEpoch = host.context().epoch;
    const auto previousReport = observationReport(previousEpoch, observed());
    executeThrough(*room, 1);
    const auto initialParty = room->party();
    REQUIRE(host.prepare(7, MatchTransition::Travel, &initialParty));
    pumpAgreement(*room);
    REQUIRE(room->sessions[2].nativeDraining());
    REQUIRE(room->sessions[2].completedNativeTick(1));
    room->commit();
    const auto epoch = host.context().epoch;
    REQUIRE(epoch == previousEpoch + 1);
    const auto report = observationReport(epoch, observed());
    CHECK(host.receive(999, report, MatchSession::Time::max()) == Admission::WrongPeer);
    CHECK(host.nativeAgreement() == nullptr);
    CHECK(host.receive(102, previousReport, MatchSession::Time::max()) == Admission::Stale);
    CHECK(host.nativeAgreement() == nullptr);
    CHECK(host.phase() == Phase::Running);
    REQUIRE(host.receive(102, report, MatchSession::Time{}) == Admission::Accepted);
    REQUIRE(host.nativeAgreement());
    CHECK(host.nativeAgreement()->agreedTick() == 0);
    executeThrough(*room, 60);
    REQUIRE(host.observeNative(60, observed(), MatchSession::Time{}));
    // The synthetic received report is authenticated evidence, but the guest
    // still needs its own local submission to validate the host's verdict.
    REQUIRE(room->sessions[2].observeNative(60, observed(), MatchSession::Time{}));
    pumpAgreement(*room);
    CHECK(host.nativeAgreement()->agreedTick() == 60);
    const auto party = room->party();
    REQUIRE(host.prepare(8, MatchTransition::Travel, &party));
    CHECK(host.nativeAgreement() == nullptr);
    pumpAgreement(*room);
    REQUIRE(room->sessions[2].nativeDraining());
    REQUIRE(room->sessions[2].completedNativeTick(60));
    CHECK(room->sessions[2].nativeAgreement() == nullptr);
    room->commit();
    CHECK(host.context().epoch == epoch + 1);
    CHECK(host.receive(102, report, MatchSession::Time::max()) == Admission::Stale);
    CHECK(host.nativeAgreement() == nullptr);
    executeThrough(*room, 60);
    for (u8 peer = 1; peer <= 2; ++peer) {
        REQUIRE(room->sessions[peer].observeNative(60, observed(2), MatchSession::Time{}));
    }
    pumpAgreement(*room);
    CHECK(host.nativeAgreement()->agreedTick() == 60);
    CHECK(host.phase() == Phase::Running);
}

TEST_CASE("native match missing scheduled observations and malformed current reports fail closed",
          "[match-lockstep][match-native-agreement]") {
    const bool missing = GENERATE(false, true);
    auto room = std::make_unique<Room>();
    room->ready();
    auto& host = room->sessions[1];
    if (missing) {
        executeThrough(*room, 120); // optional until used, but cannot start by omitting tick 60
        CHECK_FALSE(host.observeNative(120, observed(), MatchSession::Time{}));
    } else {
        auto malformed = observationReport(host.context().epoch, observed());
        malformed[7] = 1;
        CHECK(host.receive(102, malformed, MatchSession::Time{}) == Admission::Invalid);
    }
    CHECK(host.phase() == Phase::Stopped);
    CHECK(host.stopReason() == MatchStop::TransportFailure);
    pumpAgreement(*room);
    CHECK(room->sessions[2].phase() == Phase::Stopped);
    CHECK(room->sessions[2].stopReason() == MatchStop::TransportFailure);
}

TEST_CASE("native match observation transport failure and disconnect stop surviving peers",
          "[match-lockstep][match-native-agreement]") {
    const bool disconnected = GENERATE(false, true);
    auto room = std::make_unique<Room>(MatchOwners{1, 2, 3, 4});
    room->ready();
    executeThrough(*room, 60);
    if (disconnected) {
        REQUIRE(room->sessions[1].observeNative(60, observed(), MatchSession::Time{}));
        room->sessions[1].disconnected(102);
        CHECK(room->sessions[1].phase() == Phase::Stopped);
        CHECK(room->sessions[1].stopReason() == MatchStop::TransportFailure);
        pumpAgreement(*room);
        for (const u8 peer : {u8{3}, u8{4}}) {
            CHECK(room->sessions[peer].phase() == Phase::Stopped);
            CHECK(room->sessions[peer].stopReason() == MatchStop::TransportFailure);
        }
    } else {
        auto& guest = room->sessions[2];
        REQUIRE(guest.observeNative(60, observed(), MatchSession::Time{}));
        room->wires[2].result = PacketTransport::SendResult::Invalid;
        guest.flush(room->wires[2], MatchSession::Time{});
        CHECK(guest.phase() == Phase::Stopped);
        CHECK(guest.stopReason() == MatchStop::TransportFailure);
        room->wires[2].result = PacketTransport::SendResult::Sent;
        pumpAgreement(*room);
        for (u8 peer = 1; peer <= 4; ++peer) {
            CHECK(room->sessions[peer].phase() == Phase::Stopped);
            CHECK(room->sessions[peer].stopReason() == MatchStop::TransportFailure);
        }
    }
}
