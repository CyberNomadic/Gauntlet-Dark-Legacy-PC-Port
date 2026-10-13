#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/LocalMovementPrediction.h"
#include "game/screens/LocalProjectilePrediction.h"
#include "game/screens/PartyMotion.h"
#include "game/screens/PlayerAttacks.h"
#include "game/screens/SessionInputs.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
using Action = PlayerAnimator::Action;

/** Real party action/turbo execution is the oracle; only the guest rewinds value
 * checkpoints. Native effects remain live so an accidental replay dispatch is visible. */
struct TurboFixture {
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
    LevelOpponents opponents;
    LevelFixtures fixtures;
    PlayerAttacks attacks;
    std::array<PlayerRuntime, 2> native;
    PlayerAttacks::Targets targets{opponents, fixtures, {}};
    PartyMotion::Events events;
    MatchContext context;
    CombatSnapshot authority;
    LocalMovementPrediction prediction;
    usize members = 1;
    usize performed = 0;
    usize announcements = 0;
    usize rams = 0;

    explicit TurboFixture(s32 character = 0, f32 meter = 40, bool second = false) {
        const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
        device.recordDraws = false;
        REQUIRE(classes.load(root / "PDATA"));
        REQUIRE(classes.stats(character));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable owned world fixture
        auto& floor = const_cast<WorldCollision&>(world.collision());
        const Vec3 a{-100, 0, -100};
        const Vec3 b{100, 0, -100};
        const Vec3 c{100, 0, 100};
        const Vec3 d{-100, 0, 100};
        floor.build({{{0, 1, 0}, {a, b, c}, 0}, {{0, 1, 0}, {a, c, d}, 0}});
        arsenal.bind({device, classes, weapons, floor, effects, audio, &sounds, {}, false, false});
        attacks.bind({device, classes, world, weapons, effects, audio, &sounds, arsenal, dimmer});
        members = second ? 2 : 1;
        context.epoch = 6;
        context.scene = 1;
        context.owners = {1, 2, second ? u8{2} : u8{0}, 0};
        context.grants = {1, 3, second ? u32{7} : u32{0}, 0};
        authority.motion.epoch = context.epoch;
        authority.motion.tick = 10;
        authority.motion.cameraContinuity = 1;
        authority.motion.camera = {{0, 12, -18}, 0.5f, 0, 0};
        authority.motion.players[0] = SeatMotion{1, 1, Vec3{-30, 0, 0}, 0};
        authority.players[0].emplace().health = 100;
        for (usize index = 0; index < members; ++index) {
            auto& member = native[index];
            CharacterSave save;
            save.character = character;
            save.progress().health = 100;
            const Vec3 feet{static_cast<f32>(index) * 30, 0, 0};
            member.actor.spawn(static_cast<s32>(index + 1), save, classes.stats(character), feet,
                               0);
            member.figure = PlayerFigure::load(device, root, save, false);
            REQUIRE(member.figure);
            REQUIRE(member.figure->actionTree());
            member.turbo.add(meter);
            authority.motion.players[index + 1] = SeatMotion{context.grants[index + 1], 1, feet, 0};
            auto& player = authority.players[index + 1].emplace();
            player.health = 100;
            player.movement.enabled = player.movement.current = true;
            player.movement.speed = member.actor.speed();
            player.movement.radius = member.actor.radius();
            player.movement.height = member.actor.height();
            player.movement.followHeight = member.actor.followPoint().y - feet.y;
        }
        targets.players = std::span{native.data(), members};
        events.select = [](usize, const SelectorInput&, s32) {};
        events.advanceTurbo = [&](usize index, s32 ticks, f32 seconds) {
            attacks.updateTurbo(index, ticks, seconds, std::span{native.data(), members},
                                [&](s32, usize) { ++announcements; });
        };
        events.perform = [&](usize index, PartyMotion::Action action) {
            ++performed;
            if (action == PartyMotion::Action::Ram) {
                ++rams;
                attacks.ramBarrels(index, std::span{native.data(), members}, targets);
            }
        };
        events.attackDeed = [&](usize index, bool strong, bool moved, const Vec3& facing) {
            const auto& member = native[index];
            return attacks.attackDeed(member.actor, strong, targets, moved,
                                      member.figure->animator().meleeChain(), facing);
        };
        events.meleeSense = [&](usize index, bool held, const Vec3& facing) {
            return attacks.meleeSense(native[index].actor, held, targets, facing);
        };
        events.aim = [&](usize index) {
            return attacks.aim(native[index].actor, native[index].actor.facing(), targets);
        };
    }
    CombatSnapshot capture(u64 tick) const {
        auto host = authority;
        host.motion.tick = tick;
        for (usize index = 0; index < members; ++index) {
            const auto& member = native[index];
            const auto& actor = member.actor;
            auto& player = *host.players[index + 1];
            host.motion.players[index + 1]->position = actor.position();
            host.motion.players[index + 1]->yaw = std::remainder(actor.yaw(), kTwoPi);
            const auto& animator = member.figure->animator();
            const auto& cursor = animator.player();
            player.rollback = animator.checkpoint();
            player.knockback = member.knockback.checkpoint();
            player.turbo = PlayerTurboState{member.turbo.checkpoint(), member.move.checkpoint()};
            player.animation = {static_cast<u32>(animator.action()), cursor.sequence(),
                                cursor.generation(), cursor.presentationFrame(),
                                cursor.transition()};
            const auto selection = attacks.meleeAcquisition(actor, targets);
            REQUIRE(selection);
            auto& geometry = player.turboGeometry.emplace();
            geometry.selection = *selection;
            geometry.captureYaw = host.motion.players[index + 1]->yaw;
            geometry.assistedAim = attacks.aim(actor, actor.facing(), targets);
            auto& moving = geometry.moving.emplace();
            moving.origin = actor.position();
            moving.radius = PlayerMeleeState::kMaxRadius;
            moving.ticks = PlayerMeleeState::kMaxTicks;
            const auto rawTargets = attacks.meleeGeometry(actor, targets, moving.radius, false);
            REQUIRE(rawTargets);
            moving.targets = *rawTargets;
            player.shove = true; // This owned arena has no ram targets or blocking fixtures.
        }
        return host;
    }
    void begin(bool bindStats = true) {
        authority = capture(authority.motion.tick);
        prediction.begin(context, 2, world.collision());
        for (usize index = 0; index < members; ++index) {
            const auto character = native[index].actor.save().character;
            REQUIRE(prediction.setPlayer(
                static_cast<u8>(index + 1), *native[index].figure->actionTree(),
                bindStats ? classes.stats(character) : nullptr, bindStats ? character : -1));
        }
        accept(authority);
    }
    void accept(const CombatSnapshot& host, std::span<const InputCommand> commands = {}) {
        const auto bytes = CombatPacket::encode(host);
        REQUIRE(bytes);
        const auto decoded = CombatPacket::decode(*bytes);
        REQUIRE(decoded);
        authority = *decoded;
        prediction.update(&authority, commands);
    }
    InputCommand command(u64 tick, u8 seat = 1) const {
        InputCommand input;
        input.epoch = context.epoch;
        input.tick = tick;
        input.grant = context.grants[seat];
        input.seat = seat;
        return input;
    }
    void step(std::span<const InputCommand> commands) {
        std::array<PlayInput, InputCommand::kSeats> controls;
        for (const auto& command : commands) {
            REQUIRE(command.valid());
            controls[command.seat] = SessionInputs::playInput(command);
        }
        PartyMotion::step(std::span{native.data(), members}, controls, false,
                          authority.players[1]->movement.cameraYaw, 1,
                          1.0f / static_cast<f32>(context.tickRate), world.collision(), events);
    }
    CombatSnapshot shown(f32 blend = 1) const {
        auto state = authority;
        prediction.present(state, blend);
        REQUIRE(state.valid());
        return state;
    }
    void checkNative(usize index = 0) const {
        const usize seat = index + 1;
        const auto state = shown();
        REQUIRE(state.players[seat]->rollback);
        CHECK(*state.players[seat]->rollback == native[index].figure->animator().checkpoint());
        CHECK(state.players[seat]->knockback == native[index].knockback.checkpoint());
        REQUIRE(state.players[seat]->turbo);
        CHECK(state.players[seat]->turbo->meter == native[index].turbo.checkpoint());
        CHECK(state.players[seat]->turbo->move == native[index].move.checkpoint());
        CHECK(glm::distance(state.motion.players[seat]->position, native[index].actor.position()) <
              1e-5f);
        CHECK(std::remainder(state.motion.players[seat]->yaw - native[index].actor.yaw(), kTwoPi) ==
              Approx(0).margin(1e-5f));
        CHECK(state.players[seat]->health == 100);
        CHECK(state.projectiles.empty());
        const auto* replay = prediction.replayed(static_cast<u8>(seat));
        REQUIRE(replay != nullptr);
        CHECK(replay->releases.empty());
    }
    /** Checks the shared native world and one-shot services around a guest-only operation. */
    template <class Operation> void isolated(Operation operation) {
        std::array<Inventory, 2> inventory;
        std::array<PlayerAnimator::Checkpoint, 2> animation;
        std::array<PlayerTurboState, 2> turbo;
        std::array<std::vector<usize>, 2> rammed;
        for (usize index = 0; index < members; ++index) {
            inventory[index] = native[index].actor.save().progress().inventory;
            animation[index] = native[index].figure->animator().checkpoint();
            turbo[index] = {native[index].turbo.checkpoint(), native[index].move.checkpoint()};
            rammed[index] = native[index].rammed;
        }
        const auto before = std::array{effects.count(),
                                       arsenal.missiles().count(),
                                       attacks.strikes().count(),
                                       sounds.voiceCount(),
                                       performed,
                                       announcements,
                                       rams};
        usize audioEvents = 0;
        const SoundPlayer::Capture audioCapture(&sounds,
                                                [&](const SoundPlayer::Event&) { ++audioEvents; });
        operation();
        CHECK(audioEvents == 0);
        CHECK(before == std::array{effects.count(), arsenal.missiles().count(),
                                   attacks.strikes().count(), sounds.voiceCount(), performed,
                                   announcements, rams});
        for (usize index = 0; index < members; ++index) {
            CHECK(native[index].actor.save().progress().health == 100);
            CHECK(native[index].actor.save().progress().inventory == inventory[index]);
            CHECK(native[index].figure->animator().checkpoint() == animation[index]);
            CHECK(native[index].turbo.checkpoint() == turbo[index].meter);
            CHECK(native[index].move.checkpoint() == turbo[index].move);
            CHECK(native[index].rammed == rammed[index]);
        }
    }
};

