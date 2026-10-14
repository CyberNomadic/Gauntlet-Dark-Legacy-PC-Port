#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/audio/AudioMixer.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/players/Progression.h"
#include "game/screens/CombatResources.h"
#include "game/screens/OrdinaryProjectilePhase.h"
#include "game/screens/OrdinaryWeaponEmission.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Emission = OrdinaryWeaponEmission;

struct Fixture {
    std::filesystem::path root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    AudioMixer mixer{48000};
    SoundPlayer sounds{mixer};
    ClassDataSet classes;
    LevelWorld world;
    ItemArchive weapons;
    ItemArchive powerups;
    EffectTrees effects;
    LevelSoundscape audio;
    AmbientDimmer dimmer;
    PlayerArsenal arsenal;
    PlayerAttacks attacks;
    PlayerRuntime player;
    PlayerHealth health;
    LevelWatch levels;
    CombatResources catalog;
    std::unique_ptr<PlayerRuntimeCheckpoint> runtime;
    std::unique_ptr<CombatRollbackCheckpoint> owner;

    Fixture(s32 character, s32 level, bool phoenix, bool boss = false, u32 weaponFlags = 0) {
        REQUIRE(classes.load(root / "PDATA"));
        REQUIRE(weapons.load(root / "WEAPONS"));
        REQUIRE(powerups.load(root / "POWERUPS"));
        audio.open(root, &sounds, nullptr);
        const auto* stats = classes.stats(character);
        REQUIRE(stats);
        CharacterSave save;
        save.name = "TEST";
        save.character = character;
        save.color = 2;
        save.autoAttack = false;
        save.progress().health = 10000;
        save.progress().experience = levelExperience(level);
        if (phoenix) {
            save.progress().inventory.addPowerup(powerup::kSpecial, powerup::kPhoenix, 0, 60);
        }
        if (weaponFlags != 0) {
            save.progress().inventory.addPowerup(powerup::kWeapon, weaponFlags, 0, 60);
        }
        player.actor.spawn(3, save, stats, {10, 0, 20}, 0.7f);
        player.entrySave = save;
        player.figure = PlayerFigure::load(device, root, save, false);
        REQUIRE(player.figure);
        auto& figure = *player.figure;
        figure.setCompanionPowerups(device, powerups, save.progress().inventory, &weapons);
        REQUIRE(figure.phoenixActive() == phoenix);
        REQUIRE(figure.familiarTier() == PlayerFamiliar::tierFor(level));
        arsenal.bind({device, classes, weapons, world.collision(), effects, audio, &sounds,
                      "S_WEAPONHITWOOD", false, boss, &powerups});
        attacks.bind({device, classes, world, weapons, effects, audio, &sounds, arsenal, dimmer});
        std::vector banks{CombatResources::Bank{1, &weapons, {}}};
        std::vector<PlayerMissiles::Resource> physical;
        if (level >= 30) {
            REQUIRE(figure.familiarMissile().bound());
            auto* archive = figure.effects();
            REQUIRE(archive);
            banks.push_back({2, archive, {}});
            physical.push_back(
                {1, &PlayerArsenal::familiarSpec(boss), &figure.familiarMissile(), nullptr});
        }
        if (phoenix) {
            REQUIRE(arsenal.phoenixMissile().bound());
            physical.push_back(
                {2, &PlayerArsenal::familiarSpec(boss), &arsenal.phoenixMissile(), nullptr});
        }
        REQUIRE(catalog.bind(device, banks, physical));
        runtime = std::make_unique<PlayerRuntimeCheckpoint>(player, stats);
        const std::array ordered{runtime.get()};
        levels.observe(3, level);
        owner = std::make_unique<CombatRollbackCheckpoint>(ordered, health, levels, attacks,
                                                           arsenal, effects, catalog.resources());
    }
    ~Fixture() {
        owner.reset();
        runtime.reset();
        attacks.clear();
        arsenal.clear();
        effects.clear();
        catalog.clear();
        audio.close();
        sounds.stopAll();
    }
    CombatRollbackCheckpoint::State state() const {
        const auto result = owner->checkpoint();
        REQUIRE(result);
        return *result;
    }
    void release(f32 seconds) {
        auto& figure = *player.figure;
        const s32 ticks = seconds > 0.02f ? 2 : 1;
        for (s32 tick = 0; tick < 240 && !figure.animator().released(); ++tick) {
            figure.animate(0, ticks, seconds, PlayerDeed::Attack);
        }
        REQUIRE(figure.animator().released());
        REQUIRE_FALSE(figure.familiarReleased());
        const auto atWeaponRelease = figure.gameplayCheckpoint();
        REQUIRE(atWeaponRelease);
        REQUIRE(atWeaponRelease->familiarPending);
        figure.animate(0, ticks, seconds, PlayerDeed::None);
        REQUIRE(figure.familiarReleased());
    }
    Emission::Result emit(std::optional<Vec3> target = std::nullopt) {
        return Emission::familiar(arsenal, player.actor, *player.figure, target,
                                  catalog.resources());
    }
    void step(f32 seconds, std::span<const MissileTarget> targets = {}) {
        player.figure->capturePresentation();
        player.figure->animate(0, seconds > 0.02f ? 2 : 1, seconds, PlayerDeed::None);
        arsenal.missiles().update(seconds, nullptr, targets);
    }
};

