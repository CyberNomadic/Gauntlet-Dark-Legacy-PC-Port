#include <limits>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "game/enemies/EnemyKinds.h"
#include "game/screens/AttackCheckpointFixture.h"
#include "game/screens/HelpMessages.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Boundary = PlayerAttacksCheckpoint;
static_assert(!std::is_copy_constructible_v<PlayerAttacks>);
static_assert(!std::is_move_assignable_v<PlayerAttacks>);

TEST_CASE(
    "attack checkpoint resumes native potion shield and item windows without replaying payment",
    "[player-attacks-checkpoint]") {
    const s32 kind = GENERATE(0, 1, 2, 3);
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    CAPTURE(kind, seconds);
    test::AttackCheckpointFixture fixture;
    auto& player = fixture.players[0];
    auto& inventory = player.actor.save().progress().inventory;
    inventory.addPotions(2, 3);
    if (kind == 0) {
        fixture.attacks.usePotion(0, fixture.players);
    } else if (kind == 1) {
        fixture.attacks.shieldPotion(0, fixture.players);
    } else if (kind == 2) {
        inventory.addPowerup(powerup::kSpecial, powerup::kFireBreath, 3, 30, true);
        player.figure->animate(0, 1, seconds, PlayerDeed::Breathe);
        REQUIRE(player.figure->animator().itemReleased() == PlayerDeed::Breathe);
        fixture.attacks.useItemAttack(0, fixture.players);
    } else {
        fixture.attacks.shatterPotion(4, {1, 2, 3});
    }
    const auto saved = fixture.state();
    const auto visual = fixture.effectState();
    REQUIRE(fixture.attacks.pendingGameplay());
    REQUIRE_FALSE(visual.effects.empty());
    if (kind == 2) {
        REQUIRE(saved.items.size() == 1);
        CHECK(saved.items[0].item == Boundary::Item::FireBreath);
    }
    const auto paid = inventory;
    const auto health = player.actor.save().health();
    const auto uploads = fixture.device.texturesCreated;
    fixture.restore(saved, visual);
    CHECK(inventory == paid);
    CHECK(player.actor.save().health() == health);
    CHECK(fixture.helps.empty());
    CHECK(fixture.effectState() == visual); // restore never stops shared borrowed visuals
    CHECK(fixture.device.texturesCreated == uploads);

    std::vector<Boundary::State> expected;
    std::vector<EffectTrees::PoolCheckpoint> shown;
    const auto step = [&](s32 tick) {
        player.actor.place({static_cast<f32>(tick) * 0.01f, 0, 0});
        fixture.attacks.updateStrikes(seconds, fixture.players, fixture.targets);
        fixture.attacks.updateProjectiles(seconds, fixture.players, fixture.targets);
        fixture.attacks.updateShields(seconds, fixture.players, fixture.targets);
        fixture.effects.update(seconds);
    };
    for (s32 tick = 0; tick < 200; ++tick) {
        CAPTURE(tick);
        step(tick);
        expected.push_back(fixture.state());
        shown.push_back(fixture.effectState());
    }
    REQUIRE_FALSE(fixture.attacks.pendingGameplay());
    const auto expectedHelps = fixture.helps;
    CHECK(expectedHelps ==
          (kind == 0 ? std::vector<s32>{HelpMessages::kWastedMagic} : std::vector<s32>{}));
    fixture.restore(saved, visual);
    fixture.helps.clear();
    for (s32 tick = 0; tick < 200; ++tick) {
        CAPTURE(tick, kind, seconds);
        step(tick);
        CHECK(fixture.state() == expected[static_cast<usize>(tick)]);
        CHECK(fixture.effectState() == shown[static_cast<usize>(tick)]);
    }
    CHECK(fixture.helps == expectedHelps);
    CHECK(inventory == paid);
    CHECK(player.actor.save().health() == health);
    CHECK(fixture.device.texturesCreated == uploads);
}