/** Optional projectile lane leaves the action-only fixture unchanged. Every
 * expected shot originates in the live native arsenal, before guest replay. */
struct TurboProjectileFixture : TurboFixture {
    ProjectileResources resources;
    LocalProjectilePrediction projectiles;
    std::map<u64, ShotIdentity> origins;
    std::map<ShotIdentity, PlayerMissiles::FlightCheckpoint> births;

    explicit TurboProjectileFixture(s32 character, f32 meter) : TurboFixture(character, meter) {
        const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
        REQUIRE(weapons.load(root / "WEAPONS"));
        std::array<PlayerFigure*, InputCommand::kSeats> figures{};
        figures[1] = native[0].figure.get();
        REQUIRE(resources.addPlayers(device, weapons, figures));
        TurboFixture::begin();
        projectiles.begin(context, 2, world.collision());
        // The base fixture has already consumed tick ten without a weapon
        // descriptor. Restart on the same native baseline with the complete one.
        prediction.begin(context, 2, world.collision());
        REQUIRE(prediction.setPlayer(1, *native[0].figure->actionTree(), classes.stats(character),
                                     character));
        acceptShots(captureShots(10));
    }

    CombatSnapshot captureShots(u64 tick) {
        auto host = capture(tick);
        const auto& actor = native[0].actor;
        const auto& figure = *native[0].figure;
        const auto request = arsenal.weaponThrow(actor, figure.animator().attackSeconds());
        REQUIRE(request);
        auto* archive = figure.missileArchive();
        REQUIRE(archive);
        const auto tree = archive->trees.find(figure.missileTree());
        REQUIRE(tree);
        auto& weapon = host.players[1]->weapon.emplace();
        weapon.character = static_cast<u32>(request->character);
        weapon.hand = request->hand;
        weapon.speed = request->speed;
        weapon.resource = resources.treeId(archive, &archive->trees.tree(*tree));
        const auto streak = arsenal.weaponStreak(actor);
        weapon.streak = resources.streakId(streak.texture);
        weapon.tint = streak.color;
        weapon.forward = streak.forward;
        weapon.textureFrame = arsenal.weaponTextureFrame(figure);
        REQUIRE(resources.acceptsWeapon(weapon));
        REQUIRE(
            ProjectileCapture::append(host, resources, arsenal.missiles(), opponents.missiles()));
        const auto& pool = arsenal.missiles();
        std::map<u32, u64> effectInstances;
        for (usize index = 0; index < pool.visuals().count(); ++index) {
            const auto& effect = pool.visuals().effect(index);
            effectInstances.emplace(effect.id, effect.instance);
        }
        for (auto& shot : host.projectiles) {
            for (usize index = 0; index < pool.count(); ++index) {
                const auto& missile = pool.missile(index);
                const auto effect = effectInstances.find(missile.effect);
                const bool body = shot.source == ProjectileSource::PlayerEffect &&
                                  effect != effectInstances.end() &&
                                  effect->second == shot.instance;
                const bool streakPart =
                    shot.source == ProjectileSource::Streak && shot.instance == missile.instance;
                if (body || streakPart) {
                    shot.rollback = ProjectileState::Rollback{
                        origins.at(missile.instance), static_cast<u32>(request->character),
                        PlayerMissiles::flightCheckpoint(missile)};
                }
            }
            if (shot.particles && shot.source == ProjectileSource::PlayerEffect) {
                for (const auto& history : pool.visualHistory()) {
                    const auto effect = effectInstances.find(history.effect);
                    if (effect != effectInstances.end() && effect->second == shot.instance) {
                        shot.particles->origin = origins.at(history.instance);
                    }
                }
            }
        }
        REQUIRE(host.valid());
        return host;
    }

