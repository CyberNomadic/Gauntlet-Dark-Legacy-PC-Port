#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/players/PowerupEffects.h"
#include "game/screens/LocalProjectilePrediction.h"
#include "game/world/WeaponGlow.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TreeInfo actionTree() {
    TreeInfo tree;
    tree.name = "WAR";
    TreeNodeInfo node;
    node.name = "ROOT";
    node.type = 1;
    tree.nodes.push_back(node);
    for (const auto name : PlayerAnimator::kSequenceNames) {
        TreeSequenceInfo sequence;
        sequence.name = name;
        sequence.frames = 4;
        sequence.frameRate = 30;
        sequence.trackOfNode = {-1};
        tree.sequences.push_back(sequence);
    }
    return tree;
}

std::filesystem::path missileAsset() {
    const auto root = test::scratchDirectory("local-projectile-prediction");
    writeTextFile(root / "tri.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    writeTextFile(root / "objects.json", R"({"objects":[{"name":"TRI","file":"tri.obj"}]})");
    writeFile(root / "white.png", test::kTinyPng);
    writeTextFile(root / "textures.json",
                  R"({"bitmaps":[{"name":"WHITE","file":"white.png","width":2,"height":2}]})");
    writeTextFile(root / "animations.json", R"({"trees":[
      {"name":"WEAPON","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}],
       "sequences":[{"name":"ACTIVE","frames":4,"frameRate":30,
       "tracks":[{"node":0,"flags":32,"frames":[0,3],"values":[0,3]}]}]}]})");
    test::convertModelFixture(root);
    return root;
}

WorldCollision ground(bool wall, f32 wallAt = 4.5f) {
    std::vector<CollisionTriangle> triangles{
        {{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{100, 0, -100}, Vec3{100, 0, 100}}, 0},
        {{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{100, 0, 100}, Vec3{-100, 0, 100}}, 0}};
    if (wall) {
        triangles.push_back(
            {{0, 0, -1}, {Vec3{-100, 0, wallAt}, Vec3{100, 10, wallAt}, Vec3{100, 0, wallAt}}, 1});
        triangles.push_back({{0, 0, -1},
                             {Vec3{-100, 0, wallAt}, Vec3{-100, 10, wallAt}, Vec3{100, 10, wallAt}},
                             1});
    }
    WorldCollision collision;
    collision.build(triangles);
    return collision;
}

struct Prediction {
    test::FakeRenderDevice device;
    ItemArchive archive;
    ItemArchive rider;
    TreeInfo tree = actionTree();
    WorldCollision collision;
    ProjectileResources resources;
    MatchContext context;
    CombatSnapshot authority;
    LocalMovementPrediction movement;
    LocalProjectilePrediction projectiles;
    u64 endTick = 10;

    explicit Prediction(bool wall = false) : collision(ground(wall)) {
        REQUIRE(archive.load(missileAsset()));
        REQUIRE(resources.addTree(2, archive, "WEAPON", device));
        REQUIRE(resources.addStreak(3, device.whiteTexture()));
        context.epoch = 3;
        context.scene = 1;
        context.owners = {1, 2, 2, 0};
        context.grants = {1, 4, 2, 0};
        initialize();
    }
    void initialize() {
        endTick = 10;
        authority = {};
        authority.motion.epoch = context.epoch;
        authority.motion.tick = endTick;
        authority.motion.cameraContinuity = 1;
        PlayerAnimator animator;
        REQUIRE(animator.bind(tree, false));
        for (usize seat = 0; seat < 3; ++seat) {
            authority.motion.players[seat] = SeatMotion{context.grants[seat], 1, Vec3{0}, 0};
            auto& player = authority.players[seat].emplace();
            player.health = 100;
            player.rollback = animator.checkpoint();
            const auto& cursor = animator.player();
            player.animation = {static_cast<u32>(animator.action()), cursor.sequence(),
                                cursor.generation(), cursor.presentationFrame(),
                                cursor.transition()};
            player.movement.enabled = true;
            player.movement.current = true;
            player.movement.ranged = true;
            auto& weapon = player.weapon.emplace();
            weapon.hand = {0, 1, 0};
            weapon.resource = 2;
            weapon.streak = 3;
        }
        REQUIRE(authority.valid());
        movement.begin(context, 2, collision);
        REQUIRE(movement.setPlayer(1, tree));
        REQUIRE(movement.setPlayer(2, tree));
        projectiles.begin(context, 2, collision);
        movement.update(&authority, {});
        projectiles.update(&authority, movement, resources);
    }
    InputCommand input(u64 tick, u8 seat, bool attack, bool strong = false) const {
        InputCommand command;
        command.epoch = context.epoch;
        command.tick = tick;
        command.grant = context.grants[seat];
        command.seat = seat;
        if (attack) {
            command.heldButtons =
                static_cast<u32>(strong ? CommandHeld::StrongAttack : CommandHeld::Attack);
        }
        return command;
    }
    void step(bool attack = false, bool strong = false, bool twoSeats = false) {
        ++endTick;
        const std::array commands{input(endTick, 1, attack, strong),
                                  input(endTick, 2, attack, strong)};
        const auto sampled = std::span{commands}.first(twoSeats ? 2 : 1);
        movement.update(&authority, sampled);
        projectiles.update(&authority, movement, resources, sampled);
    }
    LocalMovementPrediction::Release fire(bool strong = false, bool twoSeats = false) {
        for (u64 i = 0; i < LocalMovementPrediction::kStaleTicks; ++i) {
            step(true, strong, twoSeats);
            const auto* replay = movement.replayed(1);
            if (replay != nullptr && !replay->releases.empty()) {
                return replay->releases.front();
            }
        }
        FAIL("native animator did not release within the prediction freshness window");
        return {};
    }
    CombatSnapshot shown(f32 blend = 1) {
        auto state = authority;
        movement.present(state, blend);
        projectiles.present(state, blend);
        REQUIRE(state.valid());
        return state;
    }
    void accept(const CombatSnapshot& state) {
        const auto bytes = CombatPacket::encode(state);
        REQUIRE(bytes);
        const auto decoded = CombatPacket::decode(*bytes);
        REQUIRE(decoded);
        authority = *decoded;
        movement.update(&authority, {});
        projectiles.update(&authority, movement, resources);
    }
    void modifiers(u32 first, u32 second = 0) {
        auto host = authority;
        host.motion.tick = ++endTick;
        const std::array flags{first, second};
        for (usize index = 0; index < flags.size(); ++index) {
            auto& player = *host.players[index + 1];
            PowerupEffects effects;
            effects.weapon = flags[index];
            player.weapon->shots = static_cast<u8>(effects.shots());
            if (flags[index] != 0) {
                player.modifiers =
                    PlayerWeaponModifiers{flags[index], PlayerWeaponModifiers::kMaxTicks};
            } else {
                player.modifiers.reset();
            }
        }
        accept(host);
    }
    void elemental(u32 flags, u32 character = 0) {
        REQUIRE(rider.load(missileAsset()));
        REQUIRE(resources.addTree(4, rider, "WEAPON", device));
        auto host = authority;
        host.motion.tick = ++endTick;
        auto& player = *host.players[1];
        player.modifiers = PlayerWeaponModifiers{flags, PlayerWeaponModifiers::kMaxTicks};
        PowerupEffects effects;
        effects.weapon = flags;
        player.weapon->shots = static_cast<u8>(effects.shots());
        player.weapon->character = character;
        player.weapon->rider = 4;
        accept(host);
    }
    CombatSnapshot confirmation() {
        auto state = shown();
        state.motion.tick = endTick;
        u64 instance = 21;
        for (auto& component : state.projectiles) {
            component.instance = instance++;
        }
        return state;
    }
    usize draw(const CombatSnapshot& state) {
        ReplicaProjectiles renderer;
        REQUIRE(renderer.begin(context.epoch));
        REQUIRE(renderer.show(state, resources));
        device.draws.clear();
        renderer.draw(device, resources, Mat4{1}, {}, CameraFrame{});
        return device.draws.size();
    }
};

