#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"

#include "game/players/Progression.h"
#include "game/screens/AttackCheckpointFixture.h"
#include "game/screens/HelpMessages.h"
#include "game/screens/OrdinaryCombatCheckpoint.h"
#include "game/screens/PartyDamage.h"
#include "game/screens/PartyRecords.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Boundary = OrdinaryCombatCheckpoint;

struct Fixture {
    test::AttackCheckpointFixture native;
    WorldLayout layout;
    PlayerRuntimeCheckpoint runtime{native.players[0], native.classes.stats(0)};
    std::array<PlayerRuntimeCheckpoint*, 1> ordered{&runtime};
    PlayerHealth health;
    LevelWatch levels;
    CombatRollbackCheckpoint combat{ordered,
                                    health,
                                    levels,
                                    native.attacks,
                                    native.arsenal,
                                    native.effects,
                                    {native.missileResources, {}, native.resources}};
    Boundary boundary{combat, native.opponents};
    s32 enemy = -1;
    std::vector<std::string> outputs;

    Fixture() {
        auto& player = native.players[0];
        player.actor.save().progress().health = 2000;
        player.entrySave = player.actor.save();
        levels.observe(3, 1);
        native.opponents.open({native.device, native.world, native.weapons, native.effects,
                               native.audio, native.root, 1},
                              native.players);
        auto& enemies = native.opponents.enemies();
        enemies.open(native.device, native.root, nullptr, 4, {.health = 1000}, 29);
        REQUIRE(enemies.loadKind(kGruntKind));
        writeTextFile(native.root / "world.json", R"({
            "objects":[{"name":"GROUND","position":[0,0,0]}],
            "itemInfos":[{"type":3,"name":"GRU","radius":2,"height":5,"hitPoints":10}],
            "itemInstances":[{"info":0,"minPlayers":1,"position":[20,0,0],
                "params":[1,0,7,0,5,0,20,0,0,0,0,0]}]})");
        REQUIRE(layout.load(native.root));
        REQUIRE(native.opponents.generators().bind(native.device, layout, enemies, nullptr, {}, 1));
        // Keep the generator beyond the authored burst's radius plus its own
        // radius: it must survive until the deliberate queued-reward boundary.
        const auto generator = native.opponents.generators().checkpoint();
        REQUIRE(generator);
        REQUIRE(generator->generators.size() == 1);
        REQUIRE(glm::length(generator->generators[0].position - player.actor.position()) >
                native.classes.stats(0)->moveStrikes[0].radius + layout.itemInfos()[0].radius);
        const auto spawned = enemies.spawn(
            {.kind = kGruntKind, .position = {0, 0, 5}, .generator = 0, .placed = true}, {});
        REQUIRE(spawned);
        enemy = *spawned;
        for (s32 hit = 0; hit < 9; ++hit) {
            native.opponents.strikeEnemy(enemy, 5, 0, {0, 0, 1}, 3, native.players);
        }
        REQUIRE(state().adapter.hitStreak[3] == 9);
        player.turbo.add(100);
    }

    Boundary::State state() const {
        const auto captured = boundary.checkpoint();
        REQUIRE(captured);
        REQUIRE(captured->valid());
        return *captured;
    }
    LevelOpponents::Events events() {
        LevelOpponents::Events result;
        result.hurt = [this](usize index, f32 amount, HurtKind kind, bool directed,
                             const PlayerImpact& impact) {
            auto& player = native.players[index];
            const PlayerHealth::Events feedback{
                .sound = [this](std::string_view cue) { outputs.emplace_back(cue); },
                .cry = [this](std::string_view cue) { outputs.emplace_back(cue); },
                .named = [this](std::string_view cue, f32) { outputs.emplace_back(cue); }};
            health.hurt(player, amount, kind, directed, false, 1, feedback, impact, false,
                        native.classes.stats(player.actor.save().character));
            outputs.push_back("hurt:" + std::to_string(player.actor.player()));
        };
        result.award = [this](s32 player, s32 amount, bool killed) {
            outputs.push_back("award:" + std::to_string(player) + ":" + std::to_string(amount) +
                              (killed ? ":kill" : ":hit"));
            PartyRecords::award(native.players, player, amount, killed, nullptr);
        };
        result.destroyedGenerator = [this](s32 player) {
            outputs.push_back("generator:" + std::to_string(player));
            PartyRecords::destroyedGenerator(native.players, player);
        };
        result.levels = [this] {
            for (const auto& player : native.players) {
                if (const auto changed = levels.observe(
                        player.actor.player(), experienceLevel(player.actor.save().experience()))) {
                    outputs.push_back("level:" + std::to_string(changed->to));
                }
            }
        };
        result.help = [this](s32 lesson, usize index) {
            CHECK(index == 0); // input identity 3 is party entry zero
            outputs.push_back("lesson:" + std::to_string(lesson));
            return true;
        };
        result.settleBlasts = [] {};
        result.advanceLegend = [](f32) {};
        result.advanceVictory = [](s32, f32) {};
        return result;
    }
    void step(s32 tick, f32 seconds) {
        auto& player = native.players[0];
        const s32 ticks = seconds > 0.02f ? 2 : 1;
        player.figure->animate(0, ticks, seconds,
                               tick == 0 ? PlayerDeed::TurboStrong : PlayerDeed::None);
        native.attacks.updateTurbo(0, ticks, seconds, native.players, [](s32, usize) {});
        // The ordinary scene ordering: enemy phase delivers preceding damage
        // and rewards; player projectile/strike/shield phases queue new ones.
        native.opponents.update(ticks, seconds, native.players, {}, events());
        native.attacks.updateProjectiles(seconds, native.players, native.targets);
        native.attacks.updateStrikes(seconds, native.players, native.targets);
        native.attacks.updateShields(seconds, native.players, native.targets);
        native.attacks.updateArmour(seconds, native.players, native.targets);
        if (tick == 30) {
            REQUIRE(native.opponents.generators().healthOf(0) > 0);
            native.opponents.strikeEnemy(enemy, 1000000, 0, {0, 0, 1}, 3, native.players);
            native.opponents.strikeGenerator(0, 1000000, 3, native.players);
        }
        PartyRecords::advanceTime(native.players, seconds);
        native.effects.update(seconds);
    }
};