    void acceptShots(const CombatSnapshot& host, std::span<const InputCommand> commands = {}) {
        accept(host, commands);
        projectiles.update(&authority, prediction, resources, commands);
    }

    void stepShots(const InputCommand& input) {
        step(std::array{input});
        const auto& pool = arsenal.missiles();
        for (usize index = 0; index < pool.count(); ++index) {
            const auto& missile = pool.missile(index);
            if (origins.contains(missile.instance)) {
                continue;
            }
            REQUIRE(missile.release);
            const auto& release = *missile.release;
            const ShotIdentity origin{
                context.epoch,  input.tick,      release.generation, context.grants[1], 1,
                release.strong, release.ordinal, release.emission};
            REQUIRE(origin.valid());
            REQUIRE(origin.emission != 0);
            REQUIRE(origins.emplace(missile.instance, origin).second);
            REQUIRE(births.emplace(origin, PlayerMissiles::flightCheckpoint(missile)).second);
        }
        attacks.updateProjectiles(1.0f / static_cast<f32>(context.tickRate),
                                  std::span{native.data(), members}, targets);
    }

    CombatSnapshot shownShots() {
        auto displayed = shown();
        projectiles.present(displayed, 1);
        REQUIRE(displayed.valid());
        return displayed;
    }

    void compareFlights(const CombatSnapshot& displayed) const {
        std::map<ShotIdentity, const ProjectileState*> bodies;
        for (const auto& shot : displayed.projectiles) {
            if (shot.source == ProjectileSource::PlayerEffect && shot.rollback) {
                CHECK(bodies.emplace(shot.rollback->origin, &shot).second);
            }
        }
        REQUIRE(bodies.size() == arsenal.missiles().count());
        for (usize index = 0; index < arsenal.missiles().count(); ++index) {
            const auto& missile = arsenal.missiles().missile(index);
            const auto found = bodies.find(origins.at(missile.instance));
            REQUIRE(found != bodies.end());
            const auto expected = PlayerMissiles::flightCheckpoint(missile);
            const auto& actual = found->second->rollback->flight;
            CHECK(glm::distance(actual.position, expected.position) < 1e-4f);
            CHECK(glm::distance(actual.velocity, expected.velocity) < 1e-4f);
            CHECK(actual.age == Approx(expected.age).margin(1e-6f));
            CHECK(actual.tumble == Approx(expected.tumble).margin(1e-5f));
            CHECK(actual.scale == expected.scale);
            CHECK(actual.continuity == expected.continuity);
            CHECK(glm::distance(Vec3{found->second->placement[3]}, missile.position) < 1e-4f);
        }
    }
};

