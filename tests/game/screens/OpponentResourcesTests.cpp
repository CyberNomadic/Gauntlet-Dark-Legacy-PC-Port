#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/OpponentCheckpoint.h"
#include "game/screens/OpponentResources.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct NativePools {
    test::FakeRenderDevice device;
    WorldLayout layout;
    Enemies enemies;
    Generators generators;
    EnemyMissiles missiles{813};
    OpponentCheckpoint simulation{generators, enemies, missiles};

    explicit NativePools(std::span<const s32> kinds, s32 most = Enemies::kMost) {
        const auto root =
            test::assetOrSkip("MONSTERS/GRU/ANIM.PS2").parent_path().parent_path().parent_path();
        enemies.open(device, root, nullptr, most, {}, 571);
        for (const auto kind : kinds) {
            CAPTURE(kind);
            REQUIRE(enemies.loadKind(kind));
        }
        REQUIRE(generators.bind(device, layout, enemies, nullptr, {}, 1));
    }

    OpponentState capture() const {
        const auto state = OpponentResources::capture(generators, enemies, missiles);
        REQUIRE(state);
        return *state;
    }
};

TEST_CASE("native opponent resource IDs cover every kind and slot without instance state",
          "[rollback][opponent-resources]") {
    std::set<u32> ids;
    for (s32 kind = 0; kind < kEnemyKindCount; ++kind) {
        for (s32 slot = 0; slot < OpponentResources::kMissileSlots; ++slot) {
            CAPTURE(kind, slot);
            const auto id = OpponentResources::missileId(kind, slot);
            REQUIRE(id != 0);
            CHECK(ids.insert(id).second);
        }
    }
    CHECK(ids.size() == static_cast<usize>(kEnemyKindCount * 3));
    CHECK(OpponentResources::missileId(kDeathKind, 2) != 0);
    CHECK(OpponentResources::missileId(kItKind, 2) != 0);
    for (const auto kind :
         {-1, kEnemyKindCount, std::numeric_limits<s32>::min(), std::numeric_limits<s32>::max()}) {
        CHECK(OpponentResources::missileId(kind, 0) == 0);
    }
    for (const auto slot :
         {-1, 3, std::numeric_limits<s32>::min(), std::numeric_limits<s32>::max()}) {
        CHECK(OpponentResources::missileId(kGruntKind, slot) == 0);
    }
    const Enemies unopened;
    const Generators unbound;
    const EnemyMissiles empty;
    CHECK(OpponentResources::missiles(unopened).empty());
    CHECK_FALSE(OpponentResources::capture(unbound, unopened, empty));
}

