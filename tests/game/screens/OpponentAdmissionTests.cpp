#include <array>
#include <limits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/OpponentSimulation.h"

namespace {
using namespace gdl;
using namespace gdl::game;

OpponentState ordinaryState(s32 way = kChaseWay) {
    OpponentState state;
    auto& body = state.enemies.enemies.front();
    body.instance = 1;
    body.kind = kGruntKind;
    body.state = Enemies::State::Active;
    body.algorithm = way;
    body.animator.emplace();
    body.animator->player.playing = true;
    body.animator->player.generation = 1;
    body.animator->generationBefore = 1;
    state.enemies.nextInstance = 2;
    REQUIRE(state.valid());
    return state;
}

void addGenerator(OpponentState& state, s32 kind, s32 way) {
    Generators::Slot slot;
    slot.kind = kind;
    slot.algorithm = way;
    state.generators.roster.push_back(slot);
    state.generators.generators.push_back(slot);
    state.generators.bodies.push_back({kind, {}});
    REQUIRE(state.valid());
}

TEST_CASE("opponent admission is a pure check of the complete supplied seed",
          "[rollback][opponent-admission]") {
    auto state = ordinaryState(GENERATE(kChaseWay, kThrowWay, kBombWay, kCastWay));
    addGenerator(state, kGruntKind, kChaseWay);
    state.enemies.generatorEvents.push_back({0, EnemyGeneratorEvent::Kind::Born});
    state.enemies.cues.push_back({.kind = EnemyCue::Kind::Arrow, .enemyKind = kGruntKind});
    std::array<EnemyView, 2> players;
    players[0].player = 3;
    players[1].player = 0;
    players[0].hidden = true;
    players[0].damageable = false;
    players[1].blockableAttack = true;
    OpponentSimulation::Frame frame;
    frame.players = players;
    const auto before = state;
    REQUIRE(state.valid());
    CHECK(OpponentSimulation::supported(state, frame));
    CHECK(OpponentSimulation::supported(state, frame));
    CHECK(state == before);
    // Pure admission has no bound-resource requirement. Runtime preparation
    // resolves the supplied trees before any actual native continuation.
    CHECK(OpponentSimulation::supported(OpponentState{}, {}));
}

TEST_CASE("opponent admission refuses uncaptured opponent types and algorithms",
          "[rollback][opponent-admission]") {
    auto state = ordinaryState();
    SECTION("Death") {
        state.enemies.enemies.front().kind = kDeathKind;
    }
    SECTION("IT") {
        state.enemies.enemies.front().kind = kItKind;
    }
    SECTION("Garm brood") {
        state.enemies.enemies.front().kind = kGarmBroodKind;
    }
    SECTION("suicide") {
        state.enemies.enemies.front().algorithm = kSuicideWay;
    }
    SECTION("unsupported native mind") {
        state.enemies.enemies.front().algorithm = kLungeWay;
    }
    SECTION("external combatant bodies") {
        state.enemies.combatants.push_back({});
    }
    SECTION("future Death birth") {
        addGenerator(state, kDeathKind, kChaseWay);
    }
    SECTION("future unsupported mind") {
        addGenerator(state, kGruntKind, kLungeWay);
    }
    const auto before = state;
    REQUIRE(state.valid());
    CHECK_FALSE(OpponentSimulation::supported(state, {}));
    CHECK(state == before);
}

TEST_CASE("opponent admission handles inactive slots and native patrol reset ordering",
          "[rollback][opponent-admission]") {
    auto state = ordinaryState();
    state.enemies.enemies.front().state = Enemies::State::Inactive;
    state.enemies.enemies.front().kind = kDeathKind;
    CHECK(OpponentSimulation::supported(state, {}));
    addGenerator(state, kGruntKind, kLungeWay);
    CHECK_FALSE(OpponentSimulation::supported(state, {}));
    state.enemies.generatorEvents.push_back(
        {0, GENERATE(EnemyGeneratorEvent::Kind::PatrolHit,
                     EnemyGeneratorEvent::Kind::PatrolDetached)});
    REQUIRE(state.valid());
    // The next native generator update consumes this event before choosing
    // its algorithm, replacing the unsupported way with ordinary seeking.
    CHECK(OpponentSimulation::supported(state, {}));
    state.enemies.generatorEvents.clear();
    state.generators.generators.front().state = 0;
    CHECK(OpponentSimulation::supported(state, {}));
}

TEST_CASE("opponent admission preserves dormant special placements until native selection",
          "[rollback][opponent-admission][opponent-placements]") {
    auto state = ordinaryState();
    PlacedEnemies::Placement row;
    row.kind = kGruntKind;
    row.spawn.kind = kGruntKind;
    row.spawn.algorithm = kChaseWay;
    row.spawn.position = {0, 0, 100};
    row.spawn.placed = true;
    row.spawn.priority = EnemySpawn::Priority::Visible;
    row.viewRadius = 1;
    SECTION("Death") {
        row.kind = kDeathKind;
        row.spawn.kind = kDeathKind;
    }
    SECTION("IT") {
        row.kind = kItKind;
        row.spawn.kind = kItKind;
    }
    SECTION("General retains native unused spawn kind") {
        row.kind = kGeneralEnemyKind;
    }
    SECTION("carried pickup") {
        row.carried = 37;
    }
    SECTION("unsupported future algorithm") {
        row.spawn.algorithm = kLungeWay;
    }
    SECTION("Garm brood") {
        row.kind = kGarmBroodKind;
        row.spawn.kind = kGarmBroodKind;
    }
    state.placements = PlacedEnemies::Checkpoint{{row}, {0}, ViewVolume{}, {}};
    REQUIRE(state.valid());
    const auto dormant = state;
    REQUIRE(state.placements->activations());
    CHECK(state.placements->activations()->empty());
    CHECK(OpponentSimulation::supported(state, {}));
    CHECK(state == dormant);

    // The same immutable row becomes imminent when attention approaches it.
    state.placements->attention = row.spawn.position;
    REQUIRE(state.placements->activations());
    REQUIRE(state.placements->activations()->size() == 1);
    const auto selected = state;
    CHECK_FALSE(OpponentSimulation::supported(state, {}));
    CHECK(state == selected);
    state.placements->view.reset();
    state.placements->attention = {};
    CHECK_FALSE(OpponentSimulation::supported(state, {}));

    // Consumed rows remain in the trusted roster; they must not permanently
    // veto continuation merely because an unsupported family once stood there.
    state.placements->pending.clear();
    REQUIRE(state.valid());
    CHECK(OpponentSimulation::supported(state, {}));
    CHECK(state.placements->roster == dormant.placements->roster);
}

TEST_CASE("opponent admission does not discard pending gameplay outputs",
          "[rollback][opponent-admission]") {
    auto state = ordinaryState();
    SECTION("blow") {
        state.enemies.blows.push_back({});
    }
    SECTION("reward") {
        state.enemies.losses.push_back({});
    }
    SECTION("hit feedback") {
        state.enemies.feedback.push_back({});
    }
    SECTION("enemy explosion") {
        state.enemies.bursts.push_back({});
    }
    SECTION("Death transfer") {
        state.enemies.deathEvents.push_back({});
    }
    SECTION("brood shot") {
        state.enemies.deathShots.push_back({});
    }
    SECTION("IT touch") {
        state.enemies.tagged.push_back(0);
    }
    SECTION("missile hit") {
        state.missiles.hits.push_back({});
    }
    SECTION("rock damage") {
        state.missiles.rockHits.push_back({});
    }
    SECTION("pickup damage") {
        state.missiles.pickupBlasts.push_back({});
    }
    SECTION("active blast") {
        EnemyMissiles missiles;
        missiles.burst({0, 0, 0}, 10, 5, 0, 1, -1);
        const auto value = missiles.checkpoint();
        REQUIRE(value);
        state.missiles = *value;
    }
    REQUIRE(state.valid());
    const auto before = state;
    CHECK_FALSE(OpponentSimulation::supported(state, {}));
    CHECK(state == before);
}

TEST_CASE("complete combat admission retains ordinary rewards and feedback without draining them",
          "[rollback][opponent-admission][ordinary-melee]") {
    auto state = ordinaryState();
    state.enemies.losses.push_back({});
    state.enemies.feedback.push_back({});
    REQUIRE(state.valid());
    const auto before = state;
    CHECK_FALSE(OpponentSimulation::supported(state, {}));
    CHECK(OpponentSimulation::combatSupported(state, {}));
    CHECK(state == before);
}

TEST_CASE("opponent admission rejects incomplete external player and combatant context",
          "[rollback][opponent-admission]") {
    const auto state = ordinaryState();
    std::array<EnemyView, 1> players;
    players[0].player = 0;
    OpponentSimulation::Frame frame;
    frame.players = players;
    const std::array<MissileTarget, 1> targets{};
    SECTION("captured") {
        players[0].captured = true;
    }
    SECTION("invisible") {
        players[0].invisible = true;
    }
    SECTION("melee ward") {
        players[0].meleeWard = EnemyMeleeWard::HealthVamp;
    }
    SECTION("anti death") {
        players[0].antiDeath = true;
    }
    SECTION("reflection") {
        players[0].reflects = true;
    }
    SECTION("IT") {
        players[0].it = true;
    }
    SECTION("decoy") {
        players[0].decoy = Vec3{1, 2, 3};
    }
    SECTION("held Death") {
        players[0].heldDeath = 0;
    }
    SECTION("critter") {
        frame.critters = targets;
    }
    SECTION("boss") {
        frame.bosses = targets;
    }
    CHECK_FALSE(OpponentSimulation::supported(state, frame));
}

TEST_CASE("opponent admission validates cross references before indexing native queues",
          "[rollback][opponent-admission]") {
    auto state = ordinaryState();
    SECTION("pending brood event past an empty generator roster") {
        state.enemies.generatorEvents.push_back({0, EnemyGeneratorEvent::Kind::PatrolHit});
    }
    SECTION("negative brood event") {
        state.enemies.generatorEvents.push_back({-1, EnemyGeneratorEvent::Kind::PatrolDetached});
    }
    SECTION("orphaned generator reference") {
        state.enemies.enemies.front().generator = 0;
    }
    SECTION("bad random continuation") {
        state.enemies.random.cursor = 625;
    }
    REQUIRE_FALSE(state.valid());
    CHECK_FALSE(OpponentSimulation::supported(state, {}));
}

TEST_CASE("opponent admission shares the native private frame validation",
          "[rollback][opponent-admission]") {
    const auto state = ordinaryState();
    std::array<EnemyView, 2> players;
    players[0].player = 0;
    players[1].player = 1;
    OpponentSimulation::Frame frame;
    frame.players = players;
    SECTION("invalid tick rate") {
        frame.ticks = 3;
    }
    SECTION("mismatched duration") {
        frame.seconds = 1.0f / 30;
    }
    SECTION("nonfinite duration") {
        frame.seconds = std::numeric_limits<f32>::quiet_NaN();
    }
    SECTION("invalid missile speed") {
        frame.missileSpeed = -1;
    }
    SECTION("duplicated players") {
        players[1].player = 0;
    }
    SECTION("invalid player position") {
        players[0].position.z = std::numeric_limits<f32>::infinity();
    }
    SECTION("invalid contact height") {
        players[0].collisionHeight = -1.0f;
    }
    CHECK_FALSE(OpponentSimulation::supported(state, frame));
}

TEST_CASE("opponent admission never bypasses the live flight work budget",
          "[rollback][opponent-admission][assets]") {
    const auto root =
        test::assetOrSkip("MONSTERS/GRU/ANIM.PS2").parent_path().parent_path().parent_path();
    const auto directory = test::scratchDirectory("opponent-admission");
    writeTextFile(directory / "world.json", R"({"objects":[{"name":"FLOOR","position":[0,0,0]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(directory));
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, root, nullptr, Enemies::kMost, {}, 17);
    Generators generators;
    REQUIRE(generators.bind(device, layout, enemies, nullptr, {}, 1));
    EnemyMissiles missiles{23};
    missiles.launch(EnemyMissileKind::arrow(), {80, 10, 80}, {80, 10, 100}, 1, nullptr, -1);
    OpponentSimulation simulation{generators, enemies, missiles};
    auto state = simulation.checkpoint();
    REQUIRE(state);
    REQUIRE(state->missiles.missiles.size() == 1);
    state->missiles.missiles.front().velocity = Vec3{1'000'000};
    state->missiles.missiles.front().kind.radius = 0;
    REQUIRE(state->valid());
    REQUIRE(simulation.restore(*state));
    REQUIRE(OpponentSimulation::supported(*state, {}));
    CHECK(simulation.speculate({}) == OpponentSimulation::Result::Unsupported);
    CHECK(simulation.checkpoint() == state);
}

} // namespace