TEST_CASE("native turbo volleys replay exact bearing and flight through authoritative rewinds",
          "[netplay][rollback][turbo-projectile-prediction][assets]") {
    const s32 character = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    const f32 meter = GENERATE(40.0f, 100.0f);
    TurboProjectileFixture fixture(character, meter);
    std::vector<CombatSnapshot> history{fixture.authority};
    std::set<ShotIdentity> replayed;
    std::set<ShotIdentity> audible;
    bool simultaneous = false;
    for (u64 tick = 11; tick <= 370; ++tick) {
        CAPTURE(character, meter, tick);
        auto input = fixture.command(tick);
        if (tick == 11) {
            input.pressedButtons = static_cast<u32>(CommandPress::TurboAttack);
        }
        // Cursor-facing changes during the native move exercise the actual
        // authored volley direction rather than treating every shot as body-forward.
        if (tick < 65) {
            input.aimDirection = tick < 32 ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
        }
        fixture.stepShots(input);
        auto host = fixture.captureShots(tick);
        host.players[1]->rollbackInput = input;
        history.push_back(host);
        fixture.isolated([&] {
            if (tick >= 17 && tick % 3 == 2) {
                fixture.acceptShots(history[static_cast<usize>(tick - 4 - 10)], std::array{input});
            } else {
                fixture.prediction.update(&fixture.authority, std::array{input});
                fixture.projectiles.update(&fixture.authority, fixture.prediction,
                                           fixture.resources, std::array{input});
            }
            const auto* replay = fixture.prediction.replayed(1);
            REQUIRE(replay);
            u64 frameTick = fixture.authority.motion.tick;
            for (const auto& frame : replay->frames) {
                CHECK(frame.tick == ++frameTick);
                REQUIRE(frame.tick >= 10);
                REQUIRE(frame.tick - 10 < history.size());
                const auto& native = history[static_cast<usize>(frame.tick - 10)];
                CHECK(frame.animation == *native.players[1]->rollback);
                CHECK(frame.motion.position == native.motion.players[1]->position);
                CHECK(frame.motion.yaw == native.motion.players[1]->yaw);
                CHECK(frame.motion.grant == native.motion.players[1]->grant);
                for (const auto& strike : frame.strikes) {
                    REQUIRE(PlayerAnimator::rollbackTurboAction(frame.animation.action));
                    REQUIRE(strike.slot < native.players[1]->turbo->move.all.size());
                    CHECK(strike.row == native.players[1]->turbo->move.all[strike.slot]);
                }
            }
            CHECK(frameTick == replay->endTick);
            std::map<u64, usize> perTick;
            for (const auto& release : replay->releases) {
                const ShotIdentity key{fixture.context.epoch,
                                       release.tick,
                                       release.generation,
                                       release.motion.grant,
                                       1,
                                       release.strong,
                                       0,
                                       release.emission};
                CAPTURE(key.tick, key.emission);
                REQUIRE(fixture.births.contains(key));
                CHECK(release.shots == 1);
                CHECK_FALSE(release.strong);
                CHECK(release.emission != 0);
                replayed.insert(key);
                simultaneous = simultaneous || ++perTick[key.tick] > 1;
                const auto& velocity = fixture.births.at(key).velocity;
                const Vec3 horizontal{velocity.x, 0, velocity.z};
                REQUIRE(glm::length(horizontal) > 0);
                CHECK(glm::distance(glm::normalize(horizontal), release.direction) < 1e-5f);
            }
            for (const auto& sound : fixture.projectiles.soundReleases()) {
                REQUIRE(fixture.births.contains(sound.origin));
                CHECK(sound.shots == 1);
                audible.insert(sound.origin);
            }
            const auto displayed = fixture.shownShots();
            CHECK(displayed.players[1]->rollback ==
                  fixture.native[0].figure->animator().checkpoint());
            fixture.compareFlights(displayed);
            const auto before = displayed.projectiles;
            // Repeated rendering never emits another launch or mutates a flight.
            for (s32 render = 0; render < 3; ++render) {
                const auto again = fixture.shownShots();
                REQUIRE(again.projectiles.size() == before.size());
                for (usize index = 0; index < before.size(); ++index) {
                    CHECK(again.projectiles[index].key() == before[index].key());
                    CHECK(again.projectiles[index].placement == before[index].placement);
                    CHECK(again.projectiles[index].age == before[index].age);
                }
            }
        });
    }
    std::set<ShotIdentity> nativeKeys;
    for (const auto& [key, birth] : fixture.births) {
        nativeKeys.insert(key);
        CHECK(birth.age == 0);
    }
    CHECK(replayed == nativeKeys);
    CHECK(audible == nativeKeys);
    if (character == 3 && meter == 40) { // Double Bow's left/right rows share ticks.
        CHECK_FALSE(nativeKeys.empty());
        CHECK(simultaneous);
    }
}

