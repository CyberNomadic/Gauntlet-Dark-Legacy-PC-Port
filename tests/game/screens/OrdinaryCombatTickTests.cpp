#include <algorithm>
#include <array>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"
#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/players/PowerupEffects.h"
#include "game/players/Progression.h"
#include "game/screens/OpponentResources.h"
#include "game/screens/OrdinaryCombatTick.h"
#include "game/screens/PartyRecords.h"
#include "game/world/CameraMovementLimit.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Boundary = OrdinaryCombatCheckpoint;
using Tick = OrdinaryCombatTick;
using Output = PartyDamage::Output;
using OutputKind = PartyDamage::OutputKind;

struct Fixture {
    std::filesystem::path root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    ClassDataSet classes;
    LevelWorld world;
    // Block gameplay is compared independently of the intentionally deferred
    // BLOCKFX output. No model is substituted for an unavailable visual owner.
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    AmbientDimmer dimmer;
    PlayerArsenal arsenal;
    PlayerAttacks attacks;
    LevelFixtures fixtures;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 2> players;
    std::array<std::unique_ptr<PlayerRuntimeCheckpoint>, 2> owners;
    std::array<PlayerRuntimeCheckpoint*, 2> ordered{};
    PlayerHealth health;
    LevelWatch levels;
    std::vector<EnemyMissiles::Resource> missileResources;
    std::unique_ptr<CombatRollbackCheckpoint> combat;
    std::unique_ptr<Boundary> boundary;
    std::unique_ptr<Tick> runner;
    std::vector<Output> outputs;
    std::vector<Tick::Footstep> footsteps;

