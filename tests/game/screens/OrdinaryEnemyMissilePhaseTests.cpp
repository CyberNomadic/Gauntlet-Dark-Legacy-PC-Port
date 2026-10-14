#include <algorithm>
#include <array>
#include <limits>
#include <memory>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/CombatResources.h"
#include "game/screens/LevelOpponents.h"
#include "game/screens/OpponentCombatCheckpoint.h"
#include "game/screens/OpponentSimulation.h"
#include "game/screens/OrdinaryEnemyMissilePhase.h"
#include "game/screens/PlayerRuntimeCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Phase = OrdinaryEnemyMissilePhase;

struct MissilePhaseFixture {
    std::filesystem::path root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
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
    std::unique_ptr<OpponentSimulation> pools;
    std::unique_ptr<OpponentCombatCheckpoint> adapter;
    std::unique_ptr<PlayerRuntimeCheckpoint> player;
    std::vector<PartyDamage::Output> nativeDamage;

    explicit MissilePhaseFixture(bool wall = false, bool explosive = false) {
        REQUIRE(weapons.load(root / "WEAPONS"));
        REQUIRE(classes.load(root / "PDATA"));
        CharacterSave save;
        save.name = "SHOT";
        REQUIRE(save.name.size() <= kCharacterNameLength);
        save.progress().health = 10000;
        const auto* stats = classes.stats(save.character);
        REQUIRE(stats);
        party[0].actor.spawn(2, save, stats, {0, 0, 20}, 0);
        party[0].entrySave = save;
        party[0].figure = PlayerFigure::load(device, root, save, false);
        REQUIRE(party[0].figure);
        if (explosive) {
            const auto directory = test::sampleLevel("enemy-impact-explosive-world");
            writeTextFile(directory / "world.json", R"({"objects":[
              {"name":"FLOOR","position":[0,0,0],"flags":262148}]})");
            REQUIRE(world.load(device, directory, {"TEST", 0, "TEST", "", "", "", "", 0}));
            REQUIRE(world.wouldExplodeObject(0));
        }
        if (wall) {
            // A real native collision triangle, independent of level triggers
            // and spawn schedules. It is not an authored explosive object.
            auto& collision = const_cast<WorldCollision&>(world.collision());
            collision.build({{{0, 0, -1}, {{{-20, -10, 10}, {20, -10, 10}, {0, 40, 10}}}, -1, 6}});
        }
        opponents.open({device, world, weapons, effects, audio, root, 1, true}, party);
        const std::array banks{CombatResources::Bank{1, &weapons, {}}};
        REQUIRE(resources.bind(device, banks));
        pools = std::make_unique<OpponentSimulation>(opponents.generators(), opponents.enemies(),
                                                     missiles(), &opponents.placements());
        adapter = std::make_unique<OpponentCombatCheckpoint>(opponents, bindings());
        player = std::make_unique<PlayerRuntimeCheckpoint>(party[0], stats);
        auto initial = capture();
        initial->adapter.hearers = {party[0].actor.position()};
        restore(*initial);
    }
    ~MissilePhaseFixture() {
        player.reset();
        adapter.reset();
        pools.reset();
        effects.clear();
        opponents.close();
        resources.clear();
    }
    EnemyMissiles& missiles() { return const_cast<EnemyMissiles&>(opponents.missiles()); }
    auto bindings() const { return resources.resources().sharedEffects; }

    struct Snapshot {
        OpponentState pools;
        OpponentCombatCheckpoint::State adapter;
        EffectTrees::PoolCheckpoint effects;
        PlayerRuntimeCheckpoint::State player;
        PlayerHealth::Checkpoint health;
        bool operator==(const Snapshot&) const = default;
    };
    struct Undo {
        OpponentSimulation::Prepared pools;
        OpponentCombatCheckpoint::Prepared adapter;
        EffectTrees::PreparedRestore effects;
        PlayerRuntimeCheckpoint::Prepared player;
        PlayerHealth::Checkpoint health;
    };
    std::unique_ptr<Snapshot> capture() const {
        auto result = std::make_unique<Snapshot>();
        const auto population = pools->checkpoint();
        REQUIRE(population);
        result->pools = *population;
        const auto visual = effects.checkpointPool(bindings());
        REQUIRE(visual);
        result->effects = *visual;
        const auto ordinary = adapter->checkpoint(result->effects);
        REQUIRE(ordinary);
        result->adapter = *ordinary;
        const auto actor = player->checkpoint();
        REQUIRE(actor);
        result->player = *actor;
        result->health = health.checkpoint();
        return result;
    }
    std::unique_ptr<Undo> prepare(const Snapshot& state) {
        auto population = pools->prepareRestore(state.pools);
        auto ordinary = adapter->prepareRestore(state.adapter, state.effects);
        auto visual = effects.prepareRestorePool(state.effects, bindings());
        auto actor = player->prepareRestore(state.player);
        REQUIRE(population);
        REQUIRE(ordinary);
        REQUIRE(visual);
        REQUIRE(actor);
        return std::make_unique<Undo>(std::move(*population), std::move(*ordinary),
                                      std::move(*visual), std::move(*actor), state.health);
    }
    void restore(Undo&& state) {
        REQUIRE(pools->canCommit(state.pools));
        REQUIRE(adapter->canCommit(state.adapter));
        REQUIRE(effects.canCommit(state.effects));
        REQUIRE(player->canCommit(state.player));
        REQUIRE(pools->commitRestore(std::move(state.pools)));
        REQUIRE(adapter->commitRestore(std::move(state.adapter)));
        REQUIRE(effects.commitRestore(std::move(state.effects)));
        REQUIRE(player->commitRestore(std::move(state.player)));
        REQUIRE(health.restore(state.health));
    }
    void restore(const Snapshot& state) { restore(std::move(*prepare(state))); }

    void native(s32 ticks) {
        nativeDamage.clear();
        const auto slot = party[0].actor.player();
        // Independent native health callback recording: the oracle does not
        // invoke stageHit/stageBlows or the new phase's output recorder.
        const PlayerHealth::Events output{
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
            .learnBlock = [] { FAIL("unexpected new lesson in native missile oracle"); },
            .vibrate =
                [&](s32 frames) {
                    nativeDamage.push_back({PartyDamage::OutputKind::Rumble, slot, {}, 0, frames});
                },
            .logDeaths = false};
        const LevelOpponents::Events events{
            .hurt =
                [&](usize index, f32 damage, HurtKind kind, bool directed,
                    const PlayerImpact& impact) {
                    PartyDamage::hurt(health, party[index], damage, kind, directed, {}, output,
                                      impact, classes.stats(party[index].actor.save().character));
                },
            .settleBlasts = [] {},
            .advanceLegend = [](f32) {},
            .advanceVictory = [](s32, f32) {},
            .levels = [] {}};
        opponents.update(ticks, static_cast<f32>(ticks) / 60, party, {}, events);
    }
    Phase::Result replay(s32 ticks) {
        const f32 seconds = static_cast<f32>(ticks) / 60;
        // Native beginning-of-opponent-frame timers precede flight. They are
        // owned by the encompassing Tick, not by the landing phase itself.
        party[0].breathGap = std::max(0.0f, party[0].breathGap - seconds);
        party[0].effectGap = std::max(0.0f, party[0].effectGap - seconds);
        const auto views = LevelOpponents::enemyViews(party);
        opponents.generators().setPlayerCount(1);
        const OpponentSimulation::Frame frame{
            .ticks = ticks, .seconds = seconds, .players = views, .collision = &world.collision()};
        REQUIRE(pools->stageCombat(frame) == OpponentSimulation::Result::Advanced);
        return Phase::advance(opponents, party, health, classes, {}, bindings());
    }
};

