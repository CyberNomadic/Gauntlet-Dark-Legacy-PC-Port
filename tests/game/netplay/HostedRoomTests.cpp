#include <algorithm>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "game/netplay/HostedRoom.h"
#include "game/netplay/InputCommand.h"
#include "game/netplay/OnlineSession.h"
#include "game/netplay/RollbackSessionHarness.h"
#include "game/screens/NativePlaySetup.h"

namespace {
using namespace gdl;
using namespace gdl::game;
struct Wire final : PacketTransport {
    Wire* remote = nullptr;
    std::vector<Event> incoming;
    bool congested = false;
    SendResult send(Connection id, std::span<const u8> bytes, Delivery /*delivery*/) override {
        if (congested) {
            return SendResult::Congested;
        }
        remote->incoming.push_back({EventType::Message, id, {bytes.begin(), bytes.end()}, {}});
        return SendResult::Sent;
    }
    std::vector<Event> poll() override { return std::exchange(incoming, {}); }
    std::optional<Statistics> statistics(Connection /*connection*/) const override { return {}; }
    void close(Connection id) override {
        if (remote) {
            remote->incoming.push_back({EventType::Disconnected, id, {}, {}});
        }
    }
};
struct Pair {
    Wire a, b;
    HostedRoom hostRoom, guestRoom;
    OnlineSession host, guest;
    test::RollbackSessionHarness hostInputs, guestInputs;
    Pair(u8 hostPlayers = 1, u8 guestPlayers = 1, const std::string& version = "test",
         const std::string& content = std::string(64, 'a'))
        : hostRoom(a, true, hostPlayers, "test", std::string(64, 'a')),
          guestRoom(b, false, guestPlayers, version, content),
          host(hostRoom, hostRoom.transport(), true, hostPlayers),
          guest(guestRoom, guestRoom.transport(), false, guestPlayers) {
        a.remote = &b;
        b.remote = &a;
        a.incoming.push_back({PacketTransport::EventType::Connected, 7, {}, {}});
        b.incoming.push_back({PacketTransport::EventType::Connected, 7, {}, {}});
        const OnlineSession::NativeIdentity identity{"test", "test-runtime", std::string(64, 'a'),
                                                     std::string(64, 'b'), "en"};
        OnlineSession::NativeRules rules;
        rules.scene = 1;
        REQUIRE(host.configureNative(identity, rules));
        REQUIRE(guest.configureNative(
            {version, identity.runtime, content, identity.data, identity.language}));
        CharacterProfile profile;
        profile.name = "TEST";
        REQUIRE(host.select(std::vector<CharacterProfile>(hostPlayers, profile)));
        REQUIRE(guest.select(std::vector<CharacterProfile>(guestPlayers, profile)));
    }
    ~Pair() {
        hostInputs.clear();
        guestInputs.clear();
        host.leave();
        guest.leave();
        hostRoom.leave();
        guestRoom.leave();
        a.remote = nullptr;
        b.remote = nullptr;
    }
    Pair(const Pair&) = delete;
    Pair& operator=(const Pair&) = delete;
    Pair(Pair&&) = delete;
    Pair& operator=(Pair&&) = delete;
    void pump() {
        for (s32 i = 0; i < 10; ++i) {
            host.update(0);
            guest.update(0);
            for (auto* session : {&host, &guest}) {
                if (!session->nativeSetupPending()) {
                    continue;
                }
                const auto& agreed = *session->nativeContract();
                const NativePlaySetup::Contract contract{agreed.rules.scene,
                                                         agreed.rules.autoMelee,
                                                         agreed.rules.autoActivateItems,
                                                         agreed.rules.expandedShop, agreed.rules.autoAim,
                                                         agreed.identity.build,
                                                         agreed.identity.content,
                                                         agreed.identity.data};
                const auto setup = NativePlaySetup::create(GameConfig{}, session->room()->settings,
                                                           *session->party(), {}, contract);
                REQUIRE(setup);
                REQUIRE(session->confirmNativeSetup(setup->digest()));
            }
        }
    }
};
} // namespace
TEST_CASE("peers missing replay capabilities cannot join even with the same build name",
          "[netplay][hosted-room]") {
    for (const u8 oldProtocol :
         {u8{1},  u8{13}, u8{14}, u8{15}, u8{16}, u8{17}, u8{18}, u8{19}, u8{20}, u8{21},
          u8{22}, u8{23}, u8{24}, u8{25}, u8{26}, u8{27}, u8{28}, u8{29}, u8{30}, u8{31}}) {
        Pair pair;
        pair.guest.update(0);
        bool rewrote = false;
        for (auto& event : pair.a.incoming) {
            if (event.type == PacketTransport::EventType::Message && event.bytes.size() >= 2) {
                event.bytes[1] = oldProtocol;
                rewrote = true;
            }
        }
        REQUIRE(rewrote);
        pair.pump();
        CHECK_FALSE(pair.guest.connected());
        REQUIRE(pair.host.room());
        CHECK(pair.host.room()->members.size() == 1);
    }
}

