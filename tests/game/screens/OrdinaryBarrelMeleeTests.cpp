#include <array>
#include <cmath>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/io/File.h"
#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/players/PowerupEffects.h"
#include "game/players/Progression.h"
#include "game/screens/CombatResources.h"
#include "game/screens/FloorRiding.h"
#include "game/screens/HelpMessages.h"
#include "game/screens/OpponentResources.h"
#include "game/screens/OrdinaryCombatTick.h"
#include "game/screens/PartyFigures.h"
#include "game/screens/PartyNames.h"
#include "game/screens/PartyPickups.h"
#include "game/screens/PartyRecords.h"
#include "game/screens/PlayerPowerups.h"
#include "game/screens/StageResources.h"
#include "game/world/CameraMovementLimit.h"
#include "game/world/LevelCatalog.h"

namespace gdl::game {
struct PlayerAttacksTestPeer {
    static auto meleeTarget(const PlayerAttacks& attacks, const PlayerActor& actor,
                            const PlayerAttacks::Targets& targets, const Vec3& facing, f32 reach) {
        return attacks.meleeTarget(actor, targets, facing, reach);
    }
};
} // namespace gdl::game

namespace {
using namespace gdl;
using namespace gdl::game;
using Kind = BreakableStrike::Kind;
using Boundary = OrdinaryCombatCheckpoint;
using Tick = OrdinaryCombatTick;

struct BarrelCase {
    Kind kind = Kind::Plain;
    s32 health = 1;
    s32 armor = 0;
    bool drops = false;
    bool art = true;
};

std::filesystem::path barrelStage(const std::array<BarrelCase, 2>& cases) {
    const auto directory = test::sampleLevel("ordinary-barrel-melee");
    std::string records;
    for (const auto& row : cases) {
        if (!records.empty()) {
            records += ',';
        }
        const s32 subtype = row.kind == Kind::Exploding ? 44 : row.kind == Kind::Poison ? 45 : 43;
        records += std::format(
            R"({{"type":{},"subtype":{},"name":"{}","radius":1,"height":3,"armor":{},"hitPoints":{}}})",
            row.kind == Kind::Holding ? 2 : 10, subtype, row.art ? "BARREL" : "MISSING", row.armor,
            row.health);
    }
    // Both Holding variants traverse an actual choice record. The empty choice
    // still consumes the native choice RNG before resolving to no contents.
    records += R"(,{"type":-1,"choices":[-1]},{"type":-1,"choices":[4]},
                    {"type":1,"subtype":2,"name":"KEY","radius":1,"height":1,"value":1})";
    std::string placements;
    for (usize index = 0; index < cases.size(); ++index) {
        if (!placements.empty()) {
            placements += ',';
        }
        placements +=
            std::format(R"({{"info":{},"position":[{},0,3],"params":[{},0,0,0,1,0,0,0,0,0,0,0]}})",
                        index, index == 0 ? -10 : 10, cases[index].drops ? 3 : 2);
    }
    writeTextFile(directory / "world.json",
                  R"({"objects":[{"name":"FLOOR","position":[0,0,0],"flags":4}],
                     "bounds":{"min":[-40,-20,-40],"max":[40,20,40]},"itemInfos":[)" +
                      records + R"(],"itemInstances":[)" + placements + "]}");
    writeTextFile(directory / "collision.json", R"({"objects":[
      {"object":0,"normals":[0,1,0,0,1,0],
       "vertices":[-40,0,-40,40,0,40,40,0,-40,-40,0,-40,-40,0,40,40,0,40]}]})");
    writeTextFile(directory / "animations.json", R"({"trees":[
      {"name":"BARREL","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
       "sequences":[{"name":"WHOLE","frames":2,"rate":30},
         {"name":"ACTIVE","frames":45,"rate":30,
          "tracks":[{"node":0,"flags":2,"frames":[0,44],"values":[0,1.3]}]},
         {"name":"DONE","frames":0}]},
      {"name":"KEY","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
       "sequences":[{"name":"ACTIVE","frames":2,"rate":30}]}]})");
    return directory;
}

