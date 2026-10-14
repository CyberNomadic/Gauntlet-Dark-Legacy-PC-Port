#include <array>
#include <memory>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/netplay/CombatReplica.h"
#include "game/netplay/OnlinePair.h"
#include "game/screens/OpponentResources.h"
#include "game/screens/PlayReplication.h"
#include "game/world/LevelCatalog.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct Scene {
    std::filesystem::path root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    GameConfig config;
    LevelCatalog catalog;
    LevelWorld world;
    PlayScene play;
    ProjectileResources projectiles;
    PickupResources pickups;
    FixtureResources fixtures;

    explicit Scene(bool stage = false, bool powerups = false, bool reversed = false,
                   bool shrink = false) {
        config.difficulty.level = "hard";
        if (stage) {
            REQUIRE(catalog.load(root));
            const auto level = catalog.byName("G1");
            REQUIRE(level);
            REQUIRE(world.load(device, root, *level));
        }
        GameContext context;
        context.config = &config;
        context.levels = stage ? &catalog : nullptr;
        context.unpackedRoot = root;
        PlayOptions options;
        options.welcome = false;
        options.position = Vec3{3, 2, -20};
        if (stage) {
            REQUIRE(world.startPoint(0));
            options.position = world.startPoint(0)->position;
        }
        CharacterSave save;
        save.progress().health = 1000;
        save.progress().experience = levelExperience(23);
        if (powerups) {
            save.progress().inventory.addPowerup(powerup::kSpecial, powerup::kStopTime, 0, 60);
        }
        if (shrink) {
            save.progress().inventory.addPowerup(powerup::kSpecial, powerup::kEnemyShrink, 0, 60);
        }
        const std::array party = reversed ? std::array{PartyMember{2, save}, PartyMember{0, save}}
                                          : std::array{PartyMember{0, save}, PartyMember{1, save}};
        REQUIRE(play.open(device, context, world, party, options));
        REQUIRE(play.bindReplicationResources(projectiles, pickups, fixtures));
    }

    void settle() {
        for (s32 tick = 0; tick < 400; ++tick) {
            REQUIRE(play.update(1.0 / 60, {}) == PlayOutcome::Running);
        }
        REQUIRE_FALSE(play.spawning());
    }
};

struct PublicationWire final : PacketTransport {
    CombatReplica::Packets packets;
    SendResult send(Connection /*connection*/, std::span<const u8> bytes,
                    Delivery delivery) override {
        REQUIRE(delivery == Delivery::Unreliable);
        packets.emplace_back(bytes.begin(), bytes.end());
        return SendResult::Sent;
    }
    std::vector<Event> poll() override { return {}; }
    std::optional<Statistics> statistics(Connection /*connection*/) const override { return {}; }
    void close(Connection /*connection*/) override {}
};

