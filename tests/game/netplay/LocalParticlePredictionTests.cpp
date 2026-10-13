#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/screens/LocalProjectilePrediction.h"
#include "game/world/PlayerArsenal.h"
#include "game/world/WeaponGlow.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
constexpr f32 kStep = 1.0f / 60;

WorldCollision particleArena(bool wall) {
    std::vector<CollisionTriangle> triangles{
        {{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{100, 0, -100}, Vec3{100, 0, 100}}, 0},
        {{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{100, 0, 100}, Vec3{-100, 0, 100}}, 0}};
    if (wall) {
        triangles.push_back({{0, 0, -1}, {Vec3{-100, 0, 7}, Vec3{100, 20, 7}, Vec3{100, 0, 7}}, 1});
        triangles.push_back(
            {{0, 0, -1}, {Vec3{-100, 0, 7}, Vec3{-100, 20, 7}, Vec3{100, 20, 7}}, 1});
    }
    WorldCollision result;
    result.build(triangles);
    return result;
}

struct LocalParticles {
    test::FakeRenderDevice device;
    ItemArchive weapons;
    ClassDataSet classes;
    WorldCollision collision;
    EffectTrees effects;
    LevelSoundscape audio;
    PlayerActor actor;
    std::unique_ptr<PlayerFigure> figure;
    ProjectileResources resources;
    PlayerArsenal native;
    EnemyMissiles enemies;
    MatchContext context;
    CombatSnapshot authority;
    LocalMovementPrediction movement;
    LocalProjectilePrediction projectiles;
    ReplicaProjectiles renderer;
    std::optional<ShotIdentity> origin;
    u64 tick = 10;

    LocalParticles(s32 character, u32 element, bool wall = false) : collision(particleArena(wall)) {
        const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
        REQUIRE(weapons.load(root / "WEAPONS"));
        REQUIRE(classes.load(root / "pdata"));
        CharacterSave save;
        save.character = character;
        save.color = character % kColorCount;
        save.progress().experience = levelExperience(80);
        save.progress().inventory.addPowerup(powerup::kWeapon, element, 0, 60);
        actor.spawn(1, save, classes.stats(character), Vec3{0}, 0);
        figure = PlayerFigure::load(device, root, save, false);
        REQUIRE(figure);
        REQUIRE(figure->actionTree());
        const std::array<TextureSet*, 1> lenders{&weapons.textures};
        const std::array<PlayerFigure*, InputCommand::kSeats> figures{nullptr, figure.get()};
        REQUIRE(resources.addPlayers(device, weapons, figures, lenders));
        effects.setTextureLenders(lenders);
        native.bind({device, classes, weapons, collision, effects, audio, nullptr, {}}, lenders);
        context.epoch = 5;
        context.scene = 1;
        context.owners = {0, 2, 0, 0};
        context.grants = {0, 4, 0, 0};
        authority.motion.epoch = context.epoch;
        authority.motion.tick = tick;
        authority.motion.cameraContinuity = 1;
        authority.motion.players[1] = SeatMotion{4, 1, Vec3{0}, 0};
        auto& player = authority.players[1].emplace();
        player.health = 100;
        player.rollback = figure->animator().checkpoint();
        const auto& cursor = figure->animator().player();
        player.animation = {static_cast<u32>(figure->animator().action()), cursor.sequence(),
                            cursor.generation(), cursor.presentationFrame(), cursor.transition()};
        player.movement.enabled = true;
        player.movement.current = true;
        player.movement.ranged = true;
        player.movement.speed = actor.speed();
        player.movement.radius = actor.radius();
        player.movement.height = actor.height();
        player.movement.followHeight = actor.followPoint().y;
        player.modifiers = PlayerWeaponModifiers{element, PlayerWeaponModifiers::kMaxTicks};
        const auto launch = native.weaponThrow(actor, 0);
        const auto rider = native.weaponRider(actor, *figure);
        REQUIRE(launch);
        REQUIRE(rider);
        const auto tree = figure->missileArchive()->trees.find(figure->missileTree());
        REQUIRE(tree);
        auto& weapon = player.weapon.emplace();
        weapon.character = static_cast<u32>(character);
        weapon.hand = launch->hand;
        weapon.speed = launch->speed;
        weapon.resource = resources.treeId(
            figure->missileArchive(), &figure->missileArchive()->trees.tree(*tree), rider->lenders);
        weapon.rider = resources.treeId(rider->archive, rider->tree, rider->lenders);
        const auto streak = native.weaponStreak(actor);
        weapon.streak = resources.streakId(streak.texture);
        weapon.tint = streak.color;
        weapon.forward = streak.forward;
        REQUIRE(resources.acceptsWeapon(weapon));
        REQUIRE(authority.valid());
        movement.begin(context, 2, collision);
        REQUIRE(movement.setPlayer(1, *figure->actionTree()));
        projectiles.begin(context, 2, collision);
        REQUIRE(renderer.begin(context.epoch));
        movement.update(&authority, {});
        projectiles.update(&authority, movement, resources);
    }

