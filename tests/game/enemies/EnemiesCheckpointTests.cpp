#include <algorithm>
#include <array>
#include <limits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/Enemies.h"

namespace {
using namespace gdl;
using namespace gdl::game;
constexpr f32 kStep = 1.0f / 60;
constexpr std::array kKinds{kGruntKind, kRatKind, 13, 24, kDeathKind, kItKind, kGarmBroodKind};

struct Pool {
    test::FakeRenderDevice device;
    Enemies enemies;
    std::array<EnemyView, 2> players;
    std::array<Obstacle, 1> obstacles;

    Pool() {
        const auto root =
            test::assetOrSkip("MONSTERS/GRU/ANIM.PS2").parent_path().parent_path().parent_path();
        enemies.open(device, root, nullptr, Enemies::kMost, {}, 125);
        for (const s32 kind : kKinds) {
            REQUIRE(enemies.loadKind(kind));
        }
        enemies.setContactGridBounds({-100, -10, -100}, {100, 20, 100});
        LookoutRoute route;
        route.points = {{-12, 0, 8}, {8, 0, 12}, {12, 0, -8}};
        route.next = {1, 2, 0};
        enemies.setLookouts(route);
        players[0].player = 0;
        players[0].position = {-6, 0, 0};
        players[1].player = 1;
        players[1].position = {6, 0, 0};
        obstacles[0].centre = {0, 0, 14};
        obstacles[0].halfAcross = 3;
        obstacles[0].halfAlong = 2;
        obstacles[0].height = 6;
        for (s32 slot = 0; slot < Enemies::kMost; ++slot) {
            EnemySpawn spawn;
            spawn.kind = kKinds[static_cast<usize>(slot) % kKinds.size()];
            spawn.tier = slot % 3 + 1;
            spawn.algorithm = slot % 2 == 0 ? kWanderWay : kChaseWay;
            spawn.placed = true;
            spawn.asleep = slot == 3;
            spawn.generator = slot % 4;
            spawn.position = {static_cast<f32>(slot % 5 - 2) * 9, 0,
                              static_cast<f32>(slot / 5 - 2) * 9};
            REQUIRE(enemies.spawn(spawn, {}) == slot);
        }
    }