struct Fixture {
    std::filesystem::path root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    AudioMixer mixer{48000};
    SoundPlayer sounds{mixer};
    ClassDataSet classes;
    LevelWorld world;
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
    CombatResources resources;
    std::vector<EnemyMissiles::Resource> missiles;
    std::unique_ptr<LevelWorldCheckpoint> worldState;
    std::unique_ptr<LevelFixturesCheckpoint> fixtureState;
    std::unique_ptr<CombatRollbackCheckpoint> combat;
    std::unique_ptr<Boundary> boundary;
    std::unique_ptr<Tick> runner;
    std::vector<usize> nativeHits;
    std::vector<PlayerAnimator::Action> nativeActions;
    usize callbacks = 0;

    explicit Fixture(std::array<BarrelCase, 2> cases = {}, bool nativeG1 = false) {
        REQUIRE(classes.load(root / "PDATA"));
        REQUIRE(weapons.load(root / "WEAPONS"));
        if (nativeG1) {
            LevelCatalog catalog;
            REQUIRE(catalog.load(root));
            const auto level = catalog.byName("G1");
            REQUIRE(level);
            REQUIRE(world.load(device, root, *level));
        } else {
            REQUIRE(world.load(device, barrelStage(cases), {.name = "GTEST", .directory = "."}));
        }
        world.setPlayerCount(2);
        audio.open(root, &sounds, world.audio(), 'G');
        arsenal.bind({device, classes, weapons, world.collision(), effects, audio, nullptr, "",
                      false, false});
        attacks.bind({device, classes, world, weapons, effects, audio, nullptr, arsenal, dimmer});
        fixtures.bind({device, world, weapons, effects, audio});
        fixtures.setPlayerCount(2);
        constexpr std::array kSeats{3, 1};
        for (usize index = 0; index < players.size(); ++index) {
            auto& player = players[index];
            CharacterSave save;
            save.name = "BARREL";
            save.character = 0;
            save.autoAttack = false;
            save.progress().health = 10000;
            const auto* stats = classes.stats(save.character);
            REQUIRE(stats);
            Vec3 position{index == 0 ? -10.0f : 10.0f, 0, 0};
            if (nativeG1) {
                REQUIRE(world.startPoint(0));
                position = world.startPoint(0)->position + Vec3{index == 0 ? -2.0f : 2.0f, 0, 0};
            }
            player.actor.spawn(kSeats[index], save, stats, position, 0);
            player.actor.settle(world.collision());
            FloorRiding::land(players, index, position, world.collision());
            player.entrySave = save;
            player.figure = PlayerFigure::load(device, root, save, false);
            REQUIRE(player.figure);
            player.figure->setGuardArmor(armorDefense(*stats, save.progress()));
            player.turbo.add(100);
            owners[index] = std::make_unique<PlayerRuntimeCheckpoint>(player, stats);
            ordered[index] = owners[index].get();
            levels.observe(kSeats[index], experienceLevel(save.experience()));
        }
        opponents.open({device, world, weapons, effects, audio, root, 1, true}, players);
        REQUIRE(StageResources::preload(world, opponents.enemies(), opponents.critters(),
                                        opponents.bosses()));
        missiles = OpponentResources::missiles(opponents.enemies());
        REQUIRE(resources.bindStage(device, world, weapons, classes, players, arsenal, opponents,
                                    effects));
        combat = std::make_unique<CombatRollbackCheckpoint>(
            ordered, health, levels, attacks, arsenal, effects, resources.resources());
        worldState = std::make_unique<LevelWorldCheckpoint>(world);
        fixtureState = std::make_unique<LevelFixturesCheckpoint>(fixtures);
        REQUIRE(worldState->bindResources(device));
        REQUIRE(fixtureState->bindResources());
        boundary = std::make_unique<Boundary>(*combat, opponents, missiles, worldState.get(),
                                              fixtureState.get());
        runner = std::make_unique<Tick>(*boundary, classes, fixtures);
        if (!nativeG1) {
            REQUIRE(fixtures.barrels().size() == 2);
            for (usize index = 0; index < cases.size(); ++index) {
                REQUIRE(fixtures.barrels().barrel(index).kind == cases[index].kind);
                REQUIRE(fixtures.barrels().barrel(index).figure.hasFigure() == cases[index].art);
            }
        }
    }
    ~Fixture() {
        runner.reset();
        boundary.reset();
        combat.reset();
        fixtureState.reset();
        worldState.reset();
        attacks.clear();
        arsenal.clear();
        fixtures.clear();
        effects.clear();
        resources.clear();
        opponents.close();
        audio.close();
    }
    std::unique_ptr<Boundary::State> state() const {
        auto value = boundary->checkpoint();
        REQUIRE(value);
        return std::make_unique<Boundary::State>(std::move(*value));
    }
    static Tick::Frame frame(s32 step, s32 ticks) {
        Tick::Frame result;
        result.ticks = ticks;
        result.seconds = static_cast<f32>(ticks) / 60;
        result.camera.position = {0, 35, -50};
        result.camera.pitch = .6f;
        result.projection.horizontalFov = 1.6f;
        result.projection.aspect = 2;
        result.damage.playSeconds = static_cast<f32>(step) * result.seconds;
        return result;
    }
    LevelFixtures::Events fixtureEvents() {
        return {.hurt = [&](usize, f32, HurtKind, bool, const PlayerImpact&) { ++callbacks; },
                .help =
                    [&](s32, usize) {
                        ++callbacks;
                        return false;
                    },
                .opponents = [&](const Vec3&, f32, f32, std::vector<s32>&, u32) { ++callbacks; },
                .releaseEnemy =
                    [&](s32, const Vec3&, s32) {
                        ++callbacks;
                        return false;
                    }};
    }
    PlayerAttacks::Targets targets() {
        return {.opponents = opponents,
                .fixtures = fixtures,
                .fixtureEvents = fixtureEvents(),
                .players = players};
    }
    // This is the native public phase composition, not the staged runner or a
    // second damage formula. The synthetic stage contains no opponent/pickup/
    // trigger interaction; assert those absent rather than faking their outputs.
    void native(std::span<const PlayInput> inputs, const Tick::Frame& frame) {
        nativeHits.clear();
        nativeActions.clear();
        PartyFigures::snapshot(players);
        effects.capturePresentation();
        world.capturePresentation();
        fixtures.capturePresentation();
        PartyNames{}.step(players, frame.ticks, false);
        REQUIRE(
            PlayerPowerups::update(players, frame.seconds, PlayerPowerups::Clock::Level).empty());
        for (auto& player : players) {
            REQUIRE(player.streak.step(frame.seconds).empty());
            REQUIRE_FALSE(player.gem.update(0));
        }
        world.update(frame.seconds, false);
        opponents.syncFloors();
        world.revealCrystals(frame.seconds);
        for (auto& player : players) {
            player.transport.observeContact(false);
        }
        REQUIRE(motion(inputs, frame));
        PartyRecords::advanceTime(players, frame.seconds);
        finishNative(inputs, frame);
    }
    bool motion(std::span<const PlayInput> inputs, const Tick::Frame& frame,
                const LevelFixtures::BarrelPublisher& publisher = {}) {
        nativeHits.clear();
        nativeActions.clear();
        bool accepted = true;
        const auto target = targets();
        const PartyMotion::Events events{
            .perform =
                [&](usize index, PartyMotion::Action action) {
                    auto& player = players[index];
                    if (action == PartyMotion::Action::Melee) {
                        const auto selected = PlayerAttacksTestPeer::meleeTarget(
                            attacks, player.actor, target,
                            player.meleeFacing.value_or(player.actor.facing()),
                            player.actor.reach() + PlayerAttacks::kStepReach);
                        REQUIRE(selected);
                        REQUIRE(selected->body.id >= 0);
                        REQUIRE(static_cast<usize>(selected->body.id) < fixtures.barrels().size());
                        nativeHits.push_back(static_cast<usize>(selected->body.id));
                        nativeActions.push_back(player.figure->animator().action());
                        if (publisher) {
                            accepted &= attacks.melee(index, players, target, {}, publisher);
                        } else {
                            attacks.melee(index, players, target);
                        }
                    } else if (action == PartyMotion::Action::FamiliarShot) {
                        arsenal.launchFamiliar(player.actor, player.figure.get(), std::nullopt);
                    } else {
                        REQUIRE((action == PartyMotion::Action::FirstFoot ||
                                 action == PartyMotion::Action::SecondFoot));
                    }
                },
            .select =
                [](usize, const SelectorInput& input, s32) {
                    REQUIRE_FALSE((input.up || input.down || input.left || input.right));
                },
            .advanceTurbo =
                [&](usize index, s32 ticks, f32 seconds) {
                    attacks.updateTurbo(index, ticks, seconds, players, [&](s32 id, usize) {
                        if (id != HelpMessages::kUseCombo && id != HelpMessages::kUseTurbo) {
                            REQUIRE(HelpMessages::specOf(id) == nullptr);
                        }
                    });
                },
            .aim =
                [&](usize index) {
                    return attacks.aim(players[index].actor, players[index].actor.facing(), target);
                },
            .limitMovement =
                [&](usize index, const Vec3& from, const Vec3& to) {
                    return CameraMovementLimit::constrain(
                        from, to, frame.attention, frame.camera, frame.projection,
                        players[index].actor.followPoint() - players[index].actor.position());
                },
            .attackDeed =
                [&](usize index, bool strong, bool moved, const Vec3& facing) {
                    return attacks.attackDeed(players[index].actor, strong, target, moved,
                                              players[index].figure->animator().meleeChain(),
                                              facing);
                },
            .automaticMeleeDeed = [](usize, const Vec3&) { return PlayerDeed::None; },
            .meleeSense =
                [&](usize index, bool held, const Vec3& facing) {
                    return attacks.meleeSense(players[index].actor, held, target, facing);
                },
            .resolveMovement =
                [&](usize index, const Vec3& from, const Vec3& to) {
                    return opponents.resolveMovement(players[index].actor, from, to);
                }};
        PartyMotion::step(players, inputs, false, frame.camera.yaw, frame.ticks, frame.seconds,
                          world.collision(), events);
        return accepted;
    }
    void finishNative(std::span<const PlayInput>, const Tick::Frame& frame) {
        const auto target = targets();
        fixtures.update(frame.ticks, frame.seconds, players, fixtureEvents());
        for (auto& player : players) {
            PlayerHealth::advanceSurfaceGap(player, frame.seconds);
        }
        PlayerHealth::heartbeat(players, frame.ticks, false);
        opponents.watch(
            ViewVolume::of(frame.camera, frame.projection.horizontalFov, frame.projection.aspect),
            frame.attention);
        const LevelOpponents::Events opponentEvents{
            .hurt =
                [](usize, f32, HurtKind, bool, const PlayerImpact&) {
                    FAIL("the isolated native barrel fixture unexpectedly hurt a player");
                },
            .settleBlasts = [&] { fixtures.settleBlasts(players, fixtureEvents()); },
            .advanceLegend = [](f32) {},
            .advanceVictory = [](s32, f32) {},
            .levels =
                [&] {
                    for (const auto& player : players) {
                        REQUIRE_FALSE(
                            levels.observe(player.actor.player(),
                                           experienceLevel(player.actor.save().experience())));
                    }
                },
            .award = [](s32, s32, bool) { FAIL("unexpected native opponent reward"); },
            .destroyedGenerator = [](s32) { FAIL("unexpected native generator reward"); },
            .help =
                [&](s32, usize) {
                    ++callbacks;
                    return false;
                }};
        opponents.update(frame.ticks, frame.seconds, players, fixtures.obstacles(), opponentEvents,
                         fixtures.missileStops(), fixtures.critterObstacles());
        attacks.updateProjectiles(frame.seconds, players, target);
        attacks.updateStrikes(frame.seconds, players, target);
        attacks.updateShields(frame.seconds, players, target);
        attacks.updateArmour(frame.seconds, players, target);
        effects.update(frame.seconds);
        PartyHud hud;
        PartyPickups{}.collect(device, players,
                               {.world = world,
                                .fixtures = fixtures,
                                .hud = hud,
                                .audio = audio,
                                .classes = classes,
                                .help = [&](s32, usize) {
                                    ++callbacks;
                                    return false;
                                }});
        REQUIRE(hud.pickups().cards().empty());
        std::vector<TriggerVisitor> visitors;
        for (usize index = 0; index < players.size(); ++index) {
            const auto& actor = players[index].actor;
            TriggerVisitor visitor;
            visitor.position = actor.position();
            visitor.radius = actor.reach();
            visitor.height = actor.height();
            visitor.crystals = actor.save().progress().crystals;
            visitor.gargoylePieces = actor.save().progress().relics.gargoylePieces;
            visitor.party = static_cast<s32>(index);
            if (const auto floor = world.collision().floorAt(actor.position(), .5f, 1)) {
                visitor.floorObject = floor->object;
            }
            visitors.push_back(visitor);
        }
        world.updateTriggers(frame.seconds, visitors);
        fixtures.syncFloors();
        opponents.syncFloors();
        REQUIRE(world.takeRotatorCues().empty());
        for (auto& player : players) {
            REQUIRE_FALSE(player.exitWait.step(false, frame.ticks));
        }
        REQUIRE(callbacks == 0);
    }
    Tick::Result compare(std::span<const PlayInput> inputs, const Tick::Frame& frame) {
        const auto before = state();
        native(inputs, frame);
        const auto expected = state();
        REQUIRE(boundary->restore(*before));
        const auto uploads = device.texturesCreated;
        usize audioEvents = 0;
        const SoundPlayer::Capture capture(&sounds, [&](const auto&) { ++audioEvents; });
        const auto result = runner->step(players, inputs, frame);
        CAPTURE(result.refusal, result.partyRefusal, result.refusedTarget);
        REQUIRE(result.status == Tick::Status::Advanced);
        const auto actual = state();
        CHECK((*actual == *expected));
        CHECK(device.texturesCreated == uploads);
        CHECK(callbacks == 0);
        CHECK(audioEvents == 0);
        CHECK(result.impacts.size() == nativeHits.size());
        return result;
    }
};