TEST_CASE("lazy opponent fallback preserves eager selection and exact published packets",
          "[netplay][play-opponent-capture][prepared-publication][lazy-opponent-capture][assets]") {
    enum class Mode { Complete, Tower, Arrival, RefusedComplete };
    const auto mode = GENERATE(Mode::Complete, Mode::Tower, Mode::Arrival, Mode::RefusedComplete);
    CAPTURE(mode);
    // Keep the native scene and full snapshot owners off the Windows test stack.
    auto scene = std::make_unique<Scene>(mode != Mode::Tower);
    if (mode != Mode::Arrival) {
        scene->settle();
    }
    if (mode == Mode::RefusedComplete) {
        // A valid native pool can be outside the ordinary-combat domain. Its
        // presentation/opponent checkpoint remains encodable; the full joint
        // seed must be refused by public cross-owner validation, not truncated.
        auto swarm = scene->play.enemies().checkpoint();
        REQUIRE(swarm);
        swarm->scales.bossEncounter = true;
        REQUIRE(swarm->valid());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): fixture owns the native pool
        REQUIRE(const_cast<Enemies&>(scene->play.enemies()).restore(*swarm));
    }
    auto pair = std::make_unique<test::OnlinePair>();
    pair->prepare();
    auto replication = std::make_unique<PlayReplication>();
    REQUIRE(replication->bind(scene->play, pair->host, scene->pickups, scene->fixtures));
    pair->ready();
    REQUIRE(replication->advance(pair->host, {}, scene->projectiles) ==
            PlayReplication::Result::Advanced);
    const auto* selected = replication->latest();
    REQUIRE(selected);
    REQUIRE(selected->motion.tick == 0);
    REQUIRE(selected->opponents);
    REQUIRE(selected->opponentFrame);
    CHECK(selected->combatState.has_value() == (mode == Mode::Complete));
    CHECK(selected->combatFrame.has_value() == selected->combatState.has_value());

    // Independently repeat the previous eager selection policy at the same
    // post-step boundary, using real captures and the public complete encoder.
    const auto textures = scene->device.texturesCreated;
    const auto updates = scene->device.textureUpdates;
    auto reference = std::make_unique<CombatSnapshot>(*selected);
    reference->combatState.reset();
    reference->combatFrame.reset();
    reference->opponents = OpponentResources::capture(
        scene->play.generators(), scene->play.enemies(), scene->play.enemyMissiles());
    REQUIRE(reference->opponents);
    reference->opponents->placements = scene->play.placedEnemies().checkpoint();
    REQUIRE(reference->opponents->placements);
    reference->opponentFrame = scene->play.opponentFrame(2);
    REQUIRE(reference->opponentFrame);
    auto eager = CombatPacket::prepare(*reference);
    REQUIRE(eager);
    reference->combatState = scene->play.combatState();
    reference->combatFrame = scene->play.combatFrame();
    if (mode == Mode::Complete || mode == Mode::RefusedComplete) {
        REQUIRE(reference->combatState);
        REQUIRE(reference->combatFrame);
        auto complete = CombatPacket::prepare(*reference);
        if (mode == Mode::Complete) {
            REQUIRE(complete);
            eager = std::move(complete);
        } else {
            REQUIRE(reference->combatState->valid());
            CHECK_FALSE(complete);
            reference->combatState.reset();
            reference->combatFrame.reset();
        }
    } else {
        CHECK_FALSE(reference->combatState);
        CHECK_FALSE(reference->combatFrame);
    }
    const auto selectedBytes = CombatPacket::encode(*selected);
    REQUIRE(selectedBytes);
    CHECK(static_cast<bool>(*selectedBytes ==
                            std::vector<u8>{eager->bytes().begin(), eager->bytes().end()}));
    CHECK(scene->device.texturesCreated == textures);
    CHECK(scene->device.textureUpdates == updates);
    const auto after = OpponentResources::capture(scene->play.generators(), scene->play.enemies(),
                                                  scene->play.enemyMissiles());
    REQUIRE(after);
    CHECK(after->enemies == reference->opponents->enemies);

    PublicationWire wire;
    const auto expected = CombatReplica::packets(*eager, SnapshotBlock::Compression::Automatic,
                                                 CombatReplica::Recovery::SingleLoss);
    REQUIRE(expected);
    REQUIRE_FALSE(expected->empty());
    // Publication is intentionally paced across flushes. Drain this one batch
    // within its exact burst budget, without advancing time or publishing again.
    const usize flushes =
        (expected->size() + MatchSession::kPublicationBurst - 1) / MatchSession::kPublicationBurst;
    for (usize flush = 0; flush < flushes; ++flush) {
        const usize before = wire.packets.size();
        pair->host.flush(wire);
        REQUIRE(wire.packets.size() > before);
        CHECK(wire.packets.size() - before <= MatchSession::kPublicationBurst);
    }
    CHECK(wire.packets.size() == expected->size());
    CHECK(static_cast<bool>(wire.packets == *expected));
    pair->host.flush(wire);
    // Once drained, another flush does not resend the prepared frame.
    CHECK(static_cast<bool>(wire.packets == *expected));
    for (const auto& packet : wire.packets) {
        const auto admitted = pair->guest.receive(99, packet);
        REQUIRE((admitted == MatchSession::Admission::Accepted ||
                 admitted == MatchSession::Admission::Stale));
    }
    const auto* received = pair->guest.playback().latest();
    REQUIRE(received);
    CHECK(static_cast<bool>(CombatPacket::encode(*received) == selectedBytes));
    CHECK_FALSE(replication->failed());
}