TEST_CASE("turbo volley confirmation, interruption, and travel preserve release identity",
          "[netplay][rollback][turbo-projectile-prediction][assets]") {
    TurboProjectileFixture fixture(3, 40);
    std::vector<CombatSnapshot> history{fixture.authority};
    u64 tick = 10;
    InputCommand last;
    CombatSnapshot speculative;
    for (; tick < 120 && fixture.births.empty();) {
        last = fixture.command(++tick);
        if (tick == 11) {
            last.pressedButtons = static_cast<u32>(CommandPress::TurboAttack);
        }
        fixture.stepShots(last);
        auto host = fixture.captureShots(tick);
        host.players[1]->rollbackInput = last;
        history.push_back(host);
        fixture.isolated([&] {
            if (tick >= 17 && tick % 3 == 2) {
                fixture.acceptShots(history[static_cast<usize>(tick - 4 - 10)], std::array{last});
            } else {
                fixture.prediction.update(&fixture.authority, std::array{last});
                fixture.projectiles.update(&fixture.authority, fixture.prediction,
                                           fixture.resources, std::array{last});
            }
            speculative = fixture.shownShots();
        });
    }
    REQUIRE_FALSE(fixture.births.empty());
    REQUIRE(std::ranges::any_of(speculative.projectiles, [](const auto& shot) {
        return shot.instance >= PredictedShotLedger::kPrivateInstance && shot.rollback;
    }));
    fixture.compareFlights(speculative);

    SECTION("confirmation replaces private instances without replaying their sound") {
        fixture.isolated([&] {
            fixture.acceptShots(history.back());
            REQUIRE(fixture.projectiles.soundReleases().empty());
            const auto confirmed = fixture.shownShots();
            fixture.compareFlights(confirmed);
            for (const auto& shot : confirmed.projectiles) {
                CHECK(shot.instance < PredictedShotLedger::kPrivateInstance);
            }
            // Even an old buffered frame that already contains speculative
            // copies is reduced to the one confirmed body/streak per release.
            fixture.projectiles.present(speculative, 1);
            fixture.compareFlights(speculative);
            fixture.acceptShots(history.back());
            CHECK(fixture.projectiles.soundReleases().empty());
            fixture.compareFlights(fixture.shownShots());
        });
    }

    SECTION("an authoritative interruption stops new volleys but not released weapons") {
        fixture.native[0].reaction = PlayerDeed::Flinch;
        last = fixture.command(++tick);
        fixture.stepShots(last);
        REQUIRE(fixture.native[0].figure->animator().action() == Action::HitReact);
        auto interrupted = fixture.captureShots(tick);
        interrupted.players[1]->rollbackInput = last;
        interrupted.players[1]->turbo.reset();
        interrupted.players[1]->turboGeometry.reset();
        interrupted.players[1]->shove = false;
        interrupted.players[1]->movement.enabled = false;
        const auto releases = fixture.births.size();
        fixture.isolated([&] {
            fixture.acceptShots(interrupted);
            CHECK(fixture.prediction.replayed(1) == nullptr);
            CHECK(fixture.projectiles.soundReleases().empty());
            fixture.compareFlights(fixture.shownShots());
        });
        for (s32 elapsed = 0; elapsed < 8; ++elapsed) {
            last = fixture.command(++tick);
            fixture.stepShots(last);
            fixture.isolated([&] {
                fixture.prediction.update(&fixture.authority, std::array{last});
                fixture.projectiles.update(&fixture.authority, fixture.prediction,
                                           fixture.resources, std::array{last});
                CHECK(fixture.projectiles.soundReleases().empty());
                fixture.compareFlights(fixture.shownShots());
            });
        }
        CHECK(fixture.births.size() == releases);
    }

    SECTION("travel clears pending releases and cannot revive the old epoch") {
        fixture.isolated([&] {
            auto next = fixture.context;
            ++next.epoch;
            fixture.prediction.begin(next, 2, fixture.world.collision());
            fixture.projectiles.begin(next, 2, fixture.world.collision());
            CHECK(fixture.projectiles.count() == 0);
            CHECK(fixture.projectiles.soundReleases().empty());
            fixture.prediction.update(&fixture.authority, std::array{last});
            fixture.projectiles.update(&fixture.authority, fixture.prediction, fixture.resources,
                                       std::array{last});
            CHECK(fixture.prediction.replayed(1) == nullptr);
            CHECK(fixture.projectiles.count() == 0);
            CHECK(fixture.projectiles.soundReleases().empty());
        });
    }
}

TEST_CASE("native turbo B and C replay class motion and authored payment without world effects",
          "[netplay][rollback][turbo-prediction][assets]") {
    const s32 character = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    const f32 initialMeter = GENERATE(40.0f, 100.0f);
    const bool moving = GENERATE(false, true);
    TurboFixture fixture(character, initialMeter);
    fixture.begin();
    const auto& stats = *fixture.classes.stats(character);
    auto rows = stats.strikesOf(initialMeter == 100 ? stats.moves.turboC1 : stats.moves.turboB);
    if (initialMeter == 100) {
        const auto second = stats.strikesOf(stats.moves.turboC2);
        rows.insert(rows.end(), second.begin(), second.end());
    }
    s32 paymentFrame = std::numeric_limits<s32>::max();
    for (const auto row : rows) {
        const auto& strike = stats.moveStrikes[static_cast<usize>(row)];
        if (strike.amount != 0) {
            paymentFrame = std::min(paymentFrame, strike.startFrame);
        }
    }
    REQUIRE(paymentFrame != std::numeric_limits<s32>::max());
    std::vector<CombatSnapshot> history{fixture.authority};
    bool began = false;
    bool deferred = false;
    bool expectedDeferred = false;
    bool paid = false;
    bool recovered = false;
    f32 spent = 0;
    for (u64 tick = 11; tick <= 370; ++tick) {
        auto input = fixture.command(tick);
        if (tick == 11 || (tick < 35 && tick % 3 == 0)) {
            input.pressedButtons = static_cast<u32>(CommandPress::TurboAttack);
        }
        if (moving && tick < 65) {
            input.direction = tick < 32 ? Vec2{0, 1} : Vec2{1, 0};
            input.magnitude = 1;
            input.aimDirection = tick < 32 ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
            if (tick >= 45) {
                input.heldButtons = static_cast<u32>(CommandHeld::Strafe);
            }
        }
        const f32 beforeMeter = fixture.native[0].turbo.held();
        fixture.step(std::array{input});
        const auto& member = fixture.native[0];
        if (member.turbo.held() < beforeMeter) {
            spent += beforeMeter - member.turbo.held();
        }
        const auto& animator = member.figure->animator();
        if (tick == 11) {
            CHECK(animator.action() ==
                  (initialMeter == 100 ? Action::TurboFull : Action::TurboStrong));
            // Most native B/C chains charge at frame zero; Jester B's first
            // damaging row starts at frame one and leaves an unpaid checkpoint.
            expectedDeferred = animator.player().frame() < static_cast<f32>(paymentFrame);
            CHECK(member.move.owed() == (expectedDeferred ? initialMeter : 0));
        }
        began = began || animator.turboBegan();
        deferred = deferred || (member.move.owed() > 0 && member.turbo.held() == initialMeter);
        paid = paid || member.turbo.held() < initialMeter - 1;
        recovered = recovered || (tick > 80 && !animator.turboing());
        auto host = fixture.capture(tick);
        host.players[1]->rollbackInput = input;
        history.push_back(host);
        fixture.isolated([&] {
            if (tick >= 17 && tick % 3 == 2) {
                fixture.accept(history[static_cast<usize>(tick - 4 - 10)], std::array{input});
            } else {
                fixture.prediction.update(&fixture.authority, std::array{input});
            }
            CAPTURE(character, initialMeter, moving, tick, animator.action());
            fixture.checkNative();
            constexpr std::array kRenderCounts{0, 1, 4};
            const auto renders = kRenderCounts[static_cast<usize>(tick % kRenderCounts.size())];
            for (s32 render = 0; render < renders; ++render) {
                fixture.shown(static_cast<f32>(render) / 4);
            }
            fixture.checkNative();
        });
    }
    CHECK(began);
    CHECK(deferred == expectedDeferred);
    CHECK(paid);
    CHECK(recovered);
    CHECK(spent == Approx(initialMeter));
    CHECK(fixture.attacks.strikes().count() + fixture.arsenal.missiles().count() +
              fixture.effects.count() >
          0);
}

