#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "engine/net/SessionTransport.h"

#include "game/netplay/HostedRoom.h"
#include "game/netplay/OnlineSession.h"

namespace {
using namespace gdl;
using namespace gdl::game;
template <class Predicate, class Step> void until(Predicate done, Step step) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done()) {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        step();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
std::unique_ptr<SessionTransport> open(const std::string& invitation = {}) {
    std::string error;
    auto transport = openSessionTransport({true, invitation}, error);
    REQUIRE(transport);
    until([&] { return transport->phase() != SessionTransport::Phase::Opening; }, [] {});
    INFO(transport->error());
    REQUIRE(transport->phase() == SessionTransport::Phase::Ready);
    return transport;
}
} // namespace

TEST_CASE("provider adapter carries room admission party loading pause and departure",
          "[netplay][iroh]") {
    auto wire = open();
    auto remote = open(wire->invitation());
    HostedRoom a(*wire, true, 2, "adapter-test", std::string(64, 'a'));
    HostedRoom b(*remote, false, 2, "adapter-test", std::string(64, 'a'));
    OnlineSession host(a, a.transport(), true, 2);
    OnlineSession guest(b, b.transport(), false, 2);
    CharacterProfile profile;
    profile.name = "TEST";
    REQUIRE(host.select(std::vector<CharacterProfile>(2, profile)));
    REQUIRE(guest.select(std::vector<CharacterProfile>(2, profile)));
    const auto pump = [&] {
        host.update(0.002);
        guest.update(0.002);
        REQUIRE(host.phase() != OnlineSession::Phase::Failed);
        REQUIRE(guest.phase() != OnlineSession::Phase::Failed);
    };
    until(
        [&] {
            return host.room() != nullptr && host.room()->members.size() == 2 && host.connected() &&
                   guest.connected();
        },
        pump);
    REQUIRE(guest.seats().size() == 2);
    CHECK(guest.seats()[0] == 2);
    CHECK(guest.seats()[1] == 3);
    REQUIRE(host.ready());
    REQUIRE(guest.ready());
    until([&] { return host.room()->members[1].ready; }, pump);
    REQUIRE(host.start());
    until(
        [&] {
            return host.phase() == OnlineSession::Phase::Active &&
                   guest.phase() == OnlineSession::Phase::Active;
        },
        pump);
    REQUIRE(host.match().prepare(1));
    until([&] { return guest.match().phase() == MatchSession::Phase::Loading; }, pump);
    REQUIRE(host.match().loaded());
    for (s32 i = 0; i < 10; ++i) {
        pump();
    }
    CHECK(host.match().phase() == MatchSession::Phase::Loading);
    REQUIRE(guest.match().loaded());
    until(
        [&] {
            return guest.match().phase() == MatchSession::Phase::Running &&
                   host.match().phase() == MatchSession::Phase::Running;
        },
        pump);
    REQUIRE(guest.match().requestPause());
    until(
        [&] {
            return host.match().phase() == MatchSession::Phase::Paused &&
                   guest.match().phase() == MatchSession::Phase::Paused;
        },
        pump);
    guest.leave();
    until([&] { return host.phase() == OnlineSession::Phase::Failed; },
          [&] {
              host.update(0.002);
              guest.update(0.002);
          });
}

TEST_CASE("provider room rejects incompatible content without losing the host", "[netplay][iroh]") {
    auto wire = open();
    auto remote = open(wire->invitation());
    HostedRoom a(*wire, true, 1, "adapter-test", std::string(64, 'a'));
    HostedRoom b(*remote, false, 1, "adapter-test", std::string(64, 'b'));
    OnlineSession host(a, a.transport(), true, 1);
    OnlineSession guest(b, b.transport(), false, 1);
    until([&] { return guest.phase() == OnlineSession::Phase::Failed; },
          [&] {
              host.update(0.002);
              guest.update(0.002);
          });
    CHECK(b.failure() == HostedRoom::Failure::Assets);
    CHECK(host.phase() == OnlineSession::Phase::Lobby);
}

TEST_CASE("three independent provider peers admit a late third player and start together",
          "[netplay][iroh][three-peers]") {
    auto wire = open();
    auto remote = open(wire->invitation());
    HostedRoom a(*wire, true, 1, "adapter-test", std::string(64, 'a'));
    HostedRoom b(*remote, false, 1, "adapter-test", std::string(64, 'a'));
    OnlineSession host(a, a.transport(), true, 1);
    OnlineSession first(b, b.transport(), false, 1);
    CharacterProfile profile;
    profile.name = "TEST";
    REQUIRE(host.select(std::array{profile}));
    REQUIRE(first.select(std::array{profile}));
    const auto pumpPair = [&] {
        host.update(0.002);
        first.update(0.002);
        REQUIRE(host.phase() != OnlineSession::Phase::Failed);
        REQUIRE(first.phase() != OnlineSession::Phase::Failed);
    };
    until([&] { return host.connected() && first.connected(); }, pumpPair);
    REQUIRE(host.ready());
    REQUIRE(first.ready());
    until([&] { return host.room()->members[1].ready; }, pumpPair);
    auto thirdWire = open(wire->invitation());
    HostedRoom c(*thirdWire, false, 1, "adapter-test", std::string(64, 'a'));
    OnlineSession second(c, c.transport(), false, 1);
    REQUIRE(second.select(std::array{profile}));
    usize steps = 0;
    const auto pump = [&] {
        pumpPair();
        // Unequal receive cadence, as when a distant guest batches incoming traffic.
        if (++steps % 5 == 0) {
            second.update(0.010);
        }
        INFO(second.failureDetail());
        REQUIRE(second.phase() != OnlineSession::Phase::Failed);
    };
    until(
        [&] {
            return host.room()->members.size() == 3 && first.room()->members.size() == 3 &&
                   second.connected() && host.connected();
        },
        pump);
    CHECK_FALSE(host.room()->members[0].ready);
    CHECK_FALSE(host.room()->members[1].ready);
    CHECK(first.seats()[0] == 1);
    CHECK(second.seats()[0] == 2);
    REQUIRE(host.ready());
    REQUIRE(first.ready());
    REQUIRE(second.ready());
    until([&] { return host.room()->members[1].ready && host.room()->members[2].ready; }, pump);
    REQUIRE(host.start());
    until(
        [&] {
            return host.phase() == OnlineSession::Phase::Active &&
                   first.phase() == OnlineSession::Phase::Active &&
                   second.phase() == OnlineSession::Phase::Active;
        },
        pump);
    REQUIRE(host.match().prepare(1));
    until(
        [&] {
            return first.match().phase() == MatchSession::Phase::Loading &&
                   second.match().phase() == MatchSession::Phase::Loading;
        },
        pump);
    REQUIRE(host.match().loaded());
    REQUIRE(first.match().loaded());
    for (usize i = 0; i < 50; ++i) {
        pump();
    }
    CHECK(host.match().phase() == MatchSession::Phase::Loading);
    REQUIRE(second.match().loaded());
    until(
        [&] {
            return host.match().phase() == MatchSession::Phase::Running &&
                   first.match().phase() == MatchSession::Phase::Running &&
                   second.match().phase() == MatchSession::Phase::Running;
        },
        pump);
}
