#include <array>
#include <memory>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/NativePlayDriver.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Result = NativePlayDriver::Result;

struct Wire final : PacketTransport {
    std::vector<std::vector<u8>> packets;
    SendResult send(Connection, std::span<const u8> bytes, Delivery delivery) override {
        REQUIRE(delivery == Delivery::Reliable);
        packets.emplace_back(bytes.begin(), bytes.end());
        return SendResult::Sent;
    }
    std::vector<Event> poll() override { return {}; }
    std::optional<Statistics> statistics(Connection) const override { return std::nullopt; }
    void close(Connection) override {}
};

struct NativePeer {
    test::FakeRenderDevice device;
    GameConfig config;
    LevelCatalog catalog;
    LevelWorld world;
    PlayScene scene;
    MatchSession match;
    NativePlayDriver driver;

    NativePeer(u8 peer, u8 delay) {
        device.recordDraws = false;
        const auto root = test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2")
                              .parent_path()
                              .parent_path()
                              .parent_path();
        REQUIRE(catalog.load(root));
        const auto level = catalog.byName("G1");
        REQUIRE(level);
        REQUIRE(world.load(device, root, *level));
        GameContext context;
        context.config = &config;
        context.unpackedRoot = root;
        context.levels = &catalog;
        std::array<PartyMember, 2> members;
        members[0].player = 0;
        members[1].player = 2; // Sparse global seats; each peer samples local device zero.
        for (auto& member : members) {
            member.save.name = "TEST";
            member.save.character = member.player;
            member.save.color = member.player;
            member.save.progress().health = 2000;
        }
        PlayOptions options;
        options.welcome = false;
        REQUIRE(scene.open(device, context, world, members, options));
        const std::array links{MatchLink{static_cast<u8>(peer == 1 ? 2 : 1), 1}};
        REQUIRE(match.open(peer, {1, 0, 2, 0}, links, delay));
    }
};

struct NativePair {
    std::unique_ptr<NativePeer> host, guest;
    explicit NativePair(u8 delay)
        : host(std::make_unique<NativePeer>(1, delay)),
          guest(std::make_unique<NativePeer>(2, delay)) {
        REQUIRE(host->match.prepare(1));
        pump();
        REQUIRE(host->driver.bind(host->scene, host->match));
        REQUIRE(guest->driver.bind(guest->scene, guest->match));
        REQUIRE(host->match.loaded());
        REQUIRE(guest->match.loaded());
        pump();
        REQUIRE(host->match.phase() == MatchSession::Phase::Running);
        REQUIRE(guest->match.phase() == MatchSession::Phase::Running);
    }
    void pump() {
        for (u8 pass = 0; pass < 3; ++pass) {
            for (bool reverse : {false, true}) {
                auto& from = reverse ? guest->match : host->match;
                auto& to = reverse ? host->match : guest->match;
                Wire wire;
                from.flush(wire);
                for (const auto& bytes : wire.packets) {
                    const auto admitted = to.receive(1, bytes);
                    REQUIRE((admitted == MatchSession::Admission::Accepted ||
                             admitted == MatchSession::Admission::Stale));
                }
            }
        }
    }
    void step(const SessionInputs::Frame& first = {}, const SessionInputs::Frame& second = {}) {
        REQUIRE(host->driver.sample(host->match, first));
        REQUIRE(guest->driver.sample(guest->match, second));
        pump();
        REQUIRE(host->driver.advance(host->match) == Result::Advanced);
        REQUIRE(guest->driver.advance(guest->match) == Result::Advanced);
    }
};
} // namespace

TEST_CASE("native driver runs both real scenes without a replica or checkpoint binding",
          "[native-play-driver][assets]") {
    const auto delay = GENERATE(u8{0}, u8{3});
    NativePair pair(delay);
    u32 moved = 0;
    for (u32 tick = 1; tick <= 420; ++tick) {
        CAPTURE(tick, delay);
        SessionInputs::Frame first, second;
        if (tick > 360) {
            first[0].move = {{1, 0}, 0.5f};
            second[0].move = {{-1, 0}, 0.5f};
            first[0].attack = tick % 60 < 20;
            second[0].attack = tick % 60 > 40;
        }
        const auto before = pair.host->scene.actor(0)->position();
        pair.step(first, second);
        moved += before != pair.host->scene.actor(0)->position() ? 1U : 0U;
        CHECK(pair.host->driver.completedTick() == tick);
        CHECK(pair.guest->driver.completedTick() == tick);
        for (s32 seat : {0, 2}) {
            REQUIRE(pair.host->scene.actor(seat));
            REQUIRE(pair.guest->scene.actor(seat));
            CHECK(pair.host->scene.actor(seat)->position() ==
                  pair.guest->scene.actor(seat)->position());
            CHECK(pair.host->scene.actor(seat)->yaw() == pair.guest->scene.actor(seat)->yaw());
        }
        CHECK(pair.host->scene.enemies().count() == pair.guest->scene.enemies().count());
        CHECK(pair.host->scene.missiles().count() == pair.guest->scene.missiles().count());
    }
    REQUIRE(moved > 0);
}

