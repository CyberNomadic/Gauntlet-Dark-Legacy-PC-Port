#include <cmath>
#include <limits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "AttackCheckpointFixture.h"
#include "game/screens/OrdinaryProjectilePhase.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Phase = OrdinaryProjectilePhase;

struct Fixture : test::AttackCheckpointFixture {
    explicit Fixture() : AttackCheckpointFixture(false) {
        auto& collision = const_cast<WorldCollision&>(world.collision());
        collision.build({{{0, 1, 0}, {{{-20, 0, -20}, {20, 0, -20}, {20, 0, 20}}}, -1, 4},
                         {{0, 1, 0}, {{{-20, 0, -20}, {20, 0, 20}, {-20, 0, 20}}}, -1, 4}});
        REQUIRE(collision.floorAt({0, 0, 5}, 1, 1));
        fixtures.bind({device, world, weapons, effects, audio});
        opponents.open({device, world, weapons, effects, audio, root, 1}, players);
        auto& enemies = opponents.enemies();
        enemies.open(device, root, &world.collision(), 16, {}, 173);
        REQUIRE(enemies.loadKind(kGruntKind));
        REQUIRE(enemies.spawn(
            {.kind = kGruntKind, .tier = 1, .position = {0, 0, 5}, .placed = true}, {}));
        targets.players = players;
    }
    ~Fixture() { fixtures.clear(); }

    MissileLaunch launch(f32 damage = 5, Vec3 position = {0, 3, 0},
                         Vec3 velocity = {0, 0, 30}) const {
        MissileLaunch result;
        result.owner = 3;
        result.position = position;
        result.velocity = velocity;
        result.spec = &MissileSpec::of(0);
        result.damage = damage;
        return result;
    }
    PlayerArsenalCheckpoint::State arsenalState() const {
        const auto result = PlayerArsenalCheckpoint::capture(arsenal, missileResources);
        REQUIRE(result);
        return *result;
    }
    Enemies::Checkpoint enemyState() const {
        const auto result = opponents.enemies().checkpoint();
        REQUIRE(result);
        return *result;
    }
    Phase::Result advance(f32 seconds) {
        return Phase::advance(attacks, players, targets, seconds, {missileResources, {}, {}});
    }
};

TEST_CASE("ordinary projectile phase follows the complete native swarm-hit state",
          "[ordinary-projectile-phase]") {
    const f32 seconds = GENERATE(1.0f / 60, 1.0f / 30);
    const f32 damage = GENERATE(5.0f, 10000.0f);
    Fixture native;
    Fixture replay;
    REQUIRE(native.arsenal.missiles().launch(native.launch(damage)));
    REQUIRE(replay.arsenal.missiles().launch(replay.launch(damage)));
    const auto initialNative = native.enemyState();
    const auto initialReplay = replay.enemyState();
    CHECK(initialNative == initialReplay);
    bool hit = false;
    for (s32 tick = 0; tick < 30; ++tick) {
        native.attacks.updateProjectiles(seconds, native.players, native.targets);
        const auto result = replay.advance(seconds);
        REQUIRE(result.advanced);
        hit = hit || result.impacts != 0;
        CAPTURE(tick, result.impacts);
        CHECK(replay.arsenalState() == native.arsenalState());
        CHECK(replay.enemyState() == native.enemyState());
    }
    REQUIRE(hit);
    const auto after = replay.enemyState();
    REQUIRE_FALSE(after.losses.empty());
    REQUIRE_FALSE(after.feedback.empty());
    // Reward/feedback consumption belongs to the next opponent phase, not this
    // projectile phase. Neither side has awarded experience or shown a lesson.
    CHECK(replay.players[0].actor.save().experience() == 0);
    CHECK(replay.helps.empty());
}

TEST_CASE("ordinary projectile phase rejects a later unsupported impact before all damage",
          "[ordinary-projectile-phase]") {
    Fixture fixture;
    // First missile hits the grunt. Second hits the floor in the same update;
    // even an ownerless static floor impact needs presentation/audio not yet
    // buffered by this phase. Do not damage the grunt before rejecting it.
    auto& collision = const_cast<WorldCollision&>(fixture.world.collision());
    collision.build({{{0, 1, 0}, {{{-20, 0, -20}, {20, 0, -20}, {20, 0, 20}}}, -1, 4},
                     {{0, 1, 0}, {{{-20, 0, -20}, {20, 0, 20}, {-20, 0, 20}}}, -1, 4}});
    REQUIRE(fixture.arsenal.missiles().launch(fixture.launch(10000, {0, 3, 4}, {0, 0, 1})));
    REQUIRE(fixture.arsenal.missiles().launch(fixture.launch(10000, {10, 0.01f, 0}, {0, -10, 0})));
    const auto before = fixture.arsenalState();
    const auto enemies = fixture.enemyState();
    const auto effects = fixture.effects.checkpointPool();
    REQUIRE(effects);
    const auto result = fixture.advance(1.0f / 60);
    CHECK_FALSE(result.advanced);
    CHECK(fixture.enemyState() == enemies);
    CHECK(fixture.effects.checkpointPool() == effects);
    // The caller's full transaction also rewinds the consumed impacts/flights.
    // No queue has been erased and reported as a successful simulated tick.
    REQUIRE(fixture.arsenal.missiles().restore(before.missiles, fixture.missileResources));
    CHECK(fixture.arsenalState() == before);
    CHECK(fixture.helps.empty());
}