TEST_CASE("checkpoint timing observes a single native capture without changing its state",
          "[netplay][ordinary-combat-checkpoint][checkpoint-timing]") {
    Fixture fixture;
    const auto baseline = fixture.state();
    Boundary::CaptureTimings timing;
    timing.fill(123);
    const auto measured = fixture.boundary.checkpoint(&timing);
    REQUIRE(measured);
    CHECK(static_cast<bool>(*measured == baseline));
    CHECK(timing[static_cast<usize>(Boundary::CaptureOwner::Bindings)]);
    CHECK(timing[static_cast<usize>(Boundary::CaptureOwner::Combat)]);
    CHECK(timing[static_cast<usize>(Boundary::CaptureOwner::Pools)]);
    CHECK(timing[static_cast<usize>(Boundary::CaptureOwner::Adapter)]);
    CHECK(timing[static_cast<usize>(Boundary::CaptureOwner::Links)]);
    CHECK_FALSE(timing[static_cast<usize>(Boundary::CaptureOwner::World)]);
    CHECK_FALSE(timing[static_cast<usize>(Boundary::CaptureOwner::Fixtures)]);
    CHECK_FALSE(timing[static_cast<usize>(Boundary::CaptureOwner::Camera)]);
    CHECK_FALSE(timing[static_cast<usize>(Boundary::CaptureOwner::Portals)]);
    CHECK_FALSE(timing[static_cast<usize>(Boundary::CaptureOwner::Transporters)]);
    CHECK(static_cast<bool>(fixture.state() == baseline));
}

