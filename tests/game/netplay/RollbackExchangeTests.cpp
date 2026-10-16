#include <algorithm>
#include <array>
#include <map>
#include <limits>
#include <set>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "game/netplay/RollbackExchange.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Exchange = RollbackExchange;
constexpr u64 kEpoch = 63;
constexpr PacketTransport::Connection kHost = 80;

struct Wire final : PacketTransport {
    struct Packet {
        Connection connection;
        std::vector<u8> bytes;
    };
    std::vector<Packet> packets;
    std::map<Connection, usize> budgets;
    SendResult result = SendResult::Sent;
    SendResult send(Connection connection, std::span<const u8> bytes, Delivery delivery) override {
        CHECK(delivery == Delivery::Unreliable);
        CHECK(bytes.size() <= Exchange::kMaxBytes);
        CHECK(bytes.size() + 1 < 1000); // room envelope plus one Iroh fragment
        if (result == SendResult::Sent) {
            if (const auto budget = budgets.find(connection); budget != budgets.end()) {
                if (budget->second == 0) {
                    return SendResult::Congested;
                }
                --budget->second;
            }
            packets.push_back({connection, {bytes.begin(), bytes.end()}});
        }
        return result;
    }
    std::vector<Event> poll() override { return {}; }
    std::optional<Statistics> statistics(Connection) const override { return std::nullopt; }
    void close(Connection) override {}
};

std::vector<u8> encode(const Exchange::Message& message) {
    auto bytes = Exchange::encode(message);
    REQUIRE(bytes);
    return *bytes;
}

struct Room {
    Exchange::Owners owners;
    Exchange::Grants grants{};
    std::array<Exchange, 5> peers;
    std::array<Wire, 5> wires;
    std::array<std::array<std::map<u64, std::vector<u8>>, 4>, 5> actual;
    usize sent = 0;
    usize dropped = 0;
    u64 nowUs = 0;
    bool reject = false;
    std::function<bool(u8, u8, const Exchange::Message&)> drop;
    explicit Room(Exchange::Owners roster = {1, 2, 3, 4}, u8 delay = 0) : owners(roster) {
        for (usize seat = 0; seat < owners.size(); ++seat) {
            grants[seat] = owners[seat] ? static_cast<u32>(20 + seat) : 0;
        }
        std::vector<Exchange::Link> guests;
        for (u8 peer = 2; peer <= 4; ++peer) {
            if (present(peer)) {
                guests.push_back({peer, static_cast<PacketTransport::Connection>(100 + peer)});
                const std::array host{Exchange::Link{1, kHost}};
                REQUIRE(peers[peer].open(peer, kEpoch, owners, grants, delay, host,
                                         [this, peer] { return nowUs + 100'000'000 * peer; }));
            }
        }
        REQUIRE(peers[1].open(1, kEpoch, owners, grants, delay, guests,
                              [this] { return nowUs + 100'000'000; }));
    }
    bool present(u8 peer) const { return std::ranges::find(owners, peer) != owners.end(); }
    std::vector<InputCommand> frame(u8 peer, u64 tick) const {
        std::vector<InputCommand> result;
        for (u8 seat = 0; seat < owners.size(); ++seat) {
            if (owners[seat] == peer) {
                InputCommand command;
                command.epoch = kEpoch;
                command.tick = tick;
                command.seat = seat;
                command.grant = grants[seat];
                command.pressedButtons = tick % 3 == 0 ? static_cast<u32>(CommandPress::Attack) : 0;
                result.push_back(command);
            }
        }
        return result;
    }
    bool consume(u8 peer, const Exchange::Message& message) {
        if (reject) {
            return false;
        }
        for (const auto& command : message.commands) {
            const auto bytes = InputPacket::encode(std::span{&command, 1});
            REQUIRE(bytes);
            auto& history = actual[peer][command.seat];
            const auto [found, added] = history.emplace(command.tick, *bytes);
            CHECK((added || found->second == *bytes));
        }
        return true;
    }
    void deliver(u8 sender) {
        auto packets = std::move(wires[sender].packets);
        wires[sender].packets.clear();
        // Reverse complete send bursts to exercise reordered frames and ACKs.
        std::ranges::reverse(packets);
        for (const auto& packet : packets) {
            const auto decoded = Exchange::decode(packet.bytes);
            REQUIRE(decoded);
            const u8 receiver = sender == 1 ? static_cast<u8>(packet.connection - 100) : 1;
            const auto connection = sender == 1 ? kHost : 100 + sender;
            ++sent;
            if (drop && drop(sender, receiver, *decoded)) {
                ++dropped;
                continue;
            }
            const auto result = peers[receiver].receive(
                connection, packet.bytes, 0,
                [&](const auto& m, auto) { return consume(receiver, m); });
            REQUIRE(result ==
                    (reject ? Exchange::Admission::Rejected : Exchange::Admission::Accepted));
        }
    }
    void pump() {
        // Relay before doing any simulation, not on its next native tick.
        for (u8 peer = 2; peer <= 4; ++peer) {
            if (present(peer)) {
                REQUIRE(peers[peer].flush(wires[peer]));
                deliver(peer);
            }
        }
        REQUIRE(peers[1].flush(wires[1]));
        deliver(1);
    }
};
} // namespace

