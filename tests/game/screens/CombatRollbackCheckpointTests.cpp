#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "game/players/Progression.h"
#include "game/screens/AttackCheckpointFixture.h"
#include "game/screens/CombatRollbackCheckpoint.h"
#include "game/screens/PartyDamage.h"
#include "game/screens/PartyRecords.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Boundary = CombatRollbackCheckpoint;
static_assert(std::is_aggregate_v<Boundary::State>);

struct SmallFixture {
    ClassStats stats;
    std::array<PlayerRuntime, 2> players;
    std::array<std::unique_ptr<PlayerRuntimeCheckpoint>, 2> runtimes;
    std::array<PlayerRuntimeCheckpoint*, 2> ordered{};
    PlayerHealth health;
    LevelWatch levels;
    PlayerAttacks attacks;
    PlayerArsenal arsenal;
    EffectTrees effects;
    std::unique_ptr<Boundary> owner;

    SmallFixture() {
        constexpr std::array<s32, 2> kSlots{3, 1};
        for (usize index = 0; index < players.size(); ++index) {
            CharacterSave save;
            save.name = "TEST";
            save.progress().health = 1000;
            players[index].actor.spawn(kSlots[index], save, &stats, Vec3{0}, 0);
            players[index].entrySave = save;
            runtimes[index] = std::make_unique<PlayerRuntimeCheckpoint>(players[index], &stats);
            ordered[index] = runtimes[index].get();
            levels.observe(kSlots[index], 1);
        }
        owner = std::make_unique<Boundary>(ordered, health, levels, attacks, arsenal, effects,
                                           Boundary::Resources{});
    }
    Boundary::State state() const {
        auto result = owner->checkpoint();
        REQUIRE(result);
        REQUIRE(result->valid());
        return *result;
    }
};

struct NativeFixture {
    test::AttackCheckpointFixture native;
    PlayerRuntimeCheckpoint runtime{native.players[0], native.classes.stats(0)};
    std::array<PlayerRuntimeCheckpoint*, 1> ordered{&runtime};
    PlayerHealth health;
    LevelWatch levels;
    const MissileSpec spec{"", "", 0.1f, 0, 0, false, ""};
    std::array<PlayerMissiles::Resource, 2> missiles{
        {{17, &spec, nullptr, nullptr}, {1, &MissileSpec::of(0), nullptr, nullptr}}};
    Boundary owner{ordered,
                   health,
                   levels,
                   native.attacks,
                   native.arsenal,
                   native.effects,
                   {missiles, {}, native.resources}};
    std::vector<std::string> cues;
    std::vector<LevelChange> changes;

    NativeFixture() {
        auto& actor = native.players[0].actor;
        actor.save().progress().experience = levelExperience(80);
        actor.save().progress().health = 1500;
        actor.save().progress().inventory.addPotions(1, 3);
        native.players[0].entrySave = actor.save();
        levels.observe(actor.player(), experienceLevel(actor.save().experience()));
        native.attacks.usePotion(0, native.players);
        native.attacks.shieldPotion(0, native.players);
        MissileLaunch shot;
        shot.owner = actor.player();
        shot.position = {0, 3, 0};
        shot.velocity = Vec3{0, 0, 4};
        shot.spec = &spec;
        shot.damage = 13;
        REQUIRE(native.arsenal.missiles().launch(shot));
        native.arsenal.resolvePotionKind(0);
        REQUIRE_FALSE(state().attacks.potions.empty());
        REQUIRE_FALSE(state().attacks.shields.empty());
        REQUIRE_FALSE(state().arsenal.casts.empty());
    }
    Boundary::State state() const {
        auto result = owner.checkpoint();
        REQUIRE(result);
        REQUIRE(result->valid());
        return *result;
    }
    void step(s32 tick, f32 seconds) {
        auto& player = native.players[0];
        if (tick % 11 == 0) {
            const PlayerHealth::Events events{
                .sound = [this](std::string_view sound) { cues.emplace_back(sound); },
                .cry = [this](std::string_view sound) { cues.emplace_back(sound); },
                .named = [this](std::string_view sound, f32) { cues.emplace_back(sound); }};
            health.hurt(player, 4.25f, HurtKind::Blow, true, false, 1, events, {}, false,
                        native.classes.stats(0));
        }
        player.actor.place({static_cast<f32>(tick) * 0.01f, 0, 0});
        player.figure->animate(0, 1, seconds, PlayerDeed::None);
        if (tick % 13 == 0) {
            PartyRecords::award(native.players, player.actor.player(), 300, true, nullptr);
            PartyRecords::destroyedGenerator(native.players, player.actor.player());
        }
        if (auto change = levels.observe(player.actor.player(),
                                         experienceLevel(player.actor.save().experience()))) {
            changes.push_back(*change);
        }
        PartyRecords::advanceTime(native.players, seconds);
        native.attacks.updateProjectiles(seconds, native.players, native.targets);
        native.attacks.updateStrikes(seconds, native.players, native.targets);
        native.attacks.updateShields(seconds, native.players, native.targets);
        native.attacks.updateArmour(seconds, native.players, native.targets);
        native.effects.update(seconds);
    }
};

