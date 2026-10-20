#include <limits>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "game/players/Progression.h"
#include "game/screens/AttackCheckpointFixture.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Boundary = PlayerArsenalCheckpoint;
static_assert(std::is_aggregate_v<Boundary::State>);

TEST_CASE("arsenal checkpoint restores the shared unspecified-potion stream without spending again",
          "[player-arsenal-checkpoint]") {
    test::AttackCheckpointFixture fixture(false);
    CHECK(fixture.arsenal.resolvePotionKind(0) == 1);
    CHECK(fixture.arsenal.resolvePotionKind(4) == 4);
    const auto saved = Boundary::capture(fixture.arsenal);
    REQUIRE(saved);
    REQUIRE(saved->nextPotionKind == 2);
    const auto visual = fixture.effectState();
    auto& inventory = fixture.players[0].actor.save().progress().inventory;
    inventory.addPotions(0, 2);
    const auto before = inventory;
    CHECK(fixture.arsenal.resolvePotionKind(0) == 2);
    CHECK(fixture.arsenal.resolvePotionKind(0) == 3);
    REQUIRE(Boundary::restore(fixture.arsenal, *saved, fixture.slots, visual));
    CHECK(inventory == before);
    CHECK(fixture.arsenal.resolvePotionKind(0) == 2);
    CHECK(fixture.arsenal.resolvePotionKind(0) == 3);
    CHECK(fixture.arsenal.resolvePotionKind(0) == 4);
    CHECK(fixture.arsenal.resolvePotionKind(0) == 1);
    fixture.arsenal.clear();
    const auto unbound = Boundary::capture(fixture.arsenal);
    REQUIRE(unbound);
    CHECK_FALSE(unbound->bound);
    CHECK(unbound->nextPotionKind == 2); // native clear does not reset the colour cycle
    CHECK_FALSE(Boundary::restore(fixture.arsenal, *saved, fixture.slots, visual));
    REQUIRE(Boundary::restore(fixture.arsenal, *unbound, fixture.slots, visual));
}

TEST_CASE("arsenal cast attachments resume with their external shared effect transaction",
          "[player-arsenal-checkpoint]") {
    test::AttackCheckpointFixture fixture;
    auto& actor = fixture.players[0].actor;
    actor.save().progress().experience = levelExperience(80);
    actor.save().progress().inventory.addPotions(2, 1);
    REQUIRE(fixture.arsenal.usePotion(actor));
    const auto saved = Boundary::capture(fixture.arsenal);
    REQUIRE(saved);
    REQUIRE(saved->casts.size() == 1);
    REQUIRE(saved->casts[0].owner == 3);
    const auto visual = fixture.effectState();
    const auto paid = actor.save().progress().inventory;
    const auto health = actor.save().health();
    const auto uploads = fixture.device.texturesCreated;
    std::vector<Boundary::State> expected;
    std::vector<EffectTrees::PoolCheckpoint> shown;
    const auto step = [&](s32 tick) {
        actor.place({static_cast<f32>(tick), 1, 3});
        fixture.arsenal.followCaster(actor);
        fixture.effects.update(1.0f / 30);
        const auto state = Boundary::capture(fixture.arsenal);
        REQUIRE(state);
        return *state;
    };
    for (s32 tick = 0; tick < 100; ++tick) {
        expected.push_back(step(tick));
        shown.push_back(fixture.effectState());
    }
    REQUIRE(expected.back().casts.empty());
    auto a = Boundary::prepare(fixture.arsenal, *saved, fixture.slots, visual);
    auto b = fixture.effects.prepareRestorePool(visual, fixture.resources);
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(Boundary::canCommit(fixture.arsenal, *a));
    REQUIRE(fixture.effects.canCommit(*b));
    REQUIRE(Boundary::commit(fixture.arsenal, std::move(*a)));
    REQUIRE(fixture.effects.commitRestore(std::move(*b)));
    CHECK(actor.save().progress().inventory == paid);
    CHECK(actor.save().health() == health);
    for (s32 tick = 0; tick < 100; ++tick) {
        CAPTURE(tick);
        CHECK(step(tick) == expected[static_cast<usize>(tick)]);
        CHECK(fixture.effectState() == shown[static_cast<usize>(tick)]);
    }
    CHECK(fixture.device.texturesCreated == uploads);
}

