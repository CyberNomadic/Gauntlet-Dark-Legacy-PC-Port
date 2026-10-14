#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/AssetScope.h"
#include "engine/io/File.h"
#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/players/ItemPickup.h"
#include "game/players/PowerupEffects.h"
#include "game/players/Progression.h"
#include "game/screens/CombatResources.h"
#include "game/screens/EnemyFeedbackEmission.h"
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
#include "game/world/LevelCatalog.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Boundary = OrdinaryCombatCheckpoint;
using Tick = OrdinaryCombatTick;
using Output = PartyDamage::Output;
using OutputKind = PartyDamage::OutputKind;

enum class MovingStage { None, Lift, Spinner, Transporter };

std::filesystem::path movingStage(MovingStage kind) {
    const auto directory = test::sampleLevel("ordinary-combat-moving-stage");
    if (kind == MovingStage::Transporter) {
        writeTextFile(directory / "world.json", R"({"objects":[
          {"name":"FLOOR","position":[20,1,0],"flags":4100,"next":1},
          {"name":"FLOOR","position":[0,0,0],"flags":4}],
          "animations":[{"object":0,"frames":121,"state":257,"start":0,
            "track":{"flags":16,"frames":[0,120],"values":[0,-44]}}],
          "itemInfos":[{"type":11,"name":"TRANS","radius":3,"height":4}],
          "itemInstances":[
            {"info":0,"position":[20,1,0],"params":[1,0,0,0,2,0,0,0,0,0,0,0]},
            {"info":0,"position":[-40,0,-40],"params":[2,0,0,0,1,0,0,0,0,0,0,0]}],
          "bounds":{"min":[-60,-20,-60],"max":[60,30,60]}})");
        writeTextFile(directory / "collision.json", R"({"objects":[
          {"object":0,"normals":[0,1,0,0,1,0],
           "vertices":[-1,0,-1,1,0,1,1,0,-1,-1,0,-1,-1,0,1,1,0,1]},
          {"object":1,"normals":[0,1,0,0,1,0],
           "vertices":[-60,0,-60,60,0,60,60,0,-60,-60,0,-60,-60,0,60,60,0,60]}]})");
        return directory;
    }
    const bool lift = kind == MovingStage::Lift;
    const std::string motion =
        lift ? R"("animations":[{"object":0,"frames":2000,"state":257,"start":0,
                   "track":{"flags":32,"frames":[0,1999],"values":[0,4]}}],)"
             : R"("itemInfos":[{"type":12,"subtype":0,"name":"SPIN","radius":2,"height":1}],
                  "itemInstances":[{"info":0,"flags":2,"position":[0,0,0],
                    "params":[0,0,0,0,10,215,35,60,0,0,128,63]}],)";
    writeTextFile(directory / "world.json",
                  std::string{R"({"objects":[
                    {"name":"FLOOR","position":[0,0,0],"flags":4100,"next":1},
                    {"name":"FLOOR","position":[0,-20,0],"flags":4}],)"} +
                      motion + R"("bounds":{"min":[-60,-20,-60],"max":[60,30,60]}})");
    writeTextFile(directory / "collision.json", R"({"objects":[
      {"object":0,"normals":[0,1,0,0,1,0],
       "vertices":[-20,0,-20,20,0,20,20,0,-20,-20,0,-20,-20,0,20,20,0,20]},
      {"object":1,"normals":[0,1,0,0,1,0],
       "vertices":[-60,-20,-60,60,-20,60,60,-20,-60,-60,-20,-60,-60,-20,60,60,-20,60]}]})");
    return directory;
}

struct Fixture {
    std::filesystem::path root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    ClassDataSet classes;
    LevelWorld world;
    ExitPortals portals;
    LevelTransporters transporters;
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
    CombatResources resources;
    std::unique_ptr<LevelWorldCheckpoint> worldState;
    std::unique_ptr<LevelFixturesCheckpoint> fixtureState;
    TowerCamera camera;
    std::unique_ptr<TowerCameraCheckpoint> cameraState;
    std::unique_ptr<ExitPortalsCheckpoint> portalState;
    std::unique_ptr<LevelTransportersCheckpoint> transporterState;
    std::unique_ptr<CombatRollbackCheckpoint> combat;
    std::unique_ptr<Boundary> boundary;
    std::unique_ptr<Tick> runner;
    std::vector<Output> outputs;
    std::vector<Tick::Footstep> footsteps;
    std::vector<s32> meleeHits;
    std::vector<std::pair<s32, MissileRelease>> releases;
    std::function<bool(s32, usize)> help;
    WorldLayout generatorLayout;
    bool stage = false;

