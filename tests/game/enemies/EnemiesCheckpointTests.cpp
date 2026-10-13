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

    explicit Pool(bool reverseStocks = false) {
        const auto root =
            test::assetOrSkip("MONSTERS/GRU/ANIM.PS2").parent_path().parent_path().parent_path();
        enemies.open(device, root, nullptr, Enemies::kMost, {}, 125);
        auto stocks = kKinds;
        if (reverseStocks) {
            std::ranges::reverse(stocks);
        }
        for (const s32 kind : stocks) {
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

TEST_CASE("native enemy correction resolves the same trusted stocks in different load orders",
          "[rollback][enemies][enemies-checkpoint][assets]") {
    Pool host;
    Pool guest(true);
    const auto initialHost = host.enemies.checkpoint();
    const auto initialGuest = guest.enemies.checkpoint();
    REQUIRE(initialHost);
    REQUIRE(initialGuest);
    REQUIRE(initialHost == initialGuest);
    CHECK(std::ranges::is_sorted(initialHost->stocks, {}, &Enemies::Checkpoint::Stock::kind));
    // Distinct valid clocks prove the receiver binds by kind rather than merely
    // accepting a reordered roster whose clocks all happen to be identical.
    auto seeded = *initialHost;
    for (usize index = 0; index < seeded.stocks.size(); ++index) {
        seeded.stocks[index].textures.frame = static_cast<u32>(index * 17);
        seeded.stocks[index].textures.remainder = 0.25f;
        seeded.stocks[index].textures.advance = 0.5f;
    }
    REQUIRE(seeded.valid());
    REQUIRE(host.enemies.restore(seeded));
    const auto hostUploads = host.device.texturesCreated;
    const auto guestUploads = guest.device.texturesCreated;
    for (s32 tick = 0; tick < 23; ++tick) {
        host.step(tick);
    }
    const auto correction = host.enemies.checkpoint();
    REQUIRE(correction);
    auto prepared = guest.enemies.prepareRestore(*correction);
    REQUIRE(prepared);
    CHECK(guest.enemies.checkpoint() == initialGuest);
    REQUIRE(guest.enemies.canCommit(*prepared));
    REQUIRE(guest.enemies.commitRestore(std::move(*prepared)));
    CHECK(guest.enemies.checkpoint() == correction);
    for (s32 tick = 23; tick < 72; ++tick) {
        CAPTURE(tick);
        host.step(tick);
        guest.step(tick);
        CHECK(host.enemies.checkpoint() == guest.enemies.checkpoint());
    }
    CHECK(host.device.texturesCreated == hostUploads);
    CHECK(guest.device.texturesCreated == guestUploads);

    const auto before = guest.enemies.checkpoint();
    REQUIRE(before);
    auto unsorted = *before;
    std::ranges::reverse(unsorted.stocks);
    CHECK_FALSE(unsorted.valid());
    CHECK_FALSE(guest.enemies.restore(unsorted));
    CHECK(guest.enemies.checkpoint() == before);
    auto duplicate = *before;
    duplicate.stocks[1].kind = duplicate.stocks[0].kind;
    CHECK_FALSE(duplicate.valid());
    CHECK_FALSE(guest.enemies.restore(duplicate));
    CHECK(guest.enemies.checkpoint() == before);
    auto missing = *before;
    missing.stocks.pop_back();
    REQUIRE(missing.valid());
    CHECK_FALSE(guest.enemies.restore(missing));
    CHECK(guest.enemies.checkpoint() == before);
    auto different = *before;
    REQUIRE(
        std::ranges::none_of(different.stocks, [](const auto& stock) { return stock.kind == 2; }));
    different.stocks.front().kind = 2;
    std::ranges::sort(different.stocks, {}, &Enemies::Checkpoint::Stock::kind);
    REQUIRE(different.valid());
    CHECK_FALSE(guest.enemies.restore(different));
    CHECK(guest.enemies.checkpoint() == before);
    CHECK(guest.device.texturesCreated == guestUploads);
}

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

TEST_CASE("prepared enemy correction is inert owner bound and single use",
          "[rollback][enemies][enemies-checkpoint][assets]") {
    Pool pool;
    Pool other;
    const auto baseline = pool.enemies.checkpoint();
    REQUIRE(baseline);
    pool.step(0);
    const auto current = pool.enemies.checkpoint();
    const auto otherCurrent = other.enemies.checkpoint();
    REQUIRE(current);
    CHECK(current != baseline);
    const auto uploads = pool.device.texturesCreated;
    auto prepared = pool.enemies.prepareRestore(*baseline);
    auto sibling = pool.enemies.prepareRestore(*baseline);
    REQUIRE(prepared);
    REQUIRE(sibling);
    CHECK(pool.enemies.checkpoint() == current);
    CHECK(pool.device.texturesCreated == uploads);
    CHECK(pool.enemies.canCommit(*prepared));
    CHECK_FALSE(other.enemies.canCommit(*prepared));
    CHECK_FALSE(other.enemies.commitRestore(std::move(*prepared)));
    CHECK(other.enemies.checkpoint() == otherCurrent);
    REQUIRE(pool.enemies.commitRestore(std::move(*prepared)));
    CHECK(pool.enemies.checkpoint() == baseline);
    CHECK_FALSE(pool.enemies.canCommit(*prepared));
    CHECK_FALSE(pool.enemies.commitRestore(std::move(*prepared)));
    CHECK_FALSE(pool.enemies.canCommit(*sibling));
    CHECK_FALSE(pool.enemies.commitRestore(std::move(*sibling)));
    CHECK(pool.enemies.checkpoint() == baseline);
    CHECK(pool.device.texturesCreated == uploads);
}

TEST_CASE("prepared enemy correction cannot outlive a level resource binding",
          "[rollback][enemies][enemies-checkpoint][assets]") {
    Pool pool;
    const auto baseline = pool.enemies.checkpoint();
    REQUIRE(baseline);
    auto prepared = pool.enemies.prepareRestore(*baseline);
    REQUIRE(prepared);
    pool.enemies.close();
    CHECK_FALSE(pool.enemies.canCommit(*prepared));
    CHECK_FALSE(pool.enemies.commitRestore(std::move(*prepared)));
    const auto root =
        test::assetOrSkip("MONSTERS/GRU/ANIM.PS2").parent_path().parent_path().parent_path();
    pool.enemies.open(pool.device, root, nullptr, Enemies::kMost, {}, 125);
    for (const auto kind : kKinds) {
        REQUIRE(pool.enemies.loadKind(kind));
    }
    const auto rebound = pool.enemies.checkpoint();
    REQUIRE(rebound);
    CHECK_FALSE(pool.enemies.canCommit(*prepared));
    CHECK_FALSE(pool.enemies.commitRestore(std::move(*prepared)));
    CHECK(pool.enemies.checkpoint() == rebound);
    // A new preparation resolves fresh native trees, not stale pointers.
    REQUIRE(pool.enemies.restore(*baseline));
    CHECK(pool.enemies.checkpoint() == baseline);
}
TEST_CASE("prepared enemy correction rejects a new owner reusing the same address",
          "[rollback][enemies][enemies-checkpoint][assets]") {
    test::FakeRenderDevice device;
    const auto root =
        test::assetOrSkip("MONSTERS/GRU/ANIM.PS2").parent_path().parent_path().parent_path();
    auto storage = std::make_unique<std::optional<Enemies>>(std::in_place);
    auto& first = **storage;
    first.open(device, root, nullptr, Enemies::kMost, {}, 125);
    REQUIRE(first.loadKind(kGruntKind));
    const auto baseline = first.checkpoint();
    REQUIRE(baseline);
    auto prepared = first.prepareRestore(*baseline);
    REQUIRE(prepared);
    const auto* address = &first;
    storage->reset();
    auto& replacement = storage->emplace();
    CHECK(&replacement == address);
    replacement.open(device, root, nullptr, Enemies::kMost, {}, 126);
    REQUIRE(replacement.loadKind(kGruntKind));
    const auto current = replacement.checkpoint();
    REQUIRE(current);
    CHECK_FALSE(replacement.canCommit(*prepared));
    CHECK_FALSE(replacement.commitRestore(std::move(*prepared)));
    CHECK(replacement.checkpoint() == current);
}
} // namespace