    explicit Fixture(bool explosive = false) {
        REQUIRE(classes.load(root / "PDATA"));
        if (explosive) {
            const auto directory = test::sampleLevel("ordinary-combat-pending-world-explosion");
            // A triggered explosive object need not currently hurt on touch.
            // WorldDestruction's pending blast is a separate owner from walls,
            // traps, and the surface-hazard list.
            writeTextFile(directory / "world.json", R"({"objects":[
                {"name":"WALL","position":[100,0,100],"flags":33816576}]})");
            REQUIRE(world.load(device, directory, {.name = "TEST", .directory = "."}));
            REQUIRE(world.hazards().harmful() == 0);
        }
        // All native owners borrow this exact world's collision object.
        auto& floor = const_cast<WorldCollision&>(world.collision());
        floor.build({{{0, 1, 0}, {{{-200, 0, -200}, {200, 0, -200}, {200, 0, 200}}}, -1, 14},
                     {{0, 1, 0}, {{{-200, 0, -200}, {200, 0, 200}, {-200, 0, 200}}}, -1, 14}});
        arsenal.bind({device,
                      classes,
                      weapons,
                      world.collision(),
                      effects,
                      audio,
                      nullptr,
                      {},
                      false,
                      false});
        attacks.bind({device, classes, world, weapons, effects, audio, nullptr, arsenal, dimmer});
        fixtures.bind({device, world, weapons, effects, audio});
        constexpr std::array kSlots{3, 1};
        constexpr std::array kClasses{0, 6};
        for (usize index = 0; index < players.size(); ++index) {
            auto& player = players[index];
            CharacterSave save;
            save.name = "TEST";
            save.character = kClasses[index];
            save.autoAttack = false;
            save.progress().health = 10000;
            const auto* stats = classes.stats(save.character);
            REQUIRE(stats);
            player.actor.spawn(kSlots[index], save, stats, {index == 0 ? -12.0f : 12.0f, 0, 0}, 0);
            player.entrySave = save;
            player.figure = PlayerFigure::load(device, root, save, false);
            REQUIRE(player.figure);
            player.figure->setGuardArmor(armorDefense(*stats, save.progress()));
            player.turbo.add(100);
            owners[index] = std::make_unique<PlayerRuntimeCheckpoint>(player, stats);
            ordered[index] = owners[index].get();
            levels.observe(kSlots[index], 1);
        }
        opponents.open({device, world, weapons, effects, audio, root, 1}, players);
        auto& enemies = opponents.enemies();
        enemies.open(device, root, &world.collision(), 8, {.players = 2}, 79);
        REQUIRE(enemies.loadKind(kGruntKind));
        for (const auto& player : players) {
            REQUIRE(enemies.spawn({.kind = kGruntKind,
                                   .tier = 3,
                                   .position = player.actor.position() + Vec3{0, 0, 3},
                                   .placed = true},
                                  {}));
        }
        missileResources = OpponentResources::missiles(enemies);
        combat = std::make_unique<CombatRollbackCheckpoint>(ordered, health, levels, attacks,
                                                            arsenal, effects,
                                                            CombatRollbackCheckpoint::Resources{});
        boundary = std::make_unique<Boundary>(*combat, opponents, missileResources);
        runner = std::make_unique<Tick>(*boundary, classes, fixtures);
    }
    ~Fixture() {
        runner.reset();
        boundary.reset();
        combat.reset();
        attacks.clear();
        arsenal.clear();
        fixtures.clear();
        opponents.close();
    }
    Boundary::State state() const {
        const auto value = boundary->checkpoint();
        REQUIRE(value);
        return *value;
    }
    void awaitNativePowerBlow() {
        // Start at an actual native queued-blow boundary. Waiting only six
        // seconds in the full comparison need not reach the eighth power swing.
        // No reaction, attack counter, damage flag or animator is manufactured.
        auto& enemies = opponents.enemies();
        for (s32 tick = 0; tick < 4800; ++tick) {
            enemies.update(1, 1.0f / 60, LevelOpponents::enemyViews(players));
            const auto queued = enemies.checkpoint();
            REQUIRE(queued);
            if (std::ranges::any_of(queued->blows, [](const auto& blow) {
                    return (blow.flags & PlayerImpact::kKnockBack) != 0;
                })) {
                return;
            }
            enemies.takeBlows();
        }
        FAIL("native grunt never produced its power blow");
    }
    static Tick::Frame frame(s32 tick, s32 ticks) {
        Tick::Frame result;
        result.ticks = ticks;
        result.seconds = static_cast<f32>(ticks) / 60;
        result.camera.position = {0, 35, -50};
        result.camera.pitch = 0.6f;
        result.projection.horizontalFov = 1.6f;
        result.projection.aspect = 2;
        result.damage.playSeconds = static_cast<f32>(tick) * result.seconds;
        return result;
    }
    PlayerHealth::Events hurtEvents(usize index, f32 playSeconds) {
        const s32 slot = players[index].actor.player();
        return {.block =
                    [this, index, slot](f32 taken, f32 left) {
                        const f32 before = players[index].blockLeft;
                        attacks.showBlock(index, taken, left, players);
                        if (players[index].blockLeft != before) {
                            outputs.push_back(
                                {OutputKind::Block, slot, {}, players[index].blockLeft});
                        }
                    },
                .sound =
                    [this, slot](std::string_view name) {
                        outputs.push_back({OutputKind::Sound, slot, std::string{name}});
                    },
                .cry =
                    [this, slot](std::string_view name) {
                        outputs.push_back({OutputKind::Cry, slot, std::string{name}});
                    },
                .named =
                    [this, slot](std::string_view name, f32 wait) {
                        outputs.push_back({OutputKind::Named, slot, std::string{name}, wait});
                    },
                .learnBlock =
                    [playSeconds] {
                        if (playSeconds > 60) {
                            FAIL("unexpected block lesson");
                        }
                    },
                .vibrate =
                    [this, slot](s32 frames) {
                        outputs.push_back({OutputKind::Rumble, slot, {}, 0, frames});
                    },
                .logDeaths = false};
    }
    // Independent native composition: use the public fixture/opponent phases,
    // not the runner's staged pool phase or PartyDamage::stageBlows.
    void native(std::span<const PlayInput> inputs, const Tick::Frame& frame) {
        outputs.clear();
        footsteps.clear();
        const PartyMotion::Events motion{
            .perform =
                [this](usize index, PartyMotion::Action action) {
                    REQUIRE((action == PartyMotion::Action::FirstFoot ||
                             action == PartyMotion::Action::SecondFoot));
                    const auto& player = players[index];
                    const auto& actor = player.actor;
                    const auto water =
                        world.collision().liquidAt(actor.position(), actor.height(), 0);
                    footsteps.push_back(
                        {actor.player(), action,
                         LevelSoundscape::footingOf(
                             player.floor.flags,
                             PowerupEffects::of(actor.save().progress().inventory).armor,
                             water && water->y > actor.position().y)});
                },
            .select =
                [](usize, const SelectorInput& input, s32) {
                    REQUIRE_FALSE((input.up || input.down || input.left || input.right));
                },
            .advanceTurbo =
                [this](usize index, s32 ticks, f32 seconds) {
                    attacks.updateTurbo(index, ticks, seconds, players,
                                        [](s32, usize) { FAIL("unexpected turbo lesson"); });
                },
            .limitMovement =
                [&](usize index, const Vec3& from, const Vec3& to) {
                    return CameraMovementLimit::constrain(
                        from, to, frame.attention, frame.camera, frame.projection,
                        players[index].actor.followPoint() - players[index].actor.position());
                },
            .resolveMovement =
                [&](usize index, const Vec3& from, const Vec3& to) {
                    return opponents.resolveMovement(players[index].actor, from, to);
                }};
        PartyMotion::step(players, inputs, false, frame.camera.yaw, frame.ticks, frame.seconds,
                          world.collision(), motion);
        PartyRecords::advanceTime(players, frame.seconds);
        fixtures.update(frame.ticks, frame.seconds, players, {});
        for (auto& player : players) {
            PlayerHealth::advanceSurfaceGap(player, frame.seconds);
        }
        PlayerHealth::heartbeat(players, frame.ticks, false);
        opponents.watch(
            ViewVolume::of(frame.camera, frame.projection.horizontalFov, frame.projection.aspect),
            frame.attention);
        LevelOpponents::Events events;
        events.hurt = [&](usize index, f32 amount, HurtKind kind, bool directed,
                          const PlayerImpact& impact) {
            auto& player = players[index];
            // Every fixture opponent is a native tier-three grunt. Its cue is
            // emitted by LevelOpponents before the scene's health callback.
            if (const auto sound = EnemyFeedback::meleeSound(kGruntKind, 3, {})) {
                outputs.push_back({OutputKind::Sound, player.actor.player(), *sound});
            }
            health.hurt(player, amount, kind, directed, false, frame.damage.damageScale,
                        hurtEvents(index, frame.damage.playSeconds + frame.seconds), impact, false,
                        classes.stats(player.actor.save().character));
        };
        events.settleBlasts = [&] { fixtures.settleBlasts(players, {}); };
        events.advanceLegend = [](f32) {};
        events.advanceVictory = [](s32, f32) {};
        events.levels = [&] {
            for (const auto& player : players) {
                REQUIRE_FALSE(levels.observe(player.actor.player(),
                                             experienceLevel(player.actor.save().experience())));
            }
        };
        events.help = [](s32, usize) {
            FAIL_CHECK("unexpected opponent lesson");
            return false;
        };
        opponents.update(frame.ticks, frame.seconds, players, fixtures.obstacles(), events,
                         fixtures.missileStops(), fixtures.critterObstacles());
        const PlayerAttacks::Targets targets{opponents, fixtures, {}};
        attacks.updateProjectiles(frame.seconds, players, targets);
        attacks.updateStrikes(frame.seconds, players, targets);
        attacks.updateShields(frame.seconds, players, targets);
        attacks.updateArmour(frame.seconds, players, targets);
        effects.update(frame.seconds);
    }
};