TEST_CASE("rollback datagrams bound and authenticate their envelope before parsing input",
          "[rollback-exchange]") {
    Room room;
    Exchange::Message message{kEpoch, 2, 3, {}, room.frame(2, 3)};
    const auto bytes = encode(message);
    CHECK(bytes.size() ==
          Exchange::kHeaderBytes + InputPacket::kHeaderBytes + InputPacket::kCommandBytes);
    CHECK(Exchange::recognizes(bytes));
    REQUIRE(Exchange::decode(bytes));
    for (usize size = 0; size < bytes.size(); ++size) {
        // Header alone is a valid empty heartbeat.
        CHECK(Exchange::decode(std::span{bytes}.first(size)).has_value() ==
              (size == Exchange::kHeaderBytes));
    }
    for (const usize offset : {usize{0}, usize{4}, usize{6}, usize{7}}) {
        auto bad = bytes;
        bad[offset] ^= 0x80;
        CHECK_FALSE(Exchange::decode(bad));
    }
    message.origin = 0;
    CHECK_FALSE(Exchange::encode(message));
    message.origin = 2;
    message.epoch = 0;
    CHECK_FALSE(Exchange::encode(message));
    message.epoch = kEpoch;
    message.commands[0].epoch++;
    CHECK_FALSE(Exchange::encode(message));
    message.commands[0].epoch = message.epoch;
    message.progressSerial = 0;
    CHECK_FALSE(Exchange::encode(message));
    message.progressSerial = std::numeric_limits<u64>::max();
    message.progressAgeUs = Exchange::kMaxProgressAgeUs;
    REQUIRE(Exchange::decode(encode(message)));
    ++message.progressAgeUs;
    CHECK_FALSE(Exchange::encode(message));
    CHECK_FALSE(Exchange::decode(std::vector<u8>(Exchange::kMaxBytes + 1)));
}

TEST_CASE("rollback exchange rejects unadmitted origins and does not ACK refused frames",
          "[rollback-exchange]") {
    Room room;
    Exchange::Message message{kEpoch, 2, 1, {}, room.frame(2, 1)};
    usize calls = 0;
    const auto consume = [&](const auto&, auto) {
        ++calls;
        return false;
    };
    CHECK(room.peers[1].receive(103, encode(message), 0, consume) == Exchange::Admission::WrongPeer);
    CHECK(room.peers[3].receive(999, encode(message), 0, consume) == Exchange::Admission::WrongPeer);
    CHECK(calls == 0);
    CHECK(room.peers[1].receive(102, encode(message), 0, consume) == Exchange::Admission::Rejected);
    CHECK(calls == 1);
    CHECK(room.peers[1].receivedThrough(2) == 0);
    CHECK(room.peers[1].retainedFrames(2) == 0);
    message.received[1] = 1; // cannot ACK a host input that has never been sampled
    CHECK(room.peers[1].receive(102, encode(message), 0, consume) == Exchange::Admission::Invalid);
    CHECK(calls == 1);
    message.received[1] = 0;
    message.received[3] = 1; // nor a third party's never-received input at the relay
    CHECK(room.peers[1].receive(102, encode(message), 0, consume) == Exchange::Admission::Invalid);
    CHECK(calls == 1);
    message.received[3] = 0;
    message.commands[0].grant++;
    CHECK(room.peers[1].receive(102, encode(message), 0, consume) == Exchange::Admission::Invalid);
    CHECK(calls == 1);
    message.commands[0].grant--;
    message.epoch++;
    message.commands[0].epoch++;
    CHECK(room.peers[1].receive(102, encode(message), 0, consume) == Exchange::Admission::WrongEpoch);
}