    explicit Fixture(bool explosive = false, bool statue = false, bool melee = false,
                     s32 firstClass = 0, bool ranged = false, bool stageG1 = false,
                     MovingStage moving = MovingStage::None, bool impactCue = false,
                     bool generators = false, s32 firstLevel = 0, bool phoenix = false)
        : stage(stageG1 || moving != MovingStage::None) {
        REQUIRE(classes.load(root / "PDATA"));
        if (melee || stage) {
            REQUIRE(weapons.load(root / "WEAPONS"));
        }
        if (stage) {
            LevelCatalog catalog;
            REQUIRE(catalog.load(root));
            if (moving == MovingStage::None) {
                const auto level = catalog.byName("G1");
                REQUIRE(level);
                REQUIRE(world.load(device, root, *level));
            } else {
                const auto directory = movingStage(moving);
                if (phoenix) {
                    // Bind pickup texture clocks against the final archive set,
                    // exactly as a real level load does. Loading POWERUPS after
                    // LevelWorld::load changes its borrowed archive identity.
                    std::filesystem::create_directories(directory / "POWERUPS");
                    for (const auto file : {"ANIM.PS2", "objects.ngc", "textures.ngc"}) {
                        std::filesystem::copy_file(
                            root / "POWERUPS" / file, directory / "POWERUPS" / file,
                            std::filesystem::copy_options::overwrite_existing);
                    }
                }
                REQUIRE(world.load(device, directory, {.name = "TEST", .directory = "."}));
            }
            world.setPlayerCount(2);
            portals.bind(device, world.layout(), world.itemScope(), catalog, &world.collision());
            transporters.bind(device, world.layout(), world.itemScope(), 2, &world.collision());
        }
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
        if (statue) {
            const auto directory = test::sampleLevel("ordinary-combat-dormant-statue");
            writeTextFile(directory / "world.json", R"({
              "objects":[{"name":"WALL","position":[0,0,0]}],
              "itemInfos":[{"type":4,"name":"GOLEM","radius":2,"height":5}],
              "itemInstances":[{"info":0,"minPlayers":1,"position":[-12,0,20],
                "params":[1,0,0,0,0,0,32,65,0,0,0,0]}]})");
            REQUIRE(world.load(device, directory, {.name = "G1", .directory = "."}));
        }
        if (!stage && !explosive && !statue) {
            // An unopened LevelWorld has the Tower identity. Use a real ordinary
            // level owner before installing the broad test floor below; Tower
            // visits and ceremonies deliberately remain outside this phase.
            const auto directory = test::sampleLevel("ordinary-combat-flat-stage");
            writeTextFile(directory / "world.json",
                          R"({"objects":[{"name":"FLOOR","position":[0,0,0],"flags":4}]})");
            REQUIRE(world.load(device, directory, {.name = "TEST", .directory = "."}));
            REQUIRE_FALSE(world.isTower());
        }
        // All native owners borrow this exact world's collision object.
        auto& floor = const_cast<WorldCollision&>(world.collision());
        if (!stage) {
            floor.build({{{0, 1, 0}, {{{-200, 0, -200}, {200, 0, -200}, {200, 0, 200}}}, -1, 14},
                         {{0, 1, 0}, {{{-200, 0, -200}, {200, 0, 200}, {-200, 0, 200}}}, -1, 14}});
        }
        if (phoenix && !world.powerups().loaded()) {
            REQUIRE(world.powerups().load(root / "POWERUPS"));
        }
        arsenal.bind({device, classes, weapons, world.collision(), effects, audio, nullptr,
                      impactCue ? "S_WEAPONHITWOOD" : "", false, false, &world.powerups()});
        attacks.bind({device, classes, world, weapons, effects, audio, nullptr, arsenal, dimmer});
        fixtures.bind({device, world, weapons, effects, audio});
        fixtures.setPlayerCount(2);
        constexpr std::array kSlots{3, 1};
        const std::array kClasses{firstClass, 6};
        for (usize index = 0; index < players.size(); ++index) {
            auto& player = players[index];
            CharacterSave save;
            save.name = "TEST";
            save.character = kClasses[index];
            save.autoAttack = false;
            save.progress().health = 10000;
            if (melee) {
                save.progress().experience = levelExperience(20);
            }
            if (index == 0 && firstLevel > 0) {
                save.progress().experience = levelExperience(firstLevel);
            }
            if (index == 0 && phoenix) {
                save.progress().inventory.addPowerup(powerup::kSpecial, powerup::kPhoenix, 0, 60);
            }
            const auto* stats = classes.stats(save.character);
            REQUIRE(stats);
            Vec3 position{index == 0 ? -12.0f : 12.0f, 0, 0};
            f32 yaw = 0;
            if (stage && moving == MovingStage::None) {
                const auto* start = world.startPoint(0);
                REQUIRE(start);
                yaw = start->rotation.y + kPi;
                position = start->position +
                           Vec3{std::cos(yaw), 0, -std::sin(yaw)} * (index == 0 ? -1.5f : 1.5f);
            } else if (moving != MovingStage::None) {
                position = {index == 0 ? -2.0f : 2.0f, 0, 0};
            }
            player.actor.spawn(kSlots[index], save, stats, position, yaw);
            if (stage) {
                player.actor.settle(world.collision());
                FloorRiding::land(players, index, position, world.collision());
            }
            player.entrySave = save;
            player.figure = PlayerFigure::load(device, root, save, false);
            REQUIRE(player.figure);
            player.figure->setCompanionPowerups(device, world.powerups(), save.progress().inventory,
                                                &weapons);
            player.figure->setGuardArmor(armorDefense(*stats, save.progress()));
            player.turbo.add(100);
            owners[index] = std::make_unique<PlayerRuntimeCheckpoint>(player, stats);
            ordered[index] = owners[index].get();
            levels.observe(kSlots[index], experienceLevel(save.experience()));
        }
        // Match PlayScene: native stage placements stand only when the camera
        // sees them, not all at once (including remote special creatures).
        opponents.open({device, world, weapons, effects, audio, root, 1, stage}, players);
        if (statue) {
            REQUIRE(opponents.statues().count() == 1);
            REQUIRE_FALSE(opponents.statues().woken(0));
            REQUIRE(opponents.critters().count() == 0);
        }
        auto& enemies = opponents.enemies();
        if (stage) {
            REQUIRE(
                StageResources::preload(world, enemies, opponents.critters(), opponents.bosses()));
        } else {
            enemies.open(device, root, &world.collision(), 8,
                         {.health = ranged ? 0.05f : 1.0f, .players = 2}, 79);
            REQUIRE(enemies.loadKind(kGruntKind));
            if (!generators) {
                for (const auto& player : players) {
                    REQUIRE(enemies.spawn(
                        {.kind = kGruntKind,
                         .tier = 3,
                         .position = player.actor.position() + Vec3{0, 0, ranged ? 15.0f : 3.0f},
                         .placed = true},
                        {}));
                }
            }
        }
        if (generators) {
            const auto directory = test::scratchDirectory("ordinary-generator-contact");
            // An authored negative quota keeps these isolated contact fixtures
            // from breeding. Actual G1 brood/quota continuation is tested below.
            writeTextFile(directory / "world.json", R"({
              "objects":[{"name":"GROUND","position":[0,0,0]}],
              "itemInfos":[{"type":3,"name":"GRU","radius":1,"height":5,
                             "collisionType":1,"hitPoints":1}],
              "itemInstances":[
                {"info":0,"minPlayers":1,"position":[-12,0,3],
                 "params":[3,0,7,0,255,255,1,0,0,0,0,0]},
                {"info":0,"minPlayers":1,"position":[12,0,3],
                 "params":[3,0,7,0,255,255,1,0,0,0,0,0]}]})");
            REQUIRE(generatorLayout.load(directory));
            REQUIRE(opponents.generators().bind(device, generatorLayout, enemies,
                                                &world.collision(), {}, 2));
            REQUIRE(opponents.generators().count() == 2);
        }
        missileResources = OpponentResources::missiles(enemies);
        if (melee || stage) {
            REQUIRE(resources.bindStage(device, world, weapons, classes, players, arsenal,
                                        opponents, effects));
        }
        combat = std::make_unique<CombatRollbackCheckpoint>(
            ordered, health, levels, attacks, arsenal, effects, resources.resources());
        if (stage) {
            worldState = std::make_unique<LevelWorldCheckpoint>(world);
            fixtureState = std::make_unique<LevelFixturesCheckpoint>(fixtures);
            REQUIRE(worldState->bindResources(device));
            REQUIRE(fixtureState->bindResources());
            std::vector<CameraSubject> subjects;
            for (const auto& player : players) {
                subjects.push_back({player.actor.position(), player.actor.followPoint(),
                                    player.actor.height() * .5f});
            }
            camera.reset(subjects, world.cameraMarkers(), world.cameraRange(),
                         frame(0, 1).projection);
            cameraState = std::make_unique<TowerCameraCheckpoint>(camera, world);
            portalState = std::make_unique<ExitPortalsCheckpoint>(
                portals, world.layout(), world.items(), &world.realmItems());
            REQUIRE(portalState->bindResources());
            transporterState = std::make_unique<LevelTransportersCheckpoint>(transporters);
            REQUIRE(transporterState->bindResources());
        }
        boundary = std::make_unique<Boundary>(
            *combat, opponents, missileResources, worldState.get(), fixtureState.get(),
            cameraState.get(), portalState.get(), transporterState.get());
        runner = std::make_unique<Tick>(*boundary, classes, fixtures);
    }
    ~Fixture() {
        runner.reset();
        boundary.reset();
        transporterState.reset();
        portalState.reset();
        combat.reset();
        fixtureState.reset();
        worldState.reset();
        attacks.clear();
        arsenal.clear();
        fixtures.clear();
        effects.clear();
        resources.clear();
        opponents.close();
    }
    Boundary::State state() const {
        const auto value = boundary->checkpoint();
        if (!value) {
            INFO("complete boundary refused; checking each native owner");
            const auto body = combat->checkpoint();
            REQUIRE(body);
            const auto enemies = opponents.enemies().checkpoint();
            REQUIRE(enemies);
            const auto generators = opponents.generators().checkpoint();
            REQUIRE(generators);
            OpponentCombatCheckpoint adapter{const_cast<LevelOpponents&>(opponents)};
            const auto other = adapter.checkpoint(body->effects);
            REQUIRE(other);
            if (stage) {
                const auto level = worldState->checkpoint();
                REQUIRE(level);
                const auto objects = fixtureState->checkpoint();
                REQUIRE(objects);
                for (const auto& player : body->players) {
                    INFO("player " << player.player << " floor=" << player.floor.object
                                   << " placement=" << player.floor.placement.has_value());
                    CHECK((player.floor.object == -1 ||
                           worldState->hasCollisionObject(player.floor.object)));
                }
            }
        }
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
    void native(std::span<const PlayInput> inputs, const Tick::Frame& suppliedFrame,
                bool currency = false) {
        auto frame = suppliedFrame;
        if (stage) {
            frame.camera = camera.camera();
            frame.attention = camera.attention();
        }
        outputs.clear();
        footsteps.clear();
        meleeHits.clear();
        releases.clear();
        if (stage) {
            PartyFigures::snapshot(players);
            effects.capturePresentation();
            world.capturePresentation();
            fixtures.capturePresentation();
            transporters.capturePresentation();
            PartyNames names;
            names.step(players, frame.ticks, false);
            REQUIRE(PlayerPowerups::update(players, frame.seconds, PlayerPowerups::Clock::Level)
                        .empty());
            for (auto& player : players) {
                REQUIRE(player.streak.step(frame.seconds).empty());
                REQUIRE_FALSE(player.gem.update(0));
            }
            world.update(frame.seconds, false);
            opponents.syncFloors();
            transporters.syncFloors(world.collision());
            world.revealCrystals(frame.seconds);
            for (auto& player : players) {
                player.transport.observeContact(false);
            }
            transporters.animate(frame.seconds);
        }
        for (auto& player : players) {
            player.figure->setCompanionPowerups(device, world.powerups(),
                                                player.actor.save().progress().inventory, &weapons);
        }
        const PlayerAttacks::Targets targets{
            .opponents = opponents,
            .fixtures = fixtures,
            .fixtureEvents = {},
            .players = players,
            .meleeHit = [&](s32 player) { meleeHits.push_back(player); }};
        const PartyMotion::Events motion{
            .perform =
                [&](usize index, PartyMotion::Action action) {
                    if (action == PartyMotion::Action::Melee) {
                        attacks.melee(index, players, targets);
                        return;
                    }
                    if (action == PartyMotion::Action::ThrowWeapon ||
                        action == PartyMotion::Action::StrongThrow) {
                        auto& player = players[index];
                        const auto& actor = player.actor;
                        const auto before = arsenal.missiles().count();
                        arsenal.launchWeapon(
                            actor, player.figure.get(), actor.facing(),
                            action == PartyMotion::Action::StrongThrow ? 2.0f : 1.0f, true,
                            player.cursorAiming ? std::nullopt
                                                : attacks.aim(actor, actor.facing(), targets));
                        if (arsenal.missiles().count() > before) {
                            REQUIRE(arsenal.missiles().missile(before).release);
                            releases.emplace_back(actor.player(),
                                                  *arsenal.missiles().missile(before).release);
                        }
                        return;
                    }
                    if (action == PartyMotion::Action::FamiliarShot) {
                        auto& player = players[index];
                        arsenal.launchFamiliar(
                            player.actor, player.figure.get(),
                            player.cursorAiming
                                ? std::nullopt
                                : attacks.aim(player.actor, player.actor.facing(), targets));
                        return;
                    }
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
                    attacks.updateTurbo(index, ticks, seconds, players, [&](s32 id, usize) {
                        const bool crowd = opponents.enemies().inView() >= 15;
                        if ((id == HelpMessages::kUseCombo && (!crowd || players.size() <= 1)) ||
                            (id == HelpMessages::kUseTurbo && !crowd)) {
                            return;
                        }
                        REQUIRE(HelpMessages::specOf(id) == nullptr);
                    });
                },
            .aim =
                [&](usize index) {
                    const auto& actor = players[index].actor;
                    return attacks.aim(actor, actor.facing(), targets);
                },
            .limitMovement =
                [&](usize index, const Vec3& from, const Vec3& to) {
                    return CameraMovementLimit::constrain(
                        from, to, frame.attention, frame.camera, frame.projection,
                        players[index].actor.followPoint() - players[index].actor.position());
                },
            .attackDeed =
                [&](usize index, bool strong, bool moved, const Vec3& facing) {
                    return attacks.attackDeed(players[index].actor, strong, targets, moved,
                                              players[index].figure->animator().meleeChain(),
                                              facing);
                },
            .automaticMeleeDeed =
                [&](usize index, const Vec3& facing) {
                    if (!frame.autoMelee) {
                        return PlayerDeed::None;
                    }
                    return attacks.automaticMeleeDeed(players[index].actor, targets, facing);
                },
            .meleeSense =
                [&](usize index, bool held, const Vec3& facing) {
                    return attacks.meleeSense(players[index].actor, held, targets, facing);
                },
            .resolveMovement =
                [&](usize index, const Vec3& from, const Vec3& to) {
                    return opponents.resolveMovement(players[index].actor, from, to);
                }};
        const auto subjects =
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
        events.help = [&](s32 id, usize index) {
            if (help) {
                return help(id, index);
            }
            FAIL_CHECK("unexpected opponent lesson");
            return false;
        };
        events.award = [&](s32 player, s32 experience, bool killed) {
            PartyRecords::award(players, player, experience, killed, world.level());
        };
        events.destroyedGenerator = [&](s32 player) {
            PartyRecords::destroyedGenerator(players, player);
        };
        opponents.update(frame.ticks, frame.seconds, players, fixtures.obstacles(), events,
                         fixtures.missileStops(), fixtures.critterObstacles());
        attacks.updateProjectiles(frame.seconds, players, targets);
        attacks.updateStrikes(frame.seconds, players, targets);
        attacks.updateShields(frame.seconds, players, targets);
        attacks.updateArmour(frame.seconds, players, targets);
        effects.update(frame.seconds);
        if (stage) {
            PartyPickups pickups;
            PartyHud hud;
            pickups.collect(device, players,
                            {.world = world,
                             .fixtures = fixtures,
                             .hud = hud,
                             .audio = audio,
                             .classes = classes,
                             .help = [&](s32 id, usize recipient) {
                                 if (currency) {
                                     REQUIRE(
                                         PartyHelpAdmission::suppressed(id, recipient, players));
                                     return false;
                                 }
                                 FAIL_CHECK("unexpected native pickup lesson");
                                 return false;
                             }});
            if (!currency) {
                REQUIRE(hud.pickups().cards().empty());
            }
            std::vector<TriggerVisitor> visitors;
            for (usize index = 0; index < players.size(); ++index) {
                const auto& actor = players[index].actor;
                TriggerVisitor visitor;
                visitor.position = actor.position();
                visitor.radius = actor.reach();
                visitor.height = actor.height();
                visitor.crystals = actor.save().progress().crystals;
                visitor.gargoylePieces = actor.save().progress().relics.gargoylePieces;
                visitor.sumner = actor.save().character == kSumnerClass;
                visitor.party = static_cast<s32>(index);
                if (const auto support = world.collision().floorAt(actor.position(), .5f, 1)) {
                    visitor.floorObject = support->object;
                }
                visitors.push_back(visitor);
            }
            world.updateTriggers(frame.seconds, visitors);
            fixtures.syncFloors();
            opponents.syncFloors();
            transporters.syncFloors(world.collision());
            REQUIRE(world.takeRotatorCues().empty());
            std::vector<PortalVisitor> portalVisitors;
            for (usize index = 0; index < players.size(); ++index) {
                const auto& actor = players[index].actor;
                const auto seat = static_cast<usize>(actor.player());
                portalVisitors.push_back({actor.position(), actor.radius(), static_cast<s32>(index),
                                          seat >= inputs.size() || !inputs[seat].move.any()});
            }
            REQUIRE_FALSE(portals.update(frame.ticks, frame.seconds, portalVisitors));
            REQUIRE(portals.takeWaiting().empty());
            for (auto& player : players) {
                REQUIRE_FALSE(player.exitWait.step(false, frame.ticks));
            }
            camera.update(subjects, world.cameraMarkers(), world.cameraRange(), frame.projection,
                          frame.seconds);
        }
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
        INFO(Tick::kRefusals[static_cast<usize>(result.refusal)]);
        REQUIRE(result.status == Tick::Status::Advanced);
        // Standalone callers retain per-tick atomic undo even though the
        // private network owner can retain one over an authority interval.
        CHECK(result.timings.prepareNanoseconds.has_value());
        CHECK(result.timings.captureNanoseconds.has_value());
        CHECK(result.timings.postconditionNanoseconds.has_value());
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

TEST_CASE("ordinary combat preserves native party shoves pushed recovery and late refusal undo",
          "[ordinary-combat-tick][ordinary-pushed][assets]") {
    const s32 ticks = GENERATE(1, 2);
    auto fixture = std::make_unique<Fixture>();
    auto& first = fixture->players[0].actor;
    auto& second = fixture->players[1].actor;
    const f32 separation = first.radius() + second.radius() + 0.01f;
    first.place({0, 0, -40});
    second.place({separation, 0, -40});
    const auto uploads = fixture->device.texturesCreated;
    std::unique_ptr<Boundary::State> pushed;
    bool queuedShove = false;
    bool recovered = false;
    for (s32 step = 0; step < 90; ++step) {
        CAPTURE(ticks, step);
        std::array<PlayInput, 4> input{};
        // Use real ordered party collision, not a fabricated pushed flag or
        // animation checkpoint. The receiver retains its own input slot (1).
        if (step * ticks < 20) {
            input[3].move = {Vec2{1, 0}, 1};
        }
        auto frame = Fixture::frame(step, ticks);
        frame.attention = {0, 0, -40};
        frame.camera.position += frame.attention;
        auto before = std::make_unique<Boundary::State>(fixture->state());
        fixture->native(input, frame);
        auto expected = std::make_unique<Boundary::State>(fixture->state());
        const auto outputs = fixture->outputs;
        const auto footsteps = fixture->footsteps;
        queuedShove |= fixture->players[1].knockback.pushed();
        const bool receiving =
            fixture->players[1].figure->animator().action() == PlayerAnimator::Action::Pushed;
        if (receiving && !pushed) {
            pushed = std::make_unique<Boundary::State>(*expected);
        }
        recovered |= pushed && step * ticks > 40 && !receiving &&
                     PlayerAnimator::locomotion(fixture->players[1].figure->animator().action());
        REQUIRE(fixture->boundary->restore(*before));
        const auto result = fixture->runner->step(fixture->players, input, frame);
        INFO(Tick::kRefusals[static_cast<usize>(result.refusal)]);
        INFO(Tick::kPartyRefusals[static_cast<usize>(result.partyRefusal)]);
        REQUIRE(result.status == Tick::Status::Advanced);
        CHECK_FALSE(result.refusedAction);
        CHECK(result.refusedTarget == Tick::TargetFamily::None);
        CHECK(fixture->state() == *expected);
        CHECK(result.damage == outputs);
        CHECK(result.footsteps == footsteps);
        CHECK(result.throws.empty());
        CHECK(result.strong.empty());
    }
    REQUIRE(queuedShove);
    REQUIRE(pushed);
    REQUIRE(recovered);

    // A later native damage refusal must still undo the accepted Pushed motion
    // and every other party/pool owner. Obtain a real queued power blow first.
    REQUIRE(fixture->boundary->restore(*pushed));
    fixture->awaitNativePowerBlow();
    for (auto& player : fixture->players) {
        player.actor.save().progress().health = 1;
    }
    auto before = std::make_unique<Boundary::State>(fixture->state());
    auto frame = Fixture::frame(90, ticks);
    frame.attention = {0, 0, -40};
    frame.camera.position += frame.attention;
    fixture->native({}, frame);
    REQUIRE(std::ranges::any_of(
        fixture->players, [](const auto& player) { return player.life == PlayerLife::Dying; }));
    REQUIRE(fixture->boundary->restore(*before));
    const auto result = fixture->runner->step(fixture->players, {}, frame);
    REQUIRE(result.status == Tick::Status::Unsupported);
    CHECK(result.refusal == Tick::Refusal::Damage);
    CHECK(result.subjects.empty());
    CHECK(result.damage.empty());
    CHECK(result.footsteps.empty());
    CHECK(result.throws.empty());
    CHECK(fixture->state() == *before);
    CHECK(fixture->device.texturesCreated == uploads);
}

TEST_CASE("ordinary combat reports the actual refused native power action without admitting it",
          "[ordinary-combat-tick][ordinary-party-refusal][assets]") {
    const s32 ticks = GENERATE(1, 2);
    auto fixture = std::make_unique<Fixture>(false, false, true);
    bool refusedPower = false;
    for (s32 step = 0; step < 120; ++step) {
        CAPTURE(ticks, step);
        std::array<PlayInput, 4> input{};
        input[3].attack = input[3].attackPressed = step == 0;
        input[3].strongAttack = step > 0;
        const auto frame = Fixture::frame(step, ticks);
        auto before = std::make_unique<Boundary::State>(fixture->state());
        fixture->native(input, frame);
        const auto nativeAction = fixture->players[0].figure->animator().action();
        using Action = PlayerAnimator::Action;
        const bool power = nativeAction == Action::PowerClose || nativeAction == Action::PowerLow ||
                           nativeAction == Action::PowerMed || nativeAction == Action::Spin;
        auto expected = power ? nullptr : std::make_unique<Boundary::State>(fixture->state());
        REQUIRE(fixture->boundary->restore(*before));
        const auto result = fixture->runner->step(fixture->players, input, frame);
        if (power) {
            REQUIRE(result.status == Tick::Status::Unsupported);
            CHECK(result.refusal == Tick::Refusal::PartyMotion);
            CHECK(result.partyRefusal == Tick::PartyRefusal::TurboAction);
            REQUIRE(result.refusedAction);
            CHECK(*result.refusedAction == nativeAction);
            CHECK(static_cast<usize>(*result.refusedAction) < Tick::kActionCount);
            CHECK(result.refusedTarget == Tick::TargetFamily::None);
            CHECK(result.damage.empty());
            CHECK(result.throws.empty());
            CHECK(result.strong.empty());
            CHECK(fixture->state() == *before);
            refusedPower = true;
            break;
        }
        INFO(Tick::kRefusals[static_cast<usize>(result.refusal)]);
        REQUIRE(result.status == Tick::Status::Advanced);
        CHECK_FALSE(result.refusedAction);
        CHECK(result.refusedTarget == Tick::TargetFamily::None);
        REQUIRE(expected);
        CHECK(fixture->state() == *expected);
    }
    REQUIRE(refusedPower);
}

TEST_CASE("stage combat advances the unfiltered native G1 entrance",
          "[ordinary-combat-tick][ordinary-stage-tick][assets]") {
    const s32 ticks = GENERATE(1, 2);
    auto fixture = std::make_unique<Fixture>(false, false, false, 0, false, true);
    REQUIRE(fixture->boundary->stageBound());
    fixture->players[0].nameTicks = 240;
    fixture->players[1].nameTicks = 3;
    const auto original = fixture->state();
    REQUIRE(original.stage);
    REQUIRE(original.stage->portals);
    REQUIRE_FALSE(original.stage->portals->portals.empty());
    REQUIRE_FALSE(original.stage->fixtures.chests.chests.empty());
    REQUIRE_FALSE(original.opponents.placements->roster.empty());
    REQUIRE(fixture->world.placedItems().size() > 0);
    const auto uploads = fixture->device.texturesCreated;
    const auto start = fixture->players[0].actor.position();
    for (s32 step = 0; step < 6; ++step) {
        CAPTURE(ticks, step);
        auto frame = Fixture::frame(step, ticks);
        frame.attention =
            (fixture->players[0].actor.position() + fixture->players[1].actor.position()) * .5f;
        frame.camera.position += frame.attention;
        frame.damage.realm = fixture->world.ref().realmId;
        std::array<PlayInput, 4> input{};
        input[3].move = {Vec2{0, .15f}, .15f};
        input[1].move = {Vec2{0, .15f}, .15f};
        const auto before = fixture->state();
        fixture->native(input, frame);
        const auto expected = fixture->state();
        REQUIRE(fixture->boundary->restore(before));
        const auto result = fixture->runner->step(fixture->players, input, frame);
        INFO(Tick::kRefusals[static_cast<usize>(result.refusal)]);
        REQUIRE(result.status == Tick::Status::Advanced);
        CHECK(fixture->state() == expected);
        CHECK(result.damage == fixture->outputs);
        CHECK(result.footsteps == fixture->footsteps);
        CHECK(fixture->device.texturesCreated == uploads);
        CHECK(fixture->players[0].nameTicks == 240 - (step + 1) * ticks);
        CHECK(fixture->players[1].nameTicks == std::max(3 - (step + 1) * ticks, 0));
    }
    CHECK(fixture->players[0].actor.position() != start);
    REQUIRE(fixture->boundary->restore(original));
    CHECK(fixture->state() == original);
}

TEST_CASE("stage combat refuses a real G1 pickup without spending or publishing a partial tick",
          "[ordinary-combat-tick][ordinary-stage-tick][assets]") {
    auto fixture = std::make_unique<Fixture>(false, false, false, 0, false, true);
    auto& player = fixture->players[0];
    bool touched = false;
    for (usize index = 0; index < fixture->world.placedItems().size(); ++index) {
        const auto& item = fixture->world.placedItems().item(index);
        if (!item.takeable() || item.subtype == ItemInfo::kGold ||
            item.subtype == static_cast<s32>(ItemKind::Keys)) {
            continue;
        }
        const auto floor = fixture->world.collision().floorAt(item.position, 4, 10);
        if (!floor || fixture->world.collision().moving(floor->object) ||
            (floor->objectFlags & (FloorRiding::kMoving | FloorRiding::kKeptApart)) != 0) {
            continue;
        }
        player.actor.place({item.position.x, floor->y, item.position.z});
        player.actor.settle(fixture->world.collision());
        FloorRiding::land(fixture->players, 0, player.actor.position(), fixture->world.collision());
        touched = PartyPickups::touchesPickup(fixture->players, fixture->world,
                                              fixture->fixtures.chests());
        if (touched) {
            break;
        }
    }
    REQUIRE(touched);
    const auto before = fixture->state();
    const auto textures = fixture->device.texturesCreated;
    const auto uploads = fixture->device.textureUpdates;
    auto frame = Fixture::frame(0, 1);
    frame.attention = player.actor.position();
    frame.camera.position += frame.attention;
    const auto result = fixture->runner->step(fixture->players, {}, frame);
    CHECK(result.status == Tick::Status::Unsupported);
    CHECK(result.timings.prepareNanoseconds.has_value());
    CHECK(result.subjects.empty());
    CHECK(result.damage.empty());
    CHECK(result.throws.empty());
    CHECK(result.feedback.empty());
    CHECK(fixture->state() == before);
    CHECK(fixture->device.texturesCreated == textures);
    CHECK(fixture->device.textureUpdates == uploads);
}

TEST_CASE("complete stage combat carries native lift and turntable riders across rewind",
          "[ordinary-combat-tick][ordinary-stage-tick][ordinary-moving-floor][assets]") {
    const s32 ticks = GENERATE(1, 2);
    const auto moving = GENERATE(MovingStage::Lift, MovingStage::Spinner);
    auto fixture = std::make_unique<Fixture>(false, false, false, 0, false, false, moving);
    REQUIRE(fixture->boundary->stageBound());
    REQUIRE(fixture->world.collision().moving(0));
    REQUIRE(fixture->world.worldAnimator().size() ==
            static_cast<usize>(moving == MovingStage::Lift));
    REQUIRE(fixture->world.rotators().size() == static_cast<usize>(moving == MovingStage::Spinner));
    for (const auto& player : fixture->players) {
        REQUIRE(player.floor.object == 0);
        REQUIRE(player.floor.placement);
    }
    const auto initial = fixture->state();
    const auto textures = fixture->device.texturesCreated;
    const auto uploads = fixture->device.textureUpdates;
    const std::array<PlayInput, 4> inputs{};
    bool moved = false, historicalSupport = false;
    for (s32 tick = 0; tick < 32; ++tick) {
        CAPTURE(ticks, moving, tick);
        const auto frame = Fixture::frame(tick, ticks);
        const auto before = fixture->state();
        fixture->native(inputs, frame);
        const auto expected = fixture->state();
        REQUIRE(fixture->boundary->restore(before));
        const auto result = fixture->runner->step(fixture->players, inputs, frame);
        INFO(Tick::kRefusals[static_cast<usize>(result.refusal)]);
        REQUIRE(result.status == Tick::Status::Advanced);
        CHECK(fixture->state() == expected);
        CHECK(result.damage == fixture->outputs);
        CHECK(result.footsteps == fixture->footsteps);
        for (usize index = 0; index < fixture->players.size(); ++index) {
            const auto& player = fixture->players[index];
            REQUIRE(player.floor.placement);
            CHECK(player.floor.object == 0);
            moved |= player.actor.position() != initial.combat.players[index].actor.position;
            historicalSupport |=
                player.floor.placement != fixture->world.collision().objectTransform(0);
        }
    }
    CHECK(moved);
    // Lift WorldAnimator motion is before carry; the spinner is after carry.
    // Exact rewind must retain, not overwrite, this native matrix-age difference.
    CHECK(historicalSupport == (moving == MovingStage::Spinner));
    const auto final = fixture->state();
    REQUIRE(fixture->boundary->restore(initial));
    for (s32 tick = 0; tick < 32; ++tick) {
        CAPTURE(ticks, moving, tick);
        REQUIRE(
            fixture->runner->step(fixture->players, inputs, Fixture::frame(tick, ticks)).status ==
            Tick::Status::Advanced);
    }
    CHECK(fixture->state() == final);
    CHECK(fixture->device.texturesCreated == textures);
    CHECK(fixture->device.textureUpdates == uploads);
}

TEST_CASE("stage combat refuses a carried transporter at its current native contact",
          "[ordinary-combat-tick][ordinary-stage-tick][transporter-platform][assets]") {
    const s32 ticks = GENERATE(1, 2);
    auto fixture =
        std::make_unique<Fixture>(false, false, false, 0, false, false, MovingStage::Transporter);
    LevelTransporters pads;
    pads.bind(fixture->device, fixture->world.layout(), fixture->world.itemScope(), 2,
              &fixture->world.collision());
    REQUIRE(pads.size() == 2);
    REQUIRE(pads.pad(0).support == 0);
    REQUIRE(fixture->transporters.size() == pads.size());
    const auto authored = pads.pad(0).position;
    const auto uploads = fixture->device.texturesCreated;
    bool advanced = false, refused = false;
    for (s32 step = 0; step < 180; ++step) {
        CAPTURE(ticks, step);
        const auto frame = Fixture::frame(step, ticks);
        const auto before = std::make_unique<Boundary::State>(fixture->state());
        // Observe the actual native pad after the world phase, then rewind that
        // whole phase before asking the transactional runner to execute it.
        fixture->world.update(frame.seconds, false);
        pads.syncFloors(fixture->world.collision());
        const bool touched = std::ranges::any_of(fixture->players, [&](const auto& player) {
            return pads
                .contact(player.actor.position(), player.actor.radius(), player.actor.height())
                .has_value();
        });
        REQUIRE(fixture->boundary->restore(*before));
        std::unique_ptr<Boundary::State> expected;
        if (!touched) {
            fixture->native({}, frame);
            expected = std::make_unique<Boundary::State>(fixture->state());
            REQUIRE(expected->stage);
            REQUIRE(expected->stage->transporters);
            REQUIRE(fixture->boundary->restore(*before));
        }
        const auto result = fixture->runner->step(fixture->players, {}, frame);
        INFO(Tick::kRefusals[static_cast<usize>(result.refusal)]);
        if (touched) {
            REQUIRE(pads.pad(0).position != authored);
            CHECK(result.status == Tick::Status::Unsupported);
            CHECK(result.refusal == Tick::Refusal::WorldMotion);
            CHECK(result.subjects.empty());
            CHECK(result.damage.empty());
            CHECK(result.throws.empty());
            CHECK(fixture->state() == *before);
            refused = true;
            break;
        }
        REQUIRE(result.status == Tick::Status::Advanced);
        REQUIRE(expected);
        CHECK(fixture->state() == *expected);
        advanced = true;
    }
    CHECK(advanced);
    CHECK(refused);
    CHECK(fixture->device.texturesCreated == uploads);
}

TEST_CASE("ordinary combat melee matches native hits rewards and prepared feedback",
          "[ordinary-combat-tick][ordinary-melee][assets]") {
    const s32 ticks = GENERATE(1, 2);
    const s32 character = GENERATE(0, 2, 6);
    Fixture fixture(false, false, true, character);
    const std::array initialExperience{fixture.players[0].actor.save().experience(),
                                       fixture.players[1].actor.save().experience()};
    const auto uploads = fixture.device.texturesCreated;
    std::array<bool, 4> held{};
    u32 attackers = 0;
    bool feedback = false;
    bool recovered = false;
    bool woundedEnemy = false;
    for (s32 tick = 0; tick < 200; ++tick) {
        CAPTURE(character, ticks, tick);
        std::array<PlayInput, 4> input{};
        const PlayerAttacks::Targets targets{.opponents = fixture.opponents,
                                             .fixtures = fixture.fixtures,
                                             .fixtureEvents = {},
                                             .players = fixture.players};
        for (auto& player : fixture.players) {
            const auto slot = static_cast<usize>(player.actor.player());
            const auto deed = fixture.attacks.attackDeed(player.actor, false, targets);
            // Stop asking for new attacks when the native acquisition is no
            // longer close. The in-flight swing still finishes and recovers.
            input[slot].attack =
                tick < 120 && (deed == PlayerDeed::Melee || deed == PlayerDeed::MeleeLow);
            input[slot].attackPressed = input[slot].attack && !held[slot];
            held[slot] = input[slot].attack;
        }
        const auto frame = Fixture::frame(tick, ticks);
        const auto before = fixture.state();
        fixture.native(input, frame);
        const auto expected = fixture.state();
        const auto meleeHits = fixture.meleeHits;
        const auto outputs = fixture.outputs;
        const auto footsteps = fixture.footsteps;
        REQUIRE(fixture.boundary->restore(before));
        const auto result = fixture.runner->step(fixture.players, input, frame);
        INFO(Tick::kRefusals[static_cast<usize>(result.refusal)]);
        REQUIRE(result.status == Tick::Status::Advanced);
        CHECK(fixture.state() == expected);
        CHECK(result.meleeHits == meleeHits);
        CHECK(result.damage == outputs);
        CHECK(result.footsteps == footsteps);
        for (const auto slot : result.meleeHits) {
            attackers |= 1U << static_cast<u32>(slot);
        }
        feedback |= !expected.combat.effects.effects.empty();
        for (usize index = 0; index < before.opponents.enemies.enemies.size(); ++index) {
            woundedEnemy |= expected.opponents.enemies.enemies[index].health <
                            before.opponents.enemies.enemies[index].health;
        }
        if (tick > 150) {
            recovered |= std::ranges::all_of(fixture.players, [](const auto& player) {
                return PlayerAnimator::locomotion(player.figure->animator().action());
            });
        }
    }
    CHECK(attackers == ((1U << 3) | (1U << 1)));
    CHECK(woundedEnemy);
    CHECK(feedback);
    CHECK(recovered);
    for (usize index = 0; index < fixture.players.size(); ++index) {
        const auto& player = fixture.players[index];
        CHECK(player.actor.save().experience() > initialExperience[index]);
        CHECK(player.levelKills > 0);
        CHECK(player.actor.save().progress().lifetime.enemiesKilled > 0);
    }
    CHECK(fixture.device.texturesCreated == uploads);
}

TEST_CASE("ordinary automatic melee honors both native settings and rewinds complete combat",
          "[ordinary-combat-tick][ordinary-melee][ordinary-auto-melee][assets]") {
    const s32 ticks = GENERATE(1, 2);
    const s32 character = GENERATE(0, 2, 6);
    const bool enabled = GENERATE(false, true);
    const bool preferred = GENERATE(false, true);
    auto fixture = std::make_unique<Fixture>(false, false, true, character);
    for (auto& player : fixture->players) {
        player.actor.save().autoAttack = preferred;
        // Begin within the native unpressed swing range, not the longer
        // held-attack reach. Only ordinary forward movement is supplied below.
        player.actor.place(player.actor.position() + Vec3{0, 0, 1});
    }
    const PlayerAttacks::Targets targets{.opponents = fixture->opponents,
                                         .fixtures = fixture->fixtures,
                                         .fixtureEvents = {},
                                         .players = fixture->players};
    for (const auto& player : fixture->players) {
        const auto deed =
            fixture->attacks.automaticMeleeDeed(player.actor, targets, player.actor.facing());
        REQUIRE((deed == PlayerDeed::AutoMelee || deed == PlayerDeed::AutoMeleeLow));
    }
    const auto uploads = fixture->device.texturesCreated;
    const auto updates = fixture->device.textureUpdates;
    u32 automaticActors = 0;
    u32 contacts = 0;
    for (s32 tick = 0; tick < 90; ++tick) {
        CAPTURE(ticks, character, enabled, preferred, tick);
        std::array<PlayInput, 4> input{};
        if (tick < 60) {
            for (const auto& player : fixture->players) {
                input[static_cast<usize>(player.actor.player())].move = {{0, 1}, .25f};
            }
        }
        auto frame = Fixture::frame(tick, ticks);
        frame.autoMelee = enabled;
        const auto before = std::make_unique<Boundary::State>(fixture->state());
        fixture->native(input, frame);
        const auto expected = std::make_unique<Boundary::State>(fixture->state());
        const auto meleeHits = fixture->meleeHits;
        const auto outputs = fixture->outputs;
        const auto footsteps = fixture->footsteps;
        for (const auto& player : fixture->players) {
            if (player.figure->animator().meleeing()) {
                automaticActors |= 1U << static_cast<u32>(player.actor.player());
            }
        }
        REQUIRE(fixture->boundary->restore(*before));
        const auto result = fixture->runner->step(fixture->players, input, frame);
        INFO(Tick::kRefusals[static_cast<usize>(result.refusal)]);
        REQUIRE(result.status == Tick::Status::Advanced);
        CHECK(fixture->state() == *expected);
        CHECK(result.meleeHits == meleeHits);
        CHECK(result.damage == outputs);
        CHECK(result.footsteps == footsteps);
        CHECK(result.throws.empty());
        for (const auto player : result.meleeHits) {
            contacts |= 1U << static_cast<u32>(player);
        }
    }
    const u32 expectedActors = enabled && preferred ? (1U << 3) | (1U << 1) : 0;
    CHECK(automaticActors == expectedActors);
    CHECK(contacts == expectedActors);
    CHECK(fixture->device.texturesCreated == uploads);
    CHECK(fixture->device.textureUpdates == updates);
}

TEST_CASE("ordinary stationary and strafing throws replay native release flight hit and next-phase "
          "rewards",
          "[ordinary-combat-tick][ordinary-ranged][assets]") {
    const s32 character = GENERATE(0, 2, 6);
    const s32 ticks = GENERATE(1, 2);
    const s32 style = GENERATE(0, 1, 2); // stationary, explicit strafe, cursor-relative strafe
    const bool strong = GENERATE(false, true);
    Fixture fixture(false, false, true, character, true);
    const auto uploads = fixture.device.texturesCreated;
    u32 released = 0;
    bool flew = false;
    bool pendingLoss = false;
    bool awardedNextPhase = false;
    bool observedStrafe = false;
    bool observedStrong = false;
    for (s32 tick = 0; tick < 150; ++tick) {
        CAPTURE(character, ticks, style, strong, tick);
        std::array<PlayInput, 4> input{};
        for (const auto& player : fixture.players) {
            const auto seat = static_cast<usize>(player.actor.player());
            const bool attacking = (released & (1U << seat)) == 0;
            input[seat].attack = attacking && !strong;
            input[seat].attackPressed = tick == 0 && !strong;
            input[seat].strongAttack = attacking && strong;
            if (style != 0 && attacking) {
                // Bounded native movement without actor/pose teleports.
                input[seat].move = {{(tick / 6) % 2 == 0 ? .15f : -.15f, 0}, .15f};
                input[seat].strafe = style == 1;
                if (style == 2) {
                    input[seat].aimDirection = Vec3{0, 0, 1};
                }
            }
        }
        const auto frame = Fixture::frame(tick, ticks);
        const auto before = fixture.state();
        fixture.native(input, frame);
        const auto expected = fixture.state();
        const auto releases = fixture.releases;
        for (const auto& player : fixture.players) {
            const auto action = player.figure->animator().action();
            observedStrafe |= action >= PlayerAnimator::Action::StrafeShootForward1 &&
                              action <= PlayerAnimator::Action::StrafeShootRight2;
            observedStrong |= action == PlayerAnimator::Action::StrongThrow;
        }
        REQUIRE(fixture.boundary->restore(before));
        const auto result = fixture.runner->step(fixture.players, input, frame);
        INFO(Tick::kRefusals[static_cast<usize>(result.refusal)]);
        REQUIRE(result.status == Tick::Status::Advanced);
        CHECK(fixture.state() == expected);
        REQUIRE(result.throws.size() == releases.size());
        for (usize index = 0; index < releases.size(); ++index) {
            CHECK(result.throws[index].owner == releases[index].first);
            CHECK(result.throws[index].release == releases[index].second);
            CHECK(result.throws[index].release.strong == strong);
            released |= 1U << static_cast<u32>(result.throws[index].owner);
        }
        flew |= !expected.combat.arsenal.missiles.missiles.empty();
        pendingLoss |= !expected.opponents.enemies.losses.empty();
        if (!expected.opponents.enemies.losses.empty()) {
            // These deaths happened in the post-opponent projectile phase, not
            // early enough for the reward consumer in this same native tick.
            for (usize index = 0; index < fixture.players.size(); ++index) {
                if (before.opponents.enemies.losses.empty()) {
                    CHECK(expected.combat.players[index].save.experience() ==
                          before.combat.players[index].save.experience());
                }
            }
        }
        if (!before.opponents.enemies.losses.empty()) {
            awardedNextPhase |= expected.combat.players[0].save.experience() >
                                    before.combat.players[0].save.experience() ||
                                expected.combat.players[1].save.experience() >
                                    before.combat.players[1].save.experience();
        }
    }
    if (style != 0 && !strong) {
        REQUIRE(observedStrafe);
    }
    CHECK(observedStrong == strong);
    // Sorceress free-A is an authored DAMG effect, not a weapon release. The
    // second local seat is always Sorceress; both native hits and queued rewards
    // above still have to match, including the all-Sorceress strong case.
    constexpr s32 kSorceressClass = 6;
    const u32 expectedReleases =
        strong ? (character == kSorceressClass ? 0U : 1U << 3) : (1U << 3) | (1U << 1);
    CHECK(released == expectedReleases);
    CHECK(flew == (expectedReleases != 0));
    CHECK(pendingLoss);
    CHECK(awardedNextPhase);
    CHECK(fixture.device.texturesCreated == uploads);
}

TEST_CASE("whole ordinary ticks retain blocked muzzle impacts and undo failed prepared sparks",
          "[ordinary-combat-tick][ordinary-blocked-muzzle][assets]") {
    const s32 ticks = GENERATE(1, 2);
    const s32 mode = GENERATE(0, 1, 2); // normal, strong, exhausted shared effect pool
    const bool strong = mode == 1;
    auto fixture =
        std::make_unique<Fixture>(false, false, true, 0, true, false, MovingStage::None, true);
    auto& player = fixture->players[0];
    const auto prepared = fixture->arsenal.prepareWeapon(player.actor, *player.figure,
                                                         Vec3{0, 0, 1}, strong ? 2.0f : 1.0f, true);
    REQUIRE(prepared);
    REQUIRE_FALSE(prepared->missiles.empty());
    const auto muzzle = prepared->missiles.front().position;
    const auto radius = prepared->missiles.front().spec->radius;
    auto& collision = const_cast<WorldCollision&>(fixture->world.collision());
    // The wall crosses the shared muzzle, not the player's feet. Preserve the
    // native floor and run real input/movement/aim before the release callback.
    collision.build({{{0, 1, 0}, {{{-200, 0, -200}, {200, 0, -200}, {200, 0, 200}}}, -1, 14},
                     {{0, 1, 0}, {{{-200, 0, -200}, {200, 0, 200}, {-200, 0, 200}}}, -1, 14},
                     {{0, 0, -1},
                      {muzzle + Vec3{-5, -20, 0}, muzzle + Vec3{-5, 20, 0}, muzzle + Vec3{5, 0, 0}},
                      -1,
                      2}});
    const auto clear =
        collision.resolveWalls(muzzle, radius, muzzle.y - radius * .5f, muzzle.y + radius * .5f);
    REQUIRE(glm::distance(clear, muzzle) > 1e-4f);
    const auto uploads = fixture->device.texturesCreated;
    bool blocked = false;
    s32 tail = 0;
    for (s32 step = 0; step < 120 && tail < 3; ++step) {
        CAPTURE(ticks, mode, step);
        std::array<PlayInput, 4> input{};
        input[3].aimDirection = Vec3{0, 0, 1};
        input[3].attack = !blocked && !strong;
        input[3].attackPressed = step == 0 && !strong;
        input[3].strongAttack = step == 0 && strong;
        const auto frame = Fixture::frame(step, ticks);
        auto before = std::make_unique<Boundary::State>(fixture->state());
        fixture->native(input, frame);
        const bool released = strong ? player.figure->animator().strongReleased()
                                     : player.figure->animator().released();
        const auto expected = std::make_unique<Boundary::State>(fixture->state());
        REQUIRE(fixture->releases.empty());
        REQUIRE(fixture->boundary->restore(*before));
        if (mode == 2 && released) {
            before->combat.effects.nextId = 0;
            REQUIRE(fixture->boundary->restore(*before));
        }
        const auto result = fixture->runner->step(fixture->players, input, frame);
        INFO(Tick::kRefusals[static_cast<usize>(result.refusal)]);
        INFO(Tick::kPartyRefusals[static_cast<usize>(result.partyRefusal)]);
        if (mode == 2 && released) {
            REQUIRE(result.status == Tick::Status::Unsupported);
            CHECK(result.refusal == Tick::Refusal::PartyMotion);
            CHECK(result.partyRefusal == Tick::PartyRefusal::WeaponEmission);
            CHECK(fixture->state() == *before);
            CHECK(result.impacts.empty());
            CHECK(result.throws.empty());
            CHECK(result.strong.empty());
            CHECK(result.damage.empty());
            CHECK(result.footsteps.empty());
            blocked = true;
            break;
        }
        REQUIRE(result.status == Tick::Status::Advanced);
        CHECK(result.partyRefusal == Tick::PartyRefusal::None);
        CHECK(fixture->state() == *expected);
        CHECK(result.throws.empty());
        CHECK(fixture->arsenal.missiles().count() == 0);
        if (released) {
            REQUIRE(result.impacts.size() == 1);
            CHECK(result.impacts.front().name == "S_WEAPONHITWOOD");
            CHECK(result.impacts.front().position == clear);
            bool sparks = false;
            for (usize index = 0; index < fixture->effects.count(); ++index) {
                const auto* tree = fixture->effects.effect(index).tree;
                sparks |= tree != nullptr && tree->name == "SPARKS";
            }
            CHECK(sparks);
            blocked = true;
        } else {
            CHECK(result.impacts.empty());
        }
        if (blocked) {
            ++tail;
        }
    }
    REQUIRE(blocked);
    CHECK(fixture->device.texturesCreated == uploads);
}

TEST_CASE("expired Phoenix pending release remains the native no-shooter no-op",
          "[ordinary-combat-tick][ordinary-familiar-noop][assets]") {
    const s32 ticks = GENERATE(1, 2);
    auto fixture = std::make_unique<Fixture>(false, false, true, 0, true);
    auto before = std::make_unique<Boundary::State>(fixture->state());
    REQUIRE(before->combat.players[0].figure);
    // A queued familiar release survives until the next Figure::animate even
    // when the inventory powerup expires in the preceding powerup phase.
    before->combat.players[0].figure->familiarPending = true;
    REQUIRE(fixture->boundary->restore(*before));
    const auto& player = fixture->players[0];
    REQUIRE(player.figure->familiarTier() == 0);
    REQUIRE((PowerupEffects::of(player.actor.save().progress().inventory).special &
             powerup::kPhoenix) == 0);
    const auto uploads = fixture->device.texturesCreated;
    const std::array<PlayInput, 4> input{};
    const auto frame = Fixture::frame(0, ticks);
    fixture->native(input, frame);
    REQUIRE(player.figure->familiarReleased());
    const auto expected = std::make_unique<Boundary::State>(fixture->state());
    REQUIRE(fixture->boundary->restore(*before));
    const auto result = fixture->runner->step(fixture->players, input, frame);
    REQUIRE(result.status == Tick::Status::Advanced);
    CHECK(result.partyRefusal == Tick::PartyRefusal::None);
    CHECK(fixture->state() == *expected);
    CHECK(result.throws.empty());
    CHECK(result.impacts.empty());
    CHECK(fixture->arsenal.missiles().count() == 0);
    CHECK(fixture->device.texturesCreated == uploads);
}

TEST_CASE("native strong throw timelines replay complete party damage effects and continuation",
          "[ordinary-combat-tick][ordinary-strong-phase][assets]") {
    const s32 character = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    const s32 ticks = GENERATE(1, 2);
    Fixture fixture(false, false, true, character, true);
    const auto* stats = fixture.classes.stats(character);
    REQUIRE(stats);
    TurboMove nativeMove;
    TurboMeter nativeMeter;
    nativeMove.begin(PlayerAnimator::Action::StrongThrow, stats, nativeMeter);
    const auto timeline = nativeMove.checkpoint();
    REQUIRE_FALSE(timeline.all.empty());
    CHECK(timeline.all == stats->strikesOf(stats->moves.turboAThrow));
    const auto uploads = fixture.device.texturesCreated;
    bool strongAction = false;
    bool emitted = false;
    bool released = false;
    bool returned = false;
    for (s32 step = 0; step < 120; ++step) {
        CAPTURE(character, ticks, step);
        std::array<PlayInput, 4> input{};
        input[3].strongAttack = step == 0;
        const auto frame = Fixture::frame(step, ticks);
        const auto before = fixture.state();
        fixture.native(input, frame);
        const auto expected = fixture.state();
        const auto nativeReleases = fixture.releases;
        REQUIRE(fixture.boundary->restore(before));
        const auto result = fixture.runner->step(fixture.players, input, frame);
        INFO(Tick::kRefusals[static_cast<usize>(result.refusal)]);
        REQUIRE(result.status == Tick::Status::Advanced);
        CHECK(fixture.state() == expected);
        CHECK(result.damage == fixture.outputs);
        CHECK(result.footsteps == fixture.footsteps);
        REQUIRE(result.throws.size() == nativeReleases.size());
        for (usize index = 0; index < nativeReleases.size(); ++index) {
            CHECK(result.throws[index].release == nativeReleases[index].second);
        }
        const auto action = fixture.players[0].figure->animator().action();
        strongAction |= action == PlayerAnimator::Action::StrongThrow;
        returned |= strongAction && PlayerAnimator::locomotion(action);
        emitted |= !result.strong.empty();
        released |= !result.throws.empty();
    }
    CHECK(strongAction);
    CHECK(returned);
    CHECK(emitted);
    constexpr s32 kSorceressClass = 6; // Native free-A uses DAMG, not a thrown weapon.
    CHECK(released == (character != kSorceressClass));
    CHECK(fixture.device.texturesCreated == uploads);
}

TEST_CASE("ordinary melee rewinds a hit when its authored feedback cannot be prepared",
          "[ordinary-combat-tick][ordinary-melee][assets]") {
    Fixture fixture(false, false, true);
    std::array<PlayInput, 4> input{};
    input[3].attack = true;
    for (s32 tick = 0; tick < 180; ++tick) {
        input[3].attackPressed = tick == 0;
        const auto frame = Fixture::frame(tick, 1);
        const auto before = fixture.state();
        fixture.native(input, frame);
        if (fixture.meleeHits.empty()) {
            continue;
        }
        const auto expected = fixture.state();
        REQUIRE_FALSE(expected.combat.effects.effects.empty());
        REQUIRE(expected.combat.players[0].save.experience() >
                before.combat.players[0].save.experience());
        REQUIRE(fixture.boundary->restore(before));
        REQUIRE(before.combat.effects.effects.empty());
        const auto nativeResources = fixture.resources.resources();
        // The same owners and native archives are present. Only the trusted
        // prepared effect catalog is deliberately absent, so failure occurs
        // after the native swing has hit, never by omitting the hit/effect.
        CombatRollbackCheckpoint unprepared(
            fixture.ordered, fixture.health, fixture.levels, fixture.attacks, fixture.arsenal,
            fixture.effects, {nativeResources.missiles, nativeResources.missileEffects, {}});
        Boundary transaction(unprepared, fixture.opponents, fixture.missileResources);
        Tick runner(transaction, fixture.classes, fixture.fixtures);
        const auto uploads = fixture.device.texturesCreated;
        const auto result = runner.step(fixture.players, input, frame);
        CHECK(result.status == Tick::Status::Unsupported);
        CHECK(result.timings.prepareNanoseconds.has_value());
        CHECK(result.meleeHits.empty());
        CHECK(result.feedback.empty());
        CHECK(result.damage.empty());
        CHECK(fixture.state() == before);
        CHECK(fixture.device.texturesCreated == uploads);
        return;
    }
    FAIL("native ordinary melee never hit its adjacent grunt");
}

TEST_CASE("ordinary generator contacts retain native sparse-party reward phase and effects",
          "[ordinary-combat-tick][ordinary-generator][assets]") {
    const s32 ticks = GENERATE(1, 2);
    // The two flight cases use native ordinary/strong weapon preparation. The
    // free-A area strikes are a separate target domain, not a guessed substitute.
    const s32 mode = GENERATE(0, 1, 2);
    auto fixture = std::make_unique<Fixture>(false, false, true, 0, false, false, MovingStage::None,
                                             false, true);
    const std::array initialExperience{fixture->players[0].actor.save().experience(),
                                       fixture->players[1].actor.save().experience()};
    if (mode != 0) {
        for (usize index = 0; index < fixture->players.size(); ++index) {
            auto& player = fixture->players[index];
            player.actor.place(player.actor.position() - Vec3{0, 0, 12});
            const auto target = fixture->opponents.generators().target(static_cast<s32>(index), 0);
            REQUIRE(target.acquisition);
            fixture->arsenal.launchWeapon(player.actor, player.figure.get(), player.actor.facing(),
                                          mode == 2 ? 2.0f : 1.0f, false,
                                          target.acquisition->point);
        }
        REQUIRE(fixture->arsenal.missiles().count() == fixture->players.size());
    }
    const auto uploads = fixture->device.texturesCreated;
    const auto updates = fixture->device.textureUpdates;
    u32 damaged = 0;
    u32 cues = 0;
    bool deferredRewards = false;
    for (s32 tick = 0; tick < 90; ++tick) {
        CAPTURE(ticks, mode, tick);
        std::array<PlayInput, 4> input{};
        if (mode == 0) {
            for (usize index = 0; index < fixture->players.size(); ++index) {
                if (fixture->opponents.generators().standing(static_cast<s32>(index))) {
                    auto& command =
                        input[static_cast<usize>(fixture->players[index].actor.player())];
                    command.attack = true;
                    command.attackPressed = tick == 0;
                }
            }
        }
        const auto frame = Fixture::frame(tick, ticks);
        const auto before = std::make_unique<Boundary::State>(fixture->state());
        fixture->native(input, frame);
        const auto expected = std::make_unique<Boundary::State>(fixture->state());
        const auto outputs = fixture->outputs;
        const auto footsteps = fixture->footsteps;
        REQUIRE(fixture->meleeHits.empty()); // A generator never earns a melee-enemy credit.
        for (usize index = 0; index < fixture->players.size(); ++index) {
            const auto& old = before->opponents.generators.generators[index];
            const auto& now = expected->opponents.generators.generators[index];
            if (now.health < old.health) {
                damaged |= 1U << static_cast<u32>(index);
                REQUIRE(now.state == 0);
                REQUIRE_FALSE(now.box.solid);
                REQUIRE_FALSE(expected->combat.effects.effects.empty());
                if (mode == 0) {
                    CHECK(expected->adapter.generatorRewards.empty());
                    CHECK(expected->combat.players[index].save.experience() >
                          initialExperience[index]);
                } else {
                    CHECK_FALSE(expected->adapter.generatorRewards.empty());
                    CHECK(expected->combat.players[index].save.experience() ==
                          initialExperience[index]);
                    deferredRewards = true;
                }
            }
        }
        REQUIRE(fixture->boundary->restore(*before));
        const auto result = fixture->runner->step(fixture->players, input, frame);
        INFO(Tick::kRefusals[static_cast<usize>(result.refusal)]);
        INFO(Tick::kPartyRefusals[static_cast<usize>(result.partyRefusal)]);
        REQUIRE(result.status == Tick::Status::Advanced);
        CHECK(fixture->state() == *expected);
        CHECK(result.damage == outputs);
        CHECK(result.footsteps == footsteps);
        CHECK(result.meleeHits.empty());
        for (const auto& cue : result.impacts) {
            if (cue.name == "S_GENKILLT") {
                CHECK_FALSE(cue.playerDistance);
                CHECK(cue.volume == 1);
                ++cues;
            }
        }
        if (damaged == 3 && expected->adapter.generatorRewards.empty()) {
            break;
        }
    }
    CHECK(damaged == 3);
    CHECK(cues == 2);
    CHECK(deferredRewards == (mode != 0));
    for (usize index = 0; index < fixture->players.size(); ++index) {
        const auto& player = fixture->players[index];
        CHECK(player.actor.save().experience() > initialExperience[index]);
        CHECK(player.actor.save().progress().lifetime.generatorsDestroyed == 1);
        CHECK(player.actor.save().progress().lifetime.enemiesKilled == 0);
        CHECK(player.levelKills == 0);
    }
    CHECK(fixture->device.texturesCreated == uploads);
    CHECK(fixture->device.textureUpdates == updates);
}

TEST_CASE("ordinary generator emission failure rewinds damage rewards and all native owners",
          "[ordinary-combat-tick][ordinary-generator][assets]") {
    const bool exhausted = GENERATE(false, true);
    auto fixture = std::make_unique<Fixture>(false, false, true, 0, false, false, MovingStage::None,
                                             false, true);
    std::array<PlayInput, 4> input{};
    input[3].attack = true;
    for (s32 tick = 0; tick < 180; ++tick) {
        input[3].attackPressed = tick == 0;
        const auto frame = Fixture::frame(tick, 1);
        auto before = std::make_unique<Boundary::State>(fixture->state());
        fixture->native(input, frame);
        if (fixture->opponents.generators().standing(0)) {
            continue;
        }
        REQUIRE(fixture->effects.count() > 0);
        REQUIRE(before->combat.effects.effects.empty());
        if (exhausted) {
            before->combat.effects.nextId = 0;
        }
        REQUIRE(fixture->boundary->restore(*before));
        auto nativeResources = fixture->resources.resources();
        if (!exhausted) {
            nativeResources.sharedEffects = {};
        }
        CombatRollbackCheckpoint combat(fixture->ordered, fixture->health, fixture->levels,
                                        fixture->attacks, fixture->arsenal, fixture->effects,
                                        nativeResources);
        Boundary boundary(combat, fixture->opponents, fixture->missileResources);
        Tick runner(boundary, fixture->classes, fixture->fixtures);
        const auto uploads = fixture->device.texturesCreated;
        const auto result = runner.step(fixture->players, input, frame);
        CHECK(result.status == Tick::Status::Unsupported);
        CHECK(result.refusal == Tick::Refusal::PartyMotion);
        CHECK(result.partyRefusal == Tick::PartyRefusal::GeneratorEmission);
        CHECK(result.damage.empty());
        CHECK(result.impacts.empty());
        CHECK(result.feedback.empty());
        CHECK(fixture->state() == *before);
        CHECK(fixture->device.texturesCreated == uploads);
        return;
    }
    FAIL("native ordinary swing never destroyed its adjacent generator");
}

TEST_CASE("native G1 generator transitions restore prepared debris quotas and detached brood",
          "[ordinary-generator][generator-native-phase][assets]") {
    const s32 ticks = GENERATE(1, 2);
    auto fixture = std::make_unique<Fixture>(false, false, true, 0, false, true);
    auto& opponents = fixture->opponents;
    auto& generators = opponents.generators();
    auto& enemies = opponents.enemies();
    REQUIRE(generators.count() > 0);
    // Exercise the unfiltered authored roster and native generate-single RNG,
    // not a manufactured offspring or an edited quota/countdown checkpoint.
    generators.setView(std::nullopt);
    for (s32 step = 0; step < 8; ++step) {
        generators.update(ticks, enemies, LevelOpponents::enemyViews(fixture->players),
                          fixture->fixtures.obstacles());
    }
    s32 generator = -1;
    for (const auto& body : enemies.targets()) {
        const s32 parent = enemies.generatorOf(body.id);
        if (parent >= 0 && generators.stateOf(parent) > 1) {
            auto* archive = enemies.archive(generators.kindOf(parent));
            if (archive != nullptr && archive->trees.find("GENHIT") &&
                archive->trees.find("GENDIE")) {
                generator = parent;
                break;
            }
        }
    }
    REQUIRE(generator >= 0);
    REQUIRE(generators.bredOf(generator) > 0);
    const auto initial = std::make_unique<Boundary::State>(fixture->state());
    const auto& source = initial->opponents.generators.generators[static_cast<usize>(generator)];
    REQUIRE(source.threshold > 1);
    const auto* level = fixture->world.level();
    REQUIRE(level);
    const f32 scale = LevelOpponents::generatorPowerScale(
        experienceLevel(fixture->players[0].actor.save().experience()), level->tuning.playerLevel);
    REQUIRE(scale > 0);
    const auto uploads = fixture->device.texturesCreated;
    const auto textureUpdates = fixture->device.textureUpdates;
    for (const s32 transition : {0, 1, 2}) {
        CAPTURE(ticks, generator, transition);
        REQUIRE(fixture->boundary->restore(*initial));
        const f32 damage = transition == 0   ? 1.0f
                           : transition == 1 ? source.threshold + 1.0f
                                             : source.health + 1.0f;
        const f32 power = (damage + source.armor) / scale;
        opponents.strikeGenerator(generator, power, 3, fixture->players);
        fixture->effects.update(static_cast<f32>(ticks) / 60);
        const auto expected = std::make_unique<Boundary::State>(fixture->state());
        const auto& damaged =
            expected->opponents.generators.generators[static_cast<usize>(generator)];
        REQUIRE(damaged.health < source.health);
        if (transition == 0) {
            CHECK(damaged.state == source.state);
            CHECK(expected->combat.effects.effects.empty());
        } else {
            CHECK(damaged.state < source.state);
            REQUIRE_FALSE(expected->combat.effects.effects.empty());
            if (transition == 1) {
                REQUIRE(damaged.state > 0);
                CHECK(damaged.most == source.most * 2);
            } else {
                CHECK(damaged.state == 0);
                CHECK_FALSE(damaged.box.solid);
                CHECK(std::ranges::none_of(
                    expected->opponents.enemies.enemies,
                    [&](const auto& enemy) { return enemy.generator == generator; }));
            }
        }
        REQUIRE(fixture->boundary->restore(*initial));
        std::vector<std::string> sounds;
        REQUIRE(opponents.strikeGenerator(
            generator, power, 3, fixture->players,
            [&](const GeneratorFeedbackEmission& emission) -> std::optional<u32> {
                u32 id = 0;
                if (emission.archive != nullptr) {
                    const auto prepared =
                        emission.prepare(fixture->resources.resources().sharedEffects,
                                         fixture->effects.textureLenders(), fixture->device);
                    REQUIRE(prepared);
                    id = fixture->effects.startPrepared(*prepared, emission.position, {});
                    REQUIRE(id != 0);
                    fixture->effects.placeAt(id, emission.placement);
                }
                sounds.push_back(emission.sound);
                return id;
            }));
        fixture->effects.update(static_cast<f32>(ticks) / 60);
        CHECK(sounds == std::vector<std::string>{transition == 2 ? "S_GENKILLG" : "S_GENDAMG"});
        CHECK(fixture->state() == *expected);
        const auto pending = std::make_unique<Boundary::State>(fixture->state());
        REQUIRE(pending->adapter.generatorRewards.size() == 1);
        CHECK(pending->adapter.generatorRewards.front().player == 3);
        CHECK(fixture->device.texturesCreated == uploads);
        CHECK(fixture->device.textureUpdates == textureUpdates);
    }
}

TEST_CASE("ordinary melee preserves queued native feedback and refuses promotions",
          "[ordinary-combat-tick][ordinary-melee][assets]") {
    Fixture fixture(false, false, true);
    const bool promotion = GENERATE(false, true);
    if (promotion) {
        fixture.players[0].actor.save().progress().experience = levelExperience(21) - 1;
    }
    const auto bodies = fixture.opponents.enemies().targets();
    REQUIRE_FALSE(bodies.empty());
    REQUIRE(fixture.opponents.strikeEnemy(bodies.front().id, 10000, 0, Vec3{0, 0, 1}, 3,
                                          fixture.players, true) > 0);
    const auto pending = fixture.state();
    REQUIRE_FALSE(pending.opponents.enemies.losses.empty());
    REQUIRE_FALSE(pending.opponents.enemies.feedback.empty());
    const auto uploads = fixture.device.texturesCreated;
    const std::array<PlayInput, 4> input{};
    const auto frame = Fixture::frame(0, 1);
    if (promotion) {
        const auto result = fixture.runner->step(fixture.players, input, frame);
        CHECK(result.status == Tick::Status::Unsupported);
        CHECK(result.feedback.empty());
        CHECK(result.damage.empty());
        CHECK(fixture.state() == pending);
    } else {
        fixture.native(input, frame);
        const auto expected = fixture.state();
        REQUIRE(expected.opponents.enemies.losses.empty());
        REQUIRE(expected.opponents.enemies.feedback.empty());
        REQUIRE_FALSE(expected.combat.effects.effects.empty());
        REQUIRE(fixture.boundary->restore(pending));
        const auto result = fixture.runner->step(fixture.players, input, frame);
        INFO(Tick::kRefusals[static_cast<usize>(result.refusal)]);
        REQUIRE(result.status == Tick::Status::Advanced);
        CHECK(fixture.state() == expected);
    }
    CHECK(fixture.device.texturesCreated == uploads);
}

TEST_CASE("ordinary combat drains only already satisfied native ordered lesson queues",
          "[ordinary-combat-tick][help-admission][assets]") {
    const s32 ticks = GENERATE(1, 2);
    const bool known = GENERATE(false, true);
    auto fixture = std::make_unique<Fixture>();
    fixture->players[0].actor.save().helpSeen = {HelpMessages::kDestroyGenerators};
    if (known) {
        fixture->players[1].actor.save().helpSeen = {HelpMessages::kDestroyGenerators};
    }
    auto before = std::make_unique<Boundary::State>(fixture->state());
    // An exact host correction can carry several pending requests, including
    // repeated hits. Delivery retains input-slot routing and native queue order.
    before->adapter.lessons = {{HelpMessages::kDestroyGenerators, 3},
                               {HelpMessages::kDestroyGenerators, 1},
                               {HelpMessages::kDestroyGenerators, 3}};
    REQUIRE(fixture->boundary->restore(*before));
    MessageTable strings;
    REQUIRE(strings.load(fixture->root / "TEXT/english.rom"));
    HelpMessages help;
    help.setTexts(&strings);
    std::vector<usize> delivery;
    fixture->help = [&](s32 id, usize index) {
        delivery.push_back(index);
        std::array<HelpReader, 2> readers;
        for (usize player = 0; player < readers.size(); ++player) {
            auto& runtime = fixture->players[player];
            REQUIRE(runtime.life == PlayerLife::Standing);
            readers[player] = {runtime.actor.player(), &runtime.actor.save().helpSeen,
                               &runtime.helpHeard};
        }
        return help.post(id, fixture->players[index].actor.player(), readers) != nullptr;
    };
    const std::array<PlayInput, 4> input{};
    const auto frame = Fixture::frame(0, ticks);
    fixture->native(input, frame);
    auto expected = std::make_unique<Boundary::State>(fixture->state());
    CHECK((delivery == std::vector<usize>{0, 1, 0}));
    REQUIRE(expected->adapter.lessons.empty());
    CHECK(help.showing() == !known);
    REQUIRE(fixture->boundary->restore(*before));
    const auto result = fixture->runner->step(fixture->players, input, frame);
    INFO(Tick::kRefusals[static_cast<usize>(result.refusal)]);
    if (known) {
        REQUIRE(result.status == Tick::Status::Advanced);
        CHECK(fixture->state() == *expected);
    } else {
        REQUIRE(result.status == Tick::Status::Unsupported);
        CHECK(result.damage.empty());
        CHECK(result.feedback.empty());
        CHECK(fixture->state() == *before);
    }
    CHECK(delivery.size() == 3); // private replay never invokes the live HUD callback
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
    input[3].usePotion = true;
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
        const auto result = fixture.runner->step(fixture.players, input, frame);
        INFO(Tick::kRefusals[static_cast<usize>(result.refusal)]);
        REQUIRE(result.status == Tick::Status::Advanced);
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

TEST_CASE("ordinary combat runner advances dormant banks and rewinds approach waking",
          "[ordinary-combat-tick][dormant-opponents][assets]") {
    Fixture fixture(false, true);
    std::array<PlayInput, 4> input{};
    const auto baseline = fixture.state();
    REQUIRE_FALSE(baseline.adapter.critters.banks.empty());
    fixture.native(input, Fixture::frame(0, 1));
    const auto expected = fixture.state();
    REQUIRE(expected.adapter.critters.banks != baseline.adapter.critters.banks);
    REQUIRE(fixture.boundary->restore(baseline));
    REQUIRE(fixture.runner->step(fixture.players, input, Fixture::frame(0, 1)).status ==
            Tick::Status::Advanced);
    CHECK(fixture.state() == expected);

    auto& actor = fixture.players[0].actor;
    const auto save = actor.save();
    const auto* stats = fixture.classes.stats(save.character);
    REQUIRE(stats);
    const auto& placement = fixture.opponents.statues().placement(0);
    Vec3 from = fixture.opponents.statues().positionOf(0);
    from.y = 0;
    from.z -= placement.sight + actor.radius() + 0.001f;
    actor.spawn(actor.player(), save, stats, from, 0);
    const auto before = fixture.state();
    input[3].move = {Vec2{0, 1}, 1};
    fixture.native(input, Fixture::frame(1, 1));
    REQUIRE(fixture.opponents.statues().woken(0));
    REQUIRE(fixture.boundary->restore(before));
    const auto uploads = fixture.device.texturesCreated;
    const auto refused = fixture.runner->step(fixture.players, input, Fixture::frame(1, 1));
    CHECK(refused.status == Tick::Status::Unsupported);
    CHECK(refused.damage.empty());
    CHECK(refused.footsteps.empty());
    CHECK(fixture.state() == before);
    CHECK(fixture.device.texturesCreated == uploads);
}

TEST_CASE("ordinary combat runner refuses prospective special placements before native watch",
          "[ordinary-combat-tick][dormant-opponents][assets]") {
    Fixture fixture;
    PlacedEnemies::Placement placement;
    placement.kind = kGeneralEnemyKind;
    placement.viewRadius = 5;
    placement.spawn.position = {0, 0, 0};
    fixture.opponents.placements().bind({placement});
    const auto before = fixture.state();
    const auto frame = Fixture::frame(0, 1);
    const auto activations = before.opponents.placements->activations(
        ViewVolume::of(frame.camera, frame.projection.horizontalFov, frame.projection.aspect),
        frame.attention);
    REQUIRE(activations);
    REQUIRE(activations->size() == 1);
    const auto uploads = fixture.device.texturesCreated;
    const std::array<PlayInput, 4> input{};
    CHECK(fixture.runner->step(fixture.players, input, frame).status == Tick::Status::Unsupported);
    CHECK(fixture.state() == before);
    CHECK(fixture.opponents.critters().count() == 0);
    CHECK(fixture.device.texturesCreated == uploads);
}

TEST_CASE("stage combat preserves capped gold then collects it with native Pick continuation",
          "[ordinary-combat-tick][ordinary-stage-tick][pickup-admission][assets]") {
    const s32 ticks = GENERATE(1, 2);
    auto fixture = std::make_unique<Fixture>(false, false, false, 0, false, true);
    for (auto& player : fixture->players) {
        player.actor.save().gold = 99999;
    }
    const auto& records = fixture->world.layout().itemInfos();
    const auto record = std::ranges::find_if(records, [](const auto& info) {
        // The first gold definition is COIN_GOLD, a challenge asset not in G1's
        // bound archives. Use the ordinary treasure that native G1 actually has.
        return info.type == ItemInfo::kPowerup && info.subtype == ItemInfo::kGold &&
               info.value > 0 && info.name == "TREAS_GOLD";
    });
    REQUIRE(record != records.end());
    // PlacedItems borrows level items then POWERUPS. itemScope() is the
    // different level/realm scope used by fixtures and excludes POWERUPS.
    const AssetScope pickupScope(fixture->world.items(), &fixture->world.powerups());
    const auto tree = pickupScope.findTree(record->name);
    REQUIRE(tree);
    REQUIRE(fixture->world.placeItem(fixture->device, record->name,
                                     fixture->players[0].actor.position()));
    const usize item = fixture->world.placedItems().size() - 1;
    REQUIRE(fixture->world.placedItems().item(item).archive == tree->archive);
    REQUIRE(fixture->world.placedItems().item(item).figure ==
            &tree->archive->trees.tree(tree->index));
    REQUIRE(
        PartyPickups::touchesPickup(fixture->players, fixture->world, fixture->fixtures.chests()));
    REQUIRE(PartyPickups::collectionUnchanged(fixture->players, fixture->world,
                                              fixture->fixtures.chests(), fixture->classes));
    const auto textures = fixture->device.texturesCreated;
    const auto uploads = fixture->device.textureUpdates;
    for (s32 tick = 0; tick < 6; ++tick) {
        const auto before = fixture->state();
        const auto frame = Fixture::frame(tick, ticks);
        fixture->native({}, frame);
        const auto expected = fixture->state();
        REQUIRE(fixture->boundary->restore(before));
        const auto result = fixture->runner->step(fixture->players, {}, frame);
        INFO(Tick::kRefusals[static_cast<usize>(result.refusal)]);
        REQUIRE(result.status == Tick::Status::Advanced);
        CHECK(fixture->state() == expected);
        CHECK(result.damage == fixture->outputs);
        CHECK(result.footsteps == fixture->footsteps);
        CHECK_FALSE(fixture->world.placedItems().item(item).taken);
    }
    fixture->players[0].actor.save().gold = 0;
    // Currency pickup may ask the native treasure lesson; keep this scene's
    // lesson already read so collection itself is the transaction under test.
    for (auto& player : fixture->players) {
        player.actor.save().helpSeen.push_back(17);
    }
    REQUIRE_FALSE(PartyPickups::collectionUnchanged(fixture->players, fixture->world,
                                                    fixture->fixtures.chests(), fixture->classes));
    const auto baseline = fixture->state();
    for (s32 step = 6; step < 30; ++step) {
        const auto before = fixture->state();
        const auto frame = Fixture::frame(step, ticks);
        fixture->native({}, frame, true);
        const auto expected = fixture->state();
        REQUIRE(fixture->boundary->restore(before));
        const auto result = fixture->runner->step(fixture->players, {}, frame);
        INFO(Tick::kRefusals[static_cast<usize>(result.refusal)]);
        REQUIRE(result.status == Tick::Status::Advanced);
        CHECK(fixture->state() == expected);
        CHECK(fixture->world.placedItems().item(item).taken);
        if (step == 6) {
            CHECK_FALSE(result.pickups.empty());
        } else {
            CHECK(result.pickups.empty());
        }
    }
    REQUIRE(fixture->boundary->restore(baseline));
    CHECK(fixture->state() == baseline);
    CHECK(fixture->device.texturesCreated == textures);
    CHECK(fixture->device.textureUpdates == uploads);
}
TEST_CASE("stage combat continues earned and Phoenix familiars through native delayed releases",
          "[ordinary-combat-tick][ordinary-stage-tick][familiar-stage-tick][assets]") {
    const s32 ticks = GENERATE(1, 2);
    const bool phoenix = GENERATE(false, true);
    const s32 level = GENERATE(40, 80);
    auto fixture = std::make_unique<Fixture>(false, false, true, 0, false, false, MovingStage::Lift,
                                             false, false, level, phoenix);
    REQUIRE(fixture->players[0].figure->familiarTier() > 0);
    REQUIRE(fixture->players[0].figure->phoenixActive() == phoenix);
    const auto baseline = std::make_unique<Boundary::State>(fixture->state());
    const auto textures = fixture->device.texturesCreated;
    const auto uploads = fixture->device.textureUpdates;
    bool released = false;
    bool sawPair = false;
    for (s32 tick = 0; tick < 90 / ticks; ++tick) {
        CAPTURE(tick, ticks, phoenix, level);
        std::array<PlayInput, 4> input{};
        input[3].attack = true;
        input[3].attackPressed = tick == 0;
        input[3].aimDirection = Vec3{0, 0, -1};
        const auto frame = Fixture::frame(tick, ticks);
        const auto before = std::make_unique<Boundary::State>(fixture->state());
        fixture->native(input, frame);
        const auto expected = std::make_unique<Boundary::State>(fixture->state());
        REQUIRE(fixture->boundary->restore(*before));
        const auto result = fixture->runner->step(fixture->players, input, frame);
        INFO(Tick::kRefusals[static_cast<usize>(result.refusal)]);
        INFO(Tick::kPartyRefusals[static_cast<usize>(result.partyRefusal)]);
        REQUIRE(result.status == Tick::Status::Advanced);
        CHECK(fixture->state() == *expected);
        released |= fixture->players[0].figure->familiarReleased();
        usize earned = 0;
        usize fire = 0;
        for (usize i = 0; i < fixture->arsenal.missiles().count(); ++i) {
            const auto& missile = fixture->arsenal.missiles().missile(i);
            if (!missile.release && !missile.potionRelease) {
                CHECK_FALSE(missile.breaksPotions);
                if ((missile.flags & 0x11) != 0) {
                    ++fire;
                } else {
                    ++earned;
                }
            }
        }
        sawPair |= earned > 0 && (!phoenix || fire > 0);
    }
    CHECK(released);
    CHECK(sawPair);
    REQUIRE(fixture->boundary->restore(*baseline));
    CHECK(fixture->state() == *baseline);
    CHECK(fixture->device.texturesCreated == textures);
    CHECK(fixture->device.textureUpdates == uploads);
}
} // namespace