    InputCommand command(bool attack) const {
        InputCommand input;
        input.epoch = context.epoch;
        input.tick = tick;
        input.seat = 1;
        input.grant = 4;
        input.heldButtons = attack ? static_cast<u32>(CommandHeld::Attack) : 0;
        return input;
    }

    CombatSnapshot shown() {
        auto result = authority;
        movement.present(result, 1);
        projectiles.present(result, 1);
        REQUIRE(result.valid());
        return result;
    }

    void step(bool attack = false, bool holdProjectiles = false) {
        ++tick;
        const std::array inputs{command(attack)};
        const auto inventory = actor.save().progress().inventory;
        const auto health = actor.save().health();
        const auto animator = figure->animator().checkpoint();
        const auto missileCount = native.missiles().count();
        const auto effectsCount = native.missiles().visuals().count();
        movement.update(&authority, inputs);
        projectiles.update(&authority, movement, resources, inputs);
        CHECK(actor.save().progress().inventory == inventory);
        CHECK(actor.save().health() == health);
        CHECK(figure->animator().checkpoint() == animator);
        CHECK(native.missiles().count() == missileCount);
        CHECK(native.missiles().visuals().count() == effectsCount);
        // Advance the independent native figure only after proving that replay left it
        // untouched. The same stationary input feeds its public animation path.
        figure->animate(0, 1, kStep, attack ? PlayerDeed::Attack : PlayerDeed::None);
        const auto* replay = movement.replayed(1);
        if (!origin && replay != nullptr && !replay->releases.empty()) {
            const auto& release = replay->releases.front();
            REQUIRE(release.tick == tick);
            origin = ShotIdentity{
                context.epoch, release.tick, release.generation, 4, 1, release.strong, 0};
            // Only this explicit oracle call runs the live arsenal. The local replay above
            // cannot modify the live character, emitters, inventory, or missile pool.
            REQUIRE(figure->animator().released());
            CHECK(figure->animator().player().generation() == release.generation);
            CHECK(figure->animator().attackSeconds() == release.attackSeconds);
            native.launchWeapon(actor, figure.get(), actor.facing(), 1, false);
            REQUIRE(native.missiles().count() == 1);
        }
        if (!holdProjectiles) {
            native.missiles().update(kStep, &collision);
        }
    }

    void fire() {
        for (u32 i = 0; i < LocalMovementPrediction::kStaleTicks && !origin; ++i) {
            step(true);
        }
        REQUIRE(origin);
    }

    CombatSnapshot confirmed() {
        auto result = shown();
        result.motion.tick = tick;
        REQUIRE(ProjectileCapture::append(result, resources, native.missiles(), enemies));
        REQUIRE(origin);
        for (auto& shot : result.projectiles) {
            if (shot.particles) {
                shot.particles->origin = *origin;
            }
            if (native.missiles().count() != 0) {
                shot.rollback = ProjectileState::Rollback{
                    *origin, static_cast<u32>(actor.save().character),
                    PlayerMissiles::flightCheckpoint(native.missiles().missile(0))};
            }
        }
        return result;
    }

    void accept(const CombatSnapshot& checkpoint) {
        const auto bytes = CombatPacket::encode(checkpoint);
        REQUIRE(bytes);
        const auto decoded = CombatPacket::decode(*bytes);
        REQUIRE(decoded);
        authority = *decoded;
        movement.update(&authority, {});
        projectiles.update(&authority, movement, resources);
    }