TEST_CASE("combat transaction resumes native damage reward and attack-owner phases exactly",
          "[rollback][combat-rollback-checkpoint]") {
    const f32 seconds = GENERATE(1.0f / 60, 1.0f / 30);
    NativeFixture fixture;
    for (s32 tick = 0; tick < 9; ++tick) {
        fixture.step(tick, seconds);
    }
    const auto baseline = fixture.state();
    const auto uploads = fixture.native.device.texturesCreated;
    const auto helps = fixture.native.helps.size();
    std::vector<Boundary::State> expected;
    fixture.cues.clear();
    fixture.changes.clear();
    for (s32 tick = 9; tick < 150; ++tick) {
        fixture.step(tick, seconds);
        expected.push_back(fixture.state());
    }
    const auto cues = fixture.cues;
    const auto changes = fixture.changes;
    REQUIRE(expected.back().players[0].levelKills > baseline.players[0].levelKills);
    REQUIRE(expected.back().attacks.potions.empty());
    REQUIRE(expected.back().arsenal.casts.empty());
    const auto helpsAfter = fixture.native.helps.size();
    REQUIRE(fixture.owner.restore(baseline));
    CHECK(fixture.state() == baseline);
    CHECK(fixture.native.helps.size() == helpsAfter);
    CHECK(fixture.cues == cues); // Restore is not a second hit, sound, or inventory payment.
    fixture.cues.clear();
    fixture.changes.clear();
    for (s32 tick = 9; tick < 150; ++tick) {
        CAPTURE(tick, seconds);
        fixture.step(tick, seconds);
        CHECK(fixture.state() == expected[static_cast<usize>(tick - 9)]);
    }
    CHECK(fixture.cues == cues);
    REQUIRE(fixture.changes.size() == changes.size());
    for (usize index = 0; index < changes.size(); ++index) {
        CHECK(fixture.changes[index].player == changes[index].player);
        CHECK(fixture.changes[index].from == changes[index].from);
        CHECK(fixture.changes[index].to == changes[index].to);
    }
    CHECK(fixture.native.helps.size() - helpsAfter == helpsAfter - helps);
    CHECK(fixture.native.device.texturesCreated == uploads);
}

TEST_CASE("combat transaction rejects a last-owner resource failure without partial restoration",
          "[rollback][combat-rollback-checkpoint]") {
    NativeFixture fixture;
    const auto baseline = fixture.state();
    auto target = baseline;
    target.players[0].save.gold += 100;
    target.arsenal.nextPotionKind = 4;
    target.levels.marks[0].level = 79;
    REQUIRE_FALSE(target.effects.effects.empty());
    target.effects.effects.back().resource = 9999; // Valid value, absent trusted topology.
    REQUIRE(target.valid());
    CHECK_FALSE(fixture.owner.prepareRestore(target));
    CHECK_FALSE(fixture.owner.restore(target));
    CHECK(fixture.state() == baseline);
}