std::array<PlayInput, 4> attackInput(bool first, bool second, bool edge) {
    std::array<PlayInput, 4> result{};
    result[3].attack = first;
    result[3].attackPressed = first && edge;
    result[1].attack = second;
    result[1].attackPressed = second && edge;
    return result;
}

TEST_CASE("ordinary melee keeps exact native barrel damage without breaking special barrels",
          "[ordinary-barrel-melee][assets]") {
    const auto kind = GENERATE(Kind::Plain, Kind::Holding, Kind::Exploding, Kind::Poison);
    const s32 ticks = GENERATE(1, 2);
    auto fixture = std::make_unique<Fixture>(
        std::array<BarrelCase, 2>{{{kind, 10000, 10000}, {Kind::Plain, 10000}}});
    bool hit = false;
    for (s32 step = 0; step < 90 / ticks && !hit; ++step) {
        CAPTURE(kind, ticks, step);
        const auto result =
            fixture->compare(attackInput(true, false, step == 0), Fixture::frame(step, ticks));
        if (!fixture->nativeHits.empty()) {
            REQUIRE(fixture->nativeHits == std::vector<usize>{0});
            REQUIRE(result.impacts.size() == 1);
            CHECK(result.impacts.front().name == "S_WEAPONHITWOOD");
            CHECK_FALSE(result.impacts.front().playerDistance);
            CHECK(fixture->fixtures.barrels().barrel(0).health == 9999);
            CHECK(fixture->fixtures.barrels().standing(0));
            hit = true;
        }
    }
    REQUIRE(hit);
}