    usize inspect(const CombatSnapshot& view) {
        REQUIRE(origin);
        const auto textures = device.texturesCreated;
        REQUIRE(renderer.show(view, resources));
        device.draws.clear();
        renderer.draw(device, resources, Mat4{1}, {}, CameraFrame::at({10, 20, 30}));
        std::set<std::pair<ProjectileSource, ShotIdentity>> components;
        usize particleCount = 0;
        for (const auto& shot : view.projectiles) {
            const auto* identity = shot.releaseOrigin();
            REQUIRE(identity != nullptr);
            CHECK(*identity == *origin);
            CHECK(components.emplace(shot.source, *identity).second);
            if (shot.source == ProjectileSource::PlayerEffect &&
                WeaponGlow::throwsEffectAlone(actor.save().character)) {
                FAIL("an elemental-only class must not regain its ordinary weapon body");
            }
            if (resources.hasParticles(shot.resource)) {
                REQUIRE(shot.particles);
                REQUIRE(shot.particles->origin);
                CHECK(*shot.particles->origin == *origin);
                CHECK(shot.particles->character == static_cast<u32>(actor.save().character));
                const auto* particles = renderer.particles(shot);
                REQUIRE(particles != nullptr);
                particleCount += particles->field().particleCount();
                const auto& visuals = native.missiles().visuals();
                const EffectTrees::Effect* expected = nullptr;
                for (usize index = 0; index < visuals.count(); ++index) {
                    if (resources.effectId(visuals.effect(index)) == shot.resource) {
                        expected = &visuals.effect(index);
                        break;
                    }
                }
                REQUIRE(expected != nullptr);
                const auto* history = native.missiles().visualHistory(expected->id);
                REQUIRE(history != nullptr);
                CHECK(shot.particles->flight == history->flight);
                const auto& left = particles->field();
                const auto& right = expected->particles.field();
                REQUIRE(left.size() == right.size());
                for (usize emitter = 0; emitter < right.size(); ++emitter) {
                    const auto& a = left.emitter(emitter);
                    const auto& b = right.emitter(emitter);
                    REQUIRE(a.particles().size() == b.particles().size());
                    for (usize index = 0; index < b.particles().size(); ++index) {
                        CHECK(glm::distance(a.positionOf(a.particles()[index]),
                                            b.positionOf(b.particles()[index])) < 0.00001f);
                    }
                }
                if (shot.particles->flight.retiring) {
                    CHECK_FALSE(shot.rollback);
                    CHECK(shot.particles->flight.activeSteps < shot.particles->flight.totalSteps);
                } else {
                    REQUIRE(shot.rollback);
                    CHECK(shot.rollback->origin == *shot.particles->origin);
                }
            }
        }
        CHECK(device.texturesCreated == textures);
        if (particleCount > 0) {
            CHECK_FALSE(device.draws.empty());
        }
        return particleCount;
    }
};

TEST_CASE("local elemental particles begin before host receipt and reconcile native identity",
          "[netplay][local-particles][assets]") {
    const s32 character = GENERATE(0, 2, 6);
    const u32 element = GENERATE(1U, 2U, 3U, 4U);
    CAPTURE(character, element);
    LocalParticles f(character, element);
    f.fire();
    for (u32 i = 0; i < 6; ++i) {
        f.step();
    }
    const auto pending = f.shown();
    REQUIRE_FALSE(pending.projectiles.empty());
    CHECK(f.inspect(pending) > 0);
    for (const auto& shot : pending.projectiles) {
        CHECK(shot.instance >= PredictedShotLedger::kPrivateInstance);
    }
    const auto host = f.confirmed();
    f.accept(host);
    const auto accepted = f.shown();
    CHECK(f.inspect(accepted) > 0);
    for (const auto& shot : accepted.projectiles) {
        CHECK(shot.instance < PredictedShotLedger::kPrivateInstance);
    }
    CHECK(accepted.projectiles.size() == host.projectiles.size());

    // Delayed display of a pre-confirmation roster must still substitute exactly one copy.
    auto lagged = pending;
    f.projectiles.present(lagged, 1);
    CHECK(lagged.projectiles.size() == accepted.projectiles.size());
    for (const auto& shot : lagged.projectiles) {
        CHECK(shot.instance < PredictedShotLedger::kPrivateInstance);
    }
    for (u32 i = 0; i < 4; ++i) {
        f.step();
        f.inspect(f.shown());
    }
}

TEST_CASE("locally predicted amulet impacts retain native tails but never speculative hits",
          "[netplay][local-particles][assets]") {
    const u32 element = GENERATE(1U, 2U, 3U, 4U);
    CAPTURE(element);
    LocalParticles f(0, element, true);
    f.fire();
    bool sawTail = false;
    for (u32 i = 0; i < 50; ++i) {
        f.step();
        const auto shown = f.shown();
        f.inspect(shown);
        if (f.native.missiles().count() == 0 && !shown.projectiles.empty()) {
            sawTail = true;
            for (const auto& shot : shown.projectiles) {
                REQUIRE(shot.particles);
                CHECK(shot.particles->flight.retiring);
                CHECK_FALSE(shot.rollback);
            }
            // Authoritative retirement preserves the same release identity after physical
            // removal and must not resurrect the old delayed in-flight component.
            const auto host = f.confirmed();
            f.accept(host);
            f.inspect(f.shown());
            break;
        }
    }
    CHECK(sawTail);
    const auto impacts = f.native.missiles().takeImpacts();
    REQUIRE(impacts.size() == 1); // emitted solely by the explicit native oracle pool
    CHECK(impacts.front().worldObject == 1);
}