TEST_CASE("ordinary combat transaction rewinds native timed hits losses and deferred rewards",
          "[ordinary-combat-checkpoint]") {
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    CAPTURE(seconds);
    Fixture fixture;
    // The row begins at authored frame one, not simulation tick one. Capture
    // its actual native birth at either rate, before the delayed damage starts.
    s32 firstReplayTick = 0;
    while (firstReplayTick < 12 && fixture.native.attacks.strikes().count() == 0) {
        fixture.step(firstReplayTick++, seconds);
    }
    const auto baseline = fixture.state();
    REQUIRE_FALSE(baseline.combat.attacks.strikes.strikes.empty());
    REQUIRE(firstReplayTick < 12);
    REQUIRE(baseline.combat.attacks.strikes.strikes[0].secondsLeft >
            baseline.combat.attacks.strikes.strikes[0].damageTime);
    const f32 initialHealth = baseline.opponents.enemies.enemies[fixture.enemy].health;
    const auto uploads = fixture.native.device.texturesCreated;
    std::vector<Boundary::State> expected;
    fixture.outputs.clear();
    bool timedHit = false;
    bool queuedLoss = false;
    bool queuedGenerator = false;
    for (s32 tick = firstReplayTick; tick < 100; ++tick) {
        fixture.step(tick, seconds);
        expected.push_back(fixture.state());
        const auto& state = expected.back();
        timedHit |=
            tick < 30 && state.opponents.enemies.enemies[fixture.enemy].health < initialHealth;
        queuedLoss |= !state.opponents.enemies.losses.empty();
        queuedGenerator |= !state.adapter.generatorRewards.empty();
    }
    REQUIRE(timedHit);
    REQUIRE(queuedLoss);
    REQUIRE(queuedGenerator);
    REQUIRE(expected.back().combat.players[0].levelKills > baseline.combat.players[0].levelKills);
    REQUIRE(std::ranges::find(fixture.outputs, "generator:3") != fixture.outputs.end());
    REQUIRE(std::ranges::find(fixture.outputs,
                              "lesson:" + std::to_string(HelpMessages::kDestroyGenerators)) !=
            fixture.outputs.end());
    const auto outputs = fixture.outputs;
    REQUIRE(fixture.boundary.restore(baseline));
    CHECK(fixture.state() == baseline);
    CHECK(fixture.outputs == outputs); // restoring does not deliver a hit/reward twice
    fixture.outputs.clear();
    for (s32 tick = firstReplayTick; tick < 100; ++tick) {
        CAPTURE(tick, seconds);
        fixture.step(tick, seconds);
        CHECK(fixture.state() == expected[static_cast<usize>(tick - firstReplayTick)]);
    }
    CHECK(fixture.outputs == outputs);
    CHECK(fixture.native.device.texturesCreated == uploads);

    // Capture at the actual queued damage boundary too, before the next native
    // opponent phase consumes those losses and the generator reward.
    const auto& pending = expected[static_cast<usize>(30 - firstReplayTick)];
    REQUIRE_FALSE(pending.adapter.generatorRewards.empty());
    REQUIRE(fixture.boundary.restore(pending));
    fixture.outputs.clear();
    fixture.native.opponents.settleRewards(fixture.native.players, fixture.events());
    const auto settled = fixture.state();
    REQUIRE(settled.adapter.generatorRewards.empty());
    REQUIRE(settled.opponents.enemies.losses.empty());
    const auto once = fixture.outputs;
    fixture.native.opponents.settleRewards(fixture.native.players, fixture.events());
    CHECK(fixture.outputs == once);
    REQUIRE(fixture.boundary.restore(pending));
    fixture.outputs.clear();
    fixture.native.opponents.settleRewards(fixture.native.players, fixture.events());
    CHECK(fixture.outputs == once);
    CHECK(fixture.state() == settled);
}

TEST_CASE("ordinary combat transaction rejects incoming foreign participants and special owners",
          "[ordinary-combat-checkpoint]") {
    Fixture fixture;
    const auto baseline = fixture.state();
    for (s32 mutation = 0; mutation < 13; ++mutation) {
        CAPTURE(mutation);
        auto target = baseline;
        target.combat.players[0].save.gold = 999;
        switch (mutation) {
        case 0: target.opponents.enemies.enemies[fixture.enemy].target = 0; break;
        case 1: target.opponents.enemies.blows.push_back({.player = 0, .kind = kGruntKind}); break;
        case 2: target.opponents.enemies.losses[0].player = 0; break;
        case 3: target.adapter.generatorRewards.push_back({0, 10, false}); break;
        case 4: target.adapter.lessons.push_back({HelpMessages::kDestroyGenerators, 0}); break;
        case 5: target.adapter.hitStreak[0] = 1; break;
        case 6:
            target.opponents.enemies.deathEvents.push_back({DeathEvent::Kind::Killed, 0, 3});
            break;
        case 7: target.opponents.enemies.tagged.push_back(3); break;
        case 8: target.opponents.enemies.enemies[fixture.enemy].kind = kDeathKind; break;
        case 9: target.opponents.enemies.scales.bossEncounter = true; break;
        case 10: target.opponents.placements.reset(); break;
        case 11: target.adapter.cueEffects.push_back(target.combat.effects.nextId); break;
        case 12: target.opponents.missiles.hits.push_back({.player = 0}); break;
        }
        CHECK_FALSE(target.valid());
        CHECK_FALSE(fixture.boundary.prepareRestore(target));
        CHECK_FALSE(fixture.boundary.restore(target));
        CHECK(fixture.state() == baseline);
    }
}