TEST_CASE("ordinary barrel breaks retain native empty-choice RNG and authored staves",
          "[ordinary-barrel-melee][assets]") {
    const auto kind = GENERATE(Kind::Plain, Kind::Holding);
    const bool art = GENERATE(false, true);
    const s32 ticks = GENERATE(1, 2);
    auto fixture = std::make_unique<Fixture>(
        std::array<BarrelCase, 2>{{{kind, 1, 0, false, art}, {Kind::Plain, 10000}}});
    const auto initial = fixture->state();
    bool hit = false;
    bool broken = false;
    for (s32 step = 0; step < 180 / ticks; ++step) {
        CAPTURE(kind, art, ticks, step);
        const auto result =
            fixture->compare(attackInput(!hit, false, step == 0), Fixture::frame(step, ticks));
        if (!fixture->nativeHits.empty()) {
            REQUIRE_FALSE(hit);
            hit = true;
            REQUIRE(result.impacts.size() == 1);
            CHECK(result.impacts.front().name == "S_BARREL_WOODG");
            CHECK_FALSE(result.impacts.front().playerDistance);
            CHECK(fixture->nativeActions.front() == PlayerAnimator::Action::LowKickRecover);
        }
        const auto& barrel = fixture->fixtures.barrels().barrel(0);
        broken |= barrel.state == Breakables::kBroken;
    }
    REQUIRE(hit);
    REQUIRE(broken);
    CHECK_FALSE(fixture->fixtures.barrels().barrel(0).gone);
    const auto final = fixture->state();
    if (kind == Kind::Holding) {
        CHECK(final->stage->fixtures.barrels.seed != initial->stage->fixtures.barrels.seed);
    } else {
        CHECK(final->stage->fixtures.barrels.seed == initial->stage->fixtures.barrels.seed);
    }
    CHECK(final->stage->fixtures.blasts.empty());
    CHECK(final->stage->fixtures.clouds.empty());
    CHECK(final->combat.effects.effects.empty());
}

