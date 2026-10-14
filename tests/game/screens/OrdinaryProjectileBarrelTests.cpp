#include <format>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/world/SampleLevel.h"

#include "AttackCheckpointFixture.h"
#include "game/screens/HelpMessages.h"
#include "game/screens/LevelFixturesCheckpoint.h"
#include "game/screens/OrdinaryProjectilePhase.h"
#include "game/world/LevelWorldCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Phase = OrdinaryProjectilePhase;
using Kind = BreakableStrike::Kind;

struct Barrel {
    Kind kind = Kind::Plain;
    s32 health = 100;
    bool contents = false;
};

std::filesystem::path barrelWorld(const std::array<Barrel, 2>& barrels) {
    const auto directory = test::sampleLevel("ordinary-projectile-barrels");
    std::string records;
    for (const auto& barrel : barrels) {
        if (!records.empty()) {
            records += ',';
        }
        s32 subtype = 43;
        if (barrel.kind == Kind::Exploding) {
            subtype = 44;
        } else if (barrel.kind == Kind::Poison) {
            subtype = 45;
        }
        records += std::format(
            R"({{"type":{},"subtype":{},"name":"BARREL","radius":1,"height":3,"armor":0,"hitPoints":{}}})",
            barrel.kind == Kind::Holding ? 2 : 10, subtype, barrel.health);
    }
    records += R"(,{"type":-1,"choices":[-1]},{"type":-1,"choices":[4]},
      {"type":1,"subtype":2,"name":"KEY","radius":1,"height":1,"value":1})";
    writeTextFile(directory / "world.json",
                  std::format(R"({{"objects":[{{"name":"FLOOR","position":[0,0,0],"flags":4}}],
          "itemInfos":[{}],"itemInstances":[
          {{"info":0,"position":[-10,0,3],"params":[{},0,0,0,1,0,0,0,0,0,0,0]}},
          {{"info":1,"position":[10,0,3],"params":[{},0,0,0,1,0,0,0,0,0,0,0]}}]}})",
                              records, barrels[0].contents ? 3 : 2, barrels[1].contents ? 3 : 2));
    writeTextFile(directory / "collision.json", R"({"objects":[
      {"object":0,"normals":[0,1,0,0,1,0],
       "vertices":[-40,0,-40,40,0,40,40,0,-40,-40,0,-40,-40,0,40,40,0,40]}]})");
    writeTextFile(directory / "animations.json", R"({"trees":[
      {"name":"BARREL","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
       "sequences":[{"name":"WHOLE","frames":2,"rate":30},
         {"name":"ACTIVE","frames":45,"rate":30},{"name":"DONE","frames":0}]},
      {"name":"KEY","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
       "sequences":[{"name":"ACTIVE","frames":2,"rate":30}]}]})");
    return directory;
}

struct Fixture : test::AttackCheckpointFixture {
    struct State {
        PlayerArsenalCheckpoint::State arsenal;
        PlayerAttacksCheckpoint::State attacks;
        EffectTrees::PoolCheckpoint effects;
        LevelFixturesCheckpoint::State fixtures;
        std::unique_ptr<LevelWorldCheckpoint::State> world;
    };
    std::unique_ptr<LevelFixturesCheckpoint> fixtureOwner;
    std::unique_ptr<LevelWorldCheckpoint> worldOwner;
    usize external = 0;

    explicit Fixture(std::array<Barrel, 2> barrels = {}) : AttackCheckpointFixture(false) {
        REQUIRE(world.load(device, barrelWorld(barrels), {.name = "GTEST", .directory = "."}));
        world.setPlayerCount(1);
        fixtures.bind({device, world, weapons, effects, audio});
        fixtures.setPlayerCount(1);
        REQUIRE(fixtures.barrels().size() == 2);
        for (usize index = 0; index < 2; ++index) {
            REQUIRE(fixtures.barrels().standing(index));
            REQUIRE(fixtures.barrels().barrel(index).figure.hasFigure());
        }
        opponents.open({device, world, weapons, effects, audio, root, 1}, players);
        targets.players = players;
        targets.fixtureEvents.hurt = [this](usize, f32, HurtKind, bool, const PlayerImpact&) {
            ++external;
        };
        targets.fixtureEvents.releaseEnemy = [this](s32, const Vec3&, s32) {
            ++external;
            return false;
        };
        targets.fixtureEvents.opponents = [this](const Vec3&, f32, f32, std::vector<s32>&, u32) {
            ++external;
        };
        worldOwner = std::make_unique<LevelWorldCheckpoint>(world);
        fixtureOwner = std::make_unique<LevelFixturesCheckpoint>(fixtures);
        REQUIRE(worldOwner->bindResources(device));
        REQUIRE(fixtureOwner->bindResources());
    }
    ~Fixture() {
        fixtureOwner.reset();
        worldOwner.reset();
        fixtures.clear();
    }