TEST_CASE("hosted exact-input packets fit a single provider fragment", "[netplay][hosted-room]") {
    Pair pair;
    pair.pump();
    REQUIRE(pair.host.ready());
    REQUIRE(pair.guest.ready());
    pair.pump();
    REQUIRE(pair.host.start());
    pair.pump();
    REQUIRE(pair.host.phase() == OnlineSession::Phase::Active);
    REQUIRE(pair.b.incoming.empty());

    std::vector<InputCommand> inputs(InputPacket::kMaxCommands);
    for (usize index = 0; index < inputs.size(); ++index) {
        inputs[index].epoch = 1;
        inputs[index].tick = index + 1;
        inputs[index].grant = 1;
    }
    const RollbackExchange::Message message{1, 1, 1, {}, inputs, 1};
    const auto packet = RollbackExchange::encode(message);
    REQUIRE(packet);
    REQUIRE(pair.hostRoom.transport().send(7, *packet, PacketTransport::Delivery::Unreliable) ==
            PacketTransport::SendResult::Sent);
    REQUIRE(pair.b.incoming.size() == 1);
    const auto& wrapped = pair.b.incoming.front().bytes;
    CHECK(wrapped.size() == packet->size() + 1);
    CHECK(std::equal(packet->begin(), packet->end(), wrapped.begin() + 1));
    CHECK(wrapped.size() <= 1000);
}