TEST_CASE("ordinary enemy missile landings match actual native flights and damage at both rates",
          "[ordinary-enemy-missile-phase][assets]") {
    const s32 ticks = GENERATE(1, 2);
    const bool wall = GENERATE(false, true);
    const bool bolt = GENERATE(false, true);
    auto fixture = std::make_unique<MissilePhaseFixture>(wall);
    const auto kind =
        bolt ? *enemyMissileOf(2, EnemyMissileKind::kBolt) : EnemyMissileKind::arrow();
    fixture->missiles().launch(kind, {0, 4, 0}, {0, 3, 20}, 1, nullptr, -1);
    REQUIRE(fixture->missiles().count() == 1);
    bool landed = false;
    for (s32 elapsed = 0; elapsed < 90 && !landed; elapsed += ticks) {
        CAPTURE(ticks, wall, bolt, elapsed);
        const auto before = fixture->capture();
        fixture->native(ticks);
        const auto expected = fixture->capture();
        const auto damage = fixture->nativeDamage;
        fixture->restore(*before);
        const auto uploads = fixture->device.texturesCreated;
        const auto result = fixture->replay(ticks);
        CAPTURE(result.refusal);
        REQUIRE(result.advanced());
        const auto actual = fixture->capture();
        CHECK(actual->pools == expected->pools);
        CHECK(actual->effects == expected->effects);
        CHECK(actual->adapter.cueEffects == expected->adapter.cueEffects);
        CHECK(actual->player == expected->player);
        CHECK(actual->health == expected->health);
        CHECK(result.damage == damage);
        CHECK(result.sounds.empty()); // native ordinary dry impacts add no wall sound
        CHECK(fixture->device.texturesCreated == uploads);
        CHECK(fixture->device.draws.empty());
        landed |= result.impacts != 0;
        // The full native oracle also advances unrelated empty critter texture
        // clocks. Retain that exact full-owner boundary for the next comparison.
        fixture->restore(*expected);
    }
    REQUIRE(landed);
    CHECK(fixture->missiles().count() == 0);
    if (wall) {
        REQUIRE(fixture->effects.count() == 1);
        CHECK(fixture->effects.effect(0).unlit);
        CHECK_FALSE(fixture->effects.effect(0).depthWrite);
        CHECK(fixture->effects.effect(0).tint.a == 96);
    } else {
        CHECK(fixture->party[0].actor.save().progress().health < 10000);
    }
}

