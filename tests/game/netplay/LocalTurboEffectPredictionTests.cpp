#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/LocalTurboEffectPrediction.h"
#include "game/screens/PartyMotion.h"
#include "game/screens/PlayerAttacks.h"
#include "game/screens/SessionInputs.h"

namespace {
using namespace gdl;
using namespace gdl::game;
constexpr f32 kStep = 1.0f / 60;

struct EffectFixture {
    test::FakeRenderDevice device;
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
    std::array<PlayerRuntime, 1> party;
    PlayerAttacks::Targets targets{
        .opponents = opponents, .fixtures = fixtures, .fixtureEvents = {}, .players = party};
    PartyMotion::Events events;
    TurboEffectResources resources;
    MatchContext context;
    LocalMovementPrediction movement;
    LocalTurboEffectPrediction prediction;
    std::vector<CombatSnapshot> history;
    std::vector<InputCommand> inputs;
    std::map<u32, u64> born;

    EffectFixture(s32 character, f32 meter) {
        const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
        REQUIRE(classes.load(root / "PDATA"));
        REQUIRE(weapons.load(root / "WEAPONS"));
        const auto* stats = classes.stats(character);
        REQUIRE(stats);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): owned synthetic level floor
        auto& collision = const_cast<WorldCollision&>(world.collision());
        const Vec3 a{-100, 0, -100}, b{100, 0, -100}, c{100, 0, 100}, d{-100, 0, 100};
        collision.build({{{0, 1, 0}, {a, b, c}, 0}, {{0, 1, 0}, {a, c, d}, 0}});
        CharacterSave save;
        save.character = character;
        save.progress().health = 100;
        auto& player = party.front();
        player.actor.spawn(1, save, stats, {0, 0, 0}, 0);
        player.figure = PlayerFigure::load(device, root, save, false);
        REQUIRE(player.figure);
        player.turbo.add(meter);
        const std::array lenders{&weapons.textures};
        effects.setTextureLenders(lenders);
        REQUIRE(resources.bind(device, character, *stats, *player.figure, weapons, lenders));
        arsenal.bind({device, classes, weapons, collision, effects, audio, nullptr, {}});
        attacks.bind({device, classes, world, weapons, effects, audio, nullptr, arsenal, dimmer});
        events.select = [](usize, const SelectorInput&, s32) {};
        events.advanceTurbo = [&](usize index, s32 ticks, f32 seconds) {
            attacks.updateTurbo(index, ticks, seconds, party, [](s32, usize) {});
        };
        events.attackDeed = [&](usize index, bool strong, bool moved, const Vec3& facing) {
            return attacks.attackDeed(party[index].actor, strong, targets, moved,
                                      party[index].figure->animator().meleeChain(), facing);
        };
        events.meleeSense = [&](usize index, bool held, const Vec3& facing) {
            return attacks.meleeSense(party[index].actor, held, targets, facing);
        };
        events.aim = [&](usize index) {
            return attacks.aim(party[index].actor, party[index].actor.facing(), targets);
        };
        context.epoch = 3;
        context.scene = 1;
        context.owners = {1, 2, 0, 0};
        context.grants = {1, 7, 0, 0};
        movement.begin(context, 2, collision);
        REQUIRE(movement.setPlayer(1, *player.figure->actionTree(), stats, character));
        prediction.begin(context, 2);
        REQUIRE(prediction.bind(1, resources, *player.figure, weapons,
                                PlayerFigure::bodyScale(save, {})));
        history.push_back(capture(100));
        movement.update(&history.back(), {});
    }