TEST_CASE("unsupported barrel outputs rewind the complete sparse-seat tick and earlier cues",
          "[ordinary-barrel-melee][assets]") {
    const auto unsafe = GENERATE(Kind::Holding, Kind::Exploding, Kind::Poison);
    const s32 ticks = GENERATE(1, 2);
    auto fixture = std::make_unique<Fixture>(
        std::array<BarrelCase, 2>{{{Kind::Plain, 1}, {unsafe, 1, 0, true}}});
    bool refused = false;
    for (s32 step = 0; step < 90 / ticks; ++step) {
        CAPTURE(unsafe, ticks, step);
        const auto before = fixture->state();
        const auto uploads = fixture->device.texturesCreated;
        const auto input = attackInput(true, true, step == 0);
        usize audioEvents = 0;
        Tick::Result result;
        {
            const SoundPlayer::Capture capture(&fixture->sounds,
                                               [&](const auto&) { ++audioEvents; });
            result = fixture->runner->step(fixture->players, input, Fixture::frame(step, ticks));
        }
        CHECK(audioEvents == 0);
        if (result.status == Tick::Status::Advanced) {
            REQUIRE(result.impacts.empty());
            continue;
        }
        REQUIRE(result.status == Tick::Status::Unsupported);
        CHECK(result.refusal == Tick::Refusal::PartyMotion);
        const auto expected = unsafe == Kind::Holding     ? Tick::PartyRefusal::BarrelContents
                              : unsafe == Kind::Exploding ? Tick::PartyRefusal::BarrelExplosion
                                                          : Tick::PartyRefusal::BarrelGas;
        CHECK(result.partyRefusal == expected);
        CHECK(result.refusedTarget == Tick::TargetFamily::Barrel);
        REQUIRE(result.refusedAction);
        CHECK(*result.refusedAction == PlayerAnimator::Action::LowKickRecover);
        const auto after = fixture->state();
        CHECK((*after == *before));
        CHECK(fixture->device.texturesCreated == uploads);
        CHECK(fixture->callbacks == 0);
        CHECK(result.impacts.empty());
        CHECK(result.damage.empty());
        CHECK(result.throws.empty());
        // The refused frame was a real last-hit opportunity, not an unsupported
        // input before the action. Native damage reaches the first barrel too.
        REQUIRE(before->stage->fixtures.barrels.barrels[0].health == 1);
        const auto target = fixture->targets();
        fixture->attacks.melee(0, fixture->players, target);
        CHECK(fixture->fixtures.barrels().barrel(0).health == 0);
        fixture->fixtures.strikeBarrel(1, 10000, 1, fixture->players, fixture->fixtureEvents());
        const auto native = fixture->fixtureState->checkpoint();
        REQUIRE(native);
        CHECK(native->barrels.barrels[1].health == 0);
        if (unsafe == Kind::Holding) {
            CHECK(native->barrels.seed != before->stage->fixtures.barrels.seed);
            CHECK(fixture->callbacks > 0);
            CHECK(fixture->world.placedItems().size() > before->stage->world.pickups.items.size());
        } else if (unsafe == Kind::Exploding) {
            CHECK_FALSE(native->blasts.empty());
        } else {
            CHECK_FALSE(native->clouds.empty());
        }
        refused = true;
        break;
    }
    REQUIRE(refused);
}