std::vector<MissileLaunch> nativeLaunches(const Prediction& fixture,
                                          const LocalMovementPrediction::Release& release,
                                          u8 seat = 1) {
    const auto& player = *fixture.authority.players[seat];
    const auto& weapon = *player.weapon;
    PlayerMissiles::WeaponThrow request;
    request.owner = seat;
    request.character = static_cast<s32>(weapon.character);
    request.position = release.motion.position;
    request.center = request.position + Vec3{0, player.movement.followHeight, 0};
    request.hand = weapon.hand;
    request.direction = {std::sin(release.motion.yaw), 0, std::cos(release.motion.yaw)};
    request.speed = weapon.speed;
    request.scale = release.strong ? 2.0f : 1.0f;
    request.attackSeconds = release.attackSeconds;
    request.target = release.cursorAiming ? std::nullopt : weapon.target;
    request.shots = release.shots;
    request.flags = player.modifiers ? player.modifiers->flags : 0;
    return PlayerMissiles::weaponLaunches(request);
}

void checkFan(const CombatSnapshot& shown, const PlayerMissiles& native,
              const LocalMovementPrediction::Release& release, u8 seat = 1) {
    REQUIRE(std::ranges::count_if(shown.projectiles, [seat](const auto& component) {
                return component.rollback && component.rollback->origin.seat == seat;
            }) == static_cast<s64>(native.count() * 2));
    std::array<usize, ShotIdentity::kMaxShots> components{};
    for (const auto& component : shown.projectiles) {
        REQUIRE(component.rollback);
        const auto& origin = component.rollback->origin;
        if (origin.seat != seat) {
            continue;
        }
        CHECK(origin.tick == release.tick);
        CHECK(origin.generation == release.generation);
        CHECK(origin.strong == release.strong);
        REQUIRE(origin.ordinal < release.shots);
        ++components[origin.ordinal];
        const PlayerMissiles::Missile* expected = nullptr;
        for (usize i = 0; i < native.count(); ++i) {
            if (native.missile(i).instance == static_cast<u64>(origin.ordinal) + 1) {
                expected = &native.missile(i);
                break;
            }
        }
        REQUIRE(expected != nullptr);
        CHECK(component.rollback->flight.position == expected->position);
        CHECK(component.rollback->flight.velocity == expected->velocity);
        CHECK(component.rollback->flight.age == expected->age);
        CHECK(component.rollback->flight.scale == expected->scale);
        CHECK(component.placement == PlayerMissiles::transformOf(*expected));
    }
    for (usize i = 0; i < native.count(); ++i) {
        REQUIRE(native.missile(i).instance <= components.size());
        CHECK(components[native.missile(i).instance - 1] == 2);
    }
}

TEST_CASE("native ordinary and strong release produces a renderable shot before host execution",
          "[netplay][projectile-prediction]") {
    const bool strong = GENERATE(false, true);
    Prediction fixture;
    const auto release = fixture.fire(strong);
    REQUIRE(release.strong == strong);
    const auto shown = fixture.shown();
    REQUIRE(shown.projectiles.size() == 2);
    CHECK(fixture.authority.projectiles.empty());
    CHECK(fixture.authority.players[1]->health == 100);
    PlayerMissiles::WeaponThrow request;
    request.owner = 1;
    request.center = {0, fixture.authority.players[1]->movement.followHeight, 0};
    request.hand = fixture.authority.players[1]->weapon->hand;
    request.scale = strong ? 2.0f : 1.0f;
    request.attackSeconds = release.attackSeconds;
    PlayerMissiles native;
    REQUIRE(native.launch(PlayerMissiles::weaponLaunches(request).front()));
    native.update(1.0f / 60, &fixture.collision); // release tick is already a flight tick
    REQUIRE(native.count() == 1);
    for (const auto& component : shown.projectiles) {
        REQUIRE(component.rollback);
        const auto& origin = component.rollback->origin;
        CHECK(origin.tick == release.tick);
        CHECK(origin.generation == release.generation);
        CHECK(origin.strong == strong);
        CHECK(origin.seat == 1);
        CHECK(origin.grant == fixture.context.grants[1]);
        CHECK((component.instance & PredictedShotLedger::kPrivateInstance) != 0);
        CHECK(component.rollback->flight.position == native.missile(0).position);
        CHECK(component.rollback->flight.velocity == native.missile(0).velocity);
        CHECK(component.rollback->flight.age == Approx(1.0f / 60));
        CHECK(component.rollback->flight.scale == (strong ? 2 : 1));
    }
    CHECK(shown.projectiles[0].source == ProjectileSource::PlayerEffect);
    CHECK(shown.projectiles[1].source == ProjectileSource::Streak);
    CHECK(fixture.draw(shown) == 2);
}