TEST_CASE("ordinary projectile phase refuses malformed or unsupported flight before mutation",
          "[ordinary-projectile-phase]") {
    const s32 variant = GENERATE(0, 1, 2, 3, 4, 5, 6);
    Fixture fixture;
    auto shot = fixture.launch();
    if (variant == 0) {
        shot.velocity = Vec3{1e6f, 0, 0};
    } else if (variant == 1) {
        shot.flags = powerup::kSuperShot;
    } else if (variant == 2) {
        shot.multiplayer = MultiplayerMode::Hurt;
    } else if (variant == 3) {
        shot.flags = 1; // elemental damage is not a plain weapon hit
    } else if (variant == 4) {
        shot.owner = 2; // valid input slot, but not a member of this transaction
    } else if (variant == 5) {
        shot.release = MissileRelease{0, 1, false, 0, 1U << 16};
    }
    REQUIRE(fixture.arsenal.missiles().launch(shot));
    const auto before = fixture.arsenalState();
    const auto enemies = fixture.enemyState();
    const f32 seconds = variant == 6 ? std::numeric_limits<f32>::infinity() : 1.0f / 60;
    CHECK_FALSE(fixture.advance(seconds).advanced);
    CHECK(fixture.arsenalState() == before);
    CHECK(fixture.enemyState() == enemies);
}

TEST_CASE("ordinary projectile work limits refuse aggregate work without changing native physics",
          "[ordinary-projectile-phase]") {
    Fixture fixture;
    auto shot = fixture.launch(0, {0, 100, 0}, {0, 0, 1});
    REQUIRE(fixture.arsenal.missiles().launch(shot));
    auto state = fixture.arsenalState();
    const f32 seconds = 1.0f / 30;
    // The trusted Warrior radius makes this exactly 256 native substeps.
    const f32 maximum = std::nextafter(static_cast<f32>(Phase::kMaxFlightSteps) *
                                           MissileSpec::of(0).radius * 0.5f / seconds,
                                       0.0f);
    state.missiles.missiles[0].velocity = {maximum, 0, 0};
    REQUIRE(state.valid());
    CHECK(Phase::supported(state, fixture.missileResources, seconds));
    state.missiles.missiles[0].velocity.x *= 2;
    REQUIRE(state.valid());
    CHECK_FALSE(Phase::supported(state, fixture.missileResources, seconds));
    state.missiles.missiles[0].velocity.x = maximum;
    const auto first = state.missiles.missiles[0];
    for (usize index = 1; index < Phase::kMaxTotalSteps / Phase::kMaxFlightSteps; ++index) {
        auto flight = first;
        flight.instance = static_cast<u64>(index + 1);
        state.missiles.missiles.push_back(flight);
    }
    state.missiles.nextInstance = static_cast<u64>(state.missiles.missiles.size() + 1);
    REQUIRE(state.valid());
    CHECK(Phase::supported(state, fixture.missileResources, seconds));
    auto extra = first;
    extra.instance = state.missiles.nextInstance++;
    state.missiles.missiles.push_back(extra);
    REQUIRE(state.valid());
    CHECK_FALSE(Phase::supported(state, fixture.missileResources, seconds));
}

TEST_CASE(
    "ordinary projectile admission preserves pending native impacts and rejects foreign specs",
    "[ordinary-projectile-phase]") {
    Fixture fixture;
    REQUIRE(fixture.arsenal.missiles().launch(fixture.launch()));
    auto state = fixture.arsenalState();
    PlayerMissiles::Checkpoint::Impact impact;
    impact.owner = 3;
    impact.target = 1000;
    impact.effectResource = 1;
    state.missiles.impacts.push_back(impact);
    REQUIRE(state.valid());
    CHECK(Phase::supported(state, fixture.missileResources, 1.0f / 60));
    auto resources = fixture.missileResources;
    resources[0].spec = &MissileSpec::potion();
    CHECK_FALSE(Phase::supported(state, resources, 1.0f / 60));
    CHECK_FALSE(Phase::supported(state, {}, 1.0f / 60));
    state.missiles.impacts.back().target = 2000; // generator outcome not owned by this phase yet
    CHECK_FALSE(Phase::supported(state, fixture.missileResources, 1.0f / 60));
}

} // namespace