TEST_CASE("barrel publisher preserves native rounded damage and refuses before external work",
          "[ordinary-barrel-melee][assets]") {
    const f32 power = GENERATE(.1f, 1.49f, 1.5f, 5.0f);
    auto fixture = std::make_unique<Fixture>(
        std::array<BarrelCase, 2>{{{Kind::Plain, 100, 1}, {Kind::Holding, 1, 0, true}}});
    const auto before = fixture->state();
    fixture->fixtures.strikeBarrel(0, power, 3, fixture->players, fixture->fixtureEvents());
    const auto native = fixture->state();
    REQUIRE(fixture->boundary->restore(*before));
    usize observed = 0;
    REQUIRE(
        fixture->fixtures.strikeBarrel(0, power, 3, fixture->players, fixture->fixtureEvents(),
                                       [&](const BreakableStrike& strike, std::string_view sound) {
                                           ++observed;
                                           CHECK_FALSE(strike.broken);
                                           CHECK(sound == "S_WEAPONHITWOOD");
                                           return true;
                                       }));
    CHECK(observed == 1);
    CHECK((*fixture->state() == *native));
    REQUIRE(fixture->boundary->restore(*before));
    auto undo = fixture->boundary->prepareRewind();
    REQUIRE(undo);
    CHECK_FALSE(
        fixture->fixtures.strikeBarrel(1, 10000, 1, fixture->players, fixture->fixtureEvents(),
                                       [&](const BreakableStrike& strike, std::string_view sound) {
                                           CHECK(strike.broken);
                                           CHECK(strike.contents == 4);
                                           CHECK(sound == "S_BARREL_WOODG");
                                           return false;
                                       }));
    CHECK(fixture->callbacks == 0);
    const auto changed = fixture->state();
    CHECK(changed->stage->fixtures.barrels.seed != before->stage->fixtures.barrels.seed);
    CHECK(changed->stage->world.pickups == before->stage->world.pickups);
    REQUIRE(fixture->boundary->rewind(std::move(*undo)));
    CHECK((*fixture->state() == *before));
}