TEST_CASE("elemental fans replay native flight with separate authored body and rider components",
          "[netplay][projectile-prediction][elemental-prediction]") {
    const u32 element = GENERATE(1U, 2U, 3U, 4U);
    const u32 character = GENERATE(0U, 2U, 6U, 10U, 14U, 16U);
    const bool strong = GENERATE(false, true);
    const bool spread = GENERATE(false, true);
    const u32 flags = element | (spread ? powerup::kRapidFire | powerup::kFiveWayShot : 0);
    Prediction fixture;
    fixture.elemental(flags, character);
    auto clocked = fixture.authority;
    clocked.motion.tick = ++fixture.endTick;
    clocked.players[1]->weapon->textureFrame = 17.25f;
    clocked.players[1]->weapon->riderTextureFrame = 43.5f;
    fixture.accept(clocked);
    REQUIRE(fixture.resources.acceptsWeapon(*fixture.authority.players[1]->weapon));
    const auto release = fixture.fire(strong);
    REQUIRE(release.weaponFlags == flags);
    REQUIRE(fixture.projectiles.soundReleases().size() == 1);
    CHECK(fixture.projectiles.soundReleases().front().weaponFlags == flags);
    const bool effectOnly = WeaponGlow::throwsEffectAlone(static_cast<s32>(character));
    const usize parts = effectOnly ? 2 : 3;
    const auto textures = fixture.device.texturesCreated;
    PlayerMissiles native;
    for (const auto& launch : nativeLaunches(fixture, release)) {
        REQUIRE(native.launch(launch));
    }
    for (u64 tick = 0; tick < 3; ++tick) {
        native.update(1.0f / 60, &fixture.collision);
        const auto shown = fixture.shown();
        REQUIRE(shown.projectiles.size() == parts * release.shots);
        std::array<usize, 3> counts{};
        for (const auto& component : shown.projectiles) {
            REQUIRE(component.rollback);
            const auto ordinal = component.rollback->origin.ordinal;
            REQUIRE(ordinal < native.count());
            const auto& missile = native.missile(ordinal);
            CHECK(component.rollback->flight == PlayerMissiles::flightCheckpoint(missile));
            CHECK(component.placement == PlayerMissiles::transformOf(missile));
            const f32 clockStep =
                static_cast<f32>(fixture.endTick - fixture.authority.motion.tick) * 0.5f;
            if (component.source == ProjectileSource::PlayerRider) {
                ++counts[0];
                CHECK(component.resource == 4);
                CHECK(component.textureFrame == Approx(43.5f + clockStep));
            } else if (component.source == ProjectileSource::PlayerEffect) {
                ++counts[1];
                CHECK_FALSE(effectOnly);
                CHECK(component.resource == 2);
                CHECK(component.textureFrame == Approx(17.25f + clockStep));
            } else {
                ++counts[2];
                CHECK(component.source == ProjectileSource::Streak);
            }
        }
        CHECK(counts[0] == release.shots);
        CHECK(counts[1] == (effectOnly ? 0 : release.shots));
        CHECK(counts[2] == release.shots);
        CHECK(fixture.draw(shown) == parts * release.shots);
        CHECK(fixture.device.texturesCreated == textures);
        fixture.step();
    }
}

TEST_CASE("confirmed elemental riders survive modifier expiry and native flight correction",
          "[netplay][projectile-prediction][elemental-prediction]") {
    Prediction fixture;
    fixture.elemental(4U | powerup::kFiveWayShot, 2);
    fixture.fire();
    auto host = fixture.confirmation();
    REQUIRE(host.projectiles.size() == 10);
    host.players[1]->modifiers.reset();
    host.players[1]->weapon->shots = 1;
    host.players[1]->weapon->rider = 0;
    host.players[1]->weapon->riderTextureFrame.reset();
    for (auto& component : host.projectiles) {
        component.rollback->flight.velocity.x += 0.25f;
        component.direction = component.rollback->flight.velocity;
    }
    fixture.accept(host);
    CHECK(fixture.projectiles.soundReleases().empty());
    fixture.step();
    const auto shown = fixture.shown();
    REQUIRE(shown.projectiles.size() == host.projectiles.size());
    for (const auto& component : shown.projectiles) {
        const auto previous =
            std::ranges::find(host.projectiles, component.key(), &ProjectileState::key);
        REQUIRE(previous != host.projectiles.end());
        PlayerMissiles native;
        MissileLaunch launch;
        launch.spec = &MissileSpec::of(2);
        REQUIRE(native.launch(launch));
        REQUIRE(native.restoreFlight(native.missile(0).instance, previous->rollback->flight));
        native.update(1.0f / 60, &fixture.collision);
        REQUIRE(native.count() == 1);
        CHECK(component.rollback->flight == PlayerMissiles::flightCheckpoint(native.missile(0)));
        if (component.source == ProjectileSource::PlayerRider) {
            CHECK(component.textureFrame == Approx(previous->textureFrame + 0.5f));
        } else {
            CHECK(component.source == ProjectileSource::Streak);
        }
    }
    CHECK(fixture.projectiles.soundReleases().empty());
}

TEST_CASE("unknown elemental resources never substitute a plain predicted weapon",
          "[netplay][projectile-prediction][elemental-prediction]") {
    Prediction fixture;
    fixture.elemental(1);
    auto host = fixture.authority;
    host.motion.tick = ++fixture.endTick;
    host.players[1]->weapon->rider = 65535;
    fixture.accept(host);
    CHECK_FALSE(fixture.resources.acceptsWeapon(*host.players[1]->weapon));
    fixture.fire();
    CHECK(fixture.projectiles.count() == 0);
    CHECK(fixture.projectiles.soundReleases().empty());
    CHECK(fixture.shown().projectiles.empty());
}

TEST_CASE("replayed weapon sounds observe one canonical origin per successful native fan",
          "[netplay][projectile-prediction][weapon-sound]") {
    const auto flags =
        GENERATE(0U, powerup::kThreeWayShot, powerup::kRapidFire | powerup::kFiveWayShot);
    const bool strong = GENERATE(false, true);
    Prediction fixture;
    fixture.modifiers(flags);
    CHECK(fixture.projectiles.soundReleases().empty());
    const auto release = fixture.fire(strong);
    const auto sounds = fixture.projectiles.soundReleases();
    REQUIRE(sounds.size() == 1);
    const auto observed = sounds.front();
    CHECK(observed.origin == ShotIdentity{fixture.context.epoch, release.tick, release.generation,
                                          fixture.context.grants[1], 1, strong, 0});
    CHECK(observed.shots == release.shots);
    CHECK(fixture.shown().projectiles.size() == static_cast<usize>(release.shots) * 2);
    // Rewind/replay repeats the same observation. Audio consumers own deduplication.
    fixture.projectiles.update(&fixture.authority, fixture.movement, fixture.resources);
    REQUIRE(fixture.projectiles.soundReleases().size() == 1);
    CHECK(fixture.projectiles.soundReleases().front().origin == observed.origin);
    CHECK(fixture.projectiles.soundReleases().front().shots == observed.shots);
    fixture.accept(fixture.confirmation());
    CHECK(fixture.projectiles.soundReleases().empty());
    CHECK_FALSE(fixture.shown().projectiles.empty());
    fixture.projectiles.clear();
    CHECK(fixture.projectiles.soundReleases().empty());
}

TEST_CASE("split local seats retain their own weapon sound fan metadata",
          "[netplay][projectile-prediction][weapon-sound]") {
    Prediction fixture;
    fixture.modifiers(powerup::kThreeWayShot, powerup::kFiveWayShot);
    fixture.fire(false, true);
    REQUIRE(fixture.projectiles.soundReleases().size() == 2);
    const auto first = fixture.projectiles.soundReleases()[0];
    const auto second = fixture.projectiles.soundReleases()[1];
    CHECK(first.origin.seat == 1);
    CHECK(first.origin.ordinal == 0);
    CHECK(first.shots == 3);
    CHECK(second.origin.seat == 2);
    CHECK(second.origin.ordinal == 0);
    CHECK(second.shots == 5);
    CHECK(fixture.shown().projectiles.size() == 16);
}