TEST_CASE("self hosted admission starts existing online gameplay without a room server",
          "[netplay][hosted-room]") {
    for (u8 players = 1; players <= 3; ++players) {
        Pair pair(players, 1);
        pair.a.congested = true;
        pair.pump();
        CHECK(pair.guest.phase() == OnlineSession::Phase::Joining);
        pair.a.congested = false;
        pair.pump();
        REQUIRE(pair.host.connected());
        REQUIRE(pair.guest.connected());
        REQUIRE(pair.guest.seats().size() == 1);
        CHECK(pair.guest.seats()[0] == players);
        REQUIRE(pair.host.ready());
        REQUIRE(pair.guest.ready());
        pair.pump();
        REQUIRE(pair.host.start());
        pair.pump();
        REQUIRE(pair.host.phase() == OnlineSession::Phase::Active);
        REQUIRE(pair.guest.phase() == OnlineSession::Phase::Active);
        REQUIRE_FALSE(pair.host.confirmedNativeSetup().empty());
        CHECK(pair.host.confirmedNativeSetup() == pair.guest.confirmedNativeSetup());
        REQUIRE(pair.host.match().prepare(1));
        pair.pump();
        REQUIRE(pair.guest.match().phase() == MatchSession::Phase::Loading);
        CHECK_FALSE(pair.host.match().loaded());
        CHECK_FALSE(pair.guest.match().loaded());
        REQUIRE(pair.hostInputs.bind(pair.host.match()));
        REQUIRE(pair.guestInputs.bind(pair.guest.match()));
        REQUIRE(pair.host.match().loaded());
        REQUIRE(pair.guest.match().loaded());
        pair.pump();
        REQUIRE(pair.guest.match().phase() == MatchSession::Phase::Running);
        // The agreed one-tick local buffer is executable for every owned seat;
        // admission alone is not a substitute for the native input consumer.
        REQUIRE(pair.hostInputs.advance());
        REQUIRE(pair.guestInputs.advance());
        REQUIRE(pair.hostInputs.complete());
        REQUIRE(pair.guestInputs.complete());
        CHECK(pair.host.match().nativeConfirmed() == 1);
        CHECK(pair.guest.match().nativeConfirmed() == 1);
        pair.guest.leave();
        pair.pump();
        REQUIRE(pair.host.phase() == OnlineSession::Phase::Failed);
    }
}
TEST_CASE("hosted room admission rejects mismatched builds assets and full parties",
          "[netplay][hosted-room]") {
    SECTION("build") {
        Pair pair(1, 1, "different");
        pair.pump();
        CHECK(pair.guestRoom.failure() == HostedRoom::Failure::Version);
        CHECK(pair.guest.failureDetail() == "Game versions do not match");
        CHECK(pair.host.phase() == OnlineSession::Phase::Lobby);
    }
    SECTION("assets") {
        Pair pair(1, 1, "test", std::string(64, 'b'));
        pair.pump();
        CHECK(pair.guestRoom.failure() == HostedRoom::Failure::Assets);
        CHECK(pair.guest.failureDetail() == "Game assets do not match");
    }
    SECTION("full") {
        Pair pair(3, 2);
        pair.pump();
        CHECK(pair.guestRoom.failure() == HostedRoom::Failure::Full);
        CHECK(pair.guest.failureDetail() == "Room has no free player slots");
    }
}
TEST_CASE("a hosted lobby survives guest departure and resets readiness",
          "[netplay][hosted-room]") {
    Pair pair;
    pair.pump();
    REQUIRE(pair.host.ready());
    REQUIRE(pair.guest.ready());
    pair.pump();
    pair.guest.leave();
    pair.pump();
    REQUIRE(pair.host.phase() == OnlineSession::Phase::Lobby);
    REQUIRE(pair.host.room()->members.size() == 1);
    CHECK_FALSE(pair.host.room()->members[0].ready);
}
TEST_CASE("room rules are host owned replicated and invalidate every ready vote",
          "[netplay][hosted-room][room-settings]") {
    Pair pair;
    pair.pump();
    REQUIRE(pair.host.ready());
    REQUIRE(pair.guest.ready());
    pair.pump();
    const auto revision = pair.host.room()->revision;
    const RoomSettings changed{3, 2, 1};
    CHECK_FALSE(pair.guestRoom.settings(changed));
    CHECK_FALSE(pair.hostRoom.settings({1, 2, 1})); // already two joined
    CHECK_FALSE(pair.hostRoom.settings({4, 3, 0}));
    REQUIRE(pair.hostRoom.settings(changed));
    CHECK_FALSE(pair.hostRoom.start(revision));
    pair.pump();
    CHECK(pair.host.room()->settings == changed);
    CHECK(pair.guest.room()->settings == changed);
    CHECK(pair.host.room()->revision > revision);
    CHECK_FALSE(pair.host.room()->members[0].ready);
    CHECK_FALSE(pair.host.room()->members[1].ready);
    CHECK_FALSE(pair.host.start());
    REQUIRE(pair.host.ready());
    REQUIRE(pair.guest.ready());
    pair.pump();
    REQUIRE(pair.host.start());
    pair.pump();
    CHECK_FALSE(pair.hostRoom.settings({4, 0, 0}));
    CHECK(pair.guest.room()->settings == changed);
}
TEST_CASE("room capacity is enforced when a guest requests multiple local seats",
          "[netplay][hosted-room][room-settings]") {
    Pair pair(1, 2);
    REQUIRE(pair.hostRoom.settings({2, 1, 0}));
    pair.pump();
    CHECK(pair.guestRoom.failure() == HostedRoom::Failure::Full);
    REQUIRE(pair.host.room());
    CHECK(pair.host.room()->members.size() == 1);
}
