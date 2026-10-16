#include <array>
#include <memory>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/Bosses.h"
#include "game/enemies/Critters.h"
#include "game/world/HazardSurfaces.h"
#include "game/world/NativeStateResources.h"

namespace {
using namespace gdl;
using namespace gdl::game;

template <class Owner> struct NativeOwner {
    test::FakeRenderDevice device;
    NativeStateResources resources;
    Owner owner;

    NativeOwner() {
        const auto root = test::assetOrSkip("CRITTER/GOLEM.WAD").parent_path().parent_path();
        owner.open(device, root, nullptr, {}, 'G');
    }
    NativeStateDigest::Digest digest() {
        REQUIRE(owner.registerNativeResources(resources, "stage/G5/test-owner"));
        NativeStateDigest value{resources.resources()};
        REQUIRE(owner.appendNativeDigest(value));
        REQUIRE(value.checkedFinish());
        return *value.checkedFinish();
    }
    NativeStateDigest::Digest digest(const typename Owner::LocalSnapshot& saved) {
        REQUIRE(owner.registerNativeResources(resources, "stage/G5/test-owner"));
        NativeStateDigest value{resources.resources()};
        REQUIRE(owner.appendLocalDigest(value, saved));
        REQUIRE(value.checkedFinish());
        return *value.checkedFinish();
    }
};

TEST_CASE("native boss digest observes hidden waking and legend clocks",
          "[native-digest][opponent-owners-native-digest]") {
    Bosses boss, foreign;
    Bosses::LocalSnapshot saved;
    REQUIRE(boss.saveLocal(saved));
    NativeStateDigest initial, historical, changed;
    REQUIRE(boss.appendNativeDigest(initial));
    boss.arm(1.0f / 60);
    REQUIRE_FALSE(boss.present());
    REQUIRE(boss.appendNativeDigest(changed));
    REQUIRE(boss.appendLocalDigest(historical, saved));
    CHECK(initial.checkedFinish() == historical.checkedFinish());
    CHECK(initial.checkedFinish() != changed.checkedFinish());
    NativeStateDigest rejected;
    CHECK_FALSE(foreign.appendLocalDigest(rejected, saved));
    CHECK_FALSE(rejected.checkedFinish());
    boss.close();
    NativeStateDigest stale;
    CHECK_FALSE(boss.appendLocalDigest(stale, saved));

    LegendRite rite;
    rite.begin(1, *legendWeaknessOf(38));
    rite.update(1, true, false);
    const auto before = rite;
    const auto phase = rite.stage();
    REQUIRE(rite.update(1, true, false).empty());
    REQUIRE(rite.stage() == phase);
    NativeStateDigest beforeHash, afterHash;
    beforeHash.value(before);
    afterHash.value(rite);
    CHECK(beforeHash.checkedFinish() != afterHash.checkedFinish());
}

TEST_CASE("native critter owner hashes hidden routes without reading a saved patrol's live route",
          "[native-digest][opponent-owners-native-digest][assets]") {
    auto population = std::make_unique<NativeOwner<Critters>>();
    population->owner.setLookouts({{{0, 0, 0}, {0, 0, 20}}, {1, 65535}});
    REQUIRE(population->owner.spawnGeneral({}, 0, 15));
    Critters::LocalSnapshot saved;
    REQUIRE(population->owner.saveLocal(saved));
    const auto initial = population->digest();
    REQUIRE(population->digest(saved) == initial);
    population->owner.setLookouts({{{2, 0, 0}, {2, 0, 20}}, {1, 65535}});
    CHECK(population->digest() != initial);
    CHECK(population->digest(saved) == initial);
    REQUIRE(population->owner.restoreLocal(saved));
    CHECK(population->digest() == initial);
}

TEST_CASE("native critter owner includes discarded species histories and native texture clocks",
          "[native-digest][opponent-owners-native-digest][assets]") {
    auto first = std::make_unique<NativeOwner<Critters>>();
    auto second = std::make_unique<NativeOwner<Critters>>();
    Critters::LocalSnapshot cold, general, speculative;
    REQUIRE(first->owner.saveLocal(cold));
    const auto empty = first->digest();
    REQUIRE(first->owner.spawnGeneral({}, 0, 15));
    REQUIRE(second->owner.spawnGeneral({}, 0, 15));
    REQUIRE(first->owner.saveLocal(general));
    const auto initial = first->digest();
    CHECK(initial == second->digest());
    REQUIRE(first->owner.spawnGolem({4, 0, 3}, 0));
    REQUIRE(first->owner.spawnGargoyle({8, 0, 3}, 0));
    REQUIRE(first->owner.saveLocal(speculative));
    // No resource registration in the speculative branch. The cache, not the
    // rolled-back logical roster, must register these saved borrowed bindings.
    REQUIRE(first->owner.restoreLocal(general));
    const auto uploads = first->device.texturesCreated;
    CHECK(first->digest() == initial);
    CHECK(first->digest(speculative) != initial);
    CHECK(first->device.texturesCreated == uploads);
    first->owner.resources().front()->textures.advance(1.0f / 60);
    CHECK(first->digest() != initial);
    CHECK(first->digest(general) == initial);
    REQUIRE(first->owner.restoreLocal(general));
    CHECK(first->digest() == second->digest());
    REQUIRE(first->owner.restoreLocal(cold));
    CHECK(first->digest() == empty);
    CHECK(first->device.textureUpdates == 0);
    NativeStateDigest foreign{second->resources.resources()};
    CHECK_FALSE(second->owner.appendLocalDigest(foreign, general));
    first->owner.close();
    NativeStateDigest stale{first->resources.resources()};
    CHECK_FALSE(first->owner.appendLocalDigest(stale, general));
    CHECK_FALSE(stale.checkedFinish());
}

TEST_CASE("native boss owner hashes all families and recursive children across independent loads",
          "[native-digest][opponent-owners-native-digest][assets]") {
    const s32 kind = GENERATE(34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44);
    CAPTURE(kind);
    auto first = std::make_unique<NativeOwner<Bosses>>();
    auto second = std::make_unique<NativeOwner<Bosses>>();
    Bosses::LocalSnapshot cold, saved;
    REQUIRE(first->owner.saveLocal(cold));
    const auto empty = first->digest();
    REQUIRE(first->owner.spawn(kind, {}, 0, 30));
    REQUIRE(second->owner.spawn(kind, {}, 0, 30));
    std::array<EnemyView, 1> party;
    party[0].player = 0;
    party[0].position = {0, 0, 12};
    party[0].height = 6;
    party[0].radius = 1;
    for (s32 tick = 0; tick < 160; ++tick) {
        first->owner.update(1, 1.0f / 60, party);
        second->owner.update(1, 1.0f / 60, party);
    }
    REQUIRE(first->owner.saveLocal(saved));
    const auto current = first->digest();
    CHECK(current == second->digest());
    CHECK(first->digest(saved) == current);
    const auto uploads = first->device.texturesCreated;
    first->owner.draw(first->device, Mat4{1}, {}, nullptr, 0.25f);
    first->owner.draw(first->device, Mat4{1}, {}, nullptr, 0.75f);
    CHECK(first->digest() == current);
    REQUIRE(first->owner.restoreLocal(cold));
    CHECK(first->digest() == empty);
    CHECK(first->digest(saved) == current);
    REQUIRE(first->owner.restoreLocal(saved));
    CHECK(first->digest() == second->digest());
    CHECK(first->device.texturesCreated == uploads);
    CHECK(first->device.textureUpdates == 0);
}

TEST_CASE("native combatant owner fails closed without family registration",
          "[native-digest][opponent-owners-native-digest][assets]") {
    auto population = std::make_unique<NativeOwner<Critters>>();
    REQUIRE(population->owner.spawnGeneral({}, 0, 15));
    NativeStateDigest missing;
    CHECK_FALSE(population->owner.appendNativeDigest(missing));
    CHECK_FALSE(missing.checkedFinish());
    CHECK_FALSE(population->resources.registerCombatantAssets(
        *population->owner.resources().front(), ""));
}

TEST_CASE("empty native opponent owners include configuration for their next birth",
          "[native-digest][opponent-owners-native-digest]") {
    test::FakeRenderDevice device;
    Bosses boss;
    Critters critters;
    NativeStateResources resources;
    WorldCollision collision;
    HazardSurfaces hazards;
    REQUIRE(resources.registerCollision(collision, "stage/test"));
    REQUIRE(resources.registerHazards(hazards, "stage/test"));
    const auto bossHash = [&] {
        NativeStateDigest digest{resources.resources()};
        REQUIRE(boss.appendNativeDigest(digest));
        REQUIRE(digest.checkedFinish());
        return *digest.checkedFinish();
    };
    const auto critterHash = [&] {
        NativeStateDigest digest{resources.resources()};
        REQUIRE(critters.appendNativeDigest(digest));
        REQUIRE(digest.checkedFinish());
        return *digest.checkedFinish();
    };
    // No actors or archives are loaded in these configurations.
    boss.open(device, {}, nullptr, {}, 'G');
    critters.open(device, {}, nullptr, {}, 'G');
    const auto bossInitial = bossHash();
    const auto crittersInitial = critterHash();
    EnemyScales scaled;
    scaled.speed = 2;
    boss.open(device, {}, nullptr, scaled, 'G');
    critters.open(device, {}, nullptr, scaled, 'G');
    CHECK(bossHash() != bossInitial);
    CHECK(critterHash() != crittersInitial);
    boss.open(device, {}, nullptr, {}, 'A');
    critters.open(device, {}, nullptr, {}, 'A');
    CHECK(bossHash() != bossInitial);
    CHECK(critterHash() != crittersInitial);
    boss.open(device, {}, &collision, {}, 'G');
    critters.open(device, {}, &collision, {}, 'G');
    CHECK(bossHash() != bossInitial);
    CHECK(critterHash() != crittersInitial);
    critters.open(device, {}, nullptr, {}, 'G', {}, "EAGL");
    CHECK(critterHash() != crittersInitial);
    critters.open(device, {}, nullptr, {}, 'G');
    CHECK(critterHash() == crittersInitial);
    Critters::LocalSnapshot beforeHazards;
    REQUIRE(critters.saveLocal(beforeHazards));
    critters.setHazards(&hazards);
    CHECK(critterHash() != crittersInitial);
    NativeStateDigest refused{resources.resources()};
    CHECK_FALSE(critters.appendLocalDigest(refused, beforeHazards));
    CHECK_FALSE(refused.checkedFinish());
}
} // namespace