TEST_CASE("ordinary enemy impact emission preserves native element and liquid presentation",
          "[ordinary-enemy-missile-phase][assets]") {
    const u32 element = GENERATE(0, 1, 2, 3, 4);
    auto fixture = std::make_unique<MissilePhaseFixture>();
    auto before = fixture->capture();
    EnemyMissileHit hit;
    hit.worldContact = true;
    hit.liquid = true;
    hit.flags = EnemyMissileKind::kBoltHit | element;
    hit.position = {0, 0, 10};
    before->pools.missiles.hits = {hit};
    REQUIRE(Phase::supported(before->pools.missiles));
    fixture->restore(*before);
    fixture->native(1);
    const auto expected = fixture->capture();
    fixture->restore(*before);
    const auto uploads = fixture->device.texturesCreated;
    const auto result = Phase::advance(fixture->opponents, fixture->party, fixture->health,
                                       fixture->classes, {}, fixture->bindings());
    REQUIRE(result.advanced());
    REQUIRE(result.sounds.size() == 1);
    CHECK(result.sounds[0].name == "S_SPLASH");
    CHECK(result.sounds[0].position == hit.position);
    CHECK(result.sounds[0].volume ==
          LevelSoundscape::kSplashLevel / 255 *
              LevelOpponents::attenuation(hit.position, before->adapter.hearers));
    const auto actual = fixture->capture();
    CHECK(actual->effects == expected->effects);
    CHECK(actual->adapter.cueEffects == expected->adapter.cueEffects);
    REQUIRE(fixture->effects.count() == 1);
    REQUIRE(actual->adapter.cueEffects.size() == 1);
    CHECK(actual->adapter.cueEffects.front() == fixture->effects.effect(0).id);
    CHECK(actual->pools.missiles == expected->pools.missiles);
    CHECK(fixture->device.texturesCreated == uploads);
}

TEST_CASE("ordinary enemy missile admission retains explicit unsupported output boundaries",
          "[ordinary-enemy-missile-phase]") {
    const s32 mode = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9);
    EnemyMissiles::Checkpoint state;
    state.hits.push_back({.player = 0, .damage = 10, .flags = EnemyMissileKind::kArrowHit});
    REQUIRE(Phase::supported(state));
    auto& hit = state.hits.front();
    switch (mode) {
    case 0: hit.burstRadius = 3; break;
    case 1: hit.fromBurst = true; break;
    case 2: hit.ricochet = true; break;
    case 3: hit.target = 0; break;
    case 4: hit.flags |= EnemyMissileKind::kPierces; break;
    case 5: hit.flags |= EnemyBlast::kGas; break;
    case 6: state.rockHits.push_back({0, 1}); break;
    case 7: state.pickupBlasts.push_back({{}, 1, 1, 0}); break;
    case 8: hit.damage = std::numeric_limits<f32>::quiet_NaN(); break;
    case 9: state.hits.resize(Phase::kMaxImpacts + 1); break;
    default: FAIL("unknown boundary");
    }
    CHECK_FALSE(Phase::supported(state));
}