    CombatSnapshot capture(u64 tick) const {
        CombatSnapshot state;
        state.motion.epoch = context.epoch;
        state.motion.tick = tick;
        state.motion.cameraContinuity = 1;
        state.motion.camera = {{0, 12, -18}, 0.5f, 0, 0};
        state.motion.players[0] = SeatMotion{1, 1, {-30, 0, 0}, 0};
        state.players[0].emplace().health = 100;
        const auto& native = party.front();
        const auto& actor = native.actor;
        const auto& animator = native.figure->animator();
        const auto& cursor = animator.player();
        state.motion.players[1] =
            SeatMotion{7, 1, actor.position(), std::remainder(actor.yaw(), kTwoPi)};
        auto& player = state.players[1].emplace();
        player.health = 100;
        player.movement.enabled = player.movement.current = true;
        player.movement.speed = actor.speed();
        player.movement.radius = actor.radius();
        player.movement.height = actor.height();
        player.movement.followHeight = actor.followPoint().y - actor.position().y;
        player.rollback = animator.checkpoint();
        player.transitionPose = animator.transitionPose();
        player.knockback = native.knockback.checkpoint();
        player.turbo = PlayerTurboState{native.turbo.checkpoint(), native.move.checkpoint()};
        player.animation = {static_cast<u32>(animator.action()), cursor.sequence(),
                            cursor.generation(), cursor.presentationFrame(), cursor.transition()};
        const auto selection = attacks.meleeAcquisition(actor, targets);
        REQUIRE(selection);
        auto& geometry = player.turboGeometry.emplace();
        geometry.selection = *selection;
        geometry.captureYaw = state.motion.players[1]->yaw;
        auto& region = geometry.moving.emplace();
        region.origin = actor.position();
        region.radius = PlayerMeleeState::kMaxRadius;
        region.ticks = PlayerMeleeState::kMaxTicks;
        const auto* archive = native.figure->loadedEffects();
        REQUIRE(archive);
        player.turboEffects =
            TurboEffectState{attacks.meleeDamageOf(0, party),
                             effects.textureCheckpoint(*archive, effects.textureLenders()),
                             effects.textureCheckpoint(weapons, effects.textureLenders())};
        REQUIRE(state.valid());
        return state;
    }

    void step() {
        InputCommand command;
        command.epoch = context.epoch;
        command.tick = history.back().motion.tick + 1;
        command.grant = 7;
        command.seat = 1;
        if (inputs.empty()) {
            command.pressedButtons = static_cast<u32>(CommandPress::TurboAttack);
        }
        command.aimDirection = glm::normalize(Vec3{0.2f, 0, 1});
        REQUIRE(command.valid());
        std::array<PlayInput, InputCommand::kSeats> controls;
        controls[1] = SessionInputs::playInput(command);
        effects.capturePresentation();
        PartyMotion::step(party, controls, false, 0, 1, kStep, world.collision(), events);
        attacks.updateStrikes(kStep, party, targets);
        effects.update(kStep);
        for (usize i = 0; i < effects.count(); ++i) {
            const auto& effect = effects.effect(i);
            if (effect.provenance) {
                born.try_emplace(effect.id, command.tick);
            }
        }
        history.push_back(capture(command.tick));
        inputs.push_back(command);
        const usize delay = std::min<usize>(4, inputs.size());
        const auto& authority = history[history.size() - 1 - delay];
        const auto encoded = CombatPacket::encode(authority);
        REQUIRE(encoded);
        const auto decoded = CombatPacket::decode(*encoded);
        REQUIRE(decoded);
        movement.update(&*decoded, inputs);
        const auto uploads = device.texturesCreated;
        prediction.update(&*decoded, movement);
        CHECK(device.texturesCreated == uploads);
        CHECK(device.textureUpdates == 0);
    }

    const EffectTrees::Effect* nativeEffect(const EffectIdentity& origin) const {
        for (usize i = 0; i < effects.count(); ++i) {
            const auto& effect = effects.effect(i);
            if (effect.provenance && effect.provenance->player == 1 &&
                effect.provenance->generation == origin.generation &&
                effect.provenance->occurrence == origin.occurrence &&
                effect.provenance->effectOrdinal == origin.effectOrdinal &&
                born.at(effect.id) == origin.tick) {
                return &effect;
            }
        }
        return nullptr;
    }
};