TEST_CASE("prepared familiar volleys retain both native perches and silent untagged flights",
          "[ordinary-familiar-emission][assets]") {
    const s32 character = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    const s32 configuration = GENERATE(0, 1, 2);
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    const bool boss = GENERATE(false, true);
    const s32 level = configuration == 0 ? 20 : configuration == 1 ? 30 : 80;
    const bool phoenix = configuration != 1;
    CAPTURE(character, configuration, seconds, boss);
    auto fixture = std::make_unique<Fixture>(character, level, phoenix, boss,
                                             powerup::kRapidFire | powerup::kFiveWayShot | 4U);
    fixture->release(seconds);
    const auto baseline = fixture->state();
    const std::optional<Vec3> target =
        character % 2 == 0 ? std::optional<Vec3>{{20, 9, 43}} : std::nullopt;
    const u32 uploads = fixture->device.texturesCreated;
    std::vector<SoundPlayer::Event> heard;
    usize weaponCues = 0;
    const SoundPlayer::Capture soundCapture(&fixture->sounds, [&](const auto& event) {
        if (event.kind == SoundPlayer::Event::Kind::Play) {
            heard.push_back(event);
        }
    });
    const PlayerArsenal::SoundCapture releaseCapture(&fixture->arsenal,
                                                     [&](const auto&) { ++weaponCues; });
    fixture->arsenal.launchFamiliar(fixture->player.actor, fixture->player.figure.get(), target);
    const auto expected = fixture->state();
    const auto& flights = expected.arsenal.missiles.missiles;
    REQUIRE(flights.size() == (configuration == 2 ? 2 : 1));
    CHECK(heard.empty());
    CHECK(weaponCues == 0);
    for (usize index = 0; index < flights.size(); ++index) {
        const auto& flight = flights[index];
        const bool fire = phoenix && index + 1 == flights.size();
        CHECK(flight.owner == 3);
        CHECK(flight.damage == (fire ? 10.0f : 0.1f * static_cast<f32>(level)));
        CHECK(flight.flags == (fire ? 0x11U : 0U));
        CHECK_FALSE(flight.breaksPotions);
        CHECK_FALSE(flight.release);
        CHECK_FALSE(flight.potionRelease);
        CHECK(flight.wallSound == MissileWallSound::Silent);
        CHECK(flight.streakForward == 0);
        CHECK(flight.rider == 0);
        CHECK(flight.effect != 0);
        CHECK(flight.multiplayer == MultiplayerMode::Normal);
        const Vec3 aim = target.value_or(flight.position + fixture->player.actor.facing() * 21.0f);
        const Vec3 delta = aim - flight.position;
        const f32 distance = std::hypot(delta.x, delta.z);
        const f32 inverse = distance > 0.001f ? 1.0f / distance : 1.0f;
        const f32 gravity = boss ? 0.0f : 10.0f;
        const f32 drop = boss ? 0.0f : -0.5f;
        const Vec3 nativeVelocity =
            Vec3{delta.x * inverse,
                 0.02f * (0.5f * gravity * distance * 0.02f + (delta.y + drop) * 50 * inverse),
                 delta.z * inverse} *
            35.0f;
        CHECK(flight.velocity == nativeVelocity);
    }
    if (flights.size() == 2) {
        CHECK(flights[0].instance != flights[1].instance);
        CHECK(flights[0].effect != flights[1].effect);
        CHECK(glm::distance(flights[0].position, flights[1].position) > 0.1f);
    }
    std::vector<CombatRollbackCheckpoint::State> continuation;
    for (s32 tick = 0; tick < 17; ++tick) {
        fixture->step(seconds);
        continuation.push_back(fixture->state());
    }
    REQUIRE(fixture->owner->restore(baseline));
    const auto actual = fixture->emit(target);
    REQUIRE(actual.accepted);
    REQUIRE(actual.launched);
    CHECK_FALSE(actual.cue);
    CHECK_FALSE(actual.impact);
    CHECK(fixture->state() == expected);
    for (usize tick = 0; tick < continuation.size(); ++tick) {
        CAPTURE(tick);
        fixture->step(seconds);
        CHECK(fixture->state() == continuation[tick]);
    }
    CHECK(heard.empty());
    CHECK(weaponCues == 0);
    CHECK(fixture->device.texturesCreated == uploads);
}