TEST_CASE("native stalled frames hold gameplay and retain local one-shot input exactly once",
          "[native-play-driver][assets]") {
    NativePair pair(0);
    REQUIRE(pair.host->driver.sample(pair.host->match, {}));
    const auto cameraTicks = pair.host->scene.startCamera().ticksLeft();
    for (u32 poll = 0; poll < 20; ++poll) {
        SessionInputs::Frame input;
        input[0].usePotion = poll == 3;
        input[0].attack = poll == 4;
        input[0].attackPressed = poll == 4;
        input[0].menu.pointerPressed = poll == 7;
        REQUIRE(pair.host->driver.sample(pair.host->match, input));
        REQUIRE(pair.host->driver.advance(pair.host->match) == Result::Held);
        CHECK(pair.host->driver.completedTick() == 0);
        CHECK(pair.host->scene.startCamera().ticksLeft() == cameraTicks);
    }
    REQUIRE(pair.guest->driver.sample(pair.guest->match, {}));
    pair.pump();
    REQUIRE(pair.host->driver.advance(pair.host->match) == Result::Advanced);
    REQUIRE(pair.guest->driver.advance(pair.guest->match) == Result::Advanced);
    REQUIRE(pair.host->match.resolvedInput(0));
    CHECK_FALSE(pair.host->match.resolvedInput(0)->pressed(CommandPress::UsePotion));
    pair.step();
    for (const auto* peer : {pair.host.get(), pair.guest.get()}) {
        REQUIRE(peer->match.resolvedInput(0));
        CHECK(peer->match.resolvedInput(0)->pressed(CommandPress::UsePotion));
        CHECK(peer->match.resolvedInput(0)->pressed(CommandPress::Attack));
        CHECK(peer->match.resolvedInput(0)->held(CommandHeld::Attack));
        CHECK(peer->match.resolvedInput(0)->pressed(CommandPress::MenuAny));
    }
    pair.step();
    CHECK_FALSE(pair.host->match.resolvedInput(0)->pressed(CommandPress::UsePotion));
    CHECK_FALSE(pair.guest->match.resolvedInput(0)->pressed(CommandPress::MenuAny));
    CHECK_FALSE(pair.guest->match.resolvedInput(0)->held(CommandHeld::Attack));
}

TEST_CASE("native driver never advances after disconnect", "[native-play-driver][assets]") {
    NativePair pair(0);
    pair.host->match.disconnected(1);
    CHECK(pair.host->driver.advance(pair.host->match) == Result::Failed);
    CHECK(pair.host->driver.completedTick() == 0);
    CHECK_FALSE(pair.host->driver.sample(pair.host->match, {}));
    CHECK(pair.host->driver.failed());
}

TEST_CASE("native dead-player Quit acknowledges its frame while Wait keeps the match running",
          "[native-play-driver][native-quit][assets]") {
    const auto delay = GENERATE(u8{0}, u8{3});
    const bool quit = GENERATE(false, true);
    CAPTURE(delay, quit);
    NativePair pair(delay);
    for (u32 tick = 0; tick < 420; ++tick) {
        pair.step();
    }
    // Reach the real native death/prompt state. The second sparse seat stays
    // standing, so ordinary Wait cannot become a whole-party tower return.
    for (auto* peer : {pair.host.get(), pair.guest.get()}) {
        peer->scene.hurtPlayer(0, 100000.0f, HurtKind::Blow);
        REQUIRE(peer->scene.participants()[0].life == PlayerLife::Dying);
    }
    for (u32 tick = 0; tick < 900 && (!pair.host->scene.participants()[0].towerPrompt ||
                                      !pair.guest->scene.participants()[0].towerPrompt);
         ++tick) {
        pair.step();
    }
    for (const auto* peer : {pair.host.get(), pair.guest.get()}) {
        REQUIRE(peer->scene.participants()[0].towerPrompt);
        REQUIRE(peer->scene.participants()[0].life == PlayerLife::InTower);
        REQUIRE(peer->scene.participants()[1].life == PlayerLife::Standing);
        REQUIRE_FALSE(peer->scene.participants()[0].departed);
    }
    bool answered = false;
    for (u32 tick = 0; tick <= static_cast<u32>(delay) + 1; ++tick) {
        SessionInputs::Frame input;
        input[0].menu.back = quit && tick == 0;
        input[0].menu.select = !quit && tick == 0;
        REQUIRE(pair.host->driver.sample(pair.host->match, input));
        REQUIRE(pair.guest->driver.sample(pair.guest->match, {}));
        pair.pump();
        const auto host = pair.host->driver.advance(pair.host->match);
        const auto guest = pair.guest->driver.advance(pair.guest->match);
        REQUIRE(host == guest);
        REQUIRE(host != Result::Failed);
        if (!pair.host->scene.participants()[0].towerPrompt) {
            CHECK(host == (quit ? Result::Leave : Result::Advanced));
            answered = true;
            break;
        }
        REQUIRE(host == Result::Advanced);
    }
    REQUIRE(answered);
    for (const auto* peer : {pair.host.get(), pair.guest.get()}) {
        CHECK(peer->scene.participants()[0].departed == quit);
        CHECK_FALSE(peer->scene.participants()[1].departed);
        CHECK_FALSE(peer->driver.failed());
        CHECK(peer->match.tick() == peer->driver.completedTick() + 1);
    }
    REQUIRE(pair.host->driver.completedTick() == pair.guest->driver.completedTick());
    if (!quit) {
        const auto before = pair.host->driver.completedTick();
        for (u32 tick = 0; tick < 30; ++tick) {
            pair.step();
        }
        CHECK(pair.host->driver.completedTick() == before + 30);
        CHECK_FALSE(pair.host->scene.participants()[0].departed);
        return;
    }

    for (auto* peer : {pair.host.get(), pair.guest.get()}) {
        const auto completed = peer->driver.completedTick();
        const auto sampleTick = peer->match.inputTick();
        const auto livingTime = peer->scene.actor(2)->save().progress().lifetime.playSeconds;
        // No in-flight frame remains: its second acknowledgment is refused.
        CHECK_FALSE(peer->match.completedNativeTick(completed));
        for (u32 poll = 0; poll < 30; ++poll) {
            SessionInputs::Frame input;
            input[0].attack = true;
            input[0].move = {{1, 0}, 1};
            REQUIRE(peer->driver.sample(peer->match, input));
            CHECK(peer->driver.advance(peer->match) == Result::Held);
        }
        CHECK(peer->driver.completedTick() == completed);
        CHECK(peer->match.inputTick() == sampleTick);
        CHECK(peer->scene.actor(2)->save().progress().lifetime.playSeconds == livingTime);
        CHECK_FALSE(peer->driver.failed());
    }
}