    MissileLaunch shot(usize barrel, f32 damage, bool strong = false) const {
        MissileLaunch launch;
        launch.owner = players[0].actor.player();
        launch.position = fixtures.barrels().barrel(barrel).figure.position() + Vec3{0, 1.5f, -1};
        launch.velocity = Vec3{0, 0, strong ? 60.0f : 30.0f};
        launch.damage = damage;
        launch.spec = &MissileSpec::of(0);
        launch.release = MissileRelease{.character = 0, .generation = 1, .strong = strong};
        return launch;
    }
    Phase::Result advance(f32 seconds, bool staged = false) {
        return Phase::advance(attacks, players, targets, seconds, {missileResources, {}, resources},
                              staged ? worldOwner.get() : nullptr);
    }
    std::unique_ptr<State> capture() const {
        auto result = std::make_unique<State>();
        const auto arsenalState = PlayerArsenalCheckpoint::capture(arsenal, missileResources);
        REQUIRE(arsenalState);
        result->arsenal = *arsenalState;
        result->attacks = state();
        result->effects = effectState();
        const auto fixtureState = fixtureOwner->checkpoint();
        REQUIRE(fixtureState);
        result->fixtures = *fixtureState;
        auto worldState = worldOwner->checkpoint();
        REQUIRE(worldState);
        result->world = std::make_unique<LevelWorldCheckpoint::State>(std::move(*worldState));
        return result;
    }
    void check(const State& expected) const {
        CHECK(PlayerArsenalCheckpoint::capture(arsenal, missileResources) == expected.arsenal);
        CHECK(state() == expected.attacks);
        CHECK(effectState() == expected.effects);
        CHECK(fixtureOwner->checkpoint() == expected.fixtures);
        CHECK(worldOwner->checkpoint() == *expected.world);
    }
    void restoreAll(const State& saved) {
        auto a = PlayerArsenalCheckpoint::prepare(arsenal, saved.arsenal, slots, saved.effects,
                                                  missileResources);
        auto b = PlayerAttacksCheckpoint::prepare(attacks, saved.attacks, actors, saved.effects,
                                                  missileResources);
        auto c = effects.prepareRestorePool(saved.effects, resources);
        auto d = worldOwner->prepareRestore(*saved.world);
        auto e = fixtureOwner->prepareRestore(saved.fixtures);
        REQUIRE(a);
        REQUIRE(b);
        REQUIRE(c);
        REQUIRE(d);
        REQUIRE(e);
        REQUIRE(PlayerArsenalCheckpoint::canCommit(arsenal, *a));
        REQUIRE(PlayerAttacksCheckpoint::canCommit(attacks, *b));
        REQUIRE(effects.canCommit(*c));
        REQUIRE(worldOwner->canCommit(*d));
        REQUIRE(fixtureOwner->canCommit(*e));
        REQUIRE(PlayerArsenalCheckpoint::commit(arsenal, std::move(*a)));
        REQUIRE(PlayerAttacksCheckpoint::commit(attacks, std::move(*b)));
        REQUIRE(effects.commitRestore(std::move(*c)));
        REQUIRE(worldOwner->commitRestore(std::move(*d)));
        REQUIRE(fixtureOwner->commitRestore(std::move(*e)));
    }
};