TEST_CASE("attack restoration preserves native turbo repeated-contact cooldowns and their damage",
          "[player-attacks-checkpoint]") {
    const bool flying = GENERATE(false, true);
    test::AttackCheckpointFixture fixture;
    auto& enemies = fixture.opponents.enemies();
    EnemyScales scales;
    scales.health = 100;
    fixture.opponents.open({fixture.device, fixture.world, fixture.weapons, fixture.effects,
                            fixture.audio, fixture.root, 1},
                           fixture.players);
    enemies.open(fixture.device, fixture.root, nullptr, 8, scales, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    const auto enemy =
        enemies.spawn({.kind = kGruntKind, .position = {0, 0, 5}, .placed = true}, {});
    REQUIRE(enemy);
    auto& player = fixture.players[0];
    player.turbo.add(100);
    const f32 full = enemies.healthOf(*enemy);
    constexpr f32 seconds = 1.0f / 60;
    bool hit = false;
    for (s32 tick = 0; tick < 60 && !hit; ++tick) {
        player.figure->animate(0, 1, seconds,
                               tick == 0
                                   ? (flying ? PlayerDeed::TurboFull : PlayerDeed::TurboStrong)
                                   : PlayerDeed::None);
        fixture.attacks.updateTurbo(0, 1, seconds, fixture.players, [](s32, usize) {});
        fixture.attacks.updateStrikes(seconds, fixture.players, fixture.targets);
        fixture.effects.update(seconds);
        const auto state = fixture.state();
        hit = !state.strikeSources.empty() && !state.strikeSources[0].contacts.empty();
    }
    REQUIRE(hit);
    REQUIRE(enemies.healthOf(*enemy) < full);
    const auto saved = fixture.state();
    const auto visual = fixture.effectState();
    const auto victims = enemies.checkpoint();
    REQUIRE(victims);
    REQUIRE(saved.strikeSources[0].contacts[0].remaining > 0.25f);
    CHECK(saved.strikes.strikes[0].owner == 3); // native input slot, not party index zero
    const f32 afterFirst = enemies.healthOf(*enemy);
    const f32 paid = player.turbo.held();
    fixture.restore(saved, visual);
    CHECK(enemies.checkpoint() == victims);
    CHECK(player.turbo.held() == paid);
    std::vector<f32> health;
    std::vector<Boundary::State> expected;
    for (s32 tick = 0; tick < 240; ++tick) {
        fixture.attacks.updateStrikes(seconds, fixture.players, fixture.targets);
        fixture.effects.update(seconds);
        health.push_back(enemies.healthOf(*enemy));
        expected.push_back(fixture.state());
        if (tick < 10) {
            CHECK(health.back() == afterFirst);
        }
    }
    CHECK((health.back() < afterFirst) == flying);
    fixture.restore(saved, visual);
    REQUIRE(enemies.restore(*victims));
    for (s32 tick = 0; tick < 240; ++tick) {
        CAPTURE(tick, flying);
        fixture.attacks.updateStrikes(seconds, fixture.players, fixture.targets);
        fixture.effects.update(seconds);
        CHECK(enemies.healthOf(*enemy) == health[static_cast<usize>(tick)]);
        CHECK(fixture.state() == expected[static_cast<usize>(tick)]);
    }
    CHECK(player.turbo.held() == paid);
}

TEST_CASE("attack preparation rejects malformed references and actor rebinding atomically",
          "[player-attacks-checkpoint]") {
    test::AttackCheckpointFixture fixture;
    fixture.players[0].actor.save().progress().inventory.addPotions(2, 2);
    fixture.attacks.usePotion(0, fixture.players);
    fixture.attacks.shieldPotion(0, fixture.players);
    CHECK_FALSE(Boundary::capture(fixture.attacks, fixture.actors));
    auto saved = fixture.state();
    const auto visual = fixture.effectState();
    // A stopped attachment can remain until its cleanup phase and must survive restore.
    saved.attachments.push_back({0, visual.effects[0].id, 0, Mat4{1}, true});
    saved.particles.push_back({0, 0, visual.effects[0].id, "BODY", {1, 2, 3}});
    saved.items.push_back({Boundary::Item::FireBreath, 0, 0, 0.2f, 2, true, {101}});
    fixture.restore(saved, visual);
    REQUIRE(fixture.state() == saved);
    for (s32 mutation = 0; mutation < 13; ++mutation) {
        CAPTURE(mutation);
        auto bad = saved;
        switch (mutation) {
        case 0: bad.items[0].item = static_cast<Boundary::Item>(255); break;
        case 1: bad.items[0].item = Boundary::Item::AcidBreath; break; // nonnative Pojo variant
        case 2: bad.items[0].actor = 1; break;
        case 3: bad.items[0].hit.push_back(101); break;
        case 4: bad.potions[0].impact.effectResource = 999; break;
        case 5: bad.shields[0].effect = visual.nextId; break;
        case 6: bad.shields[0].harmIn = std::numeric_limits<f32>::quiet_NaN(); break;
        case 7: bad.attachments[0].local[3][3] = 0; break;
        case 8: bad.particles[0].node.assign(257, 'A'); break;
        case 9: bad.strikeSources.push_back({1, 0, 999}); break;
        case 10: bad.actors[0].player = 0; break;
        case 11: bad.actors[0].character = 2; break;
        case 12: bad.potions.resize(Boundary::State::kMaxAreas + 1); break;
        }
        CHECK_FALSE(Boundary::prepare(fixture.attacks, bad, fixture.actors, visual,
                                      fixture.missileResources));
        CHECK(fixture.state() == saved);
        CHECK(fixture.effectState() == visual);
    }
    auto a =
        Boundary::prepare(fixture.attacks, saved, fixture.actors, visual, fixture.missileResources);
    auto sibling =
        Boundary::prepare(fixture.attacks, saved, fixture.actors, visual, fixture.missileResources);
    REQUIRE(a);
    REQUIRE(sibling);
    PlayerAttacks other;
    CHECK_FALSE(Boundary::commit(other, std::move(*a)));
    REQUIRE(Boundary::commit(fixture.attacks, std::move(*a)));
    CHECK_FALSE(Boundary::canCommit(fixture.attacks, *a));
    CHECK_FALSE(Boundary::commit(fixture.attacks, std::move(*sibling)));
    a = Boundary::prepare(fixture.attacks, saved, fixture.actors, visual, fixture.missileResources);
    REQUIRE(a);
    fixture.attacks.clear();
    CHECK_FALSE(Boundary::commit(fixture.attacks, std::move(*a)));
}
} // namespace
