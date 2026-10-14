#include <algorithm>
#include <array>
#include <memory>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/audio/AudioMixer.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/players/Progression.h"
#include "game/screens/CombatResources.h"
#include "game/screens/HelpMessages.h"
#include "game/screens/OpponentResources.h"
#include "game/screens/OrdinaryCombatCheckpoint.h"
#include "game/screens/OrdinaryStrongPhase.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Strong = OrdinaryStrongPhase;
using State = OrdinaryCombatCheckpoint::State;

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
    PlayerRuntime player;
    PlayerHealth health;
    LevelWatch levels;
    CombatResources catalog;
    std::unique_ptr<PlayerRuntimeCheckpoint> runtime;
    std::unique_ptr<CombatRollbackCheckpoint> combat;
    std::unique_ptr<OrdinaryCombatCheckpoint> owner;

    explicit Fixture(s32 character) {
        REQUIRE(classes.load(root / "PDATA"));
        REQUIRE(weapons.load(root / "WEAPONS"));
        audio.open(root, &sounds, nullptr);
        auto& floor = const_cast<WorldCollision&>(world.collision());
        floor.build({{{0, 1, 0}, {{{-100, 0, -100}, {100, 0, -100}, {100, 0, 100}}}, -1, 14},
                     {{0, 1, 0}, {{{-100, 0, -100}, {100, 0, 100}, {-100, 0, 100}}}, -1, 14}});
        const auto* stats = classes.stats(character);
        REQUIRE(stats);
        CharacterSave save;
        save.name = "STRONG";
        save.character = character;
        save.color = 2;
        save.autoAttack = false;
        save.progress().experience = levelExperience(20);
        save.progress().health = 9000;
        player.actor.spawn(3, save, stats, {}, 0);
        player.entrySave = save;
        player.figure = PlayerFigure::load(device, root, save, false);
        REQUIRE(player.figure);
        player.turbo.add(100);
        arsenal.bind({device, classes, weapons, world.collision(), effects, audio, &sounds, {}});
        attacks.bind({device, classes, world, weapons, effects, audio, &sounds, arsenal, dimmer});
        fixtures.bind({device, world, weapons, effects, audio});
        fixtures.setPlayerCount(1);
        opponents.open({device, world, weapons, effects, audio, root, 1}, party());
        auto& enemies = opponents.enemies();
        enemies.open(device, root, &world.collision(), 8, {.health = 2, .players = 1}, 79);
        REQUIRE(enemies.loadKind(kGruntKind));
        REQUIRE(enemies.spawn(
            {.kind = kGruntKind, .tier = 3, .position = {1, 0, 2}, .placed = true}, {}));
        REQUIRE(catalog.bindStage(device, world, weapons, classes, party(), arsenal, opponents,
                                  effects));
        runtime = std::make_unique<PlayerRuntimeCheckpoint>(player, stats);
        const std::array ordered{runtime.get()};
        levels.observe(3, experienceLevel(save.experience()));
        combat = std::make_unique<CombatRollbackCheckpoint>(ordered, health, levels, attacks,
                                                            arsenal, effects, catalog.resources());
        const auto missiles = OpponentResources::missiles(enemies);
        owner = std::make_unique<OrdinaryCombatCheckpoint>(*combat, opponents, missiles);
    }
    ~Fixture() {
        owner.reset();
        combat.reset();
        runtime.reset();
        attacks.clear();
        arsenal.clear();
        opponents.close();
        effects.clear();
        catalog.clear();
        audio.close();
        sounds.stopAll();
    }
    std::span<PlayerRuntime> party() { return {&player, 1}; }
    PlayerAttacks::Targets targets() {
        PlayerAttacks::Targets targets{opponents, fixtures, {}};
        targets.players = party();
        return targets;
    }
    std::unique_ptr<State> state() const {
        const auto value = owner->checkpoint();
        REQUIRE(value);
        return std::make_unique<State>(*value);
    }
    void pose(s32 tick, f32 seconds) {
        effects.capturePresentation();
        player.figure->capturePresentation();
        player.figure->animate(0, seconds > .02f ? 2 : 1, seconds,
                               tick == 0 ? PlayerDeed::StrongAttack : PlayerDeed::None);
    }
    std::vector<s32> native(s32 tick, f32 seconds) {
        pose(tick, seconds);
        std::vector<s32> lessons;
        attacks.updateTurbo(0, seconds > .02f ? 2 : 1, seconds, party(), [&](s32 id, usize index) {
            CHECK(index == 0);
            lessons.push_back(id);
        });
        attacks.updateStrikes(seconds, party(), targets());
        effects.update(seconds);
        return lessons;
    }
    Strong::Result replay(s32 tick, f32 seconds) {
        pose(tick, seconds);
        const auto action = player.figure->animator().action();
        Strong::Result result{.advanced = true};
        if (action == PlayerAnimator::Action::StrongThrow ||
            action == PlayerAnimator::Action::StrongThrowRecover) {
            result = Strong::advance(
                attacks, 0, seconds > .02f ? 2 : 1, seconds, party(), catalog.resources(),
                [](s32 id, usize) { return HelpMessages::specOf(id) == nullptr; });
        } else {
            // Neutral continuation has no free-A emission. Keep the actual
            // native meter reset/refill path, with a full meter in this fixture.
            attacks.updateTurbo(0, seconds > .02f ? 2 : 1, seconds, party(),
                                [&](s32, usize) { result.advanced = false; });
        }
        if (!result.advanced || !Strong::stepStrikes(attacks, seconds, party(), targets())) {
            result.advanced = false;
            return result;
        }
        effects.update(seconds);
        return result;
    }
};