TEST_CASE("weapon sound selection follows modifier expiry during a latched native attack",
          "[netplay][projectile-prediction][weapon-sound]") {
    const bool strong = GENERATE(false, true);
    Prediction fixture;
    fixture.modifiers(powerup::kRapidFire | powerup::kFiveWayShot);
    auto speedy = fixture.authority;
    speedy.motion.tick = ++fixture.endTick;
    speedy.players[1]->modifiers->speed = true;
    fixture.accept(speedy);
    fixture.step(true, strong);
    REQUIRE(fixture.movement.replayed(1));
    REQUIRE(fixture.movement.replayed(1)->releases.empty());
    auto expired = fixture.shown();
    expired.motion.tick = fixture.endTick;
    expired.players[1]->modifiers.reset();
    expired.players[1]->weapon->shots = 1;
    REQUIRE(expired.players[1]->rollback);
    REQUIRE(expired.players[1]->rollback->rapid);
    REQUIRE(expired.players[1]->rollback->speed);
    PlayerAnimator native;
    REQUIRE(native.bind(fixture.tree, false));
    REQUIRE(native.restore(*expired.players[1]->rollback));
    fixture.accept(expired);
    bool observed = false;
    for (u64 tick = 0; tick < LocalMovementPrediction::kStaleTicks; ++tick) {
        native.setAttackSpeed(false, false);
        native.update(PlayerMotion::Stand, 1, 1.0f / 60,
                      strong ? PlayerDeed::StrongAttack : PlayerDeed::Attack);
        fixture.step(true, strong);
        const auto shown = fixture.shown();
        REQUIRE(shown.players[1]->rollback);
        CHECK(*shown.players[1]->rollback == native.checkpoint());
        if (!native.released() && !native.strongReleased()) {
            CHECK(fixture.projectiles.soundReleases().empty());
            continue;
        }
        REQUIRE(fixture.projectiles.soundReleases().size() == 1);
        const auto& release = fixture.projectiles.soundReleases().front();
        CHECK(release.shots == 1);
        CHECK(release.origin.strong == strong);
        CHECK(release.origin.generation == native.player().generation());
        CHECK(release.origin.tick == fixture.endTick);
        CHECK(shown.projectiles.size() == 2);
        observed = true;
        break;
    }
    CHECK(observed);
}

TEST_CASE("weapon sound observations distinguish blocked muzzles from immediate flight impacts",
          "[netplay][projectile-prediction][weapon-sound][elemental-prediction]") {
    const bool obstructed = GENERATE(false, true);
    const u32 element = GENERATE(0U, 1U, 2U, 3U, 4U);
    const bool strong = GENERATE(false, true);
    Prediction fixture;
    fixture.collision = ground(true, PlayerMissiles::kMuzzle + (obstructed ? 0.5f : 1.1f));
    fixture.initialize();
    if (element != 0) {
        fixture.elemental(element);
    }
    const auto release = fixture.fire(strong);
    const auto launch = nativeLaunches(fixture, release).front();
    const f32 radius = launch.spec->radius;
    const Vec3 cleared =
        fixture.collision.resolveWalls(launch.position, radius, launch.position.y - radius * 0.5f,
                                       launch.position.y + radius * 0.5f);
    CHECK((glm::distance(cleared, launch.position) > 1e-4f) == obstructed);
    PlayerMissiles native;
    REQUIRE(native.launch(launch));
    native.update(1.0f / 60, &fixture.collision);
    REQUIRE(native.count() == 0);
    CHECK(fixture.projectiles.count() == 0);
    CHECK(fixture.shown().projectiles.empty());
    if (obstructed) {
        CHECK(fixture.projectiles.soundReleases().empty());
    } else {
        REQUIRE(fixture.projectiles.soundReleases().size() == 1);
        CHECK(fixture.projectiles.soundReleases().front().origin.tick == release.tick);
        CHECK(fixture.projectiles.soundReleases().front().shots == 1);
        CHECK(fixture.projectiles.soundReleases().front().weaponFlags == element);
        CHECK(fixture.projectiles.soundReleases().front().origin.strong == strong);
    }
    CHECK(fixture.authority.players[1]->health == 100);
}

TEST_CASE("predicted spread members use native ordinary and strong flight with independent bodies",
          "[netplay][projectile-prediction][spread-prediction]") {
    const u32 flag = GENERATE(powerup::kThreeWayShot, powerup::kFiveWayShot);
    const bool strong = GENERATE(false, true);
    const bool rapid = GENERATE(false, true);
    const bool assisted = GENERATE(false, true);
    Prediction fixture;
    fixture.modifiers(flag | (rapid ? powerup::kRapidFire : 0));
    if (assisted) {
        auto host = fixture.authority;
        host.motion.tick = ++fixture.endTick;
        host.players[1]->weapon->target = Vec3{2, 5, 25};
        fixture.accept(host);
    }
    const auto release = fixture.fire(strong);
    REQUIRE(release.shots == (flag == powerup::kFiveWayShot ? 5 : 3));
    PlayerMissiles native;
    for (const auto& launch : nativeLaunches(fixture, release)) {
        REQUIRE(native.launch(launch));
    }
    for (u64 elapsed = 0; elapsed < 5; ++elapsed) {
        native.update(1.0f / 60, &fixture.collision);
        REQUIRE(native.count() == release.shots);
        const auto shown = fixture.shown();
        checkFan(shown, native, release);
        CHECK(fixture.draw(shown) == static_cast<usize>(release.shots) * 2);
        for (const auto& component : shown.projectiles) {
            CHECK(component.textureFrame == Approx(static_cast<f32>(elapsed + 1) * 0.5f));
            CHECK((component.instance & PredictedShotLedger::kPrivateInstance) != 0);
        }
        CHECK(fixture.authority.projectiles.empty());
        CHECK(fixture.authority.players[1]->health == 100);
        fixture.step();
    }
}

TEST_CASE("partial host fan confirmation cannot duplicate or resurrect expired siblings",
          "[netplay][projectile-prediction][spread-prediction]") {
    Prediction fixture;
    fixture.modifiers(powerup::kFiveWayShot);
    const auto release = fixture.fire();
    const auto pending = fixture.shown();
    REQUIRE(pending.projectiles.size() == 10);
    auto host = fixture.confirmation();
    std::erase_if(host.projectiles, [](const auto& component) {
        return (component.rollback->origin.ordinal % 2) != 0;
    });
    fixture.accept(host);
    auto shown = pending;
    fixture.projectiles.present(shown, 1);
    REQUIRE(shown.projectiles.size() == 6);
    for (const auto& component : shown.projectiles) {
        REQUIRE(component.rollback);
        CHECK(component.rollback->origin.tick == release.tick);
        CHECK((component.rollback->origin.ordinal % 2) == 0);
        CHECK(component.instance < PredictedShotLedger::kPrivateInstance);
    }
    CHECK(fixture.draw(shown) == 6);
    const auto older = host;
    ++host.motion.tick;
    std::erase_if(host.projectiles,
                  [](const auto& component) { return component.rollback->origin.ordinal != 4; });
    fixture.accept(host);
    fixture.projectiles.update(&older, fixture.movement, fixture.resources);
    shown = pending;
    fixture.projectiles.present(shown, 1);
    REQUIRE(shown.projectiles.size() == 2);
    CHECK(shown.projectiles.front().rollback->origin.ordinal == 4);
    CHECK(fixture.draw(shown) == 2);
    ++host.motion.tick;
    host.projectiles.clear();
    fixture.accept(host);
    shown = pending;
    fixture.projectiles.present(shown, 1);
    CHECK(shown.projectiles.empty());
    CHECK(fixture.draw(shown) == 0);
}