TEST_CASE("turbo meter thresholds retain native defend priority",
          "[netplay][rollback][turbo-prediction][assets]") {
    const f32 meter = GENERATE(39.99f, 40.0f, 99.99f, 100.0f);
    TurboFixture fixture(0, meter);
    fixture.begin();
    auto input = fixture.command(11);
    input.pressedButtons =
        static_cast<u32>(CommandPress::TurboAttack) | static_cast<u32>(CommandPress::Defend);
    fixture.step(std::array{input});
    fixture.isolated([&] {
        fixture.prediction.update(&fixture.authority, std::array{input});
        fixture.checkNative();
    });
    const auto action = fixture.native[0].figure->animator().action();
    if (meter < 40) {
        CHECK(action != Action::TurboStrong);
        CHECK(action != Action::TurboFull);
        CHECK(fixture.native[0].move.owed() == 0);
    } else {
        CHECK(action == (meter < 100 ? Action::TurboStrong : Action::TurboFull));
    }
}

TEST_CASE("host interruption before the first turbo damage cancels the unpaid timeline",
          "[netplay][rollback][turbo-prediction][assets]") {
    TurboFixture fixture(7); // Native Jester B charges at frame one, not the starting frame.
    fixture.begin();
    auto input = fixture.command(11);
    input.pressedButtons = static_cast<u32>(CommandPress::TurboAttack);
    fixture.step(std::array{input});
    REQUIRE(fixture.native[0].move.owed() == 40);
    fixture.prediction.update(&fixture.authority, std::array{input});
    fixture.checkNative();
    auto& native = fixture.native[0];
    native.reaction = PlayerDeed::Flinch;
    input = fixture.command(12);
    fixture.step(std::array{input});
    REQUIRE(native.figure->animator().action() == Action::HitReact);
    CHECK(native.move.owed() == 0);
    CHECK(native.turbo.held() >= 40);
    auto correction = fixture.capture(12);
    correction.players[1]->turbo.reset();
    correction.players[1]->turboGeometry.reset();
    correction.players[1]->shove = false;
    correction.players[1]->movement.enabled = false;
    fixture.isolated([&] {
        fixture.accept(correction);
        const auto shown = fixture.shown();
        CHECK(shown.players[1]->rollback == correction.players[1]->rollback);
        CHECK(shown.players[1]->turbo == correction.players[1]->turbo);
        CHECK(fixture.prediction.replayed(1) == nullptr);
        CHECK(shown.projectiles.empty());
    });
    for (u64 tick = 13; tick < 180; ++tick) {
        fixture.step(std::array{fixture.command(tick)});
    }
    CHECK(native.move.owed() == 0);
    CHECK(native.turbo.held() >= 40);
    CHECK_FALSE(native.figure->animator().turboing());
}

TEST_CASE("rejected turbo corrects only its local seat and never spends the other meter twice",
          "[netplay][rollback][turbo-prediction][assets]") {
    TurboFixture fixture(7, 40, true);
    fixture.begin();
    std::array inputs{fixture.command(11), fixture.command(11, 2)};
    for (auto& input : inputs) {
        input.pressedButtons = static_cast<u32>(CommandPress::TurboAttack);
    }
    fixture.prediction.update(&fixture.authority, inputs);
    const auto before = fixture.shown();
    CHECK(before.players[1]->animation.action == static_cast<u32>(Action::TurboStrong));
    CHECK(before.players[2]->animation.action == static_cast<u32>(Action::TurboStrong));
    inputs[0].pressedButtons = 0;
    fixture.step(inputs);
    auto accepted = fixture.capture(11);
    accepted.players[1]->rollbackInput = inputs[0];
    accepted.players[2]->rollbackInput = inputs[1];
    fixture.isolated([&] { fixture.accept(accepted); });
    CHECK(fixture.shown().players[1]->rollback ==
          fixture.native[0].figure->animator().checkpoint());
    CHECK(fixture.shown().players[1]->turbo->move.owed == 0);
    CHECK(fixture.shown().players[2]->turbo->move.owed == 40);
    for (u64 tick = 12; tick < 100; ++tick) {
        inputs = {fixture.command(tick), fixture.command(tick, 2)};
        fixture.step(inputs);
        fixture.isolated([&] {
            // A repeated completed checkpoint cannot execute an already resolved edge.
            if (tick % 4 == 0) {
                auto host = fixture.capture(tick);
                host.players[1]->rollbackInput = inputs[0];
                host.players[2]->rollbackInput = inputs[1];
                fixture.accept(host, inputs);
            } else {
                fixture.prediction.update(&fixture.authority, inputs);
            }
            fixture.checkNative(0);
            fixture.checkNative(1);
        });
    }
    CHECK(fixture.native[0].turbo.held() > 40);
    CHECK(fixture.native[1].turbo.held() < 10);
}