TEST_CASE("combat transaction keeps native party order and validates complete ownership",
          "[rollback][combat-rollback-checkpoint]") {
    SmallFixture fixture;
    const auto baseline = fixture.state();
    REQUIRE(baseline.players[0].player == 3);
    REQUIRE(baseline.players[1].player == 1);
    for (s32 mutation = 0; mutation < 7; ++mutation) {
        CAPTURE(mutation);
        auto target = baseline;
        target.players[0].save.gold = 79;
        switch (mutation) {
        case 0: target.players[1].save.color = 1; break; // Last prepared native binding fails.
        case 1: std::swap(target.players[0], target.players[1]); break;
        case 2: target.attacks.actors[1].player = 0; break;
        case 3: target.levels.marks[1].player = 0; break;
        case 4: target.players[1] = target.players[0]; break;
        case 5: target.health.words.fill(0); break;
        case 6: target.players.resize(5); break;
        default:
            break;
        }
        CHECK_FALSE(fixture.owner->prepareRestore(target));
        CHECK(fixture.state() == baseline);
    }
    auto target = baseline;
    target.players[0].save.gold = 79;
    target.players[1].save.gold = 21;
    REQUIRE(fixture.owner->restore(target));
    CHECK(fixture.state() == target);
    const std::array<PlayerRuntimeCheckpoint*, 2> duplicates{fixture.ordered[0],
                                                             fixture.ordered[0]};
    const Boundary wrong(duplicates, fixture.health, fixture.levels, fixture.attacks,
                         fixture.arsenal, fixture.effects, {});
    CHECK_FALSE(wrong.checkpoint());
    CHECK_FALSE(wrong.prepareRestore(target));
}

TEST_CASE("combat captures retain child validation and native participant links",
          "[rollback][combat-rollback-checkpoint][checkpoint-capture-validation]") {
    SmallFixture fixture;
    const auto before = fixture.state();
    const auto nan = std::numeric_limits<f32>::quiet_NaN();
    for (s32 mutation = 0; mutation < 6; ++mutation) {
        CAPTURE(mutation);
        auto invalid = before;
        switch (mutation) {
        case 0: invalid.players[0].actor.position.x = nan; break;
        case 1: invalid.health.words.fill(0); break;
        case 2: invalid.arsenal.missiles.ricochetIn = nan; break;
        case 3: invalid.effects.banks.push_back({1, {0, nan}}); break;
        case 4: invalid.levels.marks.front().player = 0; break;
        case 5: invalid.attacks.actors.front().player = 0; break;
        default:
            break;
        }
        CHECK_FALSE(invalid.valid());
        CHECK_FALSE(fixture.owner->prepareRestore(invalid));
        CHECK(fixture.state() == before);
    }
    fixture.players[0].actor.place({nan, 0, 0});
    CHECK_FALSE(fixture.owner->checkpoint());
    fixture.players[0].actor.place(before.players[0].actor.position);
    CHECK(fixture.state() == before);
    fixture.levels.observe(0, 1); // Valid leaf, but the participant is outside this transaction.
    CHECK_FALSE(fixture.owner->checkpoint());
    fixture.levels.forget(0);
    CHECK(fixture.state() == before);
}

TEST_CASE("combat prepared restoration refuses stale participants before changing any owner",
          "[rollback][combat-rollback-checkpoint]") {
    const s32 changed = GENERATE(0, 1, 2, 3, 4, 5, 6);
    SmallFixture fixture;
    const auto baseline = fixture.state();
    auto target = baseline;
    target.players[0].save.gold = 113;
    target.players[1].save.gold = 119;
    auto prepared = fixture.owner->prepareRestore(target);
    REQUIRE(prepared);
    switch (changed) {
    case 0: fixture.effects.clear(); break;
    case 1: fixture.arsenal.clear(); break;
    case 2: fixture.attacks.clear(); break;
    case 3: fixture.levels.observe(3, 2); break;
    case 4: REQUIRE(fixture.health.restore(Mt19937{1234}.checkpoint())); break;
    case 5: REQUIRE(fixture.runtimes[1]->restore(baseline.players[1])); break;
    case 6:
        fixture.levels.forget(3);
        fixture.levels.observe(3, 1); // Same marks, different native order.
        break;
    default:
        break;
    }
    const auto before = fixture.state();
    REQUIRE_FALSE(fixture.owner->canCommit(*prepared));
    CHECK_FALSE(fixture.owner->commitRestore(std::move(*prepared)));
    CHECK(fixture.state() == before);
    CHECK(fixture.players[0].actor.save().gold == baseline.players[0].save.gold);
}