TEST_CASE("a native wall impact preserves the original ordinal of surviving fan members",
          "[netplay][projectile-prediction][spread-prediction]") {
    Prediction fixture;
    const std::vector<CollisionTriangle> triangles{
        {{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{100, 0, -100}, Vec3{100, 0, 100}}, 0},
        {{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{100, 0, 100}, Vec3{-100, 0, 100}}, 0},
        {{0, 0, -1}, {Vec3{0.4f, 0, 4.5f}, Vec3{100, 10, 4.5f}, Vec3{100, 0, 4.5f}}, 1},
        {{0, 0, -1}, {Vec3{0.4f, 0, 4.5f}, Vec3{0.4f, 10, 4.5f}, Vec3{100, 10, 4.5f}}, 1}};
    fixture.collision.build(triangles);
    fixture.initialize();
    fixture.modifiers(powerup::kFiveWayShot);
    const auto release = fixture.fire();
    PlayerMissiles native;
    for (const auto& launch : nativeLaunches(fixture, release)) {
        REQUIRE(native.launch(launch));
    }
    bool partial = false;
    bool higherOrdinalSurvives = false;
    for (s32 tick = 0; tick < 14; ++tick) {
        native.update(1.0f / 60, &fixture.collision);
        const auto shown = fixture.shown();
        checkFan(shown, native, release);
        CHECK(fixture.draw(shown) == native.count() * 2);
        partial |= native.count() != 0 && native.count() < release.shots;
        for (usize i = 0; i < native.count(); ++i) {
            higherOrdinalSurvives |= native.missile(i).instance > native.count();
        }
        fixture.step();
    }
    CHECK(partial);
    CHECK(higherOrdinalSurvives);
    CHECK_FALSE(native.takeImpacts().empty());
    CHECK(fixture.authority.projectiles.empty());
    CHECK(fixture.authority.players[1]->health == 100);
}

TEST_CASE("local seats may predict different fan sizes without cross-seat reconciliation",
          "[netplay][projectile-prediction][spread-prediction]") {
    Prediction fixture;
    fixture.modifiers(powerup::kThreeWayShot, powerup::kFiveWayShot);
    const auto first = fixture.fire(false, true);
    const auto* secondReplay = fixture.movement.replayed(2);
    REQUIRE(secondReplay != nullptr);
    REQUIRE(secondReplay->releases.size() == 1);
    const auto second = secondReplay->releases.front();
    REQUIRE(first.shots == 3);
    REQUIRE(second.shots == 5);
    PlayerMissiles firstNative;
    for (const auto& launch : nativeLaunches(fixture, first, 1)) {
        REQUIRE(firstNative.launch(launch));
    }
    PlayerMissiles secondNative;
    for (const auto& launch : nativeLaunches(fixture, second, 2)) {
        REQUIRE(secondNative.launch(launch));
    }
    firstNative.update(1.0f / 60, &fixture.collision);
    secondNative.update(1.0f / 60, &fixture.collision);
    const auto shown = fixture.shown();
    REQUIRE(shown.projectiles.size() == 16);
    checkFan(shown, firstNative, first, 1);
    checkFan(shown, secondNative, second, 2);
    CHECK(fixture.draw(shown) == 16);
    auto host = fixture.confirmation();
    std::erase_if(host.projectiles,
                  [](const auto& component) { return component.rollback->origin.seat == 1; });
    fixture.accept(host);
    auto delayed = shown;
    fixture.projectiles.present(delayed, 1);
    REQUIRE(delayed.projectiles.size() == 10);
    CHECK(std::ranges::all_of(delayed.projectiles, [](const auto& component) {
        return component.rollback->origin.seat == 2;
    }));
    CHECK(fixture.authority.players[1]->health == 100);
    CHECK(fixture.authority.players[2]->health == 100);
}

TEST_CASE("native rapid fire keeps distinct release fans on a shared texture clock",
          "[netplay][projectile-prediction][spread-prediction]") {
    Prediction fixture;
    for (auto& sequence : fixture.tree.sequences) {
        sequence.frames = 2;
    }
    fixture.initialize();
    fixture.modifiers(powerup::kRapidFire | powerup::kFiveWayShot);
    const auto first = fixture.fire();
    REQUIRE(fixture.shown().projectiles.size() == 10);
    for (u64 step = 0; step < LocalMovementPrediction::kStaleTicks; ++step) {
        fixture.step(true);
        const auto* replay = fixture.movement.replayed(1);
        REQUIRE(replay != nullptr);
        if (replay->releases.size() < 2) {
            continue;
        }
        const auto& second = replay->releases[1];
        REQUIRE(second.tick > first.tick);
        const auto shown = fixture.shown();
        REQUIRE(shown.projectiles.size() == 20);
        std::array<std::array<usize, ShotIdentity::kMaxShots>, 2> members{};
        for (const auto& component : shown.projectiles) {
            REQUIRE(component.rollback);
            const auto& origin = component.rollback->origin;
            REQUIRE((origin.tick == first.tick || origin.tick == second.tick));
            REQUIRE(origin.ordinal < ShotIdentity::kMaxShots);
            ++members[origin.tick == first.tick ? 0 : 1][origin.ordinal];
            CHECK(component.textureFrame ==
                  Approx(static_cast<f32>(fixture.endTick - first.tick + 1) * 0.5f));
        }
        for (const auto& fan : members) {
            CHECK(std::ranges::all_of(fan, [](usize count) { return count == 2; }));
        }
        CHECK(fixture.draw(shown) == 20);
        return;
    }
    FAIL("native rapid-fire animator did not release a second fan within the freshness window");
}

