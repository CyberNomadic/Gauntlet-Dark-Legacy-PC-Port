#include <array>
#include <limits>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"

#include "AttackCheckpointFixture.h"
#include "game/screens/HelpMessages.h"
#include "game/screens/OpponentCombatCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct AdapterFixture {
    std::filesystem::path root = test::attackCheckpointAssets();
    test::FakeRenderDevice device;
    LevelWorld world;
    WorldLayout layout;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    std::array<PlayerRuntime, 1> players;
    LevelOpponents opponents;
    OpponentCombatCheckpoint adapter{opponents};

    AdapterFixture() {
        players[0].actor.spawn(2, {}, nullptr, {0, 0, 20}, 0);
        opponents.open({device, world, weapons, effects, audio, root, 1}, players);
        opponents.enemies().open(device, root, nullptr, 2, {.health = 1000}, 1);
        REQUIRE(opponents.enemies().loadKind(kGruntKind));
        writeTextFile(root / "world.json", R"({
          "objects":[{"name":"GROUND","position":[0,0,0]}],
          "itemInfos":[{"type":3,"name":"GRU","radius":2,"height":5,"hitPoints":10}],
          "itemInstances":[{"info":0,"minPlayers":1,"position":[8,0,0],
                            "params":[1,0,7,0,5,0,20,0,0,0,0,0]}]})");
        REQUIRE(layout.load(root));
        REQUIRE(opponents.generators().bind(device, layout, opponents.enemies(), nullptr, {}, 1));
        REQUIRE(opponents.generators().count() == 1);
    }
    ~AdapterFixture() { opponents.close(); }
    EffectTrees::PoolCheckpoint visual() const {
        const auto result = effects.checkpointPool();
        REQUIRE(result);
        return *result;
    }
    OpponentCombatCheckpoint::State state() const {
        const auto result = adapter.checkpoint(visual());
        REQUIRE(result);
        return *result;
    }
};

TEST_CASE("ordinary opponent adapter restores every scalar and ordered pending output",
          "[opponent-combat-checkpoint]") {
    AdapterFixture fixture;
    const auto before = fixture.state();
    auto desired = before;
    desired.hitStreak = {1, 3, 9, 11};
    desired.lessons = {{HelpMessages::kDestroyGenerators, 2},
                       {HelpMessages::kDestroyGenerators, 1}};
    desired.generatorRewards = {{2, 150, false}, {1, 450, true}, {2, 25, false}};
    desired.rockHits = {{3, 5}, {1, 10}, {3, 2}};
    desired.hearers = {{1, 2, 3}, {-4, 0, 1}};
    desired.cueEffects = {3, 1};
    desired.shrink = 0.667f;
    desired.bagSeed = 0xF03454U;
    auto visual = fixture.visual();
    visual.nextId = 4; // three previously issued cues have already expired
    REQUIRE(desired.valid());
    auto prepared = fixture.adapter.prepareRestore(desired, visual);
    REQUIRE(prepared);
    CHECK(fixture.state() == before);
    CHECK(fixture.effects.count() == 0);
    REQUIRE(fixture.adapter.commitRestore(std::move(*prepared)));
    CHECK(fixture.adapter.checkpoint(visual) == desired);
    CHECK(fixture.effects.count() == 0); // restore never recreates consumed visual work
    CHECK(fixture.opponents.takeRockHits() == desired.rockHits);
    CHECK(fixture.opponents.takeRockHits().empty());
    CHECK_FALSE(fixture.adapter.canCommit(*prepared));
}

TEST_CASE("ordinary opponent adapter preparation is owner bound and atomically validated",
          "[opponent-combat-checkpoint]") {
    AdapterFixture fixture;
    const auto before = fixture.state();
    auto invalid = before;
    switch (GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8)) {
    case 0: invalid.hitStreak[1] = -1; break;
    case 1: invalid.lessons = {{-1, 1}}; break;
    case 2: invalid.generatorRewards = {{4, 10, false}}; break;
    case 3: invalid.rockHits = {{0, std::numeric_limits<f32>::quiet_NaN()}}; break;
    case 4: invalid.hearers = {{std::numeric_limits<f32>::infinity(), 0, 0}}; break;
    case 5: invalid.cueEffects = {0}; break;
    case 6: invalid.shrink = 0; break;
    case 7: invalid.cueEffects = {1, 1}; break;
    case 8: invalid.lessons.resize(OpponentCombatCheckpoint::State::kMaxRecords + 1); break;
    }
    CHECK_FALSE(fixture.adapter.prepareRestore(invalid, fixture.visual()));
    CHECK_FALSE(fixture.adapter.restore(invalid, fixture.visual()));
    CHECK(fixture.state() == before);
}