TEST_CASE("local turbo tree effects replay native B and C births poses particles and shared clocks",
          "[local-turbo-effects][assets]") {
    const s32 character = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    const f32 meter = GENERATE(40.0f, 100.0f);
    CAPTURE(character, meter);
    EffectFixture fixture(character, meter);
    usize compared = 0;
    usize supportedBirths = 0;
    for (usize tick = 0; tick < 140; ++tick) {
        CAPTURE(tick);
        fixture.step();
        if (const auto* frames = fixture.movement.replayed(1)) {
            const auto delay = std::min<usize>(4, fixture.inputs.size());
            const auto& authority = fixture.history[fixture.history.size() - 1 - delay];
            const bool weaponSeed = authority.players[1]->turboEffects->weaponBank.has_value();
            bool supported = true;
            usize births = 0;
            for (const auto& frame : frames->frames) {
                for (const auto& strike : frame.strikes) {
                    const auto prepared = fixture.resources.prepare(strike.row, {character}, 10);
                    if (!prepared || std::ranges::any_of(prepared->effects, [](const auto& effect) {
                            return effect.particle.has_value();
                        })) {
                        supported = false;
                    } else {
                        for (const auto& effect : prepared->effects) {
                            const auto* resource =
                                fixture.resources.resource(static_cast<u32>(effect.row) + 1);
                            REQUIRE(resource);
                            if (resource->kind == TurboEffectResources::Kind::Tree) {
                                supported &= weaponSeed || resource->archive != &fixture.weapons;
                                ++births;
                            }
                        }
                    }
                }
            }
            if (supported) {
                supportedBirths += births;
            }
        }
        const auto* replay = fixture.prediction.simulation(1);
        if (replay == nullptr) {
            continue;
        }
        for (const auto& entry : replay->entries()) {
            const auto* native = fixture.nativeEffect(entry.origin);
            REQUIRE(native);
            CHECK(replay->effects().checkpoint(entry.id) == fixture.effects.checkpoint(native->id));
            ++compared;
        }
        const auto* archive = fixture.party.front().figure->loadedEffects();
        if (replay->effects().count() != 0) {
            CHECK(replay->effects().textureCheckpoint(*archive, fixture.effects.textureLenders()) ==
                  fixture.effects.textureCheckpoint(*archive, fixture.effects.textureLenders()));
        }
    }
    if (supportedBirths != 0) {
        CHECK(compared > 0);
    }
    // Unsupported SFXX paths remain explicitly host-only; this test never
    // treats an empty simulation as proof of their visual parity.
    CAPTURE(supportedBirths, compared);
}

TEST_CASE("turbo effect handoff grafts confirmed identity without a delayed-frame disappearance",
          "[local-turbo-effects][assets]") {
    EffectFixture fixture(0, 40);
    REQUIRE_FALSE(fixture.history.front().players[1]->turboEffects->weaponBank);
    std::optional<EffectIdentity> observed;
    for (usize tick = 0; tick < 100 && !observed; ++tick) {
        fixture.step();
        if (const auto* replay = fixture.prediction.simulation(1);
            replay != nullptr && !replay->entries().empty()) {
            observed = replay->entries().front().origin;
        }
    }
    REQUIRE(observed);
    for (const bool grantCut : {false, true}) {
        auto cut = fixture.history.back();
        if (grantCut) {
            ++cut.motion.players[1]->grant;
        } else {
            ++cut.motion.players[1]->continuity;
        }
        CHECK_FALSE(fixture.prediction.present(cut));
        CHECK(cut.projectiles.empty());
        fixture.device.draws.clear();
        fixture.prediction.draw(fixture.device, cut, Mat4{1}, {}, {}, 1);
        CHECK(fixture.device.draws.empty());
        REQUIRE(fixture.prediction.simulation(1));
    }
    auto confirmed = fixture.history.back();
    ProjectileState effect;
    effect.instance = 99;
    effect.source = ProjectileSource::WorldEffect;
    effect.resource = 1;
    effect.continuity = 1;
    effect.effectOrigin = observed;
    confirmed.projectiles.push_back(effect);
    REQUIRE(confirmed.valid());
    fixture.movement.update(&confirmed, fixture.inputs);
    fixture.prediction.update(&confirmed, fixture.movement);
    auto delayed = fixture.history.front();
    REQUIRE(fixture.prediction.present(delayed));
    REQUIRE(delayed.projectiles.size() == 1);
    CHECK(delayed.projectiles.front().effectOrigin == observed);
    REQUIRE(fixture.prediction.present(delayed));
    CHECK(delayed.projectiles.size() == 1);

    // A live effect can outlast the pending-input history. Keep its latest
    // authoritative placement until retirement, not merely until birth+120.
    confirmed.motion.tick = observed->tick + LocalMovementPrediction::kHistory + 5;
    confirmed.projectiles.front().placement[3].x = 9;
    fixture.movement.update(&confirmed, {});
    fixture.prediction.update(&confirmed, fixture.movement);
    REQUIRE(fixture.prediction.present(delayed));
    REQUIRE(delayed.projectiles.size() == 1);
    CHECK(delayed.projectiles.front().placement[3].x == 9);

    // Once authority has retired that identity, an interpolated old mesh must
    // not resurrect it, even when no current action can be predicted.
    ++confirmed.motion.tick;
    confirmed.projectiles.clear();
    confirmed.players[1]->movement.enabled = false;
    confirmed.players[1]->turbo.reset();
    confirmed.players[1]->turboGeometry.reset();
    confirmed.players[1]->turboEffects.reset();
    REQUIRE(confirmed.valid());
    fixture.movement.update(&confirmed, {});
    fixture.prediction.update(&confirmed, fixture.movement);
    REQUIRE(fixture.prediction.present(delayed));
    CHECK(delayed.projectiles.empty());
    fixture.prediction.update(nullptr, fixture.movement);
    CHECK_FALSE(fixture.prediction.present(delayed));
    CHECK(fixture.prediction.simulation(1) == nullptr);
}