TEST_CASE("host confirmation replaces predicted body and streak and latest impact wins immediately",
          "[netplay][projectile-prediction]") {
    Prediction fixture;
    fixture.fire();
    const auto pending = fixture.shown();
    auto host = fixture.confirmation();
    fixture.accept(host);
    auto shown = fixture.shown();
    REQUIRE(shown.projectiles.size() == 2);
    CHECK(shown.projectiles[0].instance == 21);
    CHECK(shown.projectiles[1].instance == 22);
    CHECK(fixture.draw(shown) == 2);
    const auto olderHost = host;
    ++host.motion.tick;
    for (auto& component : host.projectiles) {
        component.rollback->flight.position.x += 0.5f;
        component.placement[3].x += 0.5f;
    }
    fixture.accept(host);
    shown = olderHost;
    fixture.projectiles.present(shown, 1);
    REQUIRE(shown.projectiles.size() == 2);
    CHECK(Vec3{shown.projectiles[0].placement[3]}.x == Approx(0.5f));
    CHECK(fixture.draw(shown) == 2);
    ++host.motion.tick;
    host.projectiles.clear();
    fixture.accept(host);
    shown = olderHost;
    fixture.projectiles.present(shown, 1);
    CHECK(shown.projectiles.empty());
    CHECK(fixture.draw(shown) == 0);
    fixture.projectiles.update(&olderHost, fixture.movement, fixture.resources);
    shown = pending;
    fixture.projectiles.present(shown, 1);
    CHECK(shown.projectiles.empty());
}

TEST_CASE("host rejection and stale authority withdraw released shots without side effects",
          "[netplay][projectile-prediction]") {
    const bool stale = GENERATE(false, true);
    Prediction fixture;
    fixture.fire();
    auto shown = fixture.shown();
    REQUIRE(shown.projectiles.size() == 2);
    if (stale) {
        for (u64 i = 0; i <= LocalMovementPrediction::kStaleTicks; ++i) {
            fixture.movement.update(nullptr, {});
            fixture.projectiles.update(nullptr, fixture.movement, fixture.resources);
        }
    } else {
        auto rejected = fixture.authority;
        rejected.motion.tick = fixture.endTick;
        fixture.accept(rejected);
    }
    fixture.projectiles.present(shown, 1);
    CHECK(shown.projectiles.empty());
    CHECK(fixture.projectiles.count() == 0);
    CHECK(fixture.authority.players[1]->health == 100);
    CHECK(fixture.authority.projectiles.empty());
}

TEST_CASE(
    "independent local seats retain disjoint release identities while remote shots stay buffered",
    "[netplay][projectile-prediction]") {
    Prediction fixture;
    fixture.fire(false, true);
    auto shown = fixture.shown();
    REQUIRE(shown.projectiles.size() == 4);
    CHECK(std::ranges::count_if(shown.projectiles, [](const auto& shot) {
              return shot.rollback->origin.seat == 1;
          }) == 2);
    CHECK(std::ranges::count_if(shown.projectiles, [](const auto& shot) {
              return shot.rollback->origin.seat == 2;
          }) == 2);
    auto remote = shown.projectiles.front();
    remote.instance = 1;
    remote.rollback->origin.seat = 0;
    remote.rollback->origin.grant = fixture.context.grants[0];
    shown.projectiles.insert(shown.projectiles.begin(), remote);
    fixture.projectiles.present(shown, 1);
    REQUIRE(shown.projectiles.size() == 5);
    CHECK(shown.projectiles.front().key() == remote.key());
    CHECK(shown.projectiles.front().placement == remote.placement);
    CHECK(fixture.draw(shown) == 5);
    ++fixture.context.epoch;
    ++fixture.context.grants[1];
    fixture.initialize();
    fixture.projectiles.update(nullptr, fixture.movement, fixture.resources);
    CHECK(fixture.shown().projectiles.empty());
    CHECK(fixture.projectiles.count() == 0);
}

TEST_CASE("predicted native flight stops at walls and expires without a speculative explosion",
          "[netplay][projectile-prediction]") {
    SECTION("wall contact") {
        Prediction fixture(true);
        fixture.fire();
        auto shown = fixture.shown();
        REQUIRE(shown.projectiles.size() == 2);
        const auto identity = shown.projectiles.front().rollback->origin;
        for (s32 i = 0; i < 6; ++i) {
            fixture.step();
        }
        shown = fixture.shown();
        CHECK(std::ranges::none_of(shown.projectiles, [&](const auto& shot) {
            return shot.rollback && shot.rollback->origin == identity;
        }));
        CHECK(shown.projectiles.empty());
    }
    SECTION("confirmed native lifetime") {
        Prediction fixture;
        fixture.fire();
        auto host = fixture.confirmation();
        for (auto& component : host.projectiles) {
            component.rollback->flight.age = PlayerMissiles::kLifeSeconds - 1.0f / 120;
            component.rollback->flight.position.y = 100;
            component.rollback->flight.velocity = {0, 0, 20};
        }
        fixture.accept(host);
        fixture.step();
        CHECK(fixture.shown().projectiles.empty());
        CHECK(fixture.projectiles.count() == 0);
        CHECK(fixture.authority.players[1]->health == 100);
    }
}

TEST_CASE("projectile rollback is independent of repeated interpolated render samples",
          "[netplay][projectile-prediction]") {
    const s32 renderRate = GENERATE(30, 60, 144);
    Prediction fixture;
    fixture.fire();
    const auto first = fixture.shown();
    REQUIRE(first.projectiles.size() == 2);
    const auto origin = first.projectiles.front().rollback->origin;
    const auto firstAge = first.projectiles.front().rollback->flight.age;
    for (s32 step = 0; step < 6; ++step) {
        fixture.step();
        const auto resolved = fixture.shown();
        for (s32 frame = 0; frame < renderRate; ++frame) {
            const auto rendered =
                fixture.shown(static_cast<f32>(frame) / static_cast<f32>(renderRate));
            REQUIRE(rendered.projectiles.size() == 2);
            CHECK(rendered.projectiles.front().rollback->origin == origin);
            CHECK(rendered.projectiles.front().rollback->flight.age ==
                  resolved.projectiles.front().rollback->flight.age);
        }
        CHECK(resolved.projectiles.front().rollback->flight.age ==
              Approx(firstAge + static_cast<f32>(step + 1) / 60));
    }
}

TEST_CASE("a newly created weapon texture bank starts on its first predicted release tick",
          "[netplay][projectile-prediction]") {
    Prediction fixture;
    auto host = fixture.authority;
    host.motion.tick = ++fixture.endTick;
    host.worldTextureFrame = 9876;
    fixture.accept(host);
    REQUIRE_FALSE(fixture.authority.players[1]->weapon->textureFrame);
    const auto release = fixture.fire();
    REQUIRE(release.tick > host.motion.tick + 1);
    for (u64 elapsed = 0; elapsed < 4; ++elapsed) {
        const auto shown = fixture.shown();
        REQUIRE(shown.projectiles.size() == 2);
        for (const auto& component : shown.projectiles) {
            CHECK(component.textureFrame == Approx(static_cast<f32>(elapsed + 1) * 0.5f));
            CHECK(component.rollback->origin.tick == release.tick);
        }
        CHECK(fixture.draw(shown) == 2);
        fixture.step();
    }
}