TEST_CASE("ordinary adapter rejects future cue handles but retains native expired references",
          "[opponent-combat-checkpoint]") {
    AdapterFixture fixture;
    auto desired = fixture.state();
    auto visual = fixture.visual();
    desired.cueEffects = {1};
    REQUIRE(desired.valid());
    CHECK_FALSE(fixture.adapter.prepareRestore(desired, visual));
    visual.nextId = 2;
    REQUIRE(fixture.adapter.restore(desired, visual));
    CHECK(fixture.adapter.checkpoint(visual) == desired);
    visual.nextId = 1;
    CHECK_FALSE(fixture.adapter.checkpoint(visual));
}

TEST_CASE("ordinary adapter plans survive population correction but not close or reuse",
          "[opponent-combat-checkpoint]") {
    AdapterFixture fixture;
    OpponentCombatCheckpoint other{fixture.opponents};
    const auto before = fixture.state();
    auto desired = before;
    desired.hitStreak[2] = 7;
    auto prepared = fixture.adapter.prepareRestore(desired, fixture.visual());
    auto sibling = fixture.adapter.prepareRestore(desired, fixture.visual());
    REQUIRE(prepared);
    REQUIRE(sibling);
    CHECK_FALSE(other.commitRestore(std::move(*prepared)));
    REQUIRE(fixture.adapter.canCommit(*prepared));
    const auto population = fixture.opponents.enemies().checkpoint();
    REQUIRE(population);
    REQUIRE(fixture.opponents.enemies().restore(*population));
    REQUIRE(fixture.adapter.canCommit(*prepared));
    REQUIRE(fixture.adapter.commitRestore(std::move(*prepared)));
    CHECK_FALSE(fixture.adapter.canCommit(*sibling));
    CHECK(fixture.state() == desired);
    prepared = fixture.adapter.prepareRestore(before, fixture.visual());
    REQUIRE(prepared);
    fixture.opponents.close();
    CHECK_FALSE(fixture.adapter.canCommit(*prepared));
    fixture.opponents.open({fixture.device, fixture.world, fixture.weapons, fixture.effects,
                            fixture.audio, fixture.root, 1},
                           fixture.players);
    CHECK_FALSE(fixture.adapter.commitRestore(std::move(*prepared)));
    CHECK(fixture.state().hitStreak == std::array<s32, 4>{});
}

TEST_CASE("native hit streak and generator rewards replay in their original settlement order",
          "[opponent-combat-checkpoint][generator-lesson]") {
    AdapterFixture fixture;
    const auto enemy = fixture.opponents.enemies().spawn(
        EnemySpawn{.kind = kGruntKind, .tier = 1, .generator = 0, .placed = true}, {});
    REQUIRE(enemy);
    for (s32 hit = 0; hit < 9; ++hit) {
        fixture.opponents.strikeEnemy(*enemy, 5, 0, {0, 0, 1}, 2, fixture.players);
    }
    const auto ninth = fixture.state();
    REQUIRE(ninth.hitStreak[2] == 9);
    REQUIRE(ninth.lessons.empty());
    fixture.opponents.strikeEnemy(*enemy, 5, 0, {0, 0, 1}, 2, fixture.players);
    fixture.opponents.strikeGenerator(0, 1, 1, fixture.players);
    fixture.opponents.strikeGenerator(0, 100000, 2, fixture.players);
    const auto queued = fixture.state();
    REQUIRE(queued.lessons ==
            std::vector<std::pair<s32, s32>>{{HelpMessages::kDestroyGenerators, 2}});
    REQUIRE(queued.generatorRewards.size() == 2);
    REQUIRE_FALSE(queued.generatorRewards[0].destroyed);
    REQUIRE(queued.generatorRewards[1].destroyed);
    CHECK(queued.hitStreak[2] == 0); // destruction resets only its hitter's native streak
    const auto swarm = fixture.opponents.enemies().checkpoint();
    REQUIRE(swarm);
    std::vector<std::string> events;
    LevelOpponents::Events callbacks;
    callbacks.help = [&](s32 lesson, usize player) {
        CHECK(lesson == HelpMessages::kDestroyGenerators);
        CHECK(player == 0); // identity 2 is the sole party entry
        events.emplace_back("lesson");
        return true;
    };
    callbacks.award = [&](s32 player, s32 amount, bool killed) {
        events.push_back(std::to_string(player) + ":" + std::to_string(amount) +
                         (killed ? ":kill" : ":hit"));
    };
    callbacks.destroyedGenerator = [&](s32 player) {
        events.push_back("generator:" + std::to_string(player));
    };
    callbacks.levels = [&] { events.emplace_back("levels"); };
    fixture.opponents.settleRewards(fixture.players, callbacks);
    REQUIRE(events.size() >= 5);
    CHECK(events.front() == "lesson");
    CHECK(events[events.size() - 2] == "generator:2");
    CHECK(events.back() == "levels");
    const auto expected = events;
    events.clear();
    auto prepared = fixture.adapter.prepareRestore(queued, fixture.visual());
    REQUIRE(prepared);
    REQUIRE(fixture.opponents.enemies().restore(*swarm));
    REQUIRE(fixture.adapter.commitRestore(std::move(*prepared)));
    CHECK(events.empty());
    fixture.opponents.settleRewards(fixture.players, callbacks);
    CHECK(events == expected);
    events.clear();
    fixture.opponents.settleRewards(fixture.players, callbacks);
    CHECK(events == std::vector<std::string>{"levels"});
}