TEST_CASE("ordinary enemy landing refusal leaves whole prepared undo usable",
          "[ordinary-enemy-missile-phase][assets]") {
    const s32 mode = GENERATE(0, 1, 2);
    auto fixture = std::make_unique<MissilePhaseFixture>();
    auto before = fixture->capture();
    EnemyMissileHit wall;
    wall.worldContact = true;
    wall.flags = EnemyMissileKind::kArrowHit;
    wall.position = {0, 3, 10};
    before->pools.missiles.hits = {wall};
    if (mode != 0) {
        EnemyMissileHit second;
        second.player = 2;
        second.flags = EnemyMissileKind::kArrowHit;
        second.damage = mode == 1 ? 1'000'000.0f : 10;
        second.burstRadius = mode == 2 ? 3 : 0;
        before->pools.missiles.hits.push_back(second);
    }
    fixture->restore(*before);
    auto undo = fixture->prepare(*before);
    const auto uploads = fixture->device.texturesCreated;
    const auto resources =
        mode == 0 ? std::span<const EffectTrees::PoolResource>{} : fixture->bindings();
    const auto result = Phase::advance(fixture->opponents, fixture->party, fixture->health,
                                       fixture->classes, {}, resources);
    CHECK_FALSE(result.advanced());
    CHECK(result.refusal == (mode == 0   ? Phase::Refusal::Resources
                             : mode == 1 ? Phase::Refusal::Damage
                                         : Phase::Refusal::Hit));
    CHECK(result.sounds.empty());
    CHECK(result.damage.empty());
    if (mode != 1) {
        CHECK(fixture->effects.count() == 0); // complete batch preflight precedes any birth
    } else {
        CHECK(fixture->effects.count() == 1); // refusal after real earlier native work
    }
    fixture->restore(std::move(*undo));
    CHECK(*fixture->capture() == *before);
    CHECK(fixture->device.texturesCreated == uploads);
    CHECK(fixture->device.draws.empty());
}

TEST_CASE("ordinary enemy landings preflight native explosive ancestry before dispatch",
          "[ordinary-enemy-missile-phase][assets]") {
    auto fixture = std::make_unique<MissilePhaseFixture>(false, true);
    auto before = fixture->capture();
    EnemyMissileHit hit;
    hit.worldContact = true;
    hit.worldObject = 0;
    hit.flags = EnemyMissileKind::kArrowHit;
    hit.position = {0, 0, 0};
    before->pools.missiles.hits = {hit};
    fixture->restore(*before);
    const auto motion = fixture->world.motion(false);
    auto undo = fixture->prepare(*before);
    const auto result = Phase::advance(fixture->opponents, fixture->party, fixture->health,
                                       fixture->classes, {}, fixture->bindings());
    CHECK(result.refusal == Phase::Refusal::WorldExplosion);
    CHECK(fixture->effects.count() == 0);
    CHECK_FALSE(fixture->world.hasWorldExplosions());
    CHECK(fixture->world.motion(false) == motion);
    fixture->restore(std::move(*undo));
    CHECK(*fixture->capture() == *before);
    // This is an actual native transition, not a guessed forbidden object ID.
    CHECK(fixture->world.explodeObject(0, hit.position));
    CHECK(fixture->world.hasWorldExplosions());
}

TEST_CASE("ordinary enemy impact preparation requires the exact native device and lenders",
          "[ordinary-enemy-missile-phase][assets]") {
    auto fixture = std::make_unique<MissilePhaseFixture>();
    const EnemyMissileHit hit{.flags = EnemyMissileKind::kArrowHit, .worldContact = true};
    const auto emission = EnemyMissileEmission::resolve(hit, fixture->weapons);
    REQUIRE(emission.archive);
    REQUIRE(emission.prepare(fixture->bindings(), {}, fixture->device));
    test::FakeRenderDevice otherDevice;
    CHECK_FALSE(emission.prepare(fixture->bindings(), {}, otherDevice));
    const std::array otherLenders{&fixture->weapons.textures};
    CHECK_FALSE(emission.prepare(fixture->bindings(), otherLenders, fixture->device));
    CHECK(fixture->device.draws.empty());
    CHECK(otherDevice.texturesCreated == 0);
}
} // namespace
