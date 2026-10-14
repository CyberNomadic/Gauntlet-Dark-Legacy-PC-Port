#include <algorithm>
#include <array>
#include <memory>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/CombatResources.h"
#include "game/screens/LevelOpponents.h"
#include "game/screens/OrdinaryCritterPhase.h"
#include "game/screens/PartyRecords.h"
#include "game/screens/PlayerRuntimeCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct CritterPhaseFixture {
    std::filesystem::path root = test::assetOrSkip("CRITTER/GOLEM.WAD").parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    ClassDataSet classes;
    std::array<PlayerRuntime, 1> party;
    PlayerHealth health;
    LevelOpponents opponents;
    CombatResources resources;
    std::unique_ptr<OpponentCombatCheckpoint> adapter;
    std::unique_ptr<PlayerRuntimeCheckpoint> player;
    std::vector<PartyDamage::Output> nativeDamage;

    explicit CritterPhaseFixture(CombatantKind kind = CombatantKind::General) {
        REQUIRE(weapons.load(root / "WEAPONS"));
        REQUIRE(classes.load(root / "PDATA"));
        CharacterSave save;
        save.name = "CRIT";
        REQUIRE(save.name.size() <= kCharacterNameLength);
        save.progress().health = 10000;
        const auto* stats = classes.stats(save.character);
        REQUIRE(stats);
        party[0].actor.spawn(2, save, stats, {0, 0, 300}, 0);
        party[0].entrySave = save;
        party[0].figure = PlayerFigure::load(device, root, save, false);
        REQUIRE(party[0].figure);
        opponents.open({device, world, weapons, effects, audio, root, 1, true}, party);
        // A CPU-only arena with no floors/fixtures. The native actor is bound
        // identically for the oracle and restored phase, not a replacement AI.
        auto& critters = opponents.critters();
        critters.open(device, root, nullptr, {}, 'G');
        critters.setLookouts({{{-20, 0, 0}, {-20, 0, 20}}, {1, 65535}});
        auto* archive = critters.archiveFor(kind);
        REQUIRE(archive);
        REQUIRE(critters.spawn(kind, {-20, 0, 0}, 0, {}, 4) == 0);
        const std::array banks{CombatResources::Bank{1, &weapons, {}},
                               CombatResources::Bank{2, archive, {}}};
        REQUIRE(resources.bind(device, banks));
        adapter = std::make_unique<OpponentCombatCheckpoint>(opponents, bindings());
        player = std::make_unique<PlayerRuntimeCheckpoint>(party[0], stats);
        auto initial = capture();
        initial->adapter.hearers = {party[0].actor.position()};
        restore(*initial);
    }
    ~CritterPhaseFixture() {
        adapter.reset();
        player.reset();
        effects.clear();
        opponents.close();
        resources.clear();
    }
    auto bindings() const { return resources.resources().sharedEffects; }
    struct Snapshot {
        OpponentCombatCheckpoint::State adapter;
        EffectTrees::PoolCheckpoint effects;
        PlayerRuntimeCheckpoint::State player;
        PlayerHealth::Checkpoint health;
        bool operator==(const Snapshot&) const = default;
    };
    std::unique_ptr<Snapshot> capture() const {
        auto result = std::make_unique<Snapshot>();
        const auto visual = effects.checkpointPool(bindings());
        REQUIRE(visual);
        result->effects = *visual;
        const auto state = adapter->checkpoint(result->effects);
        REQUIRE(state);
        result->adapter = *state;
        const auto actor = player->checkpoint();
        REQUIRE(actor);
        result->player = *actor;
        result->health = health.checkpoint();
        return result;
    }
    void restore(const Snapshot& state) {
        auto visual = effects.prepareRestorePool(state.effects, bindings());
        auto population = adapter->prepareRestore(state.adapter, state.effects);
        auto actor = player->prepareRestore(state.player);
        REQUIRE(visual);
        REQUIRE(population);
        REQUIRE(actor);
        REQUIRE(effects.canCommit(*visual));
        REQUIRE(adapter->canCommit(*population));
        REQUIRE(player->canCommit(*actor));
        REQUIRE(effects.commitRestore(std::move(*visual)));
        REQUIRE(adapter->commitRestore(std::move(*population)));
        REQUIRE(player->commitRestore(std::move(*actor)));
        REQUIRE(health.restore(state.health));
    }
    // Native scene outputs, independently recorded at the PlayerHealth seam.
    // This oracle does not call the new private stageHit or stageBlows helpers.
    void native(s32 ticks) {
        nativeDamage.clear();
        const auto slot = party[0].actor.player();
        const PlayerHealth::Events damage{
            .block =
                [&](f32 taken, f32 left) {
                    if (const auto duration = PartyDamage::block(party[0], taken, left)) {
                        nativeDamage.push_back(
                            {PartyDamage::OutputKind::Block, slot, {}, *duration});
                    }
                },
            .sound =
                [&](std::string_view name) {
                    nativeDamage.push_back(
                        {PartyDamage::OutputKind::Sound, slot, std::string{name}});
                },
            .cry =
                [&](std::string_view name) {
                    nativeDamage.push_back({PartyDamage::OutputKind::Cry, slot, std::string{name}});
                },
            .named =
                [&](std::string_view name, f32 wait) {
                    nativeDamage.push_back(
                        {PartyDamage::OutputKind::Named, slot, std::string{name}, wait});
                },
            .learnBlock = [] { FAIL("unexpected lesson in ordinary critter oracle"); },
            .vibrate =
                [&](s32 frames) {
                    nativeDamage.push_back({PartyDamage::OutputKind::Rumble, slot, {}, 0, frames});
                },
            .logDeaths = false};
        const LevelOpponents::Events events{
            .hurt =
                [&](usize index, f32 amount, HurtKind kind, bool directed,
                    const PlayerImpact& impact) {
                    PartyDamage::hurt(health, party[index], amount, kind, directed, {}, damage,
                                      impact, classes.stats(party[index].actor.save().character));
                },
            .settleBlasts = [] {},
            .advanceLegend = [](f32) {},
            .advanceVictory = [](s32, f32) {},
            .levels = [] {},
            .award =
                [&](s32 seat, s32 value, bool killed) {
                    PartyRecords::award(party, seat, value, killed, world.level());
                }};
        opponents.update(ticks, static_cast<f32>(ticks) / 60, party, {}, events);
    }
    OrdinaryCritterPhase::Result replay(s32 ticks) {
        const f32 seconds = static_cast<f32>(ticks) / 60;
        // The encompassing tick owns beginOrdinaryFrame BEFORE the swarm and
        // the critter phase. Mirror only its two known gap decrements here;
        // the fixed listener was initialized at the same authoritative point.
        party[0].breathGap = std::max(0.0f, party[0].breathGap - seconds);
        party[0].effectGap = std::max(0.0f, party[0].effectGap - seconds);
        const auto views = LevelOpponents::enemyViews(party);
        const auto moved =
            OrdinaryCritterPhase::advance(opponents, ticks, seconds, views, {}, party);
        if (!moved.advanced()) {
            return moved;
        }
        return OrdinaryCritterPhase::finish(opponents, party, health, classes, {}, bindings());
    }
};