TEST_CASE("rejected particle predictions disappear without replay or epoch resurrection",
          "[netplay][local-particles][assets]") {
    LocalParticles f(2, 4);
    f.fire();
    for (u32 i = 0; i < 6; ++i) {
        f.step();
    }
    const auto pending = f.shown();
    REQUIRE_FALSE(pending.projectiles.empty());
    CHECK(f.inspect(pending) > 0);
    auto rejected = f.authority;
    rejected.motion.tick = f.tick;
    f.accept(rejected);
    CHECK(f.shown().projectiles.empty());
    auto delayed = pending;
    f.projectiles.present(delayed, 1);
    CHECK(delayed.projectiles.empty());
    REQUIRE(f.renderer.show(f.shown(), f.resources));
    for (const auto& shot : pending.projectiles) {
        CHECK(f.renderer.particles(shot) == nullptr);
    }
    f.projectiles.update(&pending, f.movement, f.resources); // invalid older mixed-age receipt
    CHECK(f.shown().projectiles.empty());
    f.projectiles.clear();
    ++f.context.epoch;
    f.projectiles.begin(f.context, 2, f.collision);
    f.projectiles.update(&rejected, f.movement, f.resources);
    CHECK(f.projectiles.count() == 0);
}

TEST_CASE("confirmed live and retiring particles hold their complete clocks until resumed",
          "[netplay][local-particles][assets]") {
    const bool retiring = GENERATE(false, true);
    CAPTURE(retiring);
    LocalParticles f(0, 1, retiring);
    f.fire();
    for (u32 i = 0; i < 50; ++i) {
        f.step();
        if ((!retiring && i >= 2) || (retiring && f.native.missiles().count() == 0)) {
            break;
        }
    }
    REQUIRE((f.native.missiles().count() == 0) == retiring);
    auto held = f.confirmed();
    REQUIRE_FALSE(held.projectiles.empty());
    for (auto& shot : held.projectiles) {
        if (shot.rollback) {
            shot.rollback->held = true;
        }
        if (shot.particles) {
            shot.particles->held = true;
            CHECK(shot.particles->flight.retiring == retiring);
        }
    }
    f.accept(held);
    const auto frozen = f.shown();
    REQUIRE(f.inspect(frozen) > 0);
    for (u32 i = 0; i < 10; ++i) {
        f.step(false, true);
        const auto shown = f.shown();
        REQUIRE(shown.projectiles.size() == frozen.projectiles.size());
        for (usize component = 0; component < frozen.projectiles.size(); ++component) {
            const auto& before = frozen.projectiles[component];
            const auto& after = shown.projectiles[component];
            CHECK(after.key() == before.key());
            CHECK(after.placement == before.placement);
            CHECK(after.direction == before.direction);
            CHECK(after.age == before.age);
            CHECK(after.textureFrame == before.textureFrame);
            if (before.rollback) {
                REQUIRE(after.rollback);
                CHECK(after.rollback->held);
                CHECK(after.rollback->flight == before.rollback->flight);
            }
            if (before.particles) {
                REQUIRE(after.particles);
                CHECK(after.particles->held);
                CHECK(after.particles->flight == before.particles->flight);
            }
        }
        f.inspect(shown);
    }
    // The new authoritative packet lifts the hold; elapsed local inputs must not be
    // retroactively charged to the projectile or its already-retiring emitter.
    auto resumed = f.confirmed();
    for (auto& shot : resumed.projectiles) {
        if (shot.rollback) {
            shot.rollback->held = false;
        }
        if (shot.particles) {
            shot.particles->held = false;
        }
    }
    f.accept(resumed);
    f.step();
    f.step();
    const auto advanced = f.shown();
    bool progressed = false;
    for (const auto& shot : advanced.projectiles) {
        if (!shot.particles) {
            continue;
        }
        const auto previous = std::ranges::find_if(
            frozen.projectiles, [&](const auto& before) { return before.key() == shot.key(); });
        REQUIRE(previous != frozen.projectiles.end());
        REQUIRE(previous->particles);
        CHECK_FALSE(shot.particles->held);
        CHECK(shot.particles->flight.totalSteps == previous->particles->flight.totalSteps + 2);
        CHECK(shot.particles->flight.activeSteps ==
              previous->particles->flight.activeSteps + (retiring ? 0U : 2U));
        CHECK(shot.textureFrame == Approx(previous->textureFrame + 1));
        progressed = true;
    }
    CHECK(progressed);
    f.inspect(advanced);
}
} // namespace