TEST_CASE("ordinary adapter refuses unsupported pending placements without consuming them",
          "[opponent-combat-checkpoint]") {
    AdapterFixture fixture;
    const s32 kind = GENERATE(kDeathKind, kItKind, kGarmBroodKind, kGeneralEnemyKind);
    PlacedEnemies::Placement placement;
    placement.kind = kind;
    placement.spawn.kind = kind;
    placement.spawn.placed = true;
    fixture.opponents.placements().bind({placement});
    REQUIRE(fixture.opponents.pendingPlacements() == 1);
    CHECK_FALSE(fixture.adapter.checkpoint(fixture.visual()));
    CHECK_FALSE(fixture.adapter.prepareRestore({}, fixture.visual()));
    CHECK(fixture.opponents.pendingPlacements() == 1);
}

TEST_CASE("ordinary adapter refuses queued special-owner outcomes even with no visible body",
          "[opponent-combat-checkpoint]") {
    AdapterFixture fixture;
    const auto ordinary = fixture.state();
    auto prepared = fixture.adapter.prepareRestore(ordinary, fixture.visual());
    REQUIRE(prepared);
    auto swarm = fixture.opponents.enemies().checkpoint();
    REQUIRE(swarm);
    switch (GENERATE(0, 1, 2)) {
    case 0: swarm->deathEvents.push_back({DeathEvent::Kind::Killed, 0, 2}); break;
    case 1: swarm->deathShots.push_back({0}); break;
    case 2: swarm->tagged.push_back(2); break;
    }
    REQUIRE(swarm->valid());
    REQUIRE(fixture.opponents.enemies().restore(*swarm));
    REQUIRE(fixture.opponents.enemies().count() == 0);
    CHECK_FALSE(fixture.adapter.checkpoint(fixture.visual()));
    CHECK_FALSE(fixture.adapter.canCommit(*prepared));
    CHECK_FALSE(fixture.adapter.commitRestore(std::move(*prepared)));
    CHECK(fixture.opponents.enemies().checkpoint() == swarm);
}

TEST_CASE("ordinary adapter refuses defeated great creatures and their pending rewards",
          "[opponent-combat-checkpoint][assets]") {
    const auto native = test::assetOrSkip("CRITTER/GENERAL.WAD").parent_path().parent_path();
    AdapterFixture fixture;
    auto& critters = fixture.opponents.critters();
    critters.open(fixture.device, native, nullptr, {}, 'G');
    const auto general = critters.spawnGeneral({}, 0);
    REQUIRE(general);
    CHECK_FALSE(fixture.adapter.checkpoint(fixture.visual()));
    EnemyHit hit;
    hit.damage = 1000000;
    hit.player = 2;
    critters.hurt(*general, hit);
    REQUIRE(critters.dying(*general));
    for (s32 frame = 0; frame < 1200 && critters.count() != 0; ++frame) {
        critters.update(2, 1.0f / 30, {});
    }
    REQUIRE(critters.count() == 0);
    CHECK_FALSE(fixture.adapter.checkpoint(fixture.visual()));
    CHECK_FALSE(critters.takeLosses().empty()); // refusal never consumes unsupported rewards
    CHECK_FALSE(fixture.adapter.prepareRestore({}, fixture.visual()));
}
} // namespace