TEST_CASE("strong throw prepared phase reproduces all native families and active corrections",
          "[ordinary-strong-phase][assets]") {
    const s32 character = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    const f32 seconds = GENERATE(1.0f / 60, 1.0f / 30);
    CAPTURE(character, seconds);
    Fixture fixture{character};
    const auto baseline = fixture.state();
    std::vector<std::unique_ptr<State>> expected;
    std::vector<std::vector<SoundPlayer::Event>> cues;
    std::vector<SoundPlayer::Event> heard;
    const SoundPlayer::Capture capture(&fixture.sounds, [&](const auto& event) {
        if (event.kind == SoundPlayer::Event::Kind::Play) {
            heard.push_back(event);
        }
    });
    std::optional<usize> activeCorrection;
    bool harm = false;
    bool particles = false;
    usize nativeCues = 0;
    for (s32 tick = 0; tick < 160; ++tick) {
        heard.clear();
        const auto lessons = fixture.native(tick, seconds);
        for (const s32 id : lessons) {
            CHECK(HelpMessages::specOf(id) == nullptr);
        }
        nativeCues += heard.size();
        cues.push_back(heard);
        auto next = fixture.state();
        if (!activeCorrection && !next->combat.effects.effects.empty()) {
            activeCorrection = expected.size();
        }
        particles |= !next->combat.attacks.particles.empty();
        harm |= next->opponents.enemies != baseline->opponents.enemies;
        expected.push_back(std::move(next));
    }
    REQUIRE(nativeCues > 0);
    if (character == 2) {
        REQUIRE(particles); // WIZ's two real, independently seeded wrist emitters.
    }
    if (character == 6) {
        REQUIRE(harm); // SOR's actual expanding and flying damage, not just her pose.
    }
    if (character == 2 || character == 6 || character == 7) {
        REQUIRE(activeCorrection);
    }
    REQUIRE(fixture.owner->restore(*baseline));
    const auto uploads = fixture.device.texturesCreated;
    for (usize tick = 0; tick < expected.size(); ++tick) {
        CAPTURE(character, seconds, tick);
        heard.clear();
        const auto actual = fixture.replay(static_cast<s32>(tick), seconds);
        REQUIRE(actual.advanced);
        CHECK(heard.empty());
        REQUIRE(actual.cues.size() == cues[tick].size());
        for (usize cue = 0; cue < actual.cues.size(); ++cue) {
            REQUIRE(actual.cues[cue].classCall);
            CHECK(actual.cues[cue].classCall == cues[tick][cue].call);
            CHECK(actual.cues[cue].owner == 3);
            CHECK(actual.cues[cue].generation > 0);
        }
        const auto value = fixture.state();
        CHECK(*value == *expected[tick]);
        CHECK(fixture.device.texturesCreated == uploads);
    }
    // Reconcile into an already-born particle/tree or, for sound-only classes,
    // an active strong animation. Continue from that exact state, not birth.
    const usize correction = activeCorrection.value_or(3);
    REQUIRE(fixture.owner->restore(*expected[correction]));
    for (usize tick = correction + 1; tick < expected.size(); ++tick) {
        CAPTURE(character, seconds, tick, correction);
        heard.clear();
        REQUIRE(fixture.replay(static_cast<s32>(tick), seconds).advanced);
        const auto value = fixture.state();
        CHECK(*value == *expected[tick]);
        CHECK(heard.empty());
        CHECK(fixture.device.texturesCreated == uploads);
    }
}

TEST_CASE("strong emissions refuse missing trusted particles before mutating any owner",
          "[ordinary-strong-phase][assets]") {
    Fixture fixture{2};
    fixture.pose(0, 1.0f / 30);
    const auto* stats = fixture.classes.stats(2);
    REQUIRE(stats);
    const auto rows = stats->strikesOf(stats->moves.turboAThrow);
    REQUIRE_FALSE(rows.empty());
    auto resources = fixture.catalog.resources();
    std::vector<EffectTrees::PoolResource> incomplete{resources.sharedEffects.begin(),
                                                      resources.sharedEffects.end()};
    const auto beforeSize = incomplete.size();
    std::erase_if(incomplete, [](const auto& resource) {
        return resource.prototype && resource.prototype->tree == nullptr &&
               resource.prototype->trails.size() != 0;
    });
    REQUIRE(incomplete.size() < beforeSize);
    resources.sharedEffects = incomplete;
    const auto before = fixture.state();
    const auto uploads = fixture.device.texturesCreated;
    const auto result =
        Strong::emit(fixture.attacks, 0, {rows.front(), 0}, fixture.party(), resources);
    CHECK_FALSE(result.advanced);
    CHECK(result.cues.empty());
    const auto after = fixture.state();
    CHECK(*before == *after);
    CHECK(fixture.device.texturesCreated == uploads);
}

} // namespace
