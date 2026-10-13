#include <algorithm>
#include <array>
#include <cmath>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/netplay/OnlinePair.h"
#include "game/screens/FloorRiding.h"
#include "game/screens/OnlineOpponentReplay.h"
#include "game/screens/OpponentPrediction.h"
#include "game/screens/OpponentResources.h"
#include "game/screens/PlayReplication.h"
#include "game/screens/ReplicaStage.h"
#include "game/screens/SessionInputs.h"
#include "game/world/CameraMovementLimit.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE(
    "native G1 bootstrap respects complete opponent roster admission without touching replica art",
    "[rollback][online-opponent-replay][assets]") {
    const auto difficulty = GENERATE("easy", "normal", "hard");
    const auto players = GENERATE(1, 4);
    CAPTURE(difficulty, players);
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    GameConfig config;
    config.difficulty.level = difficulty;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G1");
    REQUIRE(level);
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    GameContext context;
    context.config = &config;
    context.levels = &catalog;
    context.unpackedRoot = root;
    std::vector<PartyMember> party;
    PartyBootstrap::Party profiles;
    MatchOwners owners{};
    for (s32 seat = 0; seat < players; ++seat) {
        CharacterSave save;
        save.name = "TEST";
        save.character = seat;
        save.color = seat;
        save.progress().health = 2000;
        party.push_back({seat, save});
        profiles[static_cast<usize>(seat)] = CharacterProfile::capture(save);
        REQUIRE(profiles[static_cast<usize>(seat)]);
        owners[static_cast<usize>(seat)] = 1;
    }
    PlayScene scene;
    PlayOptions options;
    options.welcome = false;
    REQUIRE(scene.open(device, context, world, party, options));
    ProjectileResources projectiles;
    PickupResources pickups;
    FixtureResources fixtures;
    REQUIRE(scene.bindReplicationResources(projectiles, pickups, fixtures));
    MatchSession match;
    REQUIRE(match.open(1, owners, {}));
    REQUIRE(match.prepare(1, MatchTransition::Start));
    PlayReplication driver;
    REQUIRE(driver.bind(scene, match, pickups, fixtures));
    ReplicaStage remote;
    REQUIRE(remote.open(device, root, *level, match.context(), profiles, nullptr,
                        config.difficulty.gain()));
    auto* privateOpponents = remote.opponentPrediction();
    REQUIRE(privateOpponents != nullptr);
    REQUIRE(privateOpponents->loaded());
    REQUIRE(match.loaded());
    REQUIRE(driver.advance(match, {}, projectiles) == PlayReplication::Result::Advanced);
    REQUIRE(driver.latest());
    const auto& authority = *driver.latest();
    REQUIRE(authority.opponents);
    REQUIRE(authority.opponentFrame);
    CHECK(authority.opponentFrame->initialPlayers == static_cast<u32>(players));
    CHECK(authority.opponentFrame->difficultyGain == config.difficulty.gain());
    const auto uploads = device.texturesCreated;
    const auto updates = device.textureUpdates;
    const auto native = *authority.opponents;
    REQUIRE(native.placements);
    REQUIRE_FALSE(native.placements->roster.empty());
    const auto before = privateOpponents->checkpoint();
    const auto unsupported = std::ranges::count_if(native.placements->roster, [](const auto& row) {
        return row.kind < 0 || row.kind >= kSwarmKindCount || row.kind == kGarmBroodKind ||
               row.carried.has_value();
    });
    std::vector<s32> nativeKinds;
    for (const auto& row : native.placements->roster) {
        nativeKinds.push_back(row.kind);
    }
    CAPTURE(native.placements->roster.size(), native.placements->pending.size(), unsupported,
            nativeKinds);
    const bool admitted = privateOpponents->begin(authority.motion.epoch, authority.motion.tick,
                                                  native, world.collision());
    REQUIRE(admitted == (unsupported == 0));
    CHECK(privateOpponents->active() == admitted);
    // The complete native roster is the trust boundary. Pending Generals or
    // other separately owned creatures cannot be omitted to manufacture a
    // successful ordinary-swarm bootstrap; rendering still uses authority.
    const auto expected = admitted ? std::optional{native} : before;
    CHECK(privateOpponents->checkpoint() == expected);
    // Presentation still uses its own trusted catalog and exact authority. The
    // private restore neither runs a frame nor consumes one-shot native outputs.
    REQUIRE(remote.show(authority));
    CHECK(driver.latest()->opponents == native);
    CHECK(privateOpponents->checkpoint() == expected);
    CHECK(device.texturesCreated == uploads);
    CHECK(device.textureUpdates == updates);
    privateOpponents->resetReplay();
    CHECK_FALSE(privateOpponents->active());
    CHECK(privateOpponents->checkpoint() == expected);
}

/** Native scene, character and enemy code on a deliberately synthetic static
 * floor. No hidden placements/triggers or platform clocks are removed from a
 * retail map to manufacture an eligible public gameplay checkpoint. */