TEST_CASE("native simultaneous Quit ends the fixed roster without treating seats as malformed",
          "[native-play-driver][native-quit][assets]") {
    NativePair pair(0);
    for (u32 tick = 0; tick < 420; ++tick) {
        pair.step();
    }
    for (auto* peer : {pair.host.get(), pair.guest.get()}) {
        // Isolate the prompt boundary for simultaneous responses, before the
        // native all-fallen return phase can run. No departed flag is forged;
        // both are set only by the real answerTowerPrompts input dispatch.
        for (const auto& participant : peer->scene.participants()) {
            auto& runtime = const_cast<PlayerRuntime&>(participant);
            runtime.life = PlayerLife::InTower;
            runtime.towerPrompt = true;
            runtime.actor.save().progress().health = 1;
        }
    }
    SessionInputs::Frame quit;
    quit[0].menu.back = true; // Each peer owns its local zero, global seats 0/2.
    REQUIRE(pair.host->driver.sample(pair.host->match, quit));
    REQUIRE(pair.guest->driver.sample(pair.guest->match, quit));
    pair.pump();
    for (auto* peer : {pair.host.get(), pair.guest.get()}) {
        const auto before = peer->driver.completedTick();
        REQUIRE(peer->driver.advance(peer->match) == Result::Leave);
        CHECK(peer->driver.completedTick() == before + 1);
        CHECK(peer->scene.gameOver().active()); // Native all-Quit side effect remains intact.
        CHECK(peer->scene.party().empty());
        CHECK_FALSE(peer->match.completedNativeTick(before + 1));
        REQUIRE(peer->driver.sample(peer->match, {}));
        CHECK(peer->driver.advance(peer->match) == Result::Held);
        CHECK(peer->driver.completedTick() == before + 1);
        CHECK_FALSE(peer->driver.failed());
    }
}

TEST_CASE("native GameOver outcome remains GameOver and is acknowledged before holding",
          "[native-play-driver][native-quit][assets]") {
    NativePair pair(0);
    pair.host->scene.startGameOver();
    pair.guest->scene.startGameOver();
    for (u32 tick = 1; tick <= static_cast<u32>(GameOver::kDurationTicks); ++tick) {
        REQUIRE(pair.host->driver.sample(pair.host->match, {}));
        REQUIRE(pair.guest->driver.sample(pair.guest->match, {}));
        pair.pump();
        const auto expected = tick == static_cast<u32>(GameOver::kDurationTicks) ? Result::GameOver
                                                                                 : Result::Advanced;
        CHECK(pair.host->driver.advance(pair.host->match) == expected);
        CHECK(pair.guest->driver.advance(pair.guest->match) == expected);
    }
    for (auto* peer : {pair.host.get(), pair.guest.get()}) {
        CHECK(peer->scene.gameOver().finished());
        CHECK_FALSE(peer->driver.failed());
        CHECK_FALSE(peer->match.completedNativeTick(peer->driver.completedTick()));
        REQUIRE(peer->driver.sample(peer->match, {}));
        CHECK(peer->driver.advance(peer->match) == Result::Held);
        CHECK(peer->driver.completedTick() == GameOver::kDurationTicks);
    }
}