TEST_CASE("host opponent context captures native player order and original tuning without mutation",
          "[netplay][opponent-frame][play-opponent-capture][assets]") {
    Scene scene(false, true, true);
    const auto before = OpponentResources::capture(scene.play.generators(), scene.play.enemies(),
                                                   scene.play.enemyMissiles());
    REQUIRE(before);
    REQUIRE(before->enemies.scales.players == 2);
    const u32 textures = scene.device.texturesCreated;
    const u32 updates = scene.device.textureUpdates;
    scene.config.difficulty.level = "easy";
    const auto frame = scene.play.opponentFrame(2);
    REQUIRE(frame);
    CHECK(frame->difficultyGain == 1.5f);
    CHECK(frame->initialPlayers == 2);
    CHECK(frame->held);
    CHECK(frame->timeStopped);
    const auto native = LevelOpponents::enemyViews(scene.play.participants());
    REQUIRE(native.size() == frame->players.size());
    for (usize index = 0; index < native.size(); ++index) {
        const OpponentFrame::Player expected{static_cast<u8>(native[index].player), native[index]};
        CHECK(frame->players[index] == expected);
    }
    CHECK(frame->players[0].seat == 2);
    CHECK(frame->players[1].seat == 0);
    REQUIRE(frame->players[0].view.collisionHeight);
    REQUIRE(frame->players[0].view.collisionRadius);
    CHECK(frame->players[0].view.level == 23);
    const auto bytes = OpponentFrameCodec::encode(*frame);
    REQUIRE(bytes);
    const auto decoded = OpponentFrameCodec::decode(*bytes);
    REQUIRE(decoded);
    CHECK(*decoded == *frame);
    const auto after = OpponentResources::capture(scene.play.generators(), scene.play.enemies(),
                                                  scene.play.enemyMissiles());
    REQUIRE(after);
    CHECK(*after == *before);
    CHECK(scene.device.texturesCreated == textures);
    CHECK(scene.device.textureUpdates == updates);
    CHECK_FALSE(scene.play.opponentFrame(0));
    CHECK_FALSE(scene.play.opponentFrame(5));
}

TEST_CASE("host stage opponent context carries exact held and unsupported placement observations",
          "[netplay][opponent-frame][play-opponent-capture][assets]") {
    Scene scene(true);
    const auto initial = scene.play.opponentFrame(2);
    REQUIRE(initial);
    CHECK(initial->held);
    REQUIRE(scene.world.level());
    CHECK(initial->missileSpeed == scene.world.level()->tuning.enemyMissileSpeed);
    CHECK_FALSE(initial->fixtures.empty());
    CHECK_FALSE(initial->stops.empty());
    CHECK(initial->pendingPlacements > 0);
    scene.settle();
    const auto running = scene.play.opponentFrame(2);
    REQUIRE(running);
    CHECK_FALSE(running->held);
    CHECK(running->initialPlayers == 2);
    CHECK(running->difficultyGain == 1.5f);
    const auto bytes = OpponentFrameCodec::encode(*running);
    REQUIRE(bytes);
    const auto decoded = OpponentFrameCodec::decode(*bytes);
    REQUIRE(decoded);
    CHECK(*decoded == *running);
}