TEST_CASE("shared weapon turbo effects require an authoritative texture bank seed",
          "[local-turbo-effects][assets]") {
    EffectFixture fixture(0, 40);
    auto stats = *fixture.resources.stats();
    const auto first = static_cast<usize>(stats.moves.turboB);
    REQUIRE(first < stats.moveStrikes.size());
    auto& strike = stats.moveStrikes[first];
    REQUIRE(strike.effect >= 0);
    auto& effect = stats.moveEffects[static_cast<usize>(strike.effect)];
    REQUIRE(fixture.weapons.trees.find("SPARKS"));
    REQUIRE_FALSE(fixture.party.front().figure->loadedEffects()->trees.find("SPARKS"));
    effect.tree = "SPARKS";
    effect.next = -1;
    strike.startFrame = 0;
    strike.next = -1;
    stats.moves.turboC1 = stats.moves.turboC2 = -1;
    const std::array lenders{&fixture.weapons.textures};
    REQUIRE(fixture.resources.bind(fixture.device, 0, stats, *fixture.party.front().figure,
                                   fixture.weapons, lenders));
    REQUIRE(fixture.resources.status(static_cast<s32>(first)) ==
            TurboEffectResources::Status::Ready);
    REQUIRE(fixture.movement.setPlayer(1, *fixture.party.front().figure->actionTree(), &stats, 0));
    auto authority = fixture.history.front();
    REQUIRE_FALSE(authority.players[1]->turboEffects->weaponBank);
    InputCommand command;
    command.epoch = fixture.context.epoch;
    command.tick = authority.motion.tick + 1;
    command.grant = 7;
    command.seat = 1;
    command.pressedButtons = static_cast<u32>(CommandPress::TurboAttack);
    fixture.movement.update(&authority, std::span{&command, 1});
    REQUIRE(fixture.movement.replayed(1));
    REQUIRE(fixture.movement.replayed(1)->frames.front().strikes.size() == 1);
    fixture.prediction.update(&authority, fixture.movement);
    CHECK(fixture.prediction.simulation(1) == nullptr);

    // A preceding native impact can create this bank independently of the
    // local turbo. Capture its real clock and resume without resetting phase.
    REQUIRE(fixture.effects.startSet(fixture.device, fixture.weapons, "SPARKS", {0, 0, 0}, {}));
    fixture.effects.update(7 * kStep);
    authority = fixture.capture(authority.motion.tick);
    REQUIRE(authority.players[1]->turboEffects->weaponBank);
    fixture.movement.update(&authority, std::span{&command, 1});
    fixture.prediction.update(&authority, fixture.movement);
    const auto* replay = fixture.prediction.simulation(1);
    REQUIRE(replay);
    REQUIRE(replay->entries().size() == 1);
    fixture.effects.capturePresentation();
    fixture.effects.update(kStep);
    CHECK(replay->effects().textureCheckpoint(fixture.weapons, lenders) ==
          fixture.effects.textureCheckpoint(fixture.weapons, lenders));
    fixture.prediction.clear();
    fixture.movement.clear();
    fixture.resources.clear();
}