TEST_CASE("ordinary combat transaction preflights late trusted resource and roster failures",
          "[ordinary-combat-checkpoint]") {
    Fixture fixture;
    const auto baseline = fixture.state();
    for (s32 mutation = 0; mutation < 3; ++mutation) {
        CAPTURE(mutation);
        auto target = baseline;
        target.combat.players[0].save.gold = 123;
        target.adapter.hitStreak[3] = 2;
        if (mutation == 0) {
            target.opponents.generators.roster[0].position.x += 1; // different native level binding
            target.opponents.generators.generators[0].position.x += 1;
        } else if (mutation == 1) {
            PlacedEnemies::Placement placement;
            placement.kind = kGruntKind;
            placement.spawn.kind = kGruntKind;
            target.opponents.placements->roster.push_back(placement);
        } else {
            EnemyMissiles::Checkpoint::Flight shot;
            shot.instance = 1;
            shot.kind = EnemyMissileKind::arrow();
            shot.secondsLeft = 1;
            shot.resource = 989;
            target.opponents.missiles.missiles.push_back(shot);
            target.opponents.missiles.nextInstance = 2;
        }
        REQUIRE(target.valid());
        CHECK_FALSE(fixture.boundary.prepareRestore(target));
        CHECK_FALSE(fixture.boundary.restore(target));
        CHECK(fixture.state() == baseline);
    }
}

TEST_CASE("ordinary capture still validates every child and live cross-owner reference",
          "[ordinary-combat-checkpoint][checkpoint-capture-validation]") {
    Fixture fixture;
    const auto before = fixture.state();
    const auto nan = std::numeric_limits<f32>::quiet_NaN();
    for (s32 mutation = 0; mutation < 5; ++mutation) {
        CAPTURE(mutation);
        auto invalid = before;
        switch (mutation) {
        case 0: invalid.combat.players[0].actor.position.x = nan; break;
        case 1: invalid.opponents.missiles.shrink = nan; break;
        case 2: invalid.opponents.enemies.random.words.fill(0); break;
        case 3: invalid.adapter.shrink = nan; break;
        case 4: invalid.adapter.generatorRewards.push_back({0, 10, false}); break;
        }
        CHECK_FALSE(invalid.valid());
        CHECK_FALSE(fixture.boundary.prepareRestore(invalid));
        CHECK(fixture.state() == before);
    }
    // A valid independently restored pool can still reference a player absent
    // from this transaction. Capture must retain its cross-owner refusal.
    auto& enemies = fixture.native.opponents.enemies();
    auto foreign = before.opponents.enemies;
    foreign.enemies[fixture.enemy].target = 0;
    REQUIRE(foreign.valid());
    REQUIRE(enemies.restore(foreign));
    CHECK_FALSE(fixture.boundary.checkpoint());
    REQUIRE(enemies.restore(before.opponents.enemies));
    CHECK(fixture.state() == before);
}

TEST_CASE("ordinary combat refuses retained blast cooldowns for uncaptured combatants",
          "[ordinary-combat-checkpoint]") {
    Fixture fixture;
    const auto baseline = fixture.state();
    EnemyMissiles native;
    native.blast({.stages = {1}});
    const auto missiles = native.checkpoint();
    REQUIRE(missiles);
    REQUIRE(missiles->bursts.size() == 1);
    auto target = baseline;
    target.opponents.missiles.bursts = missiles->bursts;
    target.opponents.missiles.bursts[0].swarm = {{fixture.enemy, 0.5f}};
    REQUIRE(target.valid());
    REQUIRE(fixture.boundary.prepareRestore(target));
    for (const s32 id : {OpponentSimulation::kCritterIds, OpponentSimulation::kBossIds}) {
        CAPTURE(id);
        target.opponents.missiles.bursts[0].swarm[0].id = id;
        REQUIRE(target.opponents.valid()); // valid raw pool, outside this transaction's owners
        CHECK_FALSE(target.valid());
        CHECK_FALSE(fixture.boundary.prepareRestore(target));
        CHECK_FALSE(fixture.boundary.restore(target));
        CHECK(fixture.state() == baseline);
    }
}

