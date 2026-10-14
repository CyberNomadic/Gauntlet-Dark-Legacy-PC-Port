#include <array>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeSoundBank.h"
#include "game/players/Progression.h"
#include "game/screens/CombatResources.h"
#include "game/screens/FloorRiding.h"
#include "game/screens/HelpMessages.h"
#include "game/screens/OpponentResources.h"
#include "game/screens/OrdinaryCombatTick.h"
#include "game/screens/PartyFigures.h"
#include "game/screens/PartyHelpAdmission.h"
#include "game/screens/PartyNames.h"
#include "game/screens/PartyPickups.h"
#include "game/screens/PartyRecords.h"
#include "game/screens/PlayerPowerups.h"
#include "game/screens/StageResources.h"
#include "game/world/CameraMovementLimit.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Tick = OrdinaryCombatTick;
using Boundary = OrdinaryCombatCheckpoint;

std::filesystem::path gateStage(bool moving) {
    const auto directory = test::sampleLevel(moving ? "keyed-gate-tick-lift" : "keyed-gate-tick");
    writeTextFile(directory / "world.json",
                  std::string{R"({"objects":[{"name":"FLOOR","position":[0,0,0],"flags":)"} +
                      (moving ? "4100" : "4") + R"(}],
      "bounds":{"min":[-40,-20,-40],"max":[40,20,40]},
      "itemInfos":[{"type":7,"subtype":0,"name":"GATE","radius":1,"height":3}],
      "itemInstances":[{"info":0,"position":[-4,0,0]},
                       {"info":0,"position":[4,0,0]}])" +
                      (moving ? R"(,"animations":[{"object":0,"frames":1000,"state":257,
                         "start":0,"track":{"flags":32,"frames":[0,999],"values":[0,2]}}])"
                              : "") +
                      "}");
    writeTextFile(directory / "animations.json", R"({"trees":[
      {"name":"GATE","nodes":[{"name":"ROOT","object":"WALL","parent":-1,
        "position":[0,0,0]}],"sequences":[{"name":"OFF","frames":2,"rate":30},
        {"name":"ACTIVE","frames":45,"rate":30,
         "tracks":[{"node":0,"flags":2,"frames":[0,44],"values":[0,1.3]}]},
        {"name":"OPEN","frames":0}]}]})");
    writeTextFile(directory / "collision.json", R"({"objects":[
      {"object":0,"normals":[0,1,0,0,1,0],"vertices":[
       -40,0,-40,40,0,40,40,0,-40,-40,0,-40,-40,0,40,40,0,40]}]})");
    const std::array<test::NativeSoundSample, 1> samples{{{48000, {8192, 8192, 8192, 8192}}}};
    test::writeNativeSoundBank(directory / "audio" / "GATE", R"({"sounds":[
      {"index":0,"name":"S_GATEWOODG","id":0,"duration":-1,"volume":127,
       "duck":0,"priority":0,"sequence":[{"sample":0}]}]})",
                               samples);
    return directory;
}

/** Complete quiet Stage, not a gate-only simulator. The independent oracle
 * uses public native world/party/fixture/opponent phases in PlayScene order.
 * No fixture callback is replaced with a no-op: an unexpected interaction
 * fails the test, and the allowed gate help route checks the real policy. */
struct Fixture {
    std::filesystem::path assets =
        test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    AudioMixer mixer{48000};
    SoundPlayer sound{mixer};
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
    PlayerHealth health;
    LevelWatch levels;
    CombatResources resources;
    std::unique_ptr<LevelWorldCheckpoint> worldState;
    std::unique_ptr<LevelFixturesCheckpoint> fixtureState;
    std::unique_ptr<CombatRollbackCheckpoint> combat;
    std::unique_ptr<Boundary> boundary;
    std::unique_ptr<Tick> runner;
    std::vector<SoundPlayer::Event> heard;