TEST_CASE("ordinary projectile barrel damage and break retain native continuation",
          "[ordinary-projectile-barrel]") {
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    const usize variant = GENERATE(usize{0}, usize{1}, usize{2}, usize{3}, usize{4}, usize{5});
    constexpr std::array kKinds{Kind::Plain,  Kind::Holding, Kind::Exploding,
                                Kind::Poison, Kind::Plain,   Kind::Holding};
    const Kind kind = kKinds[variant];
    const bool strong = GENERATE(false, true);
    const bool breaking = variant >= 4;
    CAPTURE(seconds, kind, strong, breaking);
    auto native =
        std::make_unique<Fixture>(std::array<Barrel, 2>{{{kind, breaking ? 1 : 100}, {}}});
    auto replay =
        std::make_unique<Fixture>(std::array<Barrel, 2>{{{kind, breaking ? 1 : 100}, {}}});
    const auto launch = native->shot(0, strong ? 2.5f : 1.5f, strong);
    REQUIRE(native->arsenal.missiles().launch(launch));
    REQUIRE(replay->arsenal.missiles().launch(launch));
    const auto before = replay->capture();
    native->check(*before);
    const auto uploads = replay->device.texturesCreated;
    native->attacks.updateProjectiles(seconds, native->players, native->targets);
    const auto expected = native->capture();
    const auto result = replay->advance(seconds);
    REQUIRE(result.advanced);
    REQUIRE(result.impacts == 1);
    CHECK(result.refusal == Phase::Refusal::None);
    CHECK_FALSE(result.refusedTarget);
    REQUIRE(result.cues.size() == 1);
    CHECK(result.cues[0].name == (breaking ? "S_BARREL_WOODG" : "S_WEAPONHITWOOD"));
    CHECK_FALSE(result.cues[0].playerDistance);
    CHECK(result.cues[0].volume == 1);
    s32 expectedHealth = strong ? 97 : 98;
    if (breaking) {
        expectedHealth = 0;
    }
    CHECK(replay->fixtures.barrels().barrel(0).health == expectedHealth);
    replay->check(*expected);
    CHECK(replay->external == 0);
    CHECK(replay->helps.empty());
    replay->restoreAll(*before);
    replay->check(*before);
    REQUIRE(replay->advance(seconds).advanced);
    replay->check(*expected);
    for (s32 tick = 0; tick < 60; ++tick) {
        native->fixtures.update(1, seconds, native->players, native->targets.fixtureEvents);
        replay->fixtures.update(1, seconds, replay->players, replay->targets.fixtureEvents);
        native->world.update(seconds);
        replay->world.update(seconds);
        replay->check(*native->capture());
    }
    CHECK(replay->device.texturesCreated == uploads);
}

TEST_CASE("late projectile barrel refusal discards cues and restores earlier native hits",
          "[ordinary-projectile-barrel]") {
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    const Kind kind = GENERATE(Kind::Holding, Kind::Exploding, Kind::Poison);
    CAPTURE(seconds, kind);
    auto fixture = std::make_unique<Fixture>(
        std::array<Barrel, 2>{{{Kind::Plain, 1}, {kind, 1, kind == Kind::Holding}}});
    REQUIRE(fixture->arsenal.missiles().launch(fixture->shot(0, 5)));
    REQUIRE(fixture->arsenal.missiles().launch(fixture->shot(1, 5, true)));
    const auto before = fixture->capture();
    const auto uploads = fixture->device.texturesCreated;
    const auto result = fixture->advance(seconds);
    CHECK_FALSE(result.advanced);
    REQUIRE(result.refusedTarget);
    CHECK(*result.refusedTarget == 1);
    auto reason = Phase::Refusal::BarrelGas;
    if (kind == Kind::Holding) {
        reason = Phase::Refusal::BarrelContents;
    } else if (kind == Kind::Exploding) {
        reason = Phase::Refusal::BarrelExplosion;
    }
    CHECK(result.refusal == reason);
    CHECK(result.cues.empty());
    CHECK(result.impacts == 0);
    CHECK(fixture->fixtures.barrels().barrel(0).health == 0);
    CHECK(fixture->fixtures.barrels().barrel(1).health == 0);
    CHECK(fixture->effects.count() == 0);
    CHECK(fixture->external == 0);
    CHECK(fixture->helps.empty());
    CHECK(fixture->worldOwner->checkpoint() == *before->world);
    fixture->restoreAll(*before);
    fixture->check(*before);
    CHECK(fixture->device.texturesCreated == uploads);
}