TEST_CASE("ordinary combat plans reject child invalidation and exact owner reuse before mutation",
          "[ordinary-combat-checkpoint]") {
    const s32 mutation = GENERATE(0, 1, 2, 3, 4);
    Fixture fixture;
    const auto baseline = fixture.state();
    auto desired = baseline;
    desired.combat.players[0].save.gold = 912;
    auto plan = fixture.boundary.prepareRestore(desired);
    REQUIRE(plan);
    switch (mutation) {
    case 0: REQUIRE(fixture.combat.restore(baseline.combat)); break;
    case 1: REQUIRE(fixture.native.opponents.enemies().restore(baseline.opponents.enemies)); break;
    case 2:
        REQUIRE(fixture.native.opponents.generators().restore(baseline.opponents.generators));
        break;
    case 3:
        REQUIRE(fixture.native.opponents.placements().restore(*baseline.opponents.placements));
        break;
    case 4: fixture.native.effects.clear(); break;
    }
    const auto current = fixture.state();
    CHECK_FALSE(fixture.boundary.canCommit(*plan));
    CHECK_FALSE(fixture.boundary.commitRestore(std::move(*plan)));
    CHECK(fixture.state() == current);

    plan = fixture.boundary.prepareRestore(desired);
    auto sibling = fixture.boundary.prepareRestore(desired);
    REQUIRE(plan);
    REQUIRE(sibling);
    Boundary wrong(fixture.combat, fixture.native.opponents);
    CHECK_FALSE(wrong.commitRestore(std::move(*plan)));
    REQUIRE(fixture.boundary.commitRestore(std::move(*plan)));
    CHECK(fixture.state() == desired);
    CHECK_FALSE(fixture.boundary.canCommit(*sibling));
    CHECK_FALSE(fixture.boundary.commitRestore(std::move(*plan)));

    alignas(Boundary) std::array<std::byte, sizeof(Boundary)> storage;
    auto* owner = std::construct_at(reinterpret_cast<Boundary*>(storage.data()), fixture.combat,
                                    fixture.native.opponents);
    auto stale = owner->prepareRestore(desired);
    REQUIRE(stale);
    std::destroy_at(owner);
    owner = std::construct_at(reinterpret_cast<Boundary*>(storage.data()), fixture.combat,
                              fixture.native.opponents);
    CHECK_FALSE(owner->commitRestore(std::move(*stale)));
    CHECK(fixture.state() == desired);
    std::destroy_at(owner);
}

TEST_CASE("ordinary transaction cannot combine player and opponent resources from different levels",
          "[ordinary-combat-checkpoint]") {
    Fixture fixture;
    LevelOpponents other;
    LevelWorld unrelatedWorld;
    other.open({fixture.native.device, unrelatedWorld, fixture.native.weapons,
                fixture.native.effects, fixture.native.audio, fixture.native.root, 1},
               fixture.native.players);
    Boundary wrong(fixture.combat, other);
    const auto before = fixture.state();
    CHECK_FALSE(wrong.checkpoint());
    CHECK_FALSE(wrong.prepareRestore(before));
    CHECK(fixture.state() == before);
    other.close();
}