TEST_CASE("ordinary critter phases resume native live families at both tick rates",
          "[ordinary-critter-phase][assets]") {
    const auto kind =
        GENERATE(CombatantKind::General, CombatantKind::Golem, CombatantKind::Gargoyle);
    const s32 ticks = GENERATE(1, 2);
    auto fixture = std::make_unique<CritterPhaseFixture>(kind);
    const auto initial = fixture->capture();
    REQUIRE(OrdinaryCritterPhase::supported(initial->adapter));
    const auto uploads = fixture->device.texturesCreated;
    for (s32 elapsed = 0; elapsed < 30; elapsed += ticks) {
        CAPTURE(kind, ticks, elapsed);
        const auto before = fixture->capture();
        fixture->native(ticks);
        const auto expected = fixture->capture();
        const auto sounds = fixture->nativeDamage;
        fixture->restore(*before);
        const auto result = fixture->replay(ticks);
        CAPTURE(result.refusal);
        REQUIRE(result.advanced());
        CHECK(result.damage == sounds);
        CHECK(*fixture->capture() == *expected);
        CHECK(fixture->device.texturesCreated == uploads);
    }
    const auto after = fixture->capture();
    CHECK(after->adapter.critters != initial->adapter.critters);
    if (kind == CombatantKind::General) {
        CHECK(after->adapter.critters.actors[0].parts[0].actor.position !=
              initial->adapter.critters.actors[0].parts[0].actor.position);
    }
    CHECK(fixture->device.draws.empty());
}