TEST_CASE("combat prepared values are single use owner bound and invalidate sibling transactions",
          "[rollback][combat-rollback-checkpoint]") {
    SmallFixture fixture;
    const SmallFixture other;
    const auto baseline = fixture.state();
    auto target = baseline;
    target.players[0].save.gold = 777;
    auto prepared = fixture.owner->prepareRestore(target);
    auto sibling = fixture.owner->prepareRestore(baseline);
    REQUIRE(prepared);
    REQUIRE(sibling);
    CHECK_FALSE(other.owner->canCommit(*prepared));
    CHECK_FALSE(other.owner->commitRestore(std::move(*prepared)));
    CHECK(fixture.state() == baseline);
    REQUIRE(fixture.owner->commitRestore(std::move(*prepared)));
    CHECK(fixture.state() == target);
    CHECK_FALSE(fixture.owner->commitRestore(std::move(*prepared)));
    CHECK_FALSE(fixture.owner->canCommit(*sibling));
    CHECK_FALSE(fixture.owner->commitRestore(std::move(*sibling)));
    CHECK(fixture.state() == target);

    alignas(Boundary) std::array<std::byte, sizeof(Boundary)> storage{};
    auto* temporary = std::construct_at(
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): byte view of a trivially copyable buffer
        reinterpret_cast<Boundary*>(storage.data()), fixture.ordered, fixture.health,
        fixture.levels, fixture.attacks, fixture.arsenal, fixture.effects, Boundary::Resources{});
    auto old = temporary->prepareRestore(baseline);
    REQUIRE(old);
    std::destroy_at(temporary);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): byte view of a trivially copyable buffer
    temporary = std::construct_at(reinterpret_cast<Boundary*>(storage.data()), fixture.ordered,
                                  fixture.health, fixture.levels, fixture.attacks, fixture.arsenal,
                                  fixture.effects, Boundary::Resources{});
    CHECK_FALSE(temporary->canCommit(*old));
    CHECK_FALSE(temporary->commitRestore(std::move(*old)));
    CHECK(fixture.state() == target);
    std::destroy_at(temporary);
}

TEST_CASE("combat bundle rejects mismatched shared effect and arsenal bindings",
          "[rollback][combat-rollback-checkpoint]") {
    NativeFixture fixture;
    const auto baseline = fixture.state();
    EffectTrees unrelatedEffects;
    const Boundary wrongEffects(fixture.ordered, fixture.health, fixture.levels,
                                fixture.native.attacks, fixture.native.arsenal, unrelatedEffects,
                                {fixture.missiles, {}, fixture.native.resources});
    CHECK_FALSE(wrongEffects.checkpoint());
    CHECK_FALSE(wrongEffects.prepareRestore(baseline));
    PlayerArsenal unrelatedArsenal;
    unrelatedArsenal.bind({fixture.native.device,
                           fixture.native.classes,
                           fixture.native.weapons,
                           fixture.native.world.collision(),
                           fixture.native.effects,
                           fixture.native.audio,
                           nullptr,
                           {}});
    const Boundary wrongArsenal(fixture.ordered, fixture.health, fixture.levels,
                                fixture.native.attacks, unrelatedArsenal, fixture.native.effects,
                                {fixture.missiles, {}, fixture.native.resources});
    CHECK_FALSE(wrongArsenal.checkpoint());
    CHECK_FALSE(wrongArsenal.prepareRestore(baseline));
    CHECK(fixture.state() == baseline);
    unrelatedArsenal.clear();
}