TEST_CASE("native spread contacts break one barrel once without replaying its cue",
          "[ordinary-projectile-barrel]") {
    const s32 count = GENERATE(3, 5);
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    auto native = std::make_unique<Fixture>(std::array<Barrel, 2>{{{Kind::Plain, 1}, {}}});
    auto replay = std::make_unique<Fixture>(std::array<Barrel, 2>{{{Kind::Plain, 1}, {}}});
    PlayerMissiles::WeaponThrow weapon;
    weapon.owner = 3;
    weapon.center = native->fixtures.barrels().barrel(0).figure.position() + Vec3{0, 1.5f, 0};
    weapon.hand.z = -PlayerMissiles::kMuzzle;
    weapon.damage = 5;
    weapon.shots = count;
    const auto fan = PlayerMissiles::weaponLaunches(weapon);
    REQUIRE(fan.size() == static_cast<usize>(count));
    for (const auto& launch : fan) {
        REQUIRE(native->arsenal.missiles().launch(launch));
        REQUIRE(replay->arsenal.missiles().launch(launch));
    }
    native->attacks.updateProjectiles(seconds, native->players, native->targets);
    const auto result = replay->advance(seconds);
    REQUIRE(result.advanced);
    CHECK(result.impacts == static_cast<usize>(count));
    REQUIRE(result.cues.size() == 1);
    CHECK(result.cues.front().name == "S_BARREL_WOODG");
    replay->check(*native->capture());
    CHECK(replay->external == 0);
    CHECK(replay->helps.empty());
}

TEST_CASE("projectile barrel dispatch refuses preexisting blasts before damage",
          "[ordinary-projectile-barrel]") {
    auto fixture = std::make_unique<Fixture>();
    fixture->fixtures.blast({1000, 0, 1000}, 1, 5, fixture->players,
                            fixture->targets.fixtureEvents);
    REQUIRE(fixture->arsenal.missiles().launch(fixture->shot(0, 5)));
    const auto before = fixture->capture();
    REQUIRE(before->fixtures.blasts.size() == 1);
    fixture->external = 0;
    const auto result = fixture->advance(1.0f / 60);
    CHECK_FALSE(result.advanced);
    CHECK(result.refusal == Phase::Refusal::BarrelBlasts);
    REQUIRE(result.refusedTarget);
    CHECK(*result.refusedTarget == 0);
    CHECK(result.cues.empty());
    CHECK(fixture->fixtureOwner->checkpoint() == before->fixtures);
    CHECK(fixture->external == 0);
    fixture->restoreAll(*before);
    fixture->check(*before);
}

TEST_CASE("queued barrel impacts retain native ordering and reject nonexistent slots",
          "[ordinary-projectile-barrel]") {
    const bool invalid = GENERATE(false, true);
    auto native = std::make_unique<Fixture>();
    auto replay = std::make_unique<Fixture>();
    for (auto* fixture : {native.get(), replay.get()}) {
        REQUIRE(fixture->arsenal.missiles().launch(fixture->shot(0, 5)));
        const std::array targets{fixture->fixtures.barrels().target(0, invalid ? 2 : 0)};
        fixture->arsenal.missiles().update(1.0f / 60, &fixture->world.collision(), targets);
    }
    const auto before = replay->capture();
    REQUIRE(before->arsenal.missiles.impacts.size() == 1);
    CHECK(Phase::supported(before->arsenal, replay->missileResources, 1.0f / 60));
    const auto result = replay->advance(1.0f / 60);
    if (invalid) {
        CHECK_FALSE(result.advanced);
        CHECK(result.refusal == Phase::Refusal::Target);
        REQUIRE(result.refusedTarget);
        CHECK(*result.refusedTarget == 2);
        CHECK(result.cues.empty());
        CHECK(replay->fixtureOwner->checkpoint() == before->fixtures);
        replay->restoreAll(*before);
        replay->check(*before);
    } else {
        native->attacks.updateProjectiles(1.0f / 60, native->players, native->targets);
        REQUIRE(result.advanced);
        CHECK(result.impacts == 1);
        replay->check(*native->capture());
    }
}