TEST_CASE("unknown turbo strike bank creation revokes the whole visual replay prefix",
          "[local-turbo-effects][assets]") {
    EffectFixture fixture(0, 40);
    auto stats = *fixture.resources.stats();
    const auto first = static_cast<usize>(stats.moves.turboB);
    REQUIRE(first < stats.moveStrikes.size());
    const auto original = stats.moveStrikes[first];
    REQUIRE(original.effect >= 0);
    REQUIRE(static_cast<usize>(original.effect) < stats.moveEffects.size());
    const auto copiedEffect = static_cast<s32>(stats.moveEffects.size());
    auto primary = stats.moveEffects[static_cast<usize>(original.effect)];
    primary.next = copiedEffect + 1;
    stats.moveEffects.push_back(primary);
    MoveEffect missing;
    missing.tree = "UNAVAILABLE_PARTICLE";
    missing.flags = 0x01000000U;
    stats.moveEffects.push_back(missing);
    const auto second = static_cast<s32>(stats.moveStrikes.size());
    stats.moveStrikes[first].effect = copiedEffect;
    stats.moveStrikes[first].startFrame = 0;
    stats.moveStrikes[first].next = second;
    auto following = original;
    following.startFrame = 0;
    following.next = -1;
    stats.moveStrikes.push_back(following);
    stats.moves.turboC1 = stats.moves.turboC2 = -1;
    const std::array lenders{&fixture.weapons.textures};
    REQUIRE(fixture.resources.bind(fixture.device, 0, stats, *fixture.party.front().figure,
                                   fixture.weapons, lenders));
    CHECK(fixture.resources.status(static_cast<s32>(first)) ==
          TurboEffectResources::Status::MissingResource);
    REQUIRE(fixture.resources.status(second) == TurboEffectResources::Status::Ready);
    REQUIRE(fixture.movement.setPlayer(1, *fixture.party.front().figure->actionTree(), &stats, 0));
    InputCommand command;
    command.epoch = fixture.context.epoch;
    command.tick = fixture.history.front().motion.tick + 1;
    command.grant = 7;
    command.seat = 1;
    command.pressedButtons = static_cast<u32>(CommandPress::TurboAttack);
    const auto& authority = fixture.history.front();
    fixture.movement.update(&authority, std::span{&command, 1});
    const auto* replay = fixture.movement.replayed(1);
    REQUIRE(replay);
    REQUIRE(replay->frames.size() == 1);
    REQUIRE(replay->frames.front().strikes.size() == 2);
    fixture.prediction.update(&authority, fixture.movement);
    CHECK(fixture.prediction.simulation(1) == nullptr);
    auto shown = authority;
    fixture.movement.present(shown, 1);
    CHECK(PlayerAnimator::rollbackTurboAction(shown.players[1]->rollback->action));
}

TEST_CASE("display-only future effect samples cannot seed native turbo replay",
          "[local-turbo-effects][assets]") {
    EffectFixture fixture(0, 40);
    auto mixed = fixture.history.front();
    ProjectileState effect;
    effect.source = ProjectileSource::WorldEffect;
    effect.instance = 1;
    effect.resource = 1;
    effect.effectOrigin = EffectIdentity{mixed.motion.epoch, mixed.motion.tick + 1, 1, 7, 1, 0, 0};
    mixed.projectiles.push_back(effect);
    REQUIRE(mixed.valid()); // Drawing permits mixed-age local presentation.
    CHECK_FALSE(CombatPacket::encode(mixed));
    fixture.prediction.update(&mixed, fixture.movement);
    CHECK(fixture.prediction.simulation(1) == nullptr);
    CHECK_FALSE(fixture.prediction.present(mixed));
}
} // namespace