TEST_CASE("eight frames of three local seats split into complete bounded datagrams",
          "[rollback-exchange]") {
    Room room({1, 2, 2, 2});
    for (u64 tick = 1; tick <= 8; ++tick) {
        REQUIRE(room.peers[2].publish(room.frame(2, tick), tick));
    }
    REQUIRE(room.peers[2].flush(room.wires[2]));
    REQUIRE(room.wires[2].packets.size() == 2);
    usize total = 0;
    for (const auto& packet : room.wires[2].packets) {
        const auto message = Exchange::decode(packet.bytes);
        REQUIRE(message);
        CHECK(message->commands.size() % 3 == 0);
        total += message->commands.size();
    }
    CHECK(total == 24);
    room.deliver(2);
    CHECK(room.peers[1].receivedThrough(2) == 8);
    for (usize seat = 1; seat < 4; ++seat) {
        CHECK(room.actual[1][seat].size() == 8);
    }
    Exchange::Message incomplete{kEpoch, 2, 9, {}, room.frame(2, 9)};
    incomplete.commands.pop_back();
    CHECK(room.peers[1].receive(102, encode(incomplete), 0, [](const auto&, auto) { return true; }) ==
          Exchange::Admission::Invalid);
    CHECK(room.peers[1].receivedThrough(2) == 8);
}

TEST_CASE("end to end ACK recovers an old guest input after the relay already received it",
          "[rollback-exchange]") {
    Room room({1, 2, 3, 0});
    bool holdFirst = true;
    room.drop = [&](u8 from, u8 to, const auto& message) {
        return holdFirst && from == 1 && to == 3 && message.origin == 2 &&
               std::ranges::any_of(message.commands,
                                   [](const auto& input) { return input.tick == 1; });
    };
    for (u64 tick = 1; tick <= 12; ++tick) {
        for (u8 peer = 1; peer <= 3; ++peer) {
            REQUIRE(room.peers[peer].publish(room.frame(peer, tick), tick));
        }
        room.pump();
    }
    REQUIRE(room.dropped > 0);
    CHECK(room.peers[1].receivedThrough(2) == 12);
    CHECK(room.peers[3].receivedThrough(2) == 0);
    CHECK(room.peers[2].acknowledgedThrough(1, 2) > 0);
    CHECK(room.peers[2].acknowledgedThrough(3, 2) == 0);
    CHECK(room.peers[2].retainedFrames(2) == 12);
    // No new samples or simulation: the stalled link must still recover.
    holdFirst = false;
    for (usize pass = 0; pass < 5; ++pass) {
        room.pump();
    }
    CHECK(room.peers[3].receivedThrough(2) == 12);
    CHECK(room.peers[2].acknowledgedThrough(3, 2) == 12);
    CHECK(room.peers[2].retainedFrames(2) == 8);
    for (u64 tick = 1; tick <= 12; ++tick) {
        CHECK(room.actual[3][1].contains(tick));
    }
}