TEST_CASE("familiar preparation is pure and requires the native delayed release latch",
          "[ordinary-familiar-emission][assets]") {
    auto fixture = std::make_unique<Fixture>(0, 80, true);
    const auto initial = fixture->state();
    const auto uploads = fixture->device.texturesCreated;
    const auto prepared = fixture->arsenal.prepareFamiliar(fixture->player.actor,
                                                           *fixture->player.figure, std::nullopt);
    REQUIRE(prepared);
    REQUIRE(prepared->size() == 2);
    CHECK(fixture->state() == initial);
    const auto premature = fixture->emit();
    CHECK_FALSE(premature.accepted);
    CHECK(fixture->state() == initial);
    fixture->release(1.0f / 60);
    const auto ready = fixture->state();
    REQUIRE(ready.players.front().figure);
    REQUIRE(ready.players.front().figure->familiarReleased);
    REQUIRE(fixture->emit().accepted);
    CHECK(fixture->arsenal.missiles().count() == 2);
    fixture->step(1.0f / 60);
    const auto following = fixture->state();
    CHECK_FALSE(fixture->player.figure->familiarReleased());
    CHECK_FALSE(fixture->emit().accepted);
    CHECK(fixture->state() == following);
    CHECK(fixture->device.texturesCreated == uploads);
}

TEST_CASE("a missing second familiar resource refuses before the first native launch",
          "[ordinary-familiar-emission][assets]") {
    auto fixture = std::make_unique<Fixture>(0, 80, true);
    fixture->release(1.0f / 60);
    const auto baseline = fixture->state();
    auto resources = fixture->catalog.resources();
    std::vector<EffectTrees::PoolResource> effects{resources.missileEffects.begin(),
                                                   resources.missileEffects.end()};
    std::erase_if(effects, [](const auto& value) {
        return value.prototype != nullptr && value.prototype->tree != nullptr &&
               value.prototype->tree->name == "PHOENIX_FBALL";
    });
    REQUIRE(effects.size() < resources.missileEffects.size());
    resources.missileEffects = effects;
    const auto refused = Emission::familiar(fixture->arsenal, fixture->player.actor,
                                            *fixture->player.figure, std::nullopt, resources);
    CHECK_FALSE(refused.accepted);
    CHECK_FALSE(refused.launched);
    CHECK_FALSE(refused.cue);
    CHECK_FALSE(refused.impact);
    CHECK(fixture->state() == baseline);
}