TEST_CASE("ordinary rewind undoes native damage rewards placements and a late refused interaction",
          "[ordinary-combat-checkpoint][combat-rewind]") {
    Fixture fixture;
    auto& player = fixture.native.players[0];
    auto& opponents = fixture.native.opponents;
    player.actor.save().progress().health = 51;
    PlacedEnemies::Placement placed;
    placed.kind = kGruntKind;
    placed.spawn.kind = kGruntKind;
    placed.spawn.position = {0, 0, 15};
    placed.spawn.placed = true;
    placed.spawn.algorithm = kWanderWay;
    placed.viewRadius = 1;
    opponents.placements().bind({placed});
    const auto baseline = fixture.state();
    const auto uploads = fixture.native.device.texturesCreated;
    const auto helps = fixture.native.helps;
    const auto* stats = fixture.native.classes.stats(0);
    REQUIRE(stats);
    std::optional<Boundary::State> rejected;
    std::vector<PartyDamage::Output> expectedDamage;
    std::vector<std::string> expectedRewards;
    s32 spawnCalls = 0;
    for (s32 attempt = 0; attempt < 2; ++attempt) {
        CAPTURE(attempt);
        auto stale = fixture.boundary.prepareRestore(baseline);
        auto rewind = fixture.boundary.prepareRewind();
        auto sibling = fixture.boundary.prepareRewind();
        REQUIRE(stale);
        REQUIRE(rewind);
        REQUIRE(sibling);
        opponents.placements().observe(ViewVolume{}, {1, 0, 0});
        opponents.placements().activate([&](const auto& record) {
            ++spawnCalls;
            REQUIRE(
                opponents.enemies().spawn(record.spawn, {}, opponents.generators().obstacles()));
        });
        REQUIRE(opponents.placements().count() == 0);
        // Even an empty activation changes the stale-restore revision, but is
        // ordinary gameplay that a preallocated full-owner undo must tolerate.
        opponents.placements().activate([](const auto&) { FAIL("duplicate placed birth"); });
        const std::array firstBlow{EnemyBlow{
            .player = 3,
            .kind = kGruntKind,
            .damage = armorDefense(*stats, player.actor.save().progress()) + 10,
        }};
        auto damage = PartyDamage::stageBlows(firstBlow, fixture.native.players, fixture.health,
                                              fixture.native.classes, {}, {});
        REQUIRE(damage.status == PartyDamage::Status::Applied);
        REQUIRE_FALSE(damage.outputs.empty());
        REQUIRE(fixture.health.checkpoint() != baseline.combat.health);
        REQUIRE(player.actor.save().health() < 50);

        const auto beforeKill = opponents.enemies().checkpoint();
        REQUIRE(beforeKill);
        // The nine setup hits still belong to this enemy's pending damage: no
        // native enemy phase has consumed them. Kill with its remaining health
        // and authored armor, not an arbitrary million-point hit which would
        // put that accumulator outside the checkpoint's numeric bounds.
        const f32 lethal =
            opponents.enemies().healthOf(fixture.enemy) + enemyKind(kGruntKind).armor + 1;
        opponents.strikeEnemy(fixture.enemy, lethal, 0, {0, 0, 1}, 3, fixture.native.players);
        REQUIRE(opponents.enemies().healthOf(fixture.enemy) <= 0);
        const auto afterKill = opponents.enemies().checkpoint();
        REQUIRE(afterKill);
        REQUIRE(afterKill->enemies[fixture.enemy].hurtPending >
                beforeKill->enemies[fixture.enemy].hurtPending);
        opponents.strikeGenerator(0, 1000000, 3, fixture.native.players);
        const auto pending = fixture.state();
        REQUIRE_FALSE(pending.opponents.enemies.losses.empty());
        REQUIRE_FALSE(pending.adapter.generatorRewards.empty());
        std::vector<std::string> rewards;
        auto events = fixture.events();
        events.award = [&](s32 slot, s32 amount, bool killed) {
            rewards.push_back("award:" + std::to_string(amount));
            PartyRecords::award(fixture.native.players, slot, amount, killed, nullptr);
        };
        events.destroyedGenerator = [&](s32 slot) {
            rewards.push_back("generator");
            PartyRecords::destroyedGenerator(fixture.native.players, slot);
        };
        events.levels = [&] {
            if (const auto changed =
                    fixture.levels.observe(3, experienceLevel(player.actor.save().experience()))) {
                rewards.push_back("level:" + std::to_string(changed->to));
            }
        };
        events.help = [&](s32 lesson, usize index) {
            REQUIRE(index == 0);
            rewards.push_back("lesson:" + std::to_string(lesson));
            return true;
        };
        opponents.settleRewards(fixture.native.players, events);
        REQUIRE_FALSE(rewards.empty());
        const s32 beforeLevel = experienceLevel(player.actor.save().experience());
        PartyRecords::award(fixture.native.players, 3,
                            levelExperience(beforeLevel + 1) - player.actor.save().experience() + 1,
                            false, nullptr);
        events.levels();
        REQUIRE(fixture.levels.checkpoint() != baseline.combat.levels);
        REQUIRE(player.levelKills > baseline.combat.players[0].levelKills);
        REQUIRE(fixture.state().adapter.generatorRewards.empty());
        REQUIRE(fixture.state().opponents.enemies.losses.empty());

        const s32 healthBeforeLateBatch = player.actor.save().health();
        const std::array late{
            EnemyBlow{.player = 3,
                      .kind = kGruntKind,
                      .damage = armorDefense(*stats, player.actor.save().progress()) + 2},
            EnemyBlow{.player = 3, .kind = kGruntKind, .ward = EnemyMeleeWard::HealthVamp}};
        const auto refused = PartyDamage::stageBlows(late, fixture.native.players, fixture.health,
                                                     fixture.native.classes, {}, {});
        REQUIRE(refused.status == PartyDamage::Status::Unsupported);
        REQUIRE(refused.outputs.empty());
        REQUIRE(player.actor.save().health() < healthBeforeLateBatch);
        const auto mutated = fixture.state();
        REQUIRE(mutated.opponents.enemies.nextInstance > baseline.opponents.enemies.nextInstance);
        REQUIRE_FALSE(fixture.boundary.canCommit(*stale));
        REQUIRE(fixture.boundary.canRewind(*rewind));
        if (rejected) {
            CHECK(mutated == *rejected);
            CHECK(damage.outputs == expectedDamage);
            CHECK(rewards == expectedRewards);
        } else {
            rejected = mutated;
            expectedDamage = damage.outputs;
            expectedRewards = rewards;
        }
        // The outer phase discards every earlier outbox too, not only the
        // refused damage batch. Rewind itself dispatches no callback.
        damage.outputs.clear();
        rewards.clear();
        REQUIRE(fixture.boundary.rewind(std::move(*rewind)));
        CHECK(fixture.state() == baseline);
        CHECK_FALSE(fixture.boundary.canRewind(*rewind));
        CHECK_FALSE(fixture.boundary.canRewind(*sibling));
        CHECK(spawnCalls == attempt + 1);
        CHECK(fixture.outputs.empty());
        CHECK(fixture.native.helps == helps);
        CHECK(fixture.native.device.texturesCreated == uploads);
    }
}