TEST_CASE("arsenal restoration includes live native potion flights queues and child invalidation",
          "[player-arsenal-checkpoint]") {
    const s32 kind = GENERATE(1, 2, 3, 4);
    test::AttackCheckpointFixture fixture(false);
    auto& actor = fixture.players[0].actor;
    actor.save().progress().inventory.addPotions(kind, 1);
    fixture.arsenal.throwPotion(actor, 18, 49);
    auto& missiles = fixture.arsenal.missiles();
    REQUIRE(missiles.count() == 1);
    const auto& launched = missiles.missile(0);
    const std::array<PlayerMissiles::Resource, 1> resources{
        {{7, launched.spec, launched.model, launched.streak.texture}}};
    missiles.update(0.13f, nullptr, {});
    const auto saved = Boundary::capture(fixture.arsenal, resources);
    REQUIRE(saved);
    CHECK(saved->valid());
    const auto direct = missiles.checkpoint(resources);
    REQUIRE(direct);
    CHECK(saved->missiles == *direct);
    REQUIRE(saved->missiles.missiles.size() == 1);
    const auto visual = fixture.effectState();
    const auto paid = actor.save().progress().inventory;
    std::vector<Boundary::State> expected;
    const auto step = [&](s32 tick) {
        missiles.update(tick % 7 == 0 ? 0 : 1.0f / 30, nullptr, {});
        const auto state = Boundary::capture(fixture.arsenal, resources);
        REQUIRE(state);
        return *state;
    };
    for (s32 tick = 0; tick < 130; ++tick) {
        expected.push_back(step(tick));
        CHECK(expected.back().valid());
    }
    REQUIRE(Boundary::restore(fixture.arsenal, *saved, fixture.slots, visual, resources));
    for (s32 tick = 0; tick < 130; ++tick) {
        CHECK(step(tick) == expected[static_cast<usize>(tick)]);
    }
    CHECK(actor.save().progress().inventory == paid);
    auto prepared = Boundary::prepare(fixture.arsenal, *saved, fixture.slots, visual, resources);
    REQUIRE(prepared);
    missiles.clear();
    CHECK_FALSE(Boundary::canCommit(fixture.arsenal, *prepared));
}

TEST_CASE("arsenal capture refuses malformed live missile state and an unresolved catalog",
          "[player-arsenal-checkpoint][checkpoint-capture-validation]") {
    test::AttackCheckpointFixture fixture(false);
    auto& actor = fixture.players[0].actor;
    actor.save().progress().inventory.addPotions(1, 1);
    fixture.arsenal.throwPotion(actor, 18, 49);
    auto& missiles = fixture.arsenal.missiles();
    REQUIRE(missiles.count() == 1);
    const auto& shot = missiles.missile(0);
    const std::array resources{
        PlayerMissiles::Resource{7, shot.spec, shot.model, shot.streak.texture}};
    const auto before = Boundary::capture(fixture.arsenal, resources);
    REQUIRE(before);
    REQUIRE(before->valid());
    CHECK_FALSE(Boundary::capture(fixture.arsenal));
    auto invalid = *before;
    invalid.missiles.ricochetIn = std::numeric_limits<f32>::quiet_NaN();
    CHECK_FALSE(invalid.valid());
    CHECK_FALSE(Boundary::prepare(fixture.arsenal, invalid, fixture.slots, fixture.effectState(),
                                  resources));
    // Keep the native identity/resource tuple intact while corrupting one
    // scalar, so the child value validator (not missing artwork) rejects it.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto& live = const_cast<PlayerMissiles::Missile&>(shot);
    const auto savedDamage = live.damage;
    live.damage = std::numeric_limits<f32>::quiet_NaN();
    CHECK_FALSE(Boundary::capture(fixture.arsenal, resources));
    live.damage = savedDamage;
    CHECK(Boundary::capture(fixture.arsenal, resources) == before);
}

TEST_CASE("arsenal preparation validates identities and state before atomic single-owner commit",
          "[player-arsenal-checkpoint]") {
    test::AttackCheckpointFixture fixture;
    auto& actor = fixture.players[0].actor;
    actor.save().progress().experience = levelExperience(80);
    actor.save().progress().inventory.addPotions(1, 1);
    REQUIRE(fixture.arsenal.usePotion(actor));
    const auto saved = Boundary::capture(fixture.arsenal);
    REQUIRE(saved);
    REQUIRE(saved->casts.size() == 1);
    const auto visual = fixture.effectState();
    for (s32 mutation = 0; mutation < 8; ++mutation) {
        CAPTURE(mutation);
        auto bad = *saved;
        switch (mutation) {
        case 0: bad.nextPotionKind = 0; break;
        case 1: bad.nextPotionKind = 5; break;
        case 2: bad.casts[0].owner = 0; break;
        case 3: bad.casts[0].effect = visual.nextId; break;
        case 4: bad.casts.push_back(bad.casts[0]); break;
        case 5: bad.casts.resize(Boundary::State::kMaxCasts + 1); break;
        case 6: bad.bound = false; break;
        case 7: bad.missiles.ricochetIn = std::numeric_limits<f32>::quiet_NaN(); break;
        default:
            break;
        }
        CHECK_FALSE(Boundary::prepare(fixture.arsenal, bad, fixture.slots, visual));
        CHECK(Boundary::capture(fixture.arsenal) == saved);
        CHECK(fixture.effectState() == visual);
    }
    auto prepared = Boundary::prepare(fixture.arsenal, *saved, fixture.slots, visual);
    auto sibling = Boundary::prepare(fixture.arsenal, *saved, fixture.slots, visual);
    REQUIRE(prepared);
    REQUIRE(sibling);
    PlayerArsenal other;
    CHECK_FALSE(Boundary::commit(other, std::move(*prepared)));
    REQUIRE(Boundary::commit(fixture.arsenal, std::move(*prepared)));
    CHECK_FALSE(Boundary::commit(fixture.arsenal, std::move(*prepared)));
    CHECK_FALSE(Boundary::commit(fixture.arsenal, std::move(*sibling)));
    prepared = Boundary::prepare(fixture.arsenal, *saved, fixture.slots, visual);
    REQUIRE(prepared);
    fixture.arsenal.clear();
    CHECK_FALSE(Boundary::commit(fixture.arsenal, std::move(*prepared)));
}
} // namespace