TEST_CASE("ordinary combat runner matches native complete party and enemy phases",
          "[ordinary-combat-tick][assets]") {
    const s32 ticks = GENERATE(1, 2);
    Fixture fixture;
    fixture.awaitNativePowerBlow();
    const auto uploads = fixture.device.texturesCreated;
    u32 hurt = 0;
    bool queuedReaction = false;
    bool consumedReaction = false;
    bool walked = false;
    bool stepped = false;
    for (s32 tick = 0; tick < 360; ++tick) {
        CAPTURE(tick, ticks);
        std::array<PlayInput, 4> input{};
        if (tick >= 160 && tick < 240) {
            input[3].move = {Vec2{tick % 40 < 20 ? 0.4f : -0.4f, 0}, 0.4f};
            input[1].move = {Vec2{tick % 40 < 20 ? -0.4f : 0.4f, 0}, 0.4f};
        }
        const auto frame = Fixture::frame(tick, ticks);
        const auto before = fixture.state();
        fixture.native(input, frame);
        const auto expected = fixture.state();
        const auto outputs = fixture.outputs;
        const auto footsteps = fixture.footsteps;
        REQUIRE(fixture.boundary->restore(before));
        const auto result = fixture.runner->step(fixture.players, input, frame);
        REQUIRE(result.status == Tick::Status::Advanced);
        CHECK(fixture.state() == expected);
        CHECK(result.damage == outputs);
        CHECK(result.footsteps == footsteps);
        for (const auto& foot : result.footsteps) {
            CHECK(foot.footing == Footing::Stair);
            stepped = true;
        }
        for (usize index = 0; index < fixture.players.size(); ++index) {
            const auto& old = before.combat.players[index];
            const auto& now = expected.combat.players[index];
            if (now.save.health() < old.save.health()) {
                hurt |= 1U << fixture.players[index].actor.player();
            }
            queuedReaction |= now.reaction != PlayerDeed::None;
            consumedReaction |=
                old.reaction != PlayerDeed::None && now.reaction == PlayerDeed::None;
            walked |= now.actor.position != old.actor.position;
        }
    }
    CHECK(hurt == ((1U << 3) | (1U << 1)));
    CHECK(queuedReaction);
    CHECK(consumedReaction);
    CHECK(walked);
    CHECK(stepped);
    CHECK(fixture.device.texturesCreated == uploads);
}