    void step(s32 tick) {
        players[0].position.x = -6 + static_cast<f32>(tick % 12) * 0.125f;
        players[1].position.z = static_cast<f32>(tick % 9) * -0.125f;
        if (tick == 3) {
            enemies.wake(3);
        }
        if (tick == 5 || tick == 24) {
            EnemyHit hit;
            hit.damage = 2;
            hit.player = 0;
            hit.flags = EnemyHit::kKnockBack;
            hit.direction = {1, 0, 0};
            enemies.hurt(0, hit);
        }
        if (tick == 9) {
            EnemyHit hit;
            hit.damage = 1000;
            hit.player = 1;
            hit.flags = EnemyHit::kMagic;
            enemies.hurt(4, hit); // Death's separate transfer/lifecycle queue
        }
        if (tick == 13 || tick == 39) {
            EnemySpawn spawn;
            spawn.kind = kRatKind;
            spawn.generator = 2;
            spawn.placed = true;
            spawn.position = {35, 0, 32};
            spawn.priority = EnemySpawn::Priority::Visible;
            REQUIRE(enemies.spawn(spawn, players, obstacles));
        }
        enemies.setShrink(tick >= 18 && tick < 28 ? 0.5f : 1);
        enemies.update(1, kStep, players, obstacles, nullptr, 1, tick >= 30 && tick < 34);
    }
};

TEST_CASE("a whole native enemy pool rewinds RNG slot order clocks and pending gameplay events",
          "[rollback][enemies][enemies-checkpoint][assets]") {
    Pool pool;
    REQUIRE(pool.enemies.count() == Enemies::kMost);
    pool.enemies.update(1, kStep, pool.players, pool.obstacles);
    const auto initial = pool.enemies.checkpoint();
    REQUIRE(initial);
    CHECK(initial->pendingTicks == 1);
    CHECK(initial->enemies.size() == Enemies::kMost);
    CHECK(initial->generatorEvents.size() == Enemies::kMost);
    const auto uploads = pool.device.texturesCreated;
    std::vector<Enemies::Checkpoint> forward;
    for (s32 tick = 0; tick < 72; ++tick) {
        pool.step(tick);
        const auto current = pool.enemies.checkpoint();
        REQUIRE(current);
        forward.push_back(*current);
    }
    CHECK_FALSE(forward.back().random == initial->random);
    CHECK_FALSE(forward.back().feedback.empty());
    CHECK_FALSE(forward.back().deathEvents.empty());
    CHECK(forward.back().nextInstance > initial->nextInstance);
    REQUIRE(pool.enemies.restore(*initial));
    CHECK(pool.enemies.checkpoint() == initial);
    for (s32 tick = 0; tick < 72; ++tick) {
        CAPTURE(tick);
        pool.step(tick);
        const auto replay = pool.enemies.checkpoint();
        REQUIRE(replay);
        CHECK(*replay == forward[static_cast<usize>(tick)]);
    }
    // Restoring and stepping never load a mesh/texture or dispatch any live-world
    // callback. The queues themselves, not just actor observations, were compared.
    CHECK(pool.device.texturesCreated == uploads);
}

TEST_CASE("enemy checkpoint restores every pending output without dispatching or duplicating it",
          "[rollback][enemies][enemies-checkpoint][assets]") {
    Pool pool;
    for (s32 tick = 0; tick < 16; ++tick) {
        pool.step(tick);
    }
    auto before = pool.enemies.checkpoint();
    REQUIRE(before);
    // Fill every output kind as well as preserving the native events above, so
    // an accidentally omitted queue cannot pass because that queue was empty.
    before->blows.push_back({.player = 1, .kind = kGruntKind, .damage = 3});
    before->losses.push_back({.enemy = 2, .kind = kRatKind, .player = 0, .experience = 4});
    before->bursts.push_back({.position = {3, 0, 2}, .damage = 5, .enemy = 6});
    before->cues.push_back({.kind = EnemyCue::Kind::Bolt, .enemyKind = kGruntKind});
    before->feedback.push_back({.kind = kRatKind, .hitCount = 2, .close = true});
    before->deathEvents.push_back(
        {.kind = DeathEvent::Kind::Return, .enemy = 4, .player = 1, .amount = 7});
    before->deathShots.push_back({.enemy = 6, .position = {2, 0, 1}});
    before->tagged.push_back(1);
    before->generatorEvents.push_back({2, EnemyGeneratorEvent::Kind::PatrolDetached});
    REQUIRE(before->valid());
    REQUIRE(pool.enemies.restore(*before));
    CHECK(pool.enemies.takeBlows() == before->blows);
    CHECK(pool.enemies.takeLosses() == before->losses);
    CHECK(pool.enemies.takeBursts() == before->bursts);
    CHECK(pool.enemies.takeCues() == before->cues);
    CHECK(pool.enemies.takeFeedback() == before->feedback);
    CHECK(pool.enemies.takeDeathEvents() == before->deathEvents);
    CHECK(pool.enemies.takeDeathShots() == before->deathShots);
    CHECK(pool.enemies.takeTagged() == before->tagged);
    CHECK(pool.enemies.takeGeneratorEvents() == before->generatorEvents);
    REQUIRE(pool.enemies.restore(*before));
    CHECK(pool.enemies.checkpoint() == before);
    CHECK(pool.enemies.takeBlows() == before->blows);
    CHECK(pool.enemies.takeLosses() == before->losses);
    CHECK(pool.enemies.takeBursts() == before->bursts);
    CHECK(pool.enemies.takeCues() == before->cues);
    CHECK(pool.enemies.takeFeedback() == before->feedback);
    CHECK(pool.enemies.takeDeathEvents() == before->deathEvents);
    CHECK(pool.enemies.takeDeathShots() == before->deathShots);
    CHECK(pool.enemies.takeTagged() == before->tagged);
    CHECK(pool.enemies.takeGeneratorEvents() == before->generatorEvents);
    CHECK(pool.enemies.takeBlows().empty());
    CHECK(pool.enemies.takeLosses().empty());
    CHECK(pool.enemies.takeBursts().empty());
    CHECK(pool.enemies.takeCues().empty());
    CHECK(pool.enemies.takeFeedback().empty());
    CHECK(pool.enemies.takeDeathEvents().empty());
    CHECK(pool.enemies.takeDeathShots().empty());
    CHECK(pool.enemies.takeTagged().empty());
    CHECK(pool.enemies.takeGeneratorEvents().empty());
}

TEST_CASE("whole enemy checkpoint rejects malformed slots and mismatched loaded stocks atomically",
          "[rollback][enemies][enemies-checkpoint][assets]") {
    Pool pool;
    pool.step(0);
    const auto before = pool.enemies.checkpoint();
    REQUIRE(before);
    const auto uploads = pool.device.texturesCreated;
    const auto reject = [&](const Enemies::Checkpoint& invalid) {
        CHECK_FALSE(pool.enemies.restore(invalid));
        CHECK(pool.enemies.checkpoint() == before);
        CHECK(pool.device.texturesCreated == uploads);
    };
    auto invalid = *before;
    invalid.enemies[0].position.x = std::numeric_limits<f32>::quiet_NaN();
    reject(invalid);
    invalid = *before;
    invalid.enemies[0].kind = kEnemyKindCount;
    reject(invalid);
    invalid = *before;
    invalid.enemies[0].animator.reset();
    reject(invalid);
    invalid = *before;
    invalid.enemies[0].animator->tree = "WRONG_BOUND_TREE";
    reject(invalid);
    invalid = *before;
    invalid.enemies[0].deathSkin = "UNTRUSTED_SKIN";
    reject(invalid);
    invalid = *before;
    invalid.enemies[1].instance = invalid.enemies[0].instance;
    reject(invalid);
    invalid = *before;
    invalid.pendingTicks = 2;
    reject(invalid);
    invalid = *before;
    invalid.nextInstance = 0;
    reject(invalid);
    invalid = *before;
    invalid.stocks.pop_back();
    reject(invalid);
    invalid = *before;
    invalid.stocks[0].textures.remainder = std::numeric_limits<f32>::infinity();
    reject(invalid);
    invalid = *before;
    invalid.lookouts.next[0] = std::numeric_limits<s32>::min();
    reject(invalid);
    pool.enemies.close();
    CHECK_FALSE(pool.enemies.restore(*before));
    CHECK_FALSE(pool.enemies.checkpoint());
}
} // namespace
