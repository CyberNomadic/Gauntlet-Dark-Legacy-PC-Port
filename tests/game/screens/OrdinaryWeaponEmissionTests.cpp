#include <array>
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
#include "game/screens/OrdinaryWeaponEmission.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Emission = OrdinaryWeaponEmission;
constexpr s32 kSorceressClass = 6;

TEST_CASE("ordinary emission refuses an unbound owner without fabricating a release",
          "[ordinary-weapon-emission]") {
    PlayerArsenal arsenal;
    PlayerActor actor;
    PlayerFigure figure;
    actor.spawn(2, {}, nullptr, {}, 0);
    const auto save = actor.save();
    const auto result = Emission::advance(arsenal, actor, figure, {0, 0, 1}, {}, {});
    CHECK_FALSE(result.launched);
    CHECK_FALSE(result.cue);
    CHECK(arsenal.missiles().count() == 0);
    CHECK(arsenal.missiles().visuals().count() == 0);
    CHECK(actor.save().toJson() == save.toJson());
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
    PlayerRuntime player;
    PlayerHealth health;
    LevelWatch levels;
    CombatResources catalog;
    std::unique_ptr<PlayerRuntimeCheckpoint> runtime;
    std::unique_ptr<CombatRollbackCheckpoint> owner;

    explicit Fixture(s32 character = 0, u32 flags = 0, s32 seat = 3) {
        REQUIRE(classes.load(root / "PDATA"));
        REQUIRE(weapons.load(root / "WEAPONS"));
        audio.open(root, &sounds, nullptr);
        const auto* stats = classes.stats(character);
        REQUIRE(stats);
        CharacterSave save;
        save.name = "TEST";
        save.character = character;
        save.color = 2;
        save.autoAttack = false;
        save.progress().health = 10000;
        save.progress().experience = levelExperience(20);
        if (flags != 0) {
            save.progress().inventory.addPowerup(powerup::kWeapon, flags, 0, 30, true);
        }
        player.actor.spawn(seat, save, stats, {10, 0, 20}, 0.25f);
        player.entrySave = save;
        player.figure = PlayerFigure::load(device, root, save, false);
        REQUIRE(player.figure);
        arsenal.bind({device, classes, weapons, world.collision(), effects, audio, &sounds, {}});
        attacks.bind({device, classes, world, weapons, effects, audio, &sounds, arsenal, dimmer});
        auto& figure = *player.figure;
        auto* archive = figure.missileArchive();
        REQUIRE(archive);
        REQUIRE_FALSE(figure.missileTree().empty());
        const auto streakIndex = weapons.textures.find("WEP_STREAK");
        REQUIRE(streakIndex);
        const auto* streak = &weapons.textures.texture(device, *streakIndex);
        const std::array banks{CombatResources::Bank{1, archive, {&weapons.textures}}};
        const std::array missiles{
            PlayerMissiles::Resource{1, &MissileSpec::of(character), &figure.missile(), streak}};
        REQUIRE(catalog.bind(device, banks, missiles));
        runtime = std::make_unique<PlayerRuntimeCheckpoint>(player, stats);
        const std::array ordered{runtime.get()};
        levels.observe(seat, experienceLevel(save.experience()));
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
    void release(f32 seconds, bool strong = false) {
        auto& figure = *player.figure;
        const bool rapid = (PowerupEffects::of(player.actor.save().progress().inventory).weapon &
                            powerup::kRapidFire) != 0;
        figure.setAttackSpeed(rapid, false);
        const s32 ticks = seconds > 0.02f ? 2 : 1;
        const bool areaOnly = strong && player.actor.save().character == kSorceressClass;
        for (s32 frame = 0;
             frame < 180 &&
             !(strong ? figure.animator().strongReleased() : figure.animator().released()) &&
             !(areaOnly &&
               figure.animator().action() == PlayerAnimator::Action::StrongThrowRecover);
             ++frame) {
            figure.animate(0, ticks, seconds,
                           strong ? PlayerDeed::StrongAttack : PlayerDeed::Attack);
        }
        REQUIRE(figure.animator().released() == !strong);
        REQUIRE(figure.animator().strongReleased() == (strong && !areaOnly));
        if (areaOnly) {
            REQUIRE(figure.animator().action() == PlayerAnimator::Action::StrongThrowRecover);
        }
    }
    CombatRollbackCheckpoint::State state() const {
        const auto state = owner->checkpoint();
        REQUIRE(state);
        return *state;
    }
    PlayerMissiles::Checkpoint flights() const {
        const auto resources = catalog.resources();
        const auto state =
            arsenal.missiles().checkpoint(resources.missiles, resources.missileEffects);
        REQUIRE(state);
        return *state;
    }
    Emission::Result emit(std::optional<Vec3> target = std::nullopt) {
        return Emission::advance(arsenal, player.actor, *player.figure, player.actor.facing(),
                                 target, catalog.resources());
    }
};

TEST_CASE("prepared ordinary release retains native physical visual and sound descriptors",
          "[ordinary-weapon-emission][assets]") {
    const s32 character = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    const f32 seconds = GENERATE(1.0f / 60, 1.0f / 30);
    const u32 flags = GENERATE(0U, powerup::kRapidFire | powerup::kFiveWayShot);
    const bool strong = GENERATE(false, true);
    CAPTURE(character, seconds, flags, strong);
    Fixture fixture(character, flags);
    fixture.release(seconds, strong);
    const auto baseline = fixture.state();
    const auto target = character == 2 ? std::optional<Vec3>{{16, 6, 40}} : std::nullopt;
    if (strong && character == kSorceressClass) {
        // AnimAction deliberately omits her ordinary-weapon release. The native
        // DAMG/SFXX timeline supplies the strong attack instead (tested separately).
        const auto result = fixture.emit(target);
        CHECK_FALSE(result.launched);
        CHECK_FALSE(result.cue);
        CHECK(fixture.state() == baseline);
        CHECK(fixture.arsenal.missiles().count() == 0);
        return;
    }
    std::vector<SoundPlayer::Event> heard;
    const SoundPlayer::Capture capture(&fixture.sounds, [&](const auto& event) {
        if (event.kind == SoundPlayer::Event::Kind::Play) {
            heard.push_back(event);
        }
    });
    const u32 uploads = fixture.device.texturesCreated;
    fixture.arsenal.launchWeapon(fixture.player.actor, fixture.player.figure.get(),
                                 fixture.player.actor.facing(), strong ? 2.0f : 1.0f, true, target);
    const auto expected = fixture.flights();
    REQUIRE(expected.missiles.size() == (flags == 0 ? 1 : 5));
    REQUIRE(expected.history.size() == expected.missiles.size());
    for (const auto& missile : expected.missiles) {
        REQUIRE(missile.release);
        CHECK(missile.release->strong == strong);
        CHECK(missile.scale == (strong ? 2.0f : 1.0f));
    }
    REQUIRE(heard.size() == 1);
    const auto nativeSound = heard.front();
    std::vector<PlayerMissiles::Checkpoint> continuation;
    for (s32 tick = 0; tick < 100; ++tick) {
        fixture.arsenal.missiles().update(seconds, nullptr);
        continuation.push_back(fixture.flights());
    }
    REQUIRE(fixture.owner->restore(baseline));
    heard.clear();
    const auto actual = fixture.emit(target);
    REQUIRE(actual.launched);
    REQUIRE(actual.cue);
    CHECK(heard.empty());
    CHECK(actual.cue->owner == 3);
    CHECK(actual.cue->release == *expected.missiles.front().release);
    if (flags == 0) {
        CHECK(actual.cue->sound.name.empty());
        CHECK(actual.cue->sound.call == fixture.player.figure->throwSound());
        CHECK(actual.cue->sound.call == nativeSound.call);
    } else {
        CHECK(actual.cue->sound.name == "S_SUPERSHOT");
        CHECK_FALSE(actual.cue->sound.call);
        SoundSet common;
        REQUIRE(common.load(fixture.root / "AUDIO/COMMON"));
        CHECK(nativeSound.call == common.find("S_SUPERSHOT"));
    }
    CHECK(fixture.flights() == expected);
    for (usize tick = 0; tick < continuation.size(); ++tick) {
        CAPTURE(character, flags, seconds, strong, tick);
        fixture.arsenal.missiles().update(seconds, nullptr);
        CHECK(fixture.flights() == continuation[tick]);
    }
    const auto after = fixture.state();
    CHECK(after.players == baseline.players);
    CHECK(after.health == baseline.health);
    CHECK(after.levels == baseline.levels);
    CHECK(after.attacks == baseline.attacks);
    CHECK(after.effects == baseline.effects);
    CHECK(heard.empty());
    CHECK(fixture.device.texturesCreated == uploads);
}

TEST_CASE("ordinary launch preflight refuses missing bindings and malformed targets atomically",
          "[ordinary-weapon-emission][assets]") {
    const s32 variant = GENERATE(0, 1, 2, 3, 4, 5);
    Fixture fixture;
    fixture.release(1.0f / 60);
    auto resources = fixture.catalog.resources();
    std::vector<PlayerMissiles::Resource> physical(resources.missiles.begin(),
                                                   resources.missiles.end());
    std::vector<EffectTrees::PoolResource> visuals(resources.missileEffects.begin(),
                                                   resources.missileEffects.end());
    auto target = std::optional<Vec3>{};
    if (variant == 0) {
        physical.clear();
    } else if (variant == 1) {
        visuals.clear();
    } else if (variant == 2) {
        physical.front().id = 0;
    } else if (variant == 3) {
        auto duplicate = physical.front();
        duplicate.id = 19;
        physical.push_back(duplicate);
    } else if (variant == 4) {
        for (auto& visual : visuals) {
            visual.lenders = {};
        }
    } else {
        target = Vec3{0, std::numeric_limits<f32>::quiet_NaN(), 1};
    }
    resources.missiles = physical;
    resources.missileEffects = visuals;
    const auto baseline = fixture.state();
    const auto uploads = fixture.device.texturesCreated;
    std::vector<SoundPlayer::Event> audio;
    const SoundPlayer::Capture capture(&fixture.sounds,
                                       [&](const auto& event) { audio.push_back(event); });
    const auto result =
        Emission::advance(fixture.arsenal, fixture.player.actor, *fixture.player.figure,
                          fixture.player.actor.facing(), target, resources);
    CHECK_FALSE(result.launched);
    CHECK_FALSE(result.cue);
    CHECK(fixture.state() == baseline);
    CHECK(audio.empty());
    CHECK(fixture.device.texturesCreated == uploads);
}

TEST_CASE("blocked native muzzle refuses before world impact art and audio",
          "[ordinary-weapon-emission][assets]") {
    Fixture fixture;
    fixture.release(1.0f / 60);
    const auto release = fixture.arsenal.prepareWeapon(fixture.player.actor, *fixture.player.figure,
                                                       fixture.player.actor.facing(), 1, true);
    REQUIRE(release);
    REQUIRE_FALSE(release->missiles.empty());
    const auto muzzle = release->missiles.front().position;
    auto& collision = const_cast<WorldCollision&>(fixture.world.collision());
    const CollisionTriangle wall{
        {1, 0, 0},
        {muzzle + Vec3{0, -20, -20}, muzzle + Vec3{0, 20, -20}, muzzle + Vec3{0, 0, 20}},
        1,
        2}; // Native wall-query bit; zero describes nonblocking decoration.
    collision.build({wall});
    const auto radius = release->missiles.front().spec->radius;
    REQUIRE(glm::distance(collision.resolveWalls(muzzle, radius, muzzle.y - radius * 0.5f,
                                                 muzzle.y + radius * 0.5f),
                          muzzle) > 1e-4f);
    const auto baseline = fixture.state();
    const auto uploads = fixture.device.texturesCreated;
    std::vector<SoundPlayer::Event> audio;
    const SoundPlayer::Capture capture(&fixture.sounds,
                                       [&](const auto& event) { audio.push_back(event); });
    const auto result = fixture.emit();
    CHECK_FALSE(result.launched);
    CHECK_FALSE(result.cue);
    CHECK(fixture.state() == baseline);
    CHECK(audio.empty());
    CHECK(fixture.device.texturesCreated == uploads);
}

TEST_CASE("a late spread refusal is rewound by the complete caller-owned combat transaction",
          "[ordinary-weapon-emission][assets]") {
    Fixture fixture(0, powerup::kThreeWayShot);
    fixture.release(1.0f / 60);
    auto baseline = fixture.state();
    // The first native flight can consume the last instance; the second must
    // fail. This exercises genuine partial private work, not a fake callback.
    baseline.arsenal.missiles.nextInstance = std::numeric_limits<u64>::max();
    REQUIRE(fixture.owner->restore(baseline));
    auto undo = fixture.owner->prepareRewind();
    REQUIRE(undo);
    const auto uploads = fixture.device.texturesCreated;
    std::vector<SoundPlayer::Event> audio;
    const SoundPlayer::Capture capture(&fixture.sounds,
                                       [&](const auto& event) { audio.push_back(event); });
    const auto result = fixture.emit();
    CHECK_FALSE(result.launched);
    CHECK_FALSE(result.cue);
    REQUIRE(fixture.arsenal.missiles().count() == 1);
    REQUIRE(fixture.owner->rewind(std::move(*undo)));
    CHECK(fixture.state() == baseline);
    CHECK(audio.empty());
    CHECK(fixture.device.texturesCreated == uploads);
}

TEST_CASE("normal emissions retain native nonsequential player ownership",
          "[ordinary-weapon-emission][assets]") {
    const s32 seat = GENERATE(0, 2, 3);
    Fixture fixture(6, 0, seat);
    fixture.release(1.0f / 60);
    const auto result = fixture.emit();
    REQUIRE(result.launched);
    REQUIRE(result.cue);
    CHECK(result.cue->owner == seat);
    const auto state = fixture.flights();
    REQUIRE(state.missiles.size() == 1);
    CHECK(state.missiles.front().owner == seat);
    REQUIRE(state.history.size() == 1);
    CHECK(state.history.front().owner == seat);
}

TEST_CASE("ordinary release refuses incompatible weapon modes without spending inventory",
          "[ordinary-weapon-emission][assets]") {
    const u32 flags = GENERATE(1U, 4U, powerup::kSuperShot);
    Fixture fixture(0, flags);
    fixture.release(1.0f / 60);
    const auto baseline = fixture.state();
    const auto uploads = fixture.device.texturesCreated;
    const auto result = fixture.emit();
    CHECK_FALSE(result.launched);
    CHECK_FALSE(result.cue);
    CHECK(fixture.state() == baseline);
    CHECK(fixture.device.texturesCreated == uploads);
}

} // namespace