TEST_CASE("staged ordinary projectiles birth holding contents once with native fan ordering",
          "[ordinary-projectile-barrel][ordinary-barrel-emission]") {
    const s32 ticks = GENERATE(1, 2);
    const s32 variant = GENERATE(0, 1, 2, 3); // normal, strong, three-way, five-way
    const bool strong = variant == 1;
    const s32 count = variant < 2 ? 1 : variant == 2 ? 3 : 5;
    const f32 seconds = static_cast<f32>(ticks) / 60;
    CAPTURE(ticks, variant);
    auto native = std::make_unique<Fixture>(std::array<Barrel, 2>{{{Kind::Holding, 1, true}, {}}});
    auto replay = std::make_unique<Fixture>(std::array<Barrel, 2>{{{Kind::Holding, 1, true}, {}}});
    for (auto* fixture : {native.get(), replay.get()}) {
        fixture->players[0].actor.save().helpSeen = {HelpMessages::kBarrelsHold};
        if (count == 1) {
            REQUIRE(fixture->arsenal.missiles().launch(fixture->shot(0, 5, strong)));
        } else {
            PlayerMissiles::WeaponThrow weapon;
            weapon.owner = fixture->players[0].actor.player();
            weapon.center =
                fixture->fixtures.barrels().barrel(0).figure.position() + Vec3{0, 1.5f, 0};
            weapon.hand.z = -PlayerMissiles::kMuzzle;
            weapon.damage = 5;
            weapon.shots = count;
            const auto fan = PlayerMissiles::weaponLaunches(weapon);
            REQUIRE(fan.size() == static_cast<usize>(count));
            for (const auto& launch : fan) {
                REQUIRE(fixture->arsenal.missiles().launch(launch));
            }
        }
    }
    const auto before = replay->capture();
    native->check(*before);
    const auto created = replay->device.texturesCreated;
    const auto uploaded = replay->device.textureUpdates;
    native->attacks.updateProjectiles(seconds, native->players, native->targets);
    const auto expected = native->capture();
    const auto result = replay->advance(seconds, true);
    REQUIRE(result.advanced);
    CHECK(result.impacts == static_cast<usize>(count));
    REQUIRE(result.cues.size() == 1);
    CHECK(result.cues.front().name == "S_BARREL_WOODG");
    REQUIRE(replay->world.placedItems().size() == 1);
    CHECK(replay->world.placedItems().item(0).info == 4);
    CHECK(replay->world.placedItems().item(0).value == 1);
    CHECK(replay->world.placedItems().item(0).noGrabSeconds == 0);
    CHECK(native->helps == std::vector<s32>{HelpMessages::kBarrelsHold});
    CHECK(native->external == 1); // live releaseEnemy was tried before the pickup fallback
    CHECK(replay->helps.empty());
    CHECK(replay->external == 0);
    replay->check(*expected);
    replay->restoreAll(*before);
    replay->check(*before);
    REQUIRE(replay->advance(seconds, true).advanced);
    replay->check(*expected);
    for (s32 tick = 0; tick < 60 / ticks; ++tick) {
        native->fixtures.update(ticks, seconds, native->players, native->targets.fixtureEvents);
        replay->fixtures.update(ticks, seconds, replay->players, replay->targets.fixtureEvents);
        native->world.update(seconds);
        replay->world.update(seconds);
        replay->check(*native->capture());
    }
    CHECK(replay->device.texturesCreated == created);
    CHECK(replay->device.textureUpdates == uploaded);
}

TEST_CASE("late staged projectile lesson refusal removes the birth and every earlier impact",
          "[ordinary-projectile-barrel][ordinary-barrel-emission]") {
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    auto fixture = std::make_unique<Fixture>(
        std::array<Barrel, 2>{{{Kind::Plain, 1}, {Kind::Holding, 1, true}}});
    REQUIRE(fixture->players[0].actor.save().helpSeen.empty());
    REQUIRE(fixture->arsenal.missiles().launch(fixture->shot(0, 5)));
    REQUIRE(fixture->arsenal.missiles().launch(fixture->shot(1, 5, true)));
    const auto before = fixture->capture();
    const auto created = fixture->device.texturesCreated;
    const auto result = fixture->advance(seconds, true);
    CHECK_FALSE(result.advanced);
    CHECK(result.refusal == Phase::Refusal::BarrelContents);
    CHECK(result.refusedTarget == std::optional<s32>{1});
    CHECK(result.cues.empty());
    CHECK(result.impacts == 0);
    CHECK(fixture->fixtures.barrels().barrel(0).health == 0);
    CHECK(fixture->fixtures.barrels().barrel(1).health == 0);
    REQUIRE(fixture->world.placedItems().size() == 1); // native birth precedes help refusal
    CHECK(fixture->world.placedItems().item(0).info == 4);
    CHECK(fixture->arsenal.missiles().count() == 0);
    CHECK(fixture->external == 0);
    CHECK(fixture->helps.empty());
    fixture->restoreAll(*before);
    fixture->check(*before);
    CHECK(fixture->world.placedItems().size() == 0);
    CHECK(fixture->device.texturesCreated == created);
}
} // namespace