TEST_CASE("combat rewind undoes native damage random progression and attack updates",
          "[rollback][combat-rollback-checkpoint][combat-rewind]") {
    NativeFixture fixture;
    auto& player = fixture.native.players[0];
    player.actor.save().progress().health = 51;
    const auto baseline = fixture.state();
    const auto uploads = fixture.native.device.texturesCreated;
    const auto helps = fixture.native.helps;
    auto stale = fixture.owner.prepareRestore(baseline);
    auto rewind = fixture.owner.prepareRewind();
    auto sibling = fixture.owner.prepareRewind();
    REQUIRE(stale);
    REQUIRE(rewind);
    REQUIRE(sibling);
    const auto* stats = fixture.native.classes.stats(0);
    REQUIRE(stats);
    const std::array blows{EnemyBlow{
        .player = 3,
        .kind = kGruntKind,
        .damage = armorDefense(*stats, player.actor.save().progress()) + 10,
    }};
    usize sounds = 0;
    const auto impact = PartyDamage::enemy(blows[0], player, {});
    REQUIRE(impact);
    PartyDamage::hurt(fixture.health, player, blows[0].damage, impact->kind, true, {},
                      {.sound = [&](std::string_view) { ++sounds; },
                       .cry = [&](std::string_view) { ++sounds; },
                       .named = [&](std::string_view, f32) { ++sounds; },
                       .logDeaths = false},
                      impact->impact, stats);
    REQUIRE(sounds > 0);
    REQUIRE(player.actor.save().health() < 50);
    REQUIRE(fixture.health.checkpoint() != baseline.health);
    PartyRecords::award(fixture.native.players, 3,
                        levelExperience(81) - player.actor.save().experience() + 1, true, nullptr);
    const auto level = fixture.levels.observe(3, experienceLevel(player.actor.save().experience()));
    REQUIRE(level);
    CHECK(level->from == 80);
    CHECK(level->to == 81);
    player.figure->animate(0, 1, 1.0f / 60, PlayerDeed::None);
    fixture.native.attacks.updateProjectiles(1.0f / 60, fixture.native.players,
                                             fixture.native.targets);
    fixture.native.effects.update(1.0f / 60);
    REQUIRE(fixture.state() != baseline);
    REQUIRE_FALSE(fixture.owner.canCommit(*stale));
    REQUIRE(fixture.owner.canRewind(*rewind));
    REQUIRE(fixture.owner.rewind(std::move(*rewind)));
    CHECK(fixture.state() == baseline);
    CHECK_FALSE(fixture.owner.canRewind(*rewind));
    CHECK_FALSE(fixture.owner.canRewind(*sibling));
    CHECK(fixture.cues.empty()); // Native outbox was buffered, never delivered.
    CHECK(fixture.native.helps == helps);
    CHECK(fixture.native.device.texturesCreated == uploads);
}

TEST_CASE("combat rewind retains owner resource and sibling invalidation checks",
          "[rollback][combat-rollback-checkpoint][combat-rewind]") {
    const s32 mutation = GENERATE(0, 1, 2, 3, 4);
    SmallFixture fixture;
    const SmallFixture other;
    const auto baseline = fixture.state();
    auto rewind = fixture.owner->prepareRewind();
    REQUIRE(rewind);
    CHECK_FALSE(other.owner->canRewind(*rewind));
    CHECK_FALSE(other.owner->rewind(std::move(*rewind)));
    REQUIRE(fixture.owner->canRewind(*rewind));
    switch (mutation) {
    case 0: fixture.effects.clear(); break;
    case 1: fixture.arsenal.clear(); break;
    case 2: fixture.attacks.clear(); break;
    case 3: REQUIRE(fixture.runtimes[1]->restore(baseline.players[1])); break;
    case 4: REQUIRE(fixture.owner->restore(baseline)); break;
    default:
        break;
    }
    const auto current = fixture.state();
    CHECK_FALSE(fixture.owner->canRewind(*rewind));
    CHECK_FALSE(fixture.owner->rewind(std::move(*rewind)));
    CHECK(fixture.state() == current);

    alignas(Boundary) std::array<std::byte, sizeof(Boundary)> storage{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): byte view of a trivially copyable buffer
    auto* owner = std::construct_at(reinterpret_cast<Boundary*>(storage.data()), fixture.ordered,
                                    fixture.health, fixture.levels, fixture.attacks,
                                    fixture.arsenal, fixture.effects, Boundary::Resources{});
    auto stale = owner->prepareRewind();
    REQUIRE(stale);
    std::destroy_at(owner);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): byte view of a trivially copyable buffer
    owner = std::construct_at(reinterpret_cast<Boundary*>(storage.data()), fixture.ordered,
                              fixture.health, fixture.levels, fixture.attacks, fixture.arsenal,
                              fixture.effects, Boundary::Resources{});
    CHECK_FALSE(owner->canRewind(*stale));
    CHECK_FALSE(owner->rewind(std::move(*stale)));
    CHECK(fixture.state() == current);
    std::destroy_at(owner);
}
} // namespace