TEST_CASE("companion birth does not inherit the ordinary weapon blocked-muzzle rule",
          "[ordinary-familiar-emission][assets]") {
    auto fixture = std::make_unique<Fixture>(0, 80, true);
    fixture->release(1.0f / 60);
    const auto launch = fixture->arsenal.prepareFamiliar(fixture->player.actor,
                                                         *fixture->player.figure, std::nullopt);
    REQUIRE(launch);
    REQUIRE(launch->size() == 2);
    const auto muzzle = launch->front().position;
    auto& collision = const_cast<WorldCollision&>(fixture->world.collision());
    collision.build(
        {{{1, 0, 0},
          {muzzle + Vec3{0, -20, -20}, muzzle + Vec3{0, 20, -20}, muzzle + Vec3{0, 0, 20}},
          1,
          2}});
    REQUIRE(glm::distance(collision.resolveWalls(muzzle, 1, muzzle.y - .5f, muzzle.y + .5f),
                          muzzle) > 1e-4f);
    const auto baseline = fixture->state();
    fixture->arsenal.launchFamiliar(fixture->player.actor, fixture->player.figure.get());
    const auto expected = fixture->state();
    REQUIRE(expected.arsenal.missiles.missiles.size() == 2);
    REQUIRE(fixture->owner->restore(baseline));
    const auto actual = fixture->emit();
    REQUIRE(actual.accepted);
    REQUIRE(actual.launched);
    CHECK_FALSE(actual.cue);
    CHECK_FALSE(actual.impact);
    CHECK(fixture->state() == expected);
}

TEST_CASE("late second familiar exhaustion rewinds flights effect RNG and delayed latches",
          "[ordinary-familiar-emission][assets]") {
    auto fixture = std::make_unique<Fixture>(2, 80, true);
    fixture->release(1.0f / 30);
    auto exhausted = fixture->state();
    exhausted.arsenal.missiles.nextInstance = std::numeric_limits<u64>::max();
    REQUIRE(fixture->owner->restore(exhausted));
    const auto baseline = fixture->state();
    auto undo = fixture->owner->prepareRewind();
    REQUIRE(undo);
    const auto uploads = fixture->device.texturesCreated;
    const auto refused = fixture->emit(Vec3{20, 9, 43});
    CHECK_FALSE(refused.accepted);
    CHECK_FALSE(refused.launched);
    CHECK_FALSE(refused.cue);
    CHECK_FALSE(refused.impact);
    // The first launch succeeds natively; the second cannot allocate an identity.
    // The public transaction, not a best-effort partial fan, owns atomicity.
    REQUIRE(fixture->arsenal.missiles().count() == 1);
    REQUIRE(fixture->owner->canRewind(*undo));
    REQUIRE(fixture->owner->rewind(std::move(*undo)));
    CHECK(fixture->state() == baseline);
    CHECK(fixture->device.texturesCreated == uploads);
}