TEST_CASE("an existing host weapon bank advances independently of the world texture clock",
          "[netplay][projectile-prediction]") {
    const f32 initial = GENERATE(0.0f, 42.25f);
    Prediction fixture;
    auto host = fixture.authority;
    host.motion.tick = ++fixture.endTick;
    host.worldTextureFrame = 9876;
    host.players[1]->weapon->textureFrame = initial;
    fixture.accept(host);
    REQUIRE(fixture.authority.players[1]->weapon->textureFrame == initial);
    fixture.fire();
    auto shown = fixture.shown();
    REQUIRE(shown.projectiles.size() == 2);
    for (const auto& component : shown.projectiles) {
        CHECK(component.textureFrame ==
              Approx(initial + static_cast<f32>(fixture.endTick - host.motion.tick) * 0.5f));
    }

    // The confirmed effect owns its exact bank phase. Extrapolation must continue
    // from this phase, even if the player's bank descriptor has a different one.
    host = fixture.confirmation();
    for (auto& component : host.projectiles) {
        component.textureFrame = 71.75f;
    }
    fixture.accept(host);
    fixture.step();
    shown = fixture.shown();
    REQUIRE(shown.projectiles.size() == 2);
    const auto effect = std::ranges::find(shown.projectiles, ProjectileSource::PlayerEffect,
                                          &ProjectileState::source);
    REQUIRE(effect != shown.projectiles.end());
    CHECK(effect->textureFrame == Approx(72.25f));
    CHECK(fixture.draw(shown) == 2);
}

TEST_CASE("consecutive predicted throws share the bank clock after the first shot hits a wall",
          "[netplay][projectile-prediction]") {
    Prediction fixture(true);
    // Keep two complete native attack cycles inside the stale-authority budget.
    for (auto& sequence : fixture.tree.sequences) {
        sequence.frames = 2;
    }
    fixture.initialize();
    const auto first = fixture.fire();
    REQUIRE(fixture.shown().projectiles.size() == 2);
    for (u64 i = 0; i < LocalMovementPrediction::kStaleTicks; ++i) {
        fixture.step(true);
        const auto* replay = fixture.movement.replayed(1);
        REQUIRE(replay != nullptr);
        if (replay->releases.size() < 2) {
            continue;
        }
        const auto& second = replay->releases[1];
        REQUIRE(second.tick > first.tick);
        const auto shown = fixture.shown();
        REQUIRE(shown.projectiles.size() == 2);
        for (const auto& component : shown.projectiles) {
            REQUIRE(component.rollback);
            CHECK(component.rollback->origin.tick == second.tick);
            CHECK(component.textureFrame ==
                  Approx(static_cast<f32>(fixture.endTick - first.tick + 1) * 0.5f));
            CHECK(component.textureFrame > 0.5f);
        }
        CHECK(fixture.draw(shown) == 2);
        return;
    }
    FAIL("native animator did not produce a second release within the freshness window");
}

TEST_CASE("confirmed native missiles outlive their owner's replay eligibility",
          "[netplay][projectile-prediction][speed-rollback]") {
    enum class Interrupt : u8 { UnsupportedInput, Death, ChangedWeapon, SpeedDeadline };
    const auto interrupt = GENERATE(Interrupt::UnsupportedInput, Interrupt::Death,
                                    Interrupt::ChangedWeapon, Interrupt::SpeedDeadline);
    Prediction fixture;
    if (interrupt == Interrupt::SpeedDeadline) {
        auto boosted = fixture.authority;
        boosted.motion.tick = ++fixture.endTick;
        boosted.players[1]->modifiers = PlayerWeaponModifiers{0, 120, true};
        boosted.players[1]->movement.speed = 8;
        fixture.accept(boosted);
    }
    fixture.fire();
    auto host = fixture.confirmation();
    if (interrupt == Interrupt::Death) {
        host.players[1]->life = ReplicaPlayerLife::Dying;
        host.players[1]->health = 0;
        host.players[1]->movement = {};
    } else if (interrupt == Interrupt::ChangedWeapon) {
        host.players[1]->weapon.reset();
    } else if (interrupt == Interrupt::SpeedDeadline) {
        // No more movement/animation may be replayed across an imminent Speed
        // expiry, but it cannot retroactively change a confirmed weapon's flight.
        host.players[1]->modifiers->ticks = 0;
    }
    fixture.accept(host);
    const auto& checkpoint = *host.projectiles.front().rollback;
    PlayerMissiles native;
    MissileLaunch launch;
    launch.spec = &MissileSpec::of(static_cast<s32>(checkpoint.character));
    REQUIRE(native.launch(launch));
    REQUIRE(native.restoreFlight(native.missile(0).instance, checkpoint.flight));
    for (s32 step = 0; step < 4; ++step) {
        auto command = fixture.input(++fixture.endTick, 1, false);
        if (interrupt == Interrupt::UnsupportedInput) {
            command.heldButtons = static_cast<u32>(CommandHeld::ShieldPotion);
        }
        fixture.movement.update(&fixture.authority, std::span{&command, 1});
        fixture.projectiles.update(&fixture.authority, fixture.movement, fixture.resources,
                                   std::span{&command, 1});
        REQUIRE((fixture.movement.replayed(1) == nullptr || !host.players[1]->weapon));
        native.update(1.0f / 60, &fixture.collision);
        REQUIRE(native.count() == 1);
        const auto shown = fixture.shown();
        REQUIRE(shown.projectiles.size() == 2);
        for (const auto& component : shown.projectiles) {
            REQUIRE(component.rollback);
            CHECK(component.instance < PredictedShotLedger::kPrivateInstance);
            CHECK(component.rollback->origin == checkpoint.origin);
            CHECK(component.rollback->flight.position == native.missile(0).position);
            CHECK(component.rollback->flight.age == Approx(native.missile(0).age));
        }
        CHECK(fixture.draw(shown) == 2);
        CHECK(fixture.authority.players[1]->health == host.players[1]->health);
    }
}

TEST_CASE("held and stale confirmed projectile clocks do not forecast additional flight",
          "[netplay][projectile-prediction]") {
    const bool held = GENERATE(false, true);
    Prediction fixture;
    fixture.fire();
    auto host = fixture.confirmation();
    for (auto& component : host.projectiles) {
        component.rollback->held = held;
    }
    fixture.accept(host);
    fixture.step();
    const auto first = fixture.shown();
    REQUIRE(first.projectiles.size() == 2);
    CHECK(first.projectiles.front().rollback->flight.age ==
          Approx(host.projectiles.front().rollback->flight.age + (held ? 0 : 1.0f / 60)));
    if (!held) {
        for (u64 step = 0; step < LocalMovementPrediction::kStaleTicks; ++step) {
            fixture.step();
        }
    }
    for (s32 step = 0; step < 3; ++step) {
        fixture.step();
        const auto shown = fixture.shown();
        REQUIRE(shown.projectiles.size() == 2);
        for (usize i = 0; i < shown.projectiles.size(); ++i) {
            REQUIRE(shown.projectiles[i].rollback);
            CHECK(shown.projectiles[i].rollback->held == held);
            CHECK(shown.projectiles[i].rollback->flight.position ==
                  host.projectiles[i].rollback->flight.position);
            CHECK(shown.projectiles[i].rollback->flight.age ==
                  host.projectiles[i].rollback->flight.age);
            CHECK(shown.projectiles[i].textureFrame == host.projectiles[i].textureFrame);
        }
    }

    // A fresh released checkpoint resumes one native flight tick, without
    // replaying the elapsed hold or the disconnected presentation interval.
    host.motion.tick = fixture.endTick;
    for (auto& component : host.projectiles) {
        component.rollback->held = false;
    }
    fixture.accept(host);
    fixture.step();
    const auto resumed = fixture.shown();
    REQUIRE(resumed.projectiles.size() == 2);
    CHECK(resumed.projectiles.front().rollback->flight.age ==
          Approx(host.projectiles.front().rollback->flight.age + 1.0f / 60));
}