TEST_CASE("rollback delivery tolerates reordered lossy traffic and bounds retained history",
          "[rollback-exchange]") {
    const u8 delay = GENERATE(u8{0}, u8{1}, u8{2});
    Room room({1, 2, 3, 4}, delay);
    room.drop = [&](u8, u8, const auto&) { return room.sent % 50 == 0; };
    for (u64 tick = 1 + delay; tick <= 240; ++tick) {
        for (u8 peer = 1; peer <= 4; ++peer) {
            REQUIRE(room.peers[peer].publish(room.frame(peer, tick), tick - delay));
        }
        room.pump();
    }
    room.drop = {};
    for (usize pass = 0; pass < 5; ++pass) {
        room.pump();
    }
    CHECK(room.dropped > 0);
    for (u8 peer = 1; peer <= 4; ++peer) {
        CHECK(room.peers[peer].failure() == Exchange::Failure::None);
        for (u8 origin = 1; origin <= 4; ++origin) {
            CHECK(room.peers[peer].receivedThrough(origin) == 240);
            CHECK(room.peers[peer].retainedFrames(origin) == 8);
            if (peer != origin) {
                CHECK(room.actual[peer][origin - 1].size() == 240 - delay);
            }
        }
    }
}

TEST_CASE("rollback congestion retains input without an expanding packet queue",
          "[rollback-exchange]") {
    Room room({1, 2, 0, 0});
    REQUIRE(room.peers[2].publish(room.frame(2, 1), 1));
    room.wires[2].result = PacketTransport::SendResult::Congested;
    for (usize retry = 0; retry < 150; ++retry) {
        REQUIRE(room.peers[2].flush(room.wires[2]));
    }
    CHECK(room.wires[2].packets.empty());
    CHECK(room.peers[2].retainedFrames(2) == 1);
    room.wires[2].result = PacketTransport::SendResult::Sent;
    room.pump();
    CHECK(room.peers[1].receivedThrough(2) == 1);
    room.peers[2].disconnected(999);
    CHECK(room.peers[2].failure() == Exchange::Failure::None);
    room.peers[2].disconnected(kHost);
    CHECK(room.peers[2].failure() == Exchange::Failure::Disconnected);
    CHECK_FALSE(room.peers[2].flush(room.wires[2]));
}

TEST_CASE("conflicting actual input stops the epoch without overwriting accepted commands",
          "[rollback-exchange]") {
    Room room({1, 2, 0, 0});
    REQUIRE(room.peers[2].publish(room.frame(2, 1), 1));
    room.pump();
    Exchange::Message bad{kEpoch, 2, 1, {}, room.frame(2, 1)};
    bad.commands[0].direction.x = -0.0f;
    CHECK(room.peers[1].receive(102, encode(bad), 0, [](const auto&, auto) { return true; }) ==
          Exchange::Admission::Conflict);
    CHECK(room.peers[1].failure() == Exchange::Failure::Conflict);
    CHECK(room.peers[1].receivedThrough(2) == 1);
    CHECK(room.peers[1].retainedFrames(2) == 1);

    auto localConflict = room.frame(2, 1);
    localConflict[0].pressedButtons = static_cast<u32>(CommandPress::Attack);
    CHECK_FALSE(room.peers[2].publish(localConflict, 1));
    CHECK(room.peers[2].failure() == Exchange::Failure::Conflict);
    CHECK_FALSE(room.peers[2].flush(room.wires[2]));
}

TEST_CASE("a one datagram budget cannot starve later routed origins", "[rollback-exchange]") {
    Room room;
    for (u8 peer = 1; peer <= 4; ++peer) {
        REQUIRE(room.peers[peer].publish(room.frame(peer, 1), 1));
    }
    for (usize pass = 0; pass < 12; ++pass) {
        for (u8 destination = 2; destination <= 4; ++destination) {
            room.wires[1].budgets[100 + destination] = 1;
        }
        room.pump();
    }
    for (u8 receiver = 1; receiver <= 4; ++receiver) {
        for (u8 origin = 1; origin <= 4; ++origin) {
            CHECK(room.peers[receiver].receivedThrough(origin) == 1);
            CHECK(room.peers[receiver].acknowledgedThrough(origin, receiver) == 1);
        }
    }
}