TEST_CASE("ordinary critter contacts preserve native damage gaps rewards and cue attachment",
          "[ordinary-critter-phase][assets]") {
    const bool breath = GENERATE(false, true);
    auto fixture = std::make_unique<CritterPhaseFixture>();
    auto before = fixture->capture();
    CombatBlow blow;
    blow.player = 2;
    blow.critter = 0;
    blow.damage = 4;
    blow.breath = breath;
    blow.gated = true;
    before->adapter.critters.events.blows = {blow, blow};
    CombatCue cue;
    cue.critter = 0;
    cue.tree = "HITCOL";
    // Choose an actual shared native tree, not a made-up prepared prototype.
    REQUIRE(fixture->weapons.trees.find(cue.tree));
    cue.position = {-20, 3, 0};
    cue.follows = true;
    cue.untilNextMove = true;
    cue.life = 0.5f;
    before->adapter.critters.events.cues.push_back(cue);
    before->adapter.critters.events.losses.push_back({0, CombatantKind::General, {}, 2, 2.75f});
    REQUIRE(before->adapter.valid());
    REQUIRE(OrdinaryCritterPhase::supported(before->adapter));
    fixture->restore(*before);
    before = fixture->capture();
    fixture->native(1);
    const auto expected = fixture->capture();
    const auto sounds = fixture->nativeDamage;
    fixture->restore(*before);
    const auto uploads = fixture->device.texturesCreated;
    const auto result = fixture->replay(1);
    REQUIRE(result.advanced());
    CHECK(result.damage == sounds);
    CHECK(*fixture->capture() == *expected);
    CHECK(fixture->party[0].breathGap == 0.25f);
    CHECK(fixture->party[0].actor.save().health() < before->player.save.health());
    CHECK(fixture->capture()->adapter.critterExperienceOwed[2] == 0.75f);
    CHECK(fixture->device.texturesCreated == uploads);
}

TEST_CASE("ordinary critter phase refuses unsupported queues without consuming them",
          "[ordinary-critter-phase][assets]") {
    const s32 variant = GENERATE(0, 1, 2, 3, 4, 5);
    auto fixture = std::make_unique<CritterPhaseFixture>();
    auto before = fixture->capture();
    switch (variant) {
    case 0: before->adapter.critters.events.grabs.push_back({2, 0}); break;
    case 1: before->adapter.critters.events.rams.push_back({0, 1}); break;
    case 2:
        before->adapter.critters.events.losses.push_back(
            {0, CombatantKind::General, {}, 2, 1, true});
        break;
    case 3:
        before->adapter.critters.events.losses.push_back(
            {0, CombatantKind::General, {}, 2, 1, false, true});
        break;
    case 4: before->adapter.critters.events.cues.push_back({.critter = 0, .shakes = true}); break;
    case 5: before->adapter.critters.events.cues.push_back({.critter = 0, .arena = true}); break;
    }
    REQUIRE(before->adapter.valid());
    CHECK_FALSE(OrdinaryCritterPhase::supported(before->adapter));
    fixture->restore(*before);
    const auto captured = fixture->capture();
    const auto result =
        OrdinaryCritterPhase::finish(fixture->opponents, fixture->party, fixture->health,
                                     fixture->classes, {}, fixture->bindings());
    CHECK_FALSE(result.advanced());
    CHECK(result.damage.empty());
    CHECK(result.sounds.empty());
    CHECK(*fixture->capture() == *captured);
}

TEST_CASE("critter cue preflight and lethal refusal leave a complete rewind usable",
          "[ordinary-critter-phase][assets]") {
    const bool lethal = GENERATE(false, true);
    auto fixture = std::make_unique<CritterPhaseFixture>();
    auto before = fixture->capture();
    if (lethal) {
        before->adapter.critters.events.blows.push_back({2, 0, 1'000'000});
    } else {
        REQUIRE(fixture->weapons.trees.find("HITCOL"));
        before->adapter.critters.events.cues.push_back({.critter = 0, .tree = "HITCOL"});
    }
    fixture->restore(*before);
    before = fixture->capture();
    auto adapterUndo = fixture->adapter->prepareRestore(before->adapter, before->effects);
    auto effectsUndo = fixture->effects.prepareRestorePool(before->effects, fixture->bindings());
    auto playerUndo = fixture->player->prepareRestore(before->player);
    REQUIRE(adapterUndo);
    REQUIRE(effectsUndo);
    REQUIRE(playerUndo);
    const auto uploads = fixture->device.texturesCreated;
    const auto result = OrdinaryCritterPhase::finish(
        fixture->opponents, fixture->party, fixture->health, fixture->classes, {},
        lethal ? fixture->bindings() : std::span<const EffectTrees::PoolResource>{});
    CHECK(result.refusal == (lethal ? OrdinaryCritterPhase::Refusal::Damage
                                    : OrdinaryCritterPhase::Refusal::Resources));
    CHECK(result.damage.empty());
    CHECK(result.sounds.empty());
    REQUIRE(fixture->adapter->canCommit(*adapterUndo));
    REQUIRE(fixture->effects.canCommit(*effectsUndo));
    REQUIRE(fixture->player->canCommit(*playerUndo));
    REQUIRE(fixture->adapter->commitRestore(std::move(*adapterUndo)));
    REQUIRE(fixture->effects.commitRestore(std::move(*effectsUndo)));
    REQUIRE(fixture->player->commitRestore(std::move(*playerUndo)));
    REQUIRE(fixture->health.restore(before->health));
    CHECK(*fixture->capture() == *before);
    CHECK(fixture->device.texturesCreated == uploads);
}
} // namespace