TEST_CASE("native opponent resource manifests restore independent owners in reverse load order",
          "[rollback][opponent-resources][assets]") {
    std::vector<s32> kinds;
    kinds.reserve(static_cast<usize>(kSwarmKindCount));
    for (s32 kind = 0; kind < kSwarmKindCount; ++kind) {
        kinds.push_back(kind);
    }
    kinds.push_back(kDeathKind);
    kinds.push_back(kItKind);
    NativePools host(kinds);
    std::ranges::reverse(kinds);
    NativePools guest(kinds);
    const auto hostUploads = host.device.texturesCreated;
    const auto guestUploads = guest.device.texturesCreated;
    const auto hostUpdates = host.device.textureUpdates;
    const auto guestUpdates = guest.device.textureUpdates;
    const auto resources = OpponentResources::missiles(host.enemies);
    const auto peerResources = OpponentResources::missiles(guest.enemies);
    REQUIRE(resources.size() >= 20);
    REQUIRE(resources.size() == peerResources.size());
    usize index = 0;
    for (s32 kind = 0; kind < kEnemyKindCount; ++kind) {
        for (s32 slot = 0; slot < OpponentResources::kMissileSlots; ++slot) {
            CAPTURE(kind, slot);
            const auto* model = host.enemies.projectileModel(kind, slot);
            const auto* peerModel = guest.enemies.projectileModel(kind, slot);
            if (model == nullptr) {
                CHECK(peerModel == nullptr);
                continue;
            }
            REQUIRE(index < resources.size());
            REQUIRE(peerModel != nullptr);
            CHECK(resources[index].id == OpponentResources::missileId(kind, slot));
            CHECK(peerResources[index].id == resources[index].id);
            CHECK(resources[index].model == model);
            CHECK(peerResources[index].model == peerModel);
            CHECK(peerModel != model);
            // Original stock addresses, not models copied for replica rendering.
            host.missiles.launch(EnemyMissileKind::bolt(20, 25, 0.5f), {0, 10, 0}, {0, 10, 100}, 1,
                                 model, 0);
            ++index;
        }
    }
    CHECK(index == resources.size());
    CHECK(host.enemies.count() == 0); // no shooter slot exists to identify any of these models
    CHECK(guest.enemies.count() == 0);
    host.missiles.launch(EnemyMissileKind::deathShot(), {20, 10, 0}, {20, 10, 100}, 1, nullptr, -1);
    host.missiles.update(0.173f, nullptr, {});
    const auto expected = host.capture();
    REQUIRE(expected.missiles.missiles.size() == resources.size() + 1);
    CHECK(expected.missiles.missiles.back().resource == 0);
    REQUIRE(guest.simulation.restore(expected, peerResources));
    CHECK(guest.capture() == expected);
    for (usize flight = 0; flight < resources.size(); ++flight) {
        CHECK(guest.missiles.missile(flight).model == peerResources[flight].model);
        CHECK(guest.missiles.missile(flight).model != resources[flight].model);
    }
    CHECK(guest.missiles.missile(resources.size()).model == nullptr);
    for (s32 tick = 0; tick < 48; ++tick) {
        CAPTURE(tick);
        host.missiles.update(1.0f / 60, nullptr, {});
        guest.missiles.update(1.0f / 60, nullptr, {});
        CHECK(guest.capture() == host.capture());
    }
    CHECK(host.device.texturesCreated == hostUploads);
    CHECK(guest.device.texturesCreated == guestUploads);
    CHECK(host.device.textureUpdates == hostUpdates);
    CHECK(guest.device.textureUpdates == guestUpdates);
}

TEST_CASE("native opponent missile identity outlives a recycled shooter slot",
          "[rollback][opponent-resources][assets]") {
    constexpr std::array kKinds{kGruntKind, 13};
    NativePools host(kKinds, 1);
    constexpr std::array kReverse{13, kGruntKind};
    NativePools guest(kReverse, 1);
    EnemySpawn spawn;
    spawn.kind = kGruntKind;
    spawn.placed = true;
    REQUIRE(host.enemies.spawn(spawn, {}) == 0);
    const auto* arrow = host.enemies.projectileModel(kGruntKind, 0);
    REQUIRE(arrow != nullptr);
    host.missiles.launch(EnemyMissileKind::arrow(), {0, 6, 0}, {0, 6, 50}, 1, arrow, 0);
    const auto before = host.capture();
    EnemyHit hit;
    hit.damage = 10000;
    hit.flags = EnemyHit::kMagic;
    host.enemies.hurt(0, hit);
    for (s32 tick = 0; tick < 600 && host.enemies.count() != 0; ++tick) {
        host.enemies.update(2, 1.0f / 30, {});
    }
    REQUIRE(host.enemies.count() == 0);
    spawn.kind = 13;
    REQUIRE(host.enemies.spawn(spawn, {}) == 0);
    const auto after = host.capture();
    REQUIRE(after.missiles.missiles.size() == 1);
    CHECK(after.missiles == before.missiles);
    CHECK(after.enemies.enemies[0].kind == 13);
    CHECK(after.missiles.missiles[0].shooter == 0);
    CHECK(after.missiles.missiles[0].resource == OpponentResources::missileId(kGruntKind, 0));
    const auto resources = OpponentResources::missiles(guest.enemies);
    const auto uploads = guest.device.texturesCreated;
    const auto updates = guest.device.textureUpdates;
    REQUIRE(guest.simulation.restore(after, resources));
    CHECK(guest.capture() == after);
    CHECK(guest.missiles.missile(0).model == guest.enemies.projectileModel(kGruntKind, 0));
    CHECK(guest.missiles.missile(0).model != guest.enemies.projectileModel(13, 0));
    host.missiles.update(0.2f, nullptr, {});
    guest.missiles.update(0.2f, nullptr, {});
    CHECK(guest.capture() == host.capture());
    CHECK(guest.device.texturesCreated == uploads);
    CHECK(guest.device.textureUpdates == updates);
}