TEST_CASE("turbo prediction declines missing bindings, geometry, and unsupported input",
          "[netplay][rollback][turbo-prediction][assets]") {
    enum class Unsafe : u8 { Stats, Geometry, Deadline, Combo, Potion, Disabled, ShoveAuthority };
    const auto unsafe = GENERATE(Unsafe::Stats, Unsafe::Geometry, Unsafe::Deadline, Unsafe::Combo,
                                 Unsafe::Potion, Unsafe::Disabled, Unsafe::ShoveAuthority);
    TurboFixture fixture;
    fixture.begin(unsafe != Unsafe::Stats);
    auto host = fixture.authority;
    host.motion.tick = 11;
    if (unsafe == Unsafe::Geometry) {
        host.players[1]->turbo.reset();
        host.players[1]->turboGeometry.reset();
        host.players[1]->shove = false;
    } else if (unsafe == Unsafe::Deadline) {
        host.players[1]->turboGeometry->moving->ticks = 1;
    } else if (unsafe == Unsafe::Disabled) {
        host.players[1]->movement.enabled = false;
        host.players[1]->turbo.reset();
        host.players[1]->turboGeometry.reset();
        host.players[1]->shove = false;
    } else if (unsafe == Unsafe::ShoveAuthority) {
        host.players[1]->shove = false;
    }
    fixture.accept(host);
    auto input = fixture.command(unsafe == Unsafe::Deadline ? 13 : 12);
    input.pressedButtons = static_cast<u32>(CommandPress::TurboAttack);
    if (unsafe == Unsafe::ShoveAuthority) {
        input.pressedButtons = static_cast<u32>(CommandPress::Charge);
    } else if (unsafe == Unsafe::Combo) {
        input.heldButtons |= static_cast<u32>(CommandHeld::Combo);
    } else if (unsafe == Unsafe::Potion) {
        input.pressedButtons |= static_cast<u32>(CommandPress::UsePotion);
    }
    fixture.isolated([&] {
        fixture.prediction.update(&fixture.authority, std::array{input});
        const auto shown = fixture.shown();
        CHECK(fixture.prediction.replayed(1) == nullptr);
        CHECK(shown.players[1]->rollback == fixture.authority.players[1]->rollback);
        CHECK(shown.players[1]->turbo == fixture.authority.players[1]->turbo);
        CHECK(shown.motion.players[1]->position == fixture.authority.motion.players[1]->position);
    });
}

TEST_CASE("native shove replay preserves charge steering, meter drain, and fixed recovery",
          "[netplay][rollback][turbo-prediction][shove-prediction][assets]") {
    enum class Steering : u8 { Neutral, Stick, CursorStrafe };
    const s32 character = GENERATE(0, 4, 6);
    const auto steering = GENERATE(Steering::Neutral, Steering::Stick, Steering::CursorStrafe);
    TurboFixture fixture(character, TurboMeter::kShoveFrom);
    fixture.begin();
    std::vector<CombatSnapshot> history{fixture.authority};
    bool began = false;
    bool continuedEmpty = false;
    bool recovered = false;
    for (u64 tick = 11; tick <= 170; ++tick) {
        auto input = fixture.command(tick);
        if (tick == 11) {
            input.pressedButtons = static_cast<u32>(CommandPress::Charge);
        }
        if (tick < 22) {
            input.heldButtons = static_cast<u32>(CommandHeld::Turbo);
        }
        if (steering == Steering::Stick && tick < 45) {
            input.direction = tick < 20 ? Vec2{0, 1} : Vec2{1, 0};
            input.magnitude = tick < 28 ? 1.0f : 0.1f;
        } else if (steering == Steering::CursorStrafe && tick < 45) {
            input.direction = tick < 22 ? Vec2{0, 1} : Vec2{-1, 0};
            input.magnitude = 1;
            input.aimDirection = tick < 22 ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
            if (tick >= 35 && tick < 40) {
                input.aimDirection = Vec3{0};
            }
            input.heldButtons |= static_cast<u32>(CommandHeld::Strafe);
        }
        const auto before = fixture.native[0].actor.position();
        const f32 meter = fixture.native[0].turbo.held();
        fixture.step(std::array{input});
        const auto& native = fixture.native[0];
        const auto& animator = native.figure->animator();
        if (tick == 11) {
            REQUIRE(animator.action() == Action::Shove);
            if (steering == Steering::Neutral) {
                CHECK(native.actor.position() == before);
            }
        } else if (tick == 12 && steering == Steering::Neutral) {
            CHECK(glm::distance(native.actor.position(), before) > 0);
        }
        began = began || animator.action() == Action::Shove;
        if (animator.action() == Action::Shove) {
            CHECK(native.turbo.held() ==
                  Approx(std::max(meter - TurboMeter::kShoveDrainPerSecond / 60, 0.0f)));
            CHECK(native.move.owed() == 0);
            CHECK(native.move.checkpoint().all.empty());
            continuedEmpty = continuedEmpty || (meter == 0 && tick >= 22);
        } else if (began) {
            recovered = true;
        }
        auto host = fixture.capture(tick);
        host.players[1]->rollbackInput = input;
        history.push_back(host);
        fixture.isolated([&] {
            if (tick >= 17 && tick % 3 == 2) {
                fixture.accept(history[static_cast<usize>(tick - 4 - 10)], std::array{input});
            } else {
                fixture.prediction.update(&fixture.authority, std::array{input});
            }
            CAPTURE(character, steering, tick, animator.action());
            fixture.checkNative();
            constexpr std::array kRenderCounts{0, 1, 4};
            const auto renders = kRenderCounts[static_cast<usize>(tick % kRenderCounts.size())];
            for (s32 render = 0; render < renders; ++render) {
                fixture.shown(static_cast<f32>(render) / 4);
            }
            fixture.checkNative();
        });
    }
    CHECK(began);
    CHECK(continuedEmpty);
    CHECK(recovered);
    CHECK(fixture.rams > 0);
    CHECK(fixture.native[0].rammed.empty());
}