TEST_CASE("publication pairs complete opponent pools and context at the same native tick",
          "[netplay][opponent-frame][play-opponent-capture][assets]") {
    Scene scene;
    scene.settle();
    test::OnlinePair pair;
    pair.prepare();
    PlayReplication replication;
    REQUIRE(replication.bind(scene.play, pair.host, scene.pickups, scene.fixtures));
    pair.ready();
    for (u64 tick = 0; tick < 7; ++tick) {
        REQUIRE(replication.advance(pair.host, {}, scene.projectiles) ==
                PlayReplication::Result::Advanced);
        const auto* snapshot = replication.latest();
        REQUIRE(snapshot);
        REQUIRE(snapshot->motion.tick == tick);
        CHECK(snapshot->opponents.has_value() == snapshot->opponentFrame.has_value());
        if (tick % PlayReplication::kSnapshotStride == 0) {
            REQUIRE(snapshot->opponents);
            REQUIRE(snapshot->opponentFrame);
            auto pools = OpponentResources::capture(scene.play.generators(), scene.play.enemies(),
                                                    scene.play.enemyMissiles());
            REQUIRE(pools);
            pools->placements = scene.play.placedEnemies().checkpoint();
            REQUIRE(pools->placements);
            CHECK(*snapshot->opponents == *pools);
            const auto frame = scene.play.opponentFrame(2);
            REQUIRE(frame);
            CHECK(*snapshot->opponentFrame == *frame);
            const auto bytes = CombatPacket::encode(*snapshot);
            REQUIRE(bytes);
            CHECK(bytes->size() <= CombatPacket::kMaxBytes);
            const auto decoded = CombatPacket::decode(*bytes);
            REQUIRE(decoded);
            REQUIRE(decoded->opponents);
            REQUIRE(decoded->opponentFrame);
            CHECK(decoded->motion.epoch == snapshot->motion.epoch);
            CHECK(decoded->motion.tick == snapshot->motion.tick);
            CHECK(*decoded->opponents == *pools);
            CHECK(*decoded->opponentFrame == *frame);
        } else {
            CHECK_FALSE(snapshot->opponents);
        }
        pair.pump();
    }
    CHECK_FALSE(replication.failed());
}

TEST_CASE("closed scenes do not supply a usable opponent frame",
          "[netplay][opponent-frame][play-opponent-capture]") {
    PlayScene closed;
    CHECK_FALSE(closed.opponentFrame(1));
}

TEST_CASE("uncaptured enemy shrinking refuses continuation without excluding ordinary enemies",
          "[netplay][opponent-frame][play-opponent-capture][assets]") {
    SECTION("new shrinker is unsafe even before the first pool scale update") {
        Scene scene(false, false, false, true);
        const auto pools = OpponentResources::capture(scene.play.generators(), scene.play.enemies(),
                                                      scene.play.enemyMissiles());
        REQUIRE(pools);
        CHECK(pools->enemies.shrink == 1.0f);
        const auto frame = scene.play.opponentFrame(2);
        REQUIRE(frame);
        CHECK(frame->unsupportedCombatants);
    }
    SECTION("ordinary native enemy bodies are inside the resumable pools") {
        Scene scene;
        const auto before = scene.play.opponentFrame(2);
        REQUIRE(before);
        REQUIRE_FALSE(before->unsupportedCombatants);
        REQUIRE(scene.play.enemies().loadKind(kGruntKind));
        REQUIRE(scene.play.enemies().spawn({.kind = kGruntKind, .placed = true}, {}));
        const auto after = scene.play.opponentFrame(2);
        REQUIRE(after);
        CHECK_FALSE(after->unsupportedCombatants);
    }
}

TEST_CASE("an in-flight player weapon marks the outside damage phase as uncaptured",
          "[netplay][opponent-frame][play-opponent-capture][assets]") {
    Scene scene;
    const auto quiet = scene.play.opponentFrame(2);
    REQUIRE(quiet);
    REQUIRE_FALSE(quiet->unsupportedCombatants);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): fixture owns the live scene
    auto& missiles = const_cast<PlayerMissiles&>(scene.play.missiles());
    MissileLaunch launch;
    launch.spec = &MissileSpec::of(0);
    launch.position = {0, 10, 0};
    launch.damage = 10;
    REQUIRE(missiles.launch(launch));
    const auto active = scene.play.opponentFrame(2);
    REQUIRE(active);
    CHECK(active->unsupportedCombatants);
    CHECK(missiles.count() == 1);
    missiles.clear();
    const auto finished = scene.play.opponentFrame(2);
    REQUIRE(finished);
    CHECK_FALSE(finished->unsupportedCombatants);
}
} // namespace