TEST_CASE("pending native blasts cannot be dispatched by an otherwise supported barrel swing",
          "[ordinary-barrel-melee][assets]") {
    const s32 ticks = GENERATE(1, 2);
    auto fixture = std::make_unique<Fixture>(
        std::array<BarrelCase, 2>{{{Kind::Plain, 10000}, {Kind::Plain, 10000}}});
    bool reached = false;
    for (s32 step = 0; step < 90 / ticks; ++step) {
        const auto before = fixture->state();
        const auto input = attackInput(true, false, step == 0);
        const auto frame = Fixture::frame(step, ticks);
        fixture->compare(input, frame);
        if (fixture->nativeHits.empty()) {
            continue;
        }
        REQUIRE(fixture->boundary->restore(*before));
        // A native, still-live ring far from every body. Its continuation is
        // owned state, but settleBlasts is not permitted from this melee lane.
        fixture->fixtures.blast({1000, 0, 1000}, 1, 0, fixture->players, fixture->fixtureEvents());
        const auto pending = fixture->state();
        REQUIRE_FALSE(pending->stage->fixtures.blasts.empty());
        const auto callbacks = fixture->callbacks;
        usize sounds = 0;
        const SoundPlayer::Capture capture(&fixture->sounds, [&](const auto&) { ++sounds; });
        const auto result = fixture->runner->step(fixture->players, input, frame);
        CHECK(result.status == Tick::Status::Unsupported);
        CHECK(result.refusal == Tick::Refusal::PartyMotion);
        CHECK(result.partyRefusal == Tick::PartyRefusal::MeleeTarget);
        CHECK(result.refusedTarget == Tick::TargetFamily::Barrel);
        CHECK(result.impacts.empty());
        CHECK((*fixture->state() == *pending));
        CHECK(fixture->callbacks == callbacks);
        CHECK(sounds == 0);
        reached = true;
        break;
    }
    REQUIRE(reached);
}