TEST_CASE("shove rollback retains native edge thresholds and turbo versus guard priority",
          "[netplay][rollback][turbo-prediction][shove-prediction][assets]") {
    const f32 meter = GENERATE(0.0f, 4.99f, 5.0f, 40.0f);
    const bool combined = GENERATE(false, true);
    TurboFixture fixture(0, meter);
    fixture.begin();
    auto input = fixture.command(11);
    input.pressedButtons = static_cast<u32>(CommandPress::Charge);
    if (combined) {
        input.pressedButtons |= static_cast<u32>(CommandPress::Defend);
        if (meter == 40) {
            input.pressedButtons |= static_cast<u32>(CommandPress::TurboAttack);
        }
    }
    fixture.step(std::array{input});
    fixture.isolated([&] {
        fixture.prediction.update(&fixture.authority, std::array{input});
        fixture.checkNative();
    });
    const auto action = fixture.native[0].figure->animator().action();
    if (meter < TurboMeter::kShoveFrom) {
        CHECK(action != Action::Shove);
        if (combined) {
            CHECK(action == Action::DefendRaise);
        }
    } else {
        CHECK(action == (combined && meter == 40 ? Action::TurboStrong : Action::Shove));
    }
}

TEST_CASE("releasing the turbo key does not cancel a native shove or change another seat",
          "[netplay][rollback][turbo-prediction][shove-prediction][assets]") {
    TurboFixture fixture(0, TurboMeter::kShoveFrom, true);
    fixture.begin();
    std::vector<CombatSnapshot> history{fixture.authority};
    const Vec3 separation = fixture.native[1].actor.position() - fixture.native[0].actor.position();
    bool recovered = false;
    for (u64 tick = 11; tick <= 130; ++tick) {
        std::array inputs{fixture.command(tick), fixture.command(tick, 2)};
        if (tick == 11) {
            inputs[0].pressedButtons = inputs[1].pressedButtons =
                static_cast<u32>(CommandPress::Charge);
        }
        inputs[0].heldButtons = static_cast<u32>(CommandHeld::Turbo);
        fixture.step(inputs);
        const auto& held = fixture.native[0];
        const auto& released = fixture.native[1];
        CHECK(held.figure->animator().checkpoint() == released.figure->animator().checkpoint());
        CHECK(held.turbo.checkpoint() == released.turbo.checkpoint());
        CHECK(glm::distance(held.actor.position() + separation, released.actor.position()) < 1e-5f);
        recovered = recovered || (tick > 11 && !held.figure->animator().shoving());
        auto host = fixture.capture(tick);
        host.players[1]->rollbackInput = inputs[0];
        host.players[2]->rollbackInput = inputs[1];
        history.push_back(host);
        fixture.isolated([&] {
            if (tick >= 17 && tick % 3 == 2) {
                fixture.accept(history[static_cast<usize>(tick - 4 - 10)], inputs);
            } else {
                fixture.prediction.update(&fixture.authority, inputs);
            }
            CAPTURE(tick);
            fixture.checkNative(0);
            fixture.checkNative(1);
        });
    }
    CHECK(recovered);
    CHECK(fixture.rams > 0);
}

TEST_CASE("a rejected charge restores its seat while another native shove keeps draining",
          "[netplay][rollback][turbo-prediction][shove-prediction][assets]") {
    TurboFixture fixture(0, 10, true);
    fixture.begin();
    std::array inputs{fixture.command(11), fixture.command(11, 2)};
    for (auto& input : inputs) {
        input.pressedButtons = static_cast<u32>(CommandPress::Charge);
    }
    fixture.isolated([&] {
        fixture.prediction.update(&fixture.authority, inputs);
        const auto shown = fixture.shown();
        REQUIRE(shown.players[1]->rollback);
        REQUIRE(shown.players[2]->rollback);
        CHECK(shown.players[1]->rollback->action == Action::Shove);
        CHECK(shown.players[2]->rollback->action == Action::Shove);
    });
    inputs[0].pressedButtons = 0;
    fixture.step(inputs);
    auto host = fixture.capture(11);
    host.players[1]->rollbackInput = inputs[0];
    host.players[2]->rollbackInput = inputs[1];
    fixture.isolated([&] {
        fixture.accept(host);
        const auto shown = fixture.shown();
        CHECK(shown.players[1]->rollback == fixture.native[0].figure->animator().checkpoint());
        CHECK(shown.players[2]->rollback == fixture.native[1].figure->animator().checkpoint());
        CHECK(shown.players[1]->turbo->meter == fixture.native[0].turbo.checkpoint());
        CHECK(shown.players[2]->turbo->meter == fixture.native[1].turbo.checkpoint());
    });
    for (u64 tick = 12; tick <= 18; ++tick) {
        inputs = {fixture.command(tick), fixture.command(tick, 2)};
        fixture.step(inputs);
        fixture.isolated([&] {
            fixture.prediction.update(&fixture.authority, inputs);
            fixture.checkNative(0);
            fixture.checkNative(1);
        });
    }
    CHECK_FALSE(fixture.native[0].figure->animator().shoving());
    CHECK(fixture.native[1].figure->animator().shoving());
    CHECK(fixture.native[0].turbo.held() > 10);
    CHECK(fixture.native[1].turbo.held() < 10);
}
} // namespace