TEST_CASE("confirmed projectile clocks only follow bounded valid input from their owning seat",
          "[netplay][projectile-prediction]") {
    enum class Invalid : u8 {
        Epoch,
        Grant,
        RemoteSeat,
        MissingSeat,
        Buttons,
        Past,
        Horizon,
        OtherLocal
    };
    const auto invalid =
        GENERATE(Invalid::Epoch, Invalid::Grant, Invalid::RemoteSeat, Invalid::MissingSeat,
                 Invalid::Buttons, Invalid::Past, Invalid::Horizon, Invalid::OtherLocal);
    Prediction fixture;
    fixture.fire();
    auto host = fixture.confirmation();
    host.players[1]->weapon.reset();
    fixture.accept(host);
    auto command = fixture.input(host.motion.tick + 1, 1, false);
    switch (invalid) {
    case Invalid::Epoch: ++command.epoch; break;
    case Invalid::Grant: ++command.grant; break;
    case Invalid::RemoteSeat: command = fixture.input(command.tick, 0, false); break;
    case Invalid::MissingSeat: command.seat = static_cast<u8>(InputCommand::kSeats); break;
    case Invalid::Buttons: command.heldButtons = InputCommand::kHeldMask + 1; break;
    case Invalid::Past: command.tick = host.motion.tick; break;
    case Invalid::Horizon:
        command.tick = host.motion.tick + PredictedShotLedger::kHorizon + 1;
        break;
    case Invalid::OtherLocal: command = fixture.input(command.tick, 2, false); break;
    }
    fixture.projectiles.update(&fixture.authority, fixture.movement, fixture.resources,
                               std::span{&command, 1});
    const auto ignored = fixture.shown();
    REQUIRE(ignored.projectiles.size() == 2);
    CHECK(ignored.projectiles.front().rollback->flight.age ==
          host.projectiles.front().rollback->flight.age);
    CHECK(ignored.projectiles.front().rollback->flight.position ==
          host.projectiles.front().rollback->flight.position);
    command = fixture.input(host.motion.tick + 1, 1, false);
    fixture.projectiles.update(&fixture.authority, fixture.movement, fixture.resources,
                               std::span{&command, 1});
    const auto accepted = fixture.shown();
    REQUIRE(accepted.projectiles.size() == 2);
    CHECK(accepted.projectiles.front().rollback->flight.age ==
          Approx(host.projectiles.front().rollback->flight.age + 1.0f / 60));
}

TEST_CASE("finite target metadata cannot launch an unbounded predicted native flight",
          "[netplay][projectile-prediction]") {
    Prediction fixture;
    auto host = fixture.authority;
    host.motion.tick = ++fixture.endTick;
    host.players[1]->weapon->target = Vec3{0, 500, PlayerMissiles::kMuzzle + 0.01f};
    REQUIRE(host.valid());
    fixture.accept(host);
    const auto release = fixture.fire();
    PlayerMissiles::WeaponThrow request;
    request.owner = 1;
    request.position = release.motion.position;
    request.center = request.position + Vec3{0, host.players[1]->movement.followHeight, 0};
    request.hand = host.players[1]->weapon->hand;
    request.target = host.players[1]->weapon->target;
    request.attackSeconds = release.attackSeconds;
    const auto launches = PlayerMissiles::weaponLaunches(request);
    REQUIRE(launches.size() == 1);
    REQUIRE(launches.front().velocity);
    const PlayerMissiles::FlightCheckpoint calculated{
        launches.front().position, *launches.front().velocity, 0, 0, launches.front().scale, 1};
    REQUIRE_FALSE(calculated.valid());
    CHECK(fixture.projectiles.count() == 0);
    CHECK(fixture.projectiles.soundReleases().empty());
    CHECK(fixture.shown().projectiles.empty());
    CHECK(fixture.authority.players[1]->health == 100);
}

TEST_CASE("full remote projectile rosters cannot resurrect rejected local visual components",
          "[netplay][projectile-prediction]") {
    Prediction fixture;
    fixture.fire();
    auto delayed = fixture.shown();
    REQUIRE(delayed.projectiles.size() == 2);
    const auto rejectedOrigin = delayed.projectiles.front().rollback->origin;
    auto remote = delayed.projectiles.front();
    remote.rollback.reset();
    while (delayed.projectiles.size() < CombatSnapshot::kMaxProjectiles) {
        remote.instance = delayed.projectiles.size();
        delayed.projectiles.push_back(remote);
    }
    std::ranges::sort(delayed.projectiles, {}, &ProjectileState::key);
    REQUIRE(delayed.valid());
    auto rejected = fixture.authority;
    rejected.motion.tick = fixture.endTick;
    fixture.accept(rejected);
    fixture.fire(false, true);
    REQUIRE(fixture.projectiles.count() == 4);
    fixture.projectiles.present(delayed, 1);
    CHECK(delayed.projectiles.size() == CombatSnapshot::kMaxProjectiles);
    CHECK(delayed.valid());
    CHECK(std::ranges::none_of(delayed.projectiles, [&](const auto& component) {
        return component.rollback && component.rollback->origin == rejectedOrigin;
    }));
    CHECK(std::ranges::count_if(delayed.projectiles, [](const auto& component) {
              return component.rollback.has_value();
          }) == 2); // Only the available visual budget, never the rejected old body/streak.
}

TEST_CASE(
    "retail knight animation data releases a predicted native weapon before host confirmation",
    "[netplay][projectile-prediction][assets]") {
    const auto path = test::assetOrSkip("PLAYERS/KNI/ANIM/ANIM.PS2");
    AnimationSet actions;
    REQUIRE(actions.load(path.parent_path()));
    REQUIRE(actions.size() > 0);
    Prediction fixture;
    fixture.tree = actions.tree(0);
    fixture.initialize();
    const auto release = fixture.fire();
    CHECK(release.tick > fixture.authority.motion.tick);
    const auto shown = fixture.shown();
    REQUIRE(shown.projectiles.size() == 2);
    CHECK(fixture.draw(shown) == 2);
    CHECK(fixture.authority.projectiles.empty());
}
} // namespace