TEST_CASE("familiar impact and native visual retirement survive a native correction",
          "[ordinary-familiar-emission][assets]") {
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    CAPTURE(seconds);
    auto host = std::make_unique<Fixture>(6, 80, true);
    auto guest = std::make_unique<Fixture>(6, 80, true);
    host->release(seconds);
    host->arsenal.launchFamiliar(host->player.actor, host->player.figure.get(), Vec3{10, 2, 40});
    host->step(seconds);
    const auto authority = host->state();
    REQUIRE(guest->owner->restore(authority));
    REQUIRE(guest->state() == authority);
    REQUIRE(host->arsenal.missiles().count() == 2);
    const auto& visuals = host->arsenal.missiles().visuals();
    REQUIRE(visuals.count() == 2);
    // These native flights have no live particles at this early contact. finish()
    // removes their meshes, and the same update prunes the empty retired effects;
    // a surviving particle tail is not a requirement of a familiar impact.
    for (usize index = 0; index < visuals.count(); ++index) {
        CAPTURE(index);
        CHECK(visuals.effect(index).particles.field().particleCount() == 0);
        CHECK(visuals.effect(index).trails.particleCount() == 0);
    }
    const auto& flight = host->arsenal.missiles().missile(0);
    const std::array targets{
        MissileTarget{1000, flight.position + flight.velocity * seconds, 8, 20}};
    const auto hostUploads = host->device.texturesCreated;
    const auto guestUploads = guest->device.texturesCreated;
    host->step(seconds, targets);
    guest->step(seconds, targets);
    auto expected = host->state();
    CHECK(guest->state() == expected);
    REQUIRE(expected.arsenal.missiles.impacts.size() == 2);
    REQUIRE(expected.arsenal.missiles.missiles.empty());
    REQUIRE(expected.arsenal.missiles.visuals.effects.empty());
    for (s32 tick = 0; tick < 35; ++tick) {
        CAPTURE(tick, seconds);
        host->step(seconds);
        guest->step(seconds);
        expected = host->state();
        CHECK(guest->state() == expected);
        if (tick % 7 == 3) {
            REQUIRE(guest->owner->restore(authority));
            guest->step(seconds, targets);
            for (s32 replay = 0; replay <= tick; ++replay) {
                guest->step(seconds);
            }
            CHECK(guest->state() == expected);
        }
    }
    CHECK(host->device.texturesCreated == hostUploads);
    CHECK(guest->device.texturesCreated == guestUploads);
}

TEST_CASE("companion flight admission requires exact native resources not an elemental mask",
          "[ordinary-familiar-emission][assets]") {
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    auto fixture = std::make_unique<Fixture>(0, 80, true);
    fixture->release(seconds);
    REQUIRE(fixture->emit().accepted);
    const auto baseline = fixture->state();
    const auto resources = fixture->catalog.resources();
    const std::span party{&fixture->player, usize{1}};
    const auto supported = [&](const auto& state) {
        return OrdinaryProjectilePhase::supported(state, resources.missiles, seconds,
                                                  &fixture->arsenal, party);
    };
    REQUIRE(supported(baseline.arsenal));
    CHECK_FALSE(OrdinaryProjectilePhase::supported(baseline.arsenal, resources.missiles, seconds));
    auto changed = baseline.arsenal;
    REQUIRE(changed.missiles.missiles.size() == 2);
    changed.missiles.missiles.front().flags = 0x11;
    CHECK_FALSE(supported(changed));
    changed = baseline.arsenal;
    changed.missiles.missiles.back().resource = changed.missiles.missiles.front().resource;
    CHECK_FALSE(supported(changed));
    changed = baseline.arsenal;
    changed.missiles.missiles.back().breaksPotions = true;
    CHECK_FALSE(supported(changed));
    changed = baseline.arsenal;
    changed.missiles.missiles.back().release = MissileRelease{1, 0, false, 0};
    CHECK_FALSE(supported(changed));
    changed = baseline.arsenal;
    PlayerMissiles::Checkpoint::Impact queued;
    queued.owner = 3;
    queued.damage = 10;
    queued.flags = 0x11;
    queued.wallSound = MissileWallSound::Silent;
    changed.missiles.impacts.push_back(queued);
    REQUIRE(changed.valid());
    CHECK_FALSE(supported(changed));
    CHECK(fixture->state() == baseline);
}
} // namespace