TEST_CASE("opponent capture refuses unknown visible models without losing native state",
          "[rollback][opponent-resources][assets]") {
    constexpr std::array kKinds{kGruntKind};
    NativePools pools(kKinds);
    const auto* arrow = pools.enemies.projectileModel(kGruntKind, 0);
    REQUIRE(arrow != nullptr);
    const auto uploads = pools.device.texturesCreated;
    const auto updates = pools.device.textureUpdates;
    TreeModel foreign;
    SECTION("a copied replica model is not an original stock resource") {
        foreign = *arrow;
    }
    SECTION("an unbound nonnull model is not a genuinely invisible missile") {}
    pools.missiles.launch(EnemyMissileKind::arrow(), {0, 6, 0}, {0, 6, 50}, 1, &foreign, 0);
    const auto generators = pools.generators.checkpoint();
    const auto enemies = pools.enemies.checkpoint();
    const auto flight = pools.missiles.missile(0);
    CHECK_FALSE(OpponentResources::capture(pools.generators, pools.enemies, pools.missiles));
    CHECK(pools.generators.checkpoint() == generators);
    CHECK(pools.enemies.checkpoint() == enemies);
    REQUIRE(pools.missiles.count() == 1);
    CHECK(pools.missiles.missile(0).model == &foreign);
    CHECK(pools.missiles.missile(0).instance == flight.instance);
    CHECK(pools.missiles.missile(0).position == flight.position);
    CHECK(pools.device.texturesCreated == uploads);
    CHECK(pools.device.textureUpdates == updates);
    pools.missiles.clear(); // the deliberately foreign model must outlive the flight
    pools.missiles.launch(EnemyMissileKind::arrow(), {0, 6, 0}, {0, 6, 50}, 1, nullptr, -1);
    const auto invisible = pools.capture();
    REQUIRE(invisible.missiles.missiles.size() == 1);
    CHECK(invisible.missiles.missiles[0].resource == 0);
}

TEST_CASE("missing opponent stock resources reject corrections atomically without loading",
          "[rollback][opponent-resources][assets]") {
    constexpr std::array kHostKinds{kGruntKind, 13};
    constexpr std::array kGuestKinds{kGruntKind};
    NativePools host(kHostKinds);
    NativePools guest(kGuestKinds);
    const auto* arrow = host.enemies.projectileModel(13, 0);
    REQUIRE(arrow != nullptr);
    host.missiles.launch(EnemyMissileKind::arrow(), {0, 6, 0}, {0, 6, 50}, 1, arrow, 0);
    const auto correction = host.capture();
    const auto before = guest.capture();
    const auto uploads = guest.device.texturesCreated;
    const auto updates = guest.device.textureUpdates;
    const auto resources = OpponentResources::missiles(guest.enemies);
    CHECK_FALSE(guest.missiles.restore(correction.missiles, resources));
    CHECK_FALSE(guest.simulation.restore(correction, resources));
    CHECK(guest.capture() == before);
    CHECK_FALSE(guest.enemies.kindLoaded(13));
    CHECK(guest.device.texturesCreated == uploads);
    CHECK(guest.device.textureUpdates == updates);
}

TEST_CASE("opponent capture rejects invalid coupling without draining any event queue",
          "[rollback][opponent-resources][assets]") {
    constexpr std::array kKinds{kGruntKind};
    NativePools pools(kKinds);
    auto state = pools.enemies.checkpoint();
    REQUIRE(state);
    state->generatorEvents.push_back({0, EnemyGeneratorEvent::Kind::Born});
    state->cues.push_back({.kind = EnemyCue::Kind::Arrow, .enemyKind = kGruntKind});
    REQUIRE(state->valid());
    REQUIRE(pools.enemies.restore(*state));
    CHECK_FALSE(OpponentResources::capture(pools.generators, pools.enemies, pools.missiles));
    CHECK(pools.enemies.checkpoint() == state);
    CHECK(pools.enemies.takeGeneratorEvents() == state->generatorEvents);
    CHECK(pools.enemies.takeCues() == state->cues);
    const auto clean = pools.capture();
    CHECK(clean.enemies.generatorEvents.empty());
    CHECK(clean.enemies.cues.empty());
}

} // namespace