TEST_CASE("native G1 ordinary barrel wins the real low-kick acquisition and prepared dispatch",
          "[ordinary-barrel-melee][barrel-native-g1][assets]") {
    const s32 ticks = GENERATE(1, 2);
    auto fixture = std::make_unique<Fixture>(std::array<BarrelCase, 2>{}, true);
    const auto target = fixture->targets();
    const auto& barrels = fixture->fixtures.barrels();
    REQUIRE(barrels.size() > 0);
    std::optional<usize> selected;
    // Keep the complete original G1 roster and asset scope. Choose an accessible
    // authored plain barrel for the initial player placement; no barrel, enemy,
    // animation or checkpoint is manufactured to make the contact eligible.
    for (usize index = 0; index < barrels.size() && !selected; ++index) {
        const auto& barrel = barrels.barrel(index);
        if (!barrels.standing(index) || barrel.kind != Kind::Plain || barrel.armor < 0) {
            continue;
        }
        for (s32 bearing = 0; bearing < 8 && !selected; ++bearing) {
            const f32 yaw = static_cast<f32>(bearing) * kPi / 4;
            const Vec3 facing{std::sin(yaw), 0, std::cos(yaw)};
            const Vec3 at = barrel.figure.position() - facing * (barrel.radius + 2);
            auto& player = fixture->players[0];
            player.actor.place(at);
            player.actor.settle(fixture->world.collision());
            player.actor.turnTo(yaw);
            FloorRiding::land(fixture->players, 0, at, fixture->world.collision());
            const auto winner = PlayerAttacksTestPeer::meleeTarget(
                fixture->attacks, player.actor, target, facing,
                player.actor.reach() + PlayerAttacks::kStepReach);
            if (winner && winner->body.id == static_cast<s32>(index) &&
                !fixture->world.fallingScenery().wouldShoot(
                    player.actor.position(), player.actor.reach() + PlayerAttacks::kStepReach)) {
                selected = index;
            }
        }
    }
    REQUIRE(selected);
    REQUIRE(barrels.barrel(*selected).kind == Kind::Plain);
    REQUIRE(barrels.barrel(*selected).figure.hasFigure());
    bool landed = false;
    for (s32 step = 0; step < 90 / ticks && !landed; ++step) {
        CAPTURE(ticks, step, *selected, barrels.barrel(*selected).instance);
        auto frame = Fixture::frame(step, ticks);
        frame.attention = fixture->players[0].actor.followPoint();
        frame.camera.position = frame.attention + Vec3{0, 35, -50};
        const auto before = fixture->state();
        const auto input = attackInput(true, false, step == 0);
        std::vector<std::pair<std::string, u32>> nativeSounds;
        {
            const SoundPlayer::Capture capture(&fixture->sounds, [&](const auto& event) {
                if (event.kind == SoundPlayer::Event::Kind::Play) {
                    nativeSounds.emplace_back(event.bank, event.call);
                }
            });
            REQUIRE(fixture->motion(input, frame));
        }
        const auto expected = fixture->state();
        if (fixture->nativeHits.empty()) {
            continue;
        }
        REQUIRE(fixture->nativeHits == std::vector<usize>{*selected});
        CHECK(fixture->nativeActions.front() == PlayerAnimator::Action::LowKickRecover);
        REQUIRE_FALSE(nativeSounds.empty());
        REQUIRE(fixture->boundary->restore(*before));
        const auto uploads = fixture->device.texturesCreated;
        std::vector<std::string> cues;
        usize immediateSounds = 0;
        {
            const SoundPlayer::Capture capture(&fixture->sounds,
                                               [&](const auto&) { ++immediateSounds; });
            REQUIRE(fixture->motion(input, frame,
                                    [&](const BreakableStrike& strike, std::string_view cue) {
                                        CHECK(strike.index == *selected);
                                        CHECK(strike.kind == Kind::Plain);
                                        CHECK(strike.contents == -1);
                                        cues.emplace_back(cue);
                                        return true;
                                    }));
        }
        CHECK(immediateSounds == 0);
        CHECK(fixture->device.texturesCreated == uploads);
        CHECK((*fixture->state() == *expected));
        std::vector<std::pair<std::string, u32>> dispatched;
        {
            const SoundPlayer::Capture capture(&fixture->sounds, [&](const auto& event) {
                if (event.kind == SoundPlayer::Event::Kind::Play) {
                    dispatched.emplace_back(event.bank, event.call);
                }
            });
            for (const auto& cue : cues) {
                fixture->audio.playNamed(cue);
            }
        }
        CHECK(dispatched == nativeSounds);
        landed = true;
    }
    REQUIRE(landed);
}

} // namespace