TEST_CASE("an unresponsive receiver cannot cause unbounded input retention",
          "[rollback-exchange]") {
    Room room({1, 2, 0, 0});
    for (u64 tick = 1; tick <= Exchange::kHistory; ++tick) {
        REQUIRE(room.peers[1].publish(room.frame(1, tick), tick));
    }
    CHECK(room.peers[1].retainedFrames(1) == Exchange::kHistory);
    CHECK_FALSE(
        room.peers[1].publish(room.frame(1, Exchange::kHistory + 1), Exchange::kHistory + 1));
    CHECK(room.peers[1].failure() == Exchange::Failure::Window);
    CHECK(room.peers[1].retainedFrames(1) == Exchange::kHistory);
    CHECK_FALSE(room.peers[1].flush(room.wires[1]));
}

TEST_CASE("relayed progress carries upstream age and residence without refreshing cached reports",
          "[rollback-exchange][rollback-pacing]") {
    Room room({1, 2, 3, 0});
    Exchange::Message source{kEpoch, 2, 10, {}, {}, 7, 0};
    room.nowUs = 80'000;
    std::optional<u8> age;
    const auto accept = [&](const auto&, std::optional<u8> transit) {
        age = transit;
        return true;
    };
    REQUIRE(room.peers[1].receive(102, encode(source), 160, accept) ==
            Exchange::Admission::Accepted);
    REQUIRE(age == 5); // first 80 ms hop
    const auto forwarded = [&](u8 origin) {
        REQUIRE(room.peers[1].flush(room.wires[1]));
        std::vector<u8> bytes;
        for (const auto& packet : room.wires[1].packets) {
            const auto message = Exchange::decode(packet.bytes);
            REQUIRE(message);
            if (packet.connection == 103 && message->origin == origin) {
                bytes = packet.bytes;
            }
        }
        room.wires[1].packets.clear();
        REQUIRE_FALSE(bytes.empty());
        return bytes;
    };
    room.nowUs = 100'000; // held at relay for 20 ms
    const auto first = forwarded(2);
    REQUIRE(Exchange::decode(first)->progressAgeUs == 100'000);
    room.nowUs = 110'000;
    REQUIRE(room.peers[3].receive(kHost, first, 20, accept) == Exchange::Admission::Accepted);
    CHECK(age == 7); // 80 + 20 + 10 ms, not the final 10 ms alone

    room.nowUs = 1'000'000; // an upstream outage; relay keeps servicing traffic
    const auto cached = forwarded(2);
    REQUIRE(Exchange::decode(cached)->progressAgeUs == Exchange::kMaxProgressAgeUs);
    REQUIRE(room.peers[3].receive(kHost, cached, 20, accept) == Exchange::Admission::Accepted);
    CHECK_FALSE(age); // a larger age on the same report is not new peer progress
    REQUIRE(room.peers[3].receive(kHost, first, 20, accept) == Exchange::Admission::Accepted);
    CHECK_FALSE(age); // nor is reordered delivery of the earlier copy

    // The source is actually paused but still publishes a new heartbeat.
    ++source.progressSerial;
    REQUIRE(room.peers[1].receive(102, encode(source), 160, accept) ==
            Exchange::Admission::Accepted);
    CHECK(age == 5);
    const auto heartbeat = forwarded(2);
    room.nowUs += 10'000;
    REQUIRE(room.peers[3].receive(kHost, heartbeat, 20, accept) ==
            Exchange::Admission::Accepted);
    CHECK(age == 6); // fresh same-head report, measured path is 90 ms again

    REQUIRE(room.peers[1].publish({}, 10));
    const auto host = forwarded(1);
    REQUIRE(Exchange::decode(host)->progressAgeUs == 0);
    room.nowUs += 10'000;
    REQUIRE(room.peers[3].receive(kHost, host, 20, accept) == Exchange::Admission::Accepted);
    CHECK(age == 1); // peer one has no upstream hop
}

TEST_CASE("progress admission is transactional and older reports still recover actual inputs",
          "[rollback-exchange][rollback-pacing]") {
    Room room({1, 2, 3, 0});
    Exchange::Message source{kEpoch, 2, 10, {}, room.frame(2, 1), 20, 0};
    usize fresh = 0;
    const auto reject = [&](const auto&, std::optional<u8> age) {
        REQUIRE(age);
        ++fresh;
        return false;
    };
    REQUIRE(room.peers[1].receive(102, encode(source), 20, reject) ==
            Exchange::Admission::Rejected);
    CHECK(room.peers[1].receivedThrough(2) == 0);
    REQUIRE(room.peers[1].flush(room.wires[1]));
    for (const auto& packet : room.wires[1].packets) {
        REQUIRE(Exchange::decode(packet.bytes));
        CHECK(Exchange::decode(packet.bytes)->origin != 2);
    }
    std::optional<u8> last;
    const auto accept = [&](const auto&, std::optional<u8> age) {
        last = age;
        fresh += age.has_value();
        return true;
    };
    REQUIRE(room.peers[1].receive(102, encode(source), 20, accept) ==
            Exchange::Admission::Accepted);
    CHECK(fresh == 2); // refusal did not consume the report serial
    CHECK(last == 1);
    CHECK(room.peers[1].receivedThrough(2) == 1);

    source.progressSerial = 19;
    source.nextTick = 9;
    source.commands = room.frame(2, 2);
    REQUIRE(room.peers[1].receive(102, encode(source), 20, accept) ==
            Exchange::Admission::Accepted);
    CHECK_FALSE(last);
    CHECK(room.peers[1].receivedThrough(2) == 2);
    source.progressSerial = 21; // a new serial cannot move the head backwards
    CHECK(room.peers[1].receive(102, encode(source), 20, accept) ==
          Exchange::Admission::Invalid);
    CHECK(fresh == 2);
    source.progressSerial = 20; // the old serial cannot describe a different head
    CHECK(room.peers[1].receive(102, encode(source), 20, accept) ==
          Exchange::Admission::Conflict);
    CHECK(room.peers[1].failure() == Exchange::Failure::Conflict);
}

TEST_CASE("progress ages saturate and unknown RTT remains a bounded pacing estimate",
          "[rollback-exchange][rollback-pacing]") {
    const s32 ping = GENERATE(s32{-1}, std::numeric_limits<s32>::max());
    Room room({1, 2, 0, 0});
    Exchange::Message message{kEpoch, 2, 1, {}, {}, 1, 0};
    REQUIRE(room.peers[1].receive(102, encode(message), ping,
                                  [](const auto&, std::optional<u8> age) {
                                      REQUIRE(age == RollbackInputs::kPredictionTicks);
                                      return true;
                                  }) == Exchange::Admission::Accepted);
    room.nowUs = std::numeric_limits<u64>::max() - 500'000'000;
    REQUIRE(room.peers[1].flush(room.wires[1]));
    REQUIRE_FALSE(room.wires[1].packets.empty());
    for (const auto& packet : room.wires[1].packets) {
        const auto decoded = Exchange::decode(packet.bytes);
        REQUIRE(decoded);
        CHECK(decoded->progressAgeUs == Exchange::kMaxProgressAgeUs);
    }
    room.nowUs = 0; // bad injected clocks must not unsigned-underflow residence
    CHECK_FALSE(room.peers[1].flush(room.wires[1]));
    CHECK(room.peers[1].failure() == Exchange::Failure::Transport);
}

TEST_CASE("asymmetric star progress paces an actually faster peer without slowing equal peers",
          "[rollback-exchange][rollback-pacing]") {
    const usize count = GENERATE(usize{3}, usize{4});
    Room room(count == 3 ? Exchange::Owners{1, 2, 3, 0} : Exchange::Owners{1, 2, 3, 4});
    std::array<RollbackInputs, 5> inputs;
    std::array<usize, 5> idles{};
    // Guest A is 80 ms from the host, B 10 ms, C 30 ms. The receiver only
    // supplies its direct link RTT; no test code adds an upstream hop.
    constexpr std::array<u64, 5> hopUs{0, 0, 80'000, 10'000, 30'000};
    struct Pending {
        u8 sender, receiver;
        u64 arrives;
        std::vector<u8> bytes;
    };
    std::vector<Pending> pending;
    for (u8 peer = 1; peer <= count; ++peer) {
        REQUIRE(inputs[peer].open(peer, kEpoch, room.owners, room.grants, 0));
    }
    for (u64 wall = 0; wall < 240; ++wall) {
        room.nowUs = wall * 1'000'000 / 60;
        std::ranges::sort(pending, {}, &Pending::arrives);
        usize delivered = 0;
        while (delivered < pending.size() && pending[delivered].arrives <= room.nowUs) {
            const auto& packet = pending[delivered++];
            const u8 guest = packet.sender == 1 ? packet.receiver : packet.sender;
            const auto connection = packet.sender == 1 ? kHost : 100 + packet.sender;
            REQUIRE(room.peers[packet.receiver].receive(
                        connection, packet.bytes, static_cast<s32>(hopUs[guest] / 500),
                        [&](const Exchange::Message& message, std::optional<u8> age) {
                            if (!message.commands.empty()) {
                                const auto admitted = inputs[packet.receiver].submit(
                                    message.origin, message.commands);
                                REQUIRE((admitted == RollbackInputs::Admission::Accepted ||
                                         admitted == RollbackInputs::Admission::Duplicate ||
                                         admitted == RollbackInputs::Admission::Stale));
                            }
                            if (age) {
                                const auto progress = inputs[packet.receiver].observePeerTick(
                                    message.origin, message.epoch, message.nextTick, *age);
                                REQUIRE((progress == RollbackInputs::Admission::Accepted ||
                                         progress == RollbackInputs::Admission::Duplicate));
                            }
                            return true;
                        }) == Exchange::Admission::Accepted);
        }
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(delivered));
        for (u8 peer = 1; peer <= count; ++peer) {
            // Only after the equal-rate control, run the host's scheduler twice
            // as fast. Other peers retain their own simulation and 60 Hz cadence.
            const usize budget = wall >= 180 && peer == 1 ? 2 : 1;
            for (usize step = 0; step < budget; ++step) {
                if (inputs[peer].sampleReady()) {
                    auto frame = room.frame(peer, inputs[peer].sampleTick());
                    for (auto& command : frame) {
                        command.pressedButtons = 0; // neutral prediction needs no rewind
                    }
                    REQUIRE(inputs[peer].sample(frame));
                }
                if (inputs[peer].timeSyncIdle()) {
                    ++idles[peer];
                } else if (inputs[peer].ready()) {
                    REQUIRE(inputs[peer].advance());
                }
                CHECK_FALSE(inputs[peer].mismatchTick());
                CHECK(inputs[peer].tick() - 1 - inputs[peer].confirmedTick() <=
                      RollbackInputs::kPredictionTicks);
            }
            REQUIRE(room.peers[peer].publish(inputs[peer].redundantInputs(), inputs[peer].tick()));
        }
        if (wall == 179) {
            for (u8 peer = 1; peer <= count; ++peer) {
                CAPTURE(count, peer);
                CHECK(idles[peer] == 0);
                CHECK(inputs[peer].tick() == 181);
            }
        }
        for (u8 peer = 1; peer <= count; ++peer) {
            REQUIRE(room.peers[peer].flush(room.wires[peer]));
            for (auto& packet : room.wires[peer].packets) {
                const u8 receiver = peer == 1 ? static_cast<u8>(packet.connection - 100) : 1;
                const u8 guest = peer == 1 ? receiver : peer;
                pending.push_back({peer, receiver, room.nowUs + hopUs[guest], std::move(packet.bytes)});
            }
            room.wires[peer].packets.clear();
        }
    }
    CHECK(idles[1] > 0);
    for (u8 peer = 1; peer <= count; ++peer) {
        CHECK(inputs[peer].tick() > 220);
        CHECK(room.peers[peer].failure() == Exchange::Failure::None);
    }
}