TEST_CASE("ordinary rewind refuses replaced children resources and owner lifetimes atomically",
          "[ordinary-combat-checkpoint][combat-rewind]") {
    const s32 mutation = GENERATE(0, 1, 2, 3, 4, 5);
    Fixture fixture;
    const auto baseline = fixture.state();
    auto rewind = fixture.boundary.prepareRewind();
    REQUIRE(rewind);
    Boundary wrong(fixture.combat, fixture.native.opponents);
    CHECK_FALSE(wrong.canRewind(*rewind));
    CHECK_FALSE(wrong.rewind(std::move(*rewind)));
    REQUIRE(fixture.boundary.canRewind(*rewind));
    switch (mutation) {
    case 0: REQUIRE(fixture.combat.restore(baseline.combat)); break;
    case 1: REQUIRE(fixture.native.opponents.enemies().restore(baseline.opponents.enemies)); break;
    case 2:
        REQUIRE(fixture.native.opponents.generators().restore(baseline.opponents.generators));
        break;
    case 3:
        fixture.native.opponents.placements().bind(baseline.opponents.placements->roster);
        break;
    case 4: fixture.native.effects.clear(); break;
    case 5: REQUIRE(fixture.boundary.restore(baseline)); break;
    }
    const auto current = fixture.state();
    CHECK_FALSE(fixture.boundary.canRewind(*rewind));
    CHECK_FALSE(fixture.boundary.rewind(std::move(*rewind)));
    CHECK(fixture.state() == current);

    alignas(Boundary) std::array<std::byte, sizeof(Boundary)> storage;
    auto* owner = std::construct_at(reinterpret_cast<Boundary*>(storage.data()), fixture.combat,
                                    fixture.native.opponents);
    auto stale = owner->prepareRewind();
    REQUIRE(stale);
    std::destroy_at(owner);
    owner = std::construct_at(reinterpret_cast<Boundary*>(storage.data()), fixture.combat,
                              fixture.native.opponents);
    CHECK_FALSE(owner->canRewind(*stale));
    CHECK_FALSE(owner->rewind(std::move(*stale)));
    CHECK(fixture.state() == current);
    std::destroy_at(owner);
}
} // namespace