TEST_CASE("ordinary combat runner rewinds a late second-player lethal blow",
          "[ordinary-combat-tick][assets]") {
    Fixture fixture;
    fixture.players[0].actor.save().progress().health = 51;
    auto baseline = fixture.state();
    const auto* stats = fixture.classes.stats(fixture.players[0].actor.save().character);
    REQUIRE(stats);
    const f32 firstDamage = armorDefense(*stats, fixture.players[0].actor.save().progress()) + 10;
    baseline.opponents.enemies.blows = {{.player = 3, .kind = kGruntKind, .damage = firstDamage},
                                        {.player = 1, .kind = kGruntKind, .damage = 20000}};
    REQUIRE(fixture.boundary->restore(baseline));
    const auto before = fixture.state();
    auto proof = fixture.boundary->prepareRewind();
    REQUIRE(proof);
    const auto first =
        PartyDamage::stageBlows(std::span{baseline.opponents.enemies.blows}.first(1),
                                fixture.players, fixture.health, fixture.classes, {}, {});
    REQUIRE(first.status == PartyDamage::Status::Applied);
    REQUIRE(fixture.players[0].life == PlayerLife::Standing);
    REQUIRE(fixture.health.checkpoint() != before.combat.health);
    REQUIRE(fixture.boundary->rewind(std::move(*proof)));
    REQUIRE(fixture.state() == before);
    const auto uploads = fixture.device.texturesCreated;
    const std::array<PlayInput, 4> input{};
    const auto result = fixture.runner->step(fixture.players, input, Fixture::frame(0, 1));
    REQUIRE(result.status == Tick::Status::Unsupported);
    CHECK(result.damage.empty());
    CHECK(result.footsteps.empty());
    CHECK(result.enemyCues.empty());
    CHECK(fixture.state() == before);
    CHECK(fixture.device.texturesCreated == uploads);
}