    explicit Fixture(const std::filesystem::path& directory) {
        REQUIRE(classes.load(assets / "PDATA"));
        REQUIRE(weapons.load(assets / "WEAPONS"));
        REQUIRE(world.load(device, directory, {.realmId = 7, .name = "TEST", .directory = "."}));
        world.setPlayerCount(2);
        const LevelAudioInfo info{.bank = "GATE"};
        audio.open(directory, &sound, &info, 'G');
        arsenal.bind({device, classes, weapons, world.collision(), effects, audio});
        attacks.bind({device, classes, world, weapons, effects, audio, nullptr, arsenal, dimmer});
        fixtures.bind({device, world, weapons, effects, audio});
        fixtures.setPlayerCount(2);
        REQUIRE(fixtures.gates().size() == 2);
        std::array<PlayerRuntimeCheckpoint*, 2> ordered{};
        for (usize index = 0; index < players.size(); ++index) {
            auto& player = players[index];
            CharacterSave save;
            save.name = "GATE";
            save.character = 0;
            save.color = static_cast<s32>(index);
            save.autoAttack = false;
            save.progress().health = 10000;
            save.progress().inventory.keys = 1;
            const auto* stats = classes.stats(save.character);
            REQUIRE(stats);
            const Vec3 position{index == 0 ? -4.0f : 4.0f, 0, -3};
            player.actor.spawn(index == 0 ? 3 : 1, save, stats, position, 0);
            player.actor.settle(world.collision());
            FloorRiding::land(players, index, position, world.collision());
            // The half-stick approach below lasts fifty native ticks. Start
            // outside contact, but close enough to reach this axis-aligned gate
            // at the loaded class's actual pace before that input ends.
            const auto& box = fixtures.gates().gate(index).box;
            REQUIRE_FALSE(box.touchedBy(player.actor.position(), player.actor.radius()));
            const f32 gap =
                box.centre.z - player.actor.position().z - box.halfAlong - player.actor.radius();
            REQUIRE(gap > 0);
            REQUIRE(player.actor.speed() * .5f * (50.0f / 60) > gap);
            player.entrySave = save;
            player.figure = PlayerFigure::load(device, assets, save, false);
            REQUIRE(player.figure);
            player.figure->setGuardArmor(armorDefense(*stats, save.progress()));
            owners[index] = std::make_unique<PlayerRuntimeCheckpoint>(player, stats);
            ordered[index] = owners[index].get();
            levels.observe(player.actor.player(), experienceLevel(save.experience()));
        }
        opponents.open({device, world, weapons, effects, audio, assets, 1, true}, players);
        REQUIRE(opponents.enemies().count() == 0);
        REQUIRE(StageResources::preload(world, opponents.enemies(), opponents.critters(),
                                        opponents.bosses()));
        REQUIRE(resources.bindStage(device, world, weapons, classes, players, arsenal, opponents,
                                    effects));
        combat = std::make_unique<CombatRollbackCheckpoint>(
            ordered, health, levels, attacks, arsenal, effects, resources.resources());
        worldState = std::make_unique<LevelWorldCheckpoint>(world);
        fixtureState = std::make_unique<LevelFixturesCheckpoint>(fixtures);
        REQUIRE(worldState->bindResources(device));
        REQUIRE(fixtureState->bindResources());
        boundary = std::make_unique<Boundary>(*combat, opponents,
                                              OpponentResources::missiles(opponents.enemies()),
                                              worldState.get(), fixtureState.get());
        runner = std::make_unique<Tick>(*boundary, classes, fixtures);
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
        opponents.close();
        effects.clear();
        resources.clear();
        audio.close();
    }
    std::unique_ptr<Boundary::State> state() const {
        auto value = boundary->checkpoint();
        REQUIRE(value);
        return std::make_unique<Boundary::State>(std::move(*value));
    }
    static Tick::Frame frame(s32 elapsed, s32 ticks) {
        Tick::Frame result;
        result.ticks = ticks;
        result.seconds = static_cast<f32>(ticks) / 60;
        result.camera.position = {0, 35, -50};
        result.camera.pitch = .6f;
        result.projection.horizontalFov = 1.6f;
        result.projection.aspect = 2;
        result.damage.realm = 7;
        result.damage.playSeconds = static_cast<f32>(elapsed) / 60;
        return result;
    }
    void native(const std::array<PlayInput, 4>& input, const Tick::Frame& frame) {
        heard.clear();
        SoundPlayer::Capture capture(&sound, [&](const auto& event) { heard.push_back(event); });
        PartyFigures::snapshot(players);
        effects.capturePresentation();
        world.capturePresentation();
        fixtures.capturePresentation();
        PartyNames::advance(players, frame.ticks);
        REQUIRE(
            PlayerPowerups::update(players, frame.seconds, PlayerPowerups::Clock::Level).empty());
        for (auto& player : players) {
            REQUIRE(player.streak.step(frame.seconds).empty());
            REQUIRE_FALSE(player.gem.update(0));
            REQUIRE(player.figure->syncCompanionPowerups(player.actor.save().progress().inventory));
        }
        world.update(frame.seconds, false);
        opponents.syncFloors();
        world.revealCrystals(frame.seconds);
        for (auto& player : players) {
            player.transport.observeContact(false);
        }
        const PlayerAttacks::Targets targets{
            .opponents = opponents, .fixtures = fixtures, .fixtureEvents = {}, .players = players};
        PartyMotion::Events motion;
        motion.perform = [](usize, PartyMotion::Action action) {
            REQUIRE((action == PartyMotion::Action::FirstFoot ||
                     action == PartyMotion::Action::SecondFoot));
        };
        motion.select = [](usize, const SelectorInput& selection, s32) {
            REQUIRE_FALSE((selection.up || selection.down || selection.left || selection.right));
        };
        motion.advanceTurbo = [&](usize index, s32 ticks, f32 seconds) {
            attacks.updateTurbo(index, ticks, seconds, players,
                                [](s32, usize) { FAIL("unexpected turbo lesson"); });
        };
        motion.aim = [&](usize index) {
            const auto& actor = players[index].actor;
            return attacks.aim(actor, actor.facing(), targets);
        };
        motion.limitMovement = [&](usize index, const Vec3& from, const Vec3& to) {
            const auto& actor = players[index].actor;
            return CameraMovementLimit::constrain(from, to, frame.attention, frame.camera,
                                                  frame.projection,
                                                  actor.followPoint() - actor.position());
        };
        motion.meleeSense = [&](usize index, bool held, const Vec3& facing) {
            return attacks.meleeSense(players[index].actor, held, targets, facing);
        };
        motion.resolveMovement = [&](usize index, const Vec3& from, const Vec3& to) {
            return opponents.resolveMovement(players[index].actor, from, to);
        };
        PartyMotion::step(players, input, false, frame.camera.yaw, frame.ticks, frame.seconds,
                          world.collision(), motion);
        PartyRecords::advanceTime(players, frame.seconds);
        const LevelFixtures::Events fixtureEvents{.help = [&](s32 id, usize recipient) {
            REQUIRE(PartyHelpAdmission::suppressed(id, recipient, players));
            return false;
        }};
        fixtures.update(frame.ticks, frame.seconds, players, fixtureEvents);
        for (auto& player : players) {
            PlayerHealth::advanceSurfaceGap(player, frame.seconds);
        }
        PlayerHealth::heartbeat(players, frame.ticks, false);
        opponents.watch(
            ViewVolume::of(frame.camera, frame.projection.horizontalFov, frame.projection.aspect),
            frame.attention);
        LevelOpponents::Events events;
        events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {
            FAIL("unexpected damage in gate-only stage");
        };
        events.settleBlasts = [&] { fixtures.settleBlasts(players, fixtureEvents); };
        events.advanceLegend = [&](f32) { REQUIRE_FALSE(opponents.bosses().present()); };
        events.advanceVictory = [&](s32, f32) { REQUIRE_FALSE(opponents.bosses().present()); };
        events.levels = [&] {
            for (const auto& player : players) {
                REQUIRE_FALSE(levels.observe(player.actor.player(),
                                             experienceLevel(player.actor.save().experience())));
            }
        };
        events.help = [](s32, usize) {
            FAIL("unexpected opponent lesson");
            return false;
        };
        opponents.update(frame.ticks, frame.seconds, players, fixtures.obstacles(), events,
                         fixtures.missileStops(), fixtures.critterObstacles());
        attacks.updateProjectiles(frame.seconds, players, targets);
        attacks.updateStrikes(frame.seconds, players, targets);
        attacks.updateShields(frame.seconds, players, targets);
        attacks.updateArmour(frame.seconds, players, targets);
        effects.update(frame.seconds);
        PartyPickups pickups;
        PartyHud hud;
        pickups.collect(device, players,
                        {.world = world,
                         .fixtures = fixtures,
                         .hud = hud,
                         .audio = audio,
                         .classes = classes,
                         .help = [](s32, usize) {
                             FAIL("unexpected pickup lesson");
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
    }
};

TEST_CASE("whole ordinary Stage opens keyed gates with native passability and rewind",
          "[rollback][ordinary-keyed-gate-tick][assets]") {
    const s32 ticks = GENERATE(1, 2);
    const bool moving = GENERATE(false, true);
    const auto directory = gateStage(moving);
    auto native = std::make_unique<Fixture>(directory);
    auto replay = std::make_unique<Fixture>(directory);
    const auto initial = native->state();
    REQUIRE(replay->boundary->restore(*initial));
    auto undo = replay->boundary->prepareRewind();
    REQUIRE(undo);
    const auto uploads = replay->device.texturesCreated;
    bool opening = false, passableOpening = false, opened = false;
    usize cues = 0;
    for (s32 elapsed = 0; elapsed < 180; elapsed += ticks) {
        std::array<PlayInput, 4> input{};
        if (elapsed < 50) {
            input[3].move = {{0, 1}, .5f};
            input[1].move = {{0, 1}, .5f};
        }
        const auto frame = Fixture::frame(elapsed, ticks);
        native->native(input, frame);
        std::vector<SoundPlayer::Event> leakedAudio;
        const auto result = [&] {
            SoundPlayer::Capture capture(&replay->sound,
                                         [&](const auto& event) { leakedAudio.push_back(event); });
            return replay->runner->step(replay->players, input, frame);
        }();
        CAPTURE(ticks, moving, elapsed, result.refusal, result.partyRefusal);
        REQUIRE(result.status == Tick::Status::Advanced);
        CHECK(leakedAudio.empty());
        CHECK(*replay->state() == *native->state());
        const auto& gate = replay->fixtures.gates().gate(0);
        opening |= gate.state == LockedGates::kOpening;
        passableOpening |= gate.state == LockedGates::kOpening && !gate.box.solid;
        opened |= gate.state == LockedGates::kOpen;
        REQUIRE(result.fixtureSounds.size() == native->heard.size());
        for (const auto& cue : result.fixtureSounds) {
            CHECK(cue.name == "S_GATEWOODG");
            CHECK(cue.volume == 127.0f / 255);
        }
        for (const auto& event : native->heard) {
            REQUIRE(event.kind == SoundPlayer::Event::Kind::Play);
            CHECK(event.call == 0);
            CHECK(event.value == 127.0f / 255);
            ++cues;
        }
        CHECK(replay->device.texturesCreated == uploads);
        if (elapsed == 60) {
            // A new authority boundary in the opening animation resumes exact
            // clocks, key counts, support lag and collision in another owner.
            const auto correction = native->state();
            REQUIRE(replay->boundary->canRewind(*undo));
            REQUIRE(replay->boundary->rewind(std::move(*undo)));
            CHECK(*replay->state() == *initial);
            REQUIRE(replay->boundary->restore(*correction));
            undo = replay->boundary->prepareRewind(*initial);
            REQUIRE(undo);
        }
    }
    CHECK(opening);
    CHECK(passableOpening);
    CHECK(opened);
    CHECK(cues == 2);
    for (const auto& player : replay->players) {
        CHECK(player.actor.save().progress().inventory.keys == 0);
    }
    REQUIRE(replay->boundary->rewind(std::move(*undo)));
    CHECK(*replay->state() == *initial);
}

TEST_CASE("whole Stage refuses a new gate lesson without spending an earlier key",
          "[rollback][ordinary-keyed-gate-tick][assets]") {
    auto fixture = std::make_unique<Fixture>(gateStage(false));
    for (usize index = 0; index < fixture->players.size(); ++index) {
        auto& actor = fixture->players[index].actor;
        actor.slide(fixture->fixtures.gates().gate(index).figure.position() - actor.position(),
                    nullptr);
        actor.save().progress().inventory.keys = index == 0 ? 1 : 0;
    }
    const auto before = fixture->state();
    const std::array<PlayInput, 4> input{};
    const auto result = fixture->runner->step(fixture->players, input, Fixture::frame(0, 1));
    REQUIRE(result.status == Tick::Status::Unsupported);
    CHECK(result.refusal == Tick::Refusal::Fixtures);
    CHECK(result.fixtureSounds.empty());
    CHECK(*fixture->state() == *before);
    CHECK(fixture->fixtures.gates().gate(0).state == LockedGates::kShut);
    CHECK(fixture->players[0].actor.save().progress().inventory.keys == 1);
}
} // namespace