struct CoupledScene {
    std::filesystem::path assets =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    std::filesystem::path directory = test::sampleLevel("coupled-opponent-replay");
    test::FakeRenderDevice device;
    GameConfig config;
    LevelCatalog catalog;
    LevelWorld world;
    PlayScene play;
    test::OnlinePair pair;
    ProjectileResources projectiles;
    PickupResources pickups;
    FixtureResources fixtures;
    PlayReplication driver;
    OpponentPrediction opponents;
    OnlineOpponentReplay coordinator;
    LocalMovementPrediction movement;
    CombatSnapshot authority;
    s32 enemy = -1;

    CoupledScene() {
        writeTextFile(directory / "world.json", R"({"objects":[
            {"name":"FLOOR","position":[0,0,0],"flags":4}],"animations":[]})");
        REQUIRE(catalog.load(assets));
        auto level = catalog.byName("G1");
        REQUIRE(level);
        level->directory = directory.string();
        REQUIRE(world.load(device, assets, *level));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): this fixture owns the test world
        auto& collision = const_cast<WorldCollision&>(world.collision());
        collision.build(
            {{{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{100, 0, 100}, Vec3{100, 0, -100}}, 0, 4},
             {{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{-100, 0, 100}, Vec3{100, 0, 100}}, 0, 4}});
        GameContext context;
        context.config = &config;
        context.unpackedRoot = assets;
        PlayOptions options;
        options.welcome = false;
        options.position = Vec3{0};
        REQUIRE(pair.hostSession.party());
        std::vector<PartyMember> party;
        for (usize seat = 0; seat < pair.hostSession.party()->size(); ++seat) {
            if (const auto& profile = (*pair.hostSession.party())[seat]) {
                party.push_back({static_cast<s32>(seat), profile->gameplayCopy()});
            }
        }
        REQUIRE(play.open(device, context, world, party, options));
        // Settle arrival and the shared follow-camera with a compact party. A
        // last-moment teleport from a widely separated party would compare a
        // changing native camera boundary against the captured replay boundary.
        place(0, {4, 0, 0});
        place(1, {0, 0, 0});
        for (s32 tick = 0; tick < 400; ++tick) {
            REQUIRE(play.update(1.0 / 60, {}) == PlayOutcome::Running);
        }
        REQUIRE(play.enemies().loadKind(kGruntKind));
        const auto spawned = play.enemies().spawn(
            {.kind = kGruntKind, .algorithm = kChaseWay, .position = {0, 0, 12}, .placed = true},
            {});
        REQUIRE(spawned);
        enemy = *spawned;
        for (s32 tick = 0; tick < Enemies::kPlacedStun; ++tick) {
            REQUIRE(play.update(1.0 / 60, {}) == PlayOutcome::Running);
        }
        REQUIRE(play.movementPredictable());
        REQUIRE_FALSE(play.animator(1)->entering());
        REQUIRE(play.bindReplicationResources(projectiles, pickups, fixtures));
        pair.prepare();
        REQUIRE(driver.bind(play, pair.host, pickups, fixtures));
        pair.ready();
        REQUIRE(driver.advance(pair.host, {}, projectiles) == PlayReplication::Result::Advanced);
        REQUIRE(driver.latest());
        authority = *driver.latest();
        REQUIRE(authority.opponents);
        REQUIRE(authority.opponentFrame);
        REQUIRE(authority.opponentFrame->pendingPlacements == 0);
        REQUIRE_FALSE(authority.opponentFrame->unsupportedCombatants);
        REQUIRE_FALSE(authority.opponentFrame->held);
        REQUIRE(authority.players[1]->movement.enabled);
        REQUIRE(authority.players[1]->locomotion);
        REQUIRE(opponents.open(device, assets, world, play.enemies(), 2, config.difficulty.gain()));
        coordinator.begin(pair.host.context(), 2, opponents, world.scene(), world.layout(),
                          world.collision());
        movement.begin(pair.host.context(), 2, world.collision(), &world.scene(), &world.layout());
        REQUIRE(movement.setPlayer(1, *player(1).figure->actionTree()));
        movement.update(&authority, {}, coordinator.callbacks(&authority));
    }

    PlayerRuntime& player(usize seat) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): fixture-owned native runtime
        return const_cast<PlayerRuntime&>(play.participants()[seat]);
    }
    void place(usize seat, const Vec3& position) {
        auto& runtime = player(seat);
        runtime.actor.place(position);
        runtime.actor.turnTo(0);
        runtime.floor = {};
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): fixture-owned native party
        auto party = std::span{const_cast<PlayerRuntime*>(play.participants().data()),
                               play.participants().size()};
        FloorRiding::land(party, seat, position, world.collision());
    }
    InputCommand command(u64 tick) const {
        InputCommand input;
        input.epoch = authority.motion.epoch;
        input.tick = tick;
        input.seat = 1;
        input.grant = authority.motion.players[1]->grant;
        const auto yaw = -authority.players[1]->movement.cameraYaw;
        input.direction = {std::sin(yaw), std::cos(yaw)};
        input.magnitude = 1;
        return input;
    }
};