TEST_CASE("ordinary combat runner refuses foreign class binding and unsupported input atomically",
          "[ordinary-combat-tick][assets]") {
    Fixture fixture;
    const auto before = fixture.state();
    ClassDataSet otherClasses;
    REQUIRE(otherClasses.load(fixture.root / "PDATA"));
    Tick wrong(*fixture.boundary, otherClasses, fixture.fixtures);
    std::array<PlayInput, 4> input{};
    CHECK(wrong.step(fixture.players, input, Fixture::frame(0, 1)).status == Tick::Status::Invalid);
    input[3].attackPressed = true;
    CHECK(fixture.runner->step(fixture.players, input, Fixture::frame(0, 1)).status ==
          Tick::Status::Unsupported);
    CHECK(fixture.state() == before);
}

TEST_CASE("ordinary combat runner accepts held guard and preserves native block state",
          "[ordinary-combat-tick][assets]") {
    Fixture fixture;
    std::array<PlayInput, 4> input{};
    for (s32 tick = 0; tick < 60; ++tick) {
        for (const auto& player : fixture.players) {
            auto& command = input[static_cast<usize>(player.actor.player())];
            command.turbo = true;
            command.defendPressed = tick == 0;
        }
        const auto frame = Fixture::frame(tick, 1);
        const auto before = fixture.state();
        fixture.native(input, frame);
        const auto expected = fixture.state();
        REQUIRE(fixture.boundary->restore(before));
        REQUIRE(fixture.runner->step(fixture.players, input, frame).status ==
                Tick::Status::Advanced);
        REQUIRE(fixture.state() == expected);
        if (std::ranges::all_of(fixture.players, [](const auto& player) {
                return player.figure->animator().defending();
            })) {
            break;
        }
    }
    REQUIRE(std::ranges::all_of(
        fixture.players, [](const auto& player) { return player.figure->animator().defending(); }));
    auto pending = fixture.state();
    for (const auto& player : fixture.players) {
        const auto* stats = fixture.classes.stats(player.actor.save().character);
        REQUIRE(stats);
        pending.opponents.enemies.blows.push_back(
            {.player = player.actor.player(),
             .kind = kGruntKind,
             .tier = 3,
             .damage = armorDefense(*stats, player.actor.save().progress()) + 20});
    }
    REQUIRE(fixture.boundary->restore(pending));
    for (auto& command : input) {
        command.defendPressed = false;
    }
    const auto frame = Fixture::frame(20, 1);
    fixture.native(input, frame);
    const auto expected = fixture.state();
    const auto outputs = fixture.outputs;
    REQUIRE(expected.combat.players[0].blocked);
    REQUIRE(expected.combat.players[1].blocked);
    REQUIRE(std::ranges::count(outputs, OutputKind::Block, &Output::kind) == 2);
    REQUIRE(fixture.boundary->restore(pending));
    const auto replay = fixture.runner->step(fixture.players, input, frame);
    REQUIRE(replay.status == Tick::Status::Advanced);
    CHECK(replay.damage == outputs);
    CHECK(fixture.state() == expected);
}

TEST_CASE("ordinary combat runner leaves pending world destruction explosions untouched",
          "[ordinary-combat-tick][assets]") {
    Fixture fixture(true);
    const std::array<PlayInput, 4> input{};
    REQUIRE_FALSE(fixture.world.hasWorldExplosions());
    REQUIRE(fixture.world.collision().movingObjectCount() == 0);
    REQUIRE(fixture.world.walls().size() == 0);
    REQUIRE(fixture.runner->step(fixture.players, input, Fixture::frame(0, 1)).status ==
            Tick::Status::Advanced);
    const Vec3 blast{4, 5, 6};
    REQUIRE(fixture.world.explodeObject(0, blast));
    REQUIRE(fixture.world.hasWorldExplosions());
    REQUIRE(fixture.world.hazards().harmful() == 0);
    const auto before = fixture.state();
    const auto result = fixture.runner->step(fixture.players, input, Fixture::frame(1, 1));
    CHECK(result.status == Tick::Status::Unsupported);
    CHECK(result.damage.empty());
    CHECK(fixture.state() == before);
    REQUIRE(fixture.world.hasWorldExplosions());
    CHECK(fixture.world.takeWorldExplosions() == std::vector<Vec3>{blast});
    CHECK_FALSE(fixture.world.hasWorldExplosions());
}
} // namespace