TEST_CASE("tick-major callbacks replay actual native players and enemies once per common tick",
          "[rollback][online-opponent-replay][assets]") {
    CoupledScene fixture;
    const auto authority = fixture.authority;
    const auto uploads = fixture.device.texturesCreated;
    const auto initial = fixture.play.enemies().positionOf(fixture.enemy);
    const auto callbacks = fixture.coordinator.callbacks(&authority);
    REQUIRE(callbacks.begin);
    REQUIRE(callbacks.beforeTick);
    REQUIRE(callbacks.afterTick);
    for (u64 offset = 1; offset <= 12; ++offset) {
        CAPTURE(offset);
        const auto input = fixture.command(authority.motion.tick + offset);
        const Vec3 before = fixture.player(1).actor.position();
        PlayScene::Inputs nativeInputs;
        nativeInputs[1] = SessionInputs::playInput(input);
        REQUIRE(fixture.play.update(1.0 / 60, nativeInputs) == PlayOutcome::Running);
        const auto& envelope = authority.players[1]->movement;
        const Vec3 after = fixture.player(1).actor.position();
        const Vec3 capturedLimit = CameraMovementLimit::constrain(
            before, after, envelope.attention, authority.motion.camera,
            {authority.motion.horizontalFov, authority.motion.aspect},
            Vec3{0, envelope.followHeight, 0});
        REQUIRE(glm::distance(capturedLimit, after) < 0.0001f);
        fixture.movement.update(&authority, std::span{&input, 1}, callbacks);
        REQUIRE(fixture.movement.replayed(1));
        REQUIRE(fixture.coordinator.active());
        REQUIRE(fixture.opponents.enemies() != nullptr);
        const auto* privateEnemies = fixture.opponents.enemies();
        CHECK(privateEnemies->positionOf(fixture.enemy) ==
              fixture.play.enemies().positionOf(fixture.enemy));
        CHECK(privateEnemies->animatorOf(fixture.enemy)->checkpoint() ==
              fixture.play.enemies().animatorOf(fixture.enemy)->checkpoint());
        auto shown = authority;
        fixture.movement.present(shown, 1);
        CHECK(glm::distance(shown.motion.players[1]->position, fixture.player(1).actor.position()) <
              0.0001f);
        CHECK(shown.players[1]->rollback == fixture.player(1).figure->animator().checkpoint());
        CHECK(fixture.opponents.tick() == input.tick);
    }
    CHECK(fixture.play.enemies().positionOf(fixture.enemy) != initial);
    CHECK(fixture.device.texturesCreated == uploads);
    CHECK(fixture.authority.opponents == authority.opponents);
    fixture.coordinator.clear();
    CHECK_FALSE(fixture.coordinator.active());
}

TEST_CASE("coupled continuation refuses outside gameplay and withdraws both replay outputs",
          "[rollback][online-opponent-replay][assets]") {
    CoupledScene fixture;
    const auto input = fixture.command(fixture.authority.motion.tick + 1);
    fixture.movement.update(&fixture.authority, std::span{&input, 1},
                            fixture.coordinator.callbacks(&fixture.authority));
    REQUIRE(fixture.coordinator.active());
    REQUIRE(fixture.opponents.active());
    auto unsupported = fixture.authority;
    SECTION("pending native placement") {
        ++unsupported.opponentFrame->pendingPlacements;
    }
    SECTION("outside combatant") {
        unsupported.opponentFrame->unsupportedCombatants = true;
    }
    SECTION("held opponent phase") {
        unsupported.opponentFrame->held = true;
    }
    SECTION("time stop") {
        unsupported.opponentFrame->timeStopped = true;
    }
    const auto callbacks = fixture.coordinator.callbacks(&unsupported);
    CHECK_FALSE(callbacks.begin);
    CHECK_FALSE(fixture.coordinator.active());
    CHECK_FALSE(fixture.opponents.active());
    CHECK(fixture.opponents.checkpoint() == fixture.authority.opponents);

    // An admitted pass that later selects an unsupported action must atomically
    // withdraw its player result too, not leave a mixed-age private world.
    fixture.movement.begin(fixture.pair.host.context(), 2, fixture.world.collision(),
                           &fixture.world.scene(), &fixture.world.layout());
    REQUIRE(fixture.movement.setPlayer(1, *fixture.player(1).figure->actionTree()));
    auto rejected = input;
    rejected.heldButtons = static_cast<u32>(CommandHeld::Combo);
    fixture.movement.update(&fixture.authority, std::span{&rejected, 1},
                            fixture.coordinator.callbacks(&fixture.authority));
    CHECK_FALSE(fixture.movement.replayed(1));
    CHECK_FALSE(fixture.coordinator.active());
    CHECK_FALSE(fixture.opponents.active());
    CHECK(fixture.opponents.checkpoint() == fixture.authority.opponents);
}

} // namespace
