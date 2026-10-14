#include <cmath>
#include <format>
#include <limits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/world/SampleLevel.h"

#include "AttackCheckpointFixture.h"
#include "game/screens/LevelFixturesCheckpoint.h"
#include "game/screens/OrdinaryProjectilePhase.h"
#include "game/world/FallingSceneryCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Phase = OrdinaryProjectilePhase;

struct Fixture : test::AttackCheckpointFixture {
    WorldLayout sceneryLayout;
    ModelSet sceneryModels;
    TextureSet sceneryTextures;
    explicit Fixture() : AttackCheckpointFixture(false) {
        auto& collision = const_cast<WorldCollision&>(world.collision());
        collision.build({{{0, 1, 0}, {{{-20, 0, -20}, {20, 0, -20}, {20, 0, 20}}}, -1, 4},
                         {{0, 1, 0}, {{{-20, 0, -20}, {20, 0, 20}, {-20, 0, 20}}}, -1, 4}});
        REQUIRE(collision.floorAt({0, 0, 5}, 1, 1));
        fixtures.bind({device, world, weapons, effects, audio});
        opponents.open({device, world, weapons, effects, audio, root, 1}, players);
        auto& enemies = opponents.enemies();
        enemies.open(device, root, &world.collision(), 16, {}, 173);
        REQUIRE(enemies.loadKind(kGruntKind));
        REQUIRE(enemies.spawn(
            {.kind = kGruntKind, .tier = 1, .position = {0, 0, 5}, .placed = true}, {}));
        targets.players = players;
    }
    ~Fixture() {
        fixtures.clear();
        world.fallingScenery().clear();
    }

    void impactArt(bool native = false) {
        const auto directory = native ? test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path()
                                      : test::sampleLevel("ordinary-impact-art");
        if (!native) {
            writeTextFile(directory / "animations.json", R"({"trees":[
          {"name":"SPARKS","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
           "sequences":[{"name":"ACTIVE","frames":10,"rate":30}]},
          {"name":"EXPSMALL","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
           "sequences":[{"name":"ACTIVE","frames":10,"rate":30}]}]})");
        }
        REQUIRE(weapons.load(directory));
        textures.bind(weapons.trees.textureAnimations(), weapons.textures, device);
        for (const auto* name : {"SPARKS", "EXPSMALL"}) {
            REQUIRE(prototypes.start(device, weapons, name, {}));
        }
        for (usize index = 0; index < prototypes.count(); ++index) {
            resources.push_back(
                {static_cast<u32>(index + 1), &weapons, {}, &textures, &prototypes.effect(index)});
        }
        arsenal.bind({device, classes, weapons, world.collision(), effects, audio, nullptr,
                      "S_WEAPONHITWOOD"});
    }

    void impactWorld(s32 fixture = -1, bool explosive = false) {
        const auto directory = test::sampleLevel("ordinary-impact-world");
        writeTextFile(directory / "animations.json", R"({"trees":[
          {"name":"STOP","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
           "sequences":[{"name":"OFF","frames":2,"rate":30},
             {"name":"ON","frames":90,"rate":30},{"name":"OPEN","frames":90,"rate":30}]}]})");
        constexpr std::array types{2, 7, 8};
        constexpr std::array subtypes{46, 1, Traps::kTentWall};
        const s32 type = fixture >= 0 ? types[static_cast<usize>(fixture)] : 1;
        const s32 subtype = fixture >= 0 ? subtypes[static_cast<usize>(fixture)] : 2;
        writeTextFile(directory / "world.json",
                      std::format(R"({{"objects":[{{"name":"FLOOR","position":[0,0,0],"flags":{}}}],
          "itemInfos":[{{"type":{},"subtype":{},"name":"STOP","radius":1,"height":4,
            "activeType":16,"activeOff":1}}],"itemInstances":{}}})",
                                  explosive ? 0x40000U | 4U : 4U, type, subtype,
                                  fixture >= 0 ? R"([{"info":0,"position":[10,0,5]}])" : "[]"));
        writeTextFile(directory / "collision.json", R"({"objects":[
          {"object":0,"normals":[0,1,0,0,1,0],
           "vertices":[-30,0,-30,-30,0,30,30,0,30,-30,0,-30,30,0,30,30,0,-30]}]})");
        fixtures.clear();
        REQUIRE(world.load(device, directory, {"TEST", 0, "TEST", "", "", "", "", 0}));
        fixtures.bind({device, world, weapons, effects, audio});
        fixtures.setPlayerCount(1);
        if (fixture == 2) {
            REQUIRE(fixtures.traps().size() == 1);
            REQUIRE(const_cast<Traps&>(fixtures.traps()).update(2, 1.0f / 30, {}).empty());
            REQUIRE(fixtures.traps().trap(0).action == 1);
        }
        if (fixture >= 0) {
            REQUIRE(fixtures.inertStops().size() == 1);
        }
    }

    void addScenery(const Vec3& position, s32 subtype = FallingScenery::kShootFall,
                    s32 minPlayers = 1) {
        const auto directory = test::sampleLevel("ordinary-projectile-scenery");
        writeTextFile(directory / "world.json",
                      std::format(
                          R"({{"objects":[{{"name":"FLOOR","position":[0,0,0]}}],
               "itemInfos":[{{"type":10,"subtype":{},"radius":0.25,"height":0.25}}],
               "itemInstances":[{{"info":0,"name":"WALL","position":[{},{},{}],
                                    "minPlayers":{}}}]}})",
                          subtype, position.x, position.y, position.z, minPlayers));
        REQUIRE(sceneryLayout.load(directory));
        REQUIRE(sceneryModels.load(directory));
        REQUIRE(sceneryTextures.load(directory));
        world.fallingScenery().bind(device, sceneryLayout, sceneryModels, sceneryTextures, "G1");
        REQUIRE(world.fallingScenery().size() == 1);
    }

    MissileLaunch launch(f32 damage = 5, Vec3 position = {0, 3, 0},
                         Vec3 velocity = {0, 0, 30}) const {
        MissileLaunch result;
        result.owner = 3;
        result.position = position;
        result.velocity = velocity;
        result.spec = &MissileSpec::of(0);
        result.damage = damage;
        return result;
    }
    PlayerArsenalCheckpoint::State arsenalState() const {
        const auto result = PlayerArsenalCheckpoint::capture(arsenal, missileResources);
        REQUIRE(result);
        return *result;
    }
    Enemies::Checkpoint enemyState() const {
        const auto result = opponents.enemies().checkpoint();
        REQUIRE(result);
        return *result;
    }
    Phase::Result advance(f32 seconds) {
        return Phase::advance(attacks, players, targets, seconds,
                              {missileResources, {}, resources});
    }
};

TEST_CASE("ordinary world and liquid impacts preserve native effects and buffered cues",
          "[ordinary-projectile-phase][ordinary-world-impact]") {
    const f32 seconds = GENERATE(1.0f / 60, 1.0f / 30);
    const s32 character = GENERATE(0, 7);
    const auto wallSound = GENERATE(MissileWallSound::Level, MissileWallSound::ThreeWay,
                                    MissileWallSound::FiveWay, MissileWallSound::Silent);
    const s32 contact = GENERATE(0, 1, 2); // floor, wall, liquid floor
    const bool liquid = contact == 2;
    CAPTURE(seconds, character, wallSound, contact);
    Fixture native, replay;
    native.impactArt();
    replay.impactArt();
    native.impactWorld();
    replay.impactWorld();
    native.missileResources[0].spec = &MissileSpec::of(character);
    replay.missileResources[0].spec = &MissileSpec::of(character);
    if (liquid) {
        for (auto* fixture : {&native, &replay}) {
            auto& collision = const_cast<WorldCollision&>(fixture->world.collision());
            collision.build({{{0, 1, 0},
                              {{{-20, 0, -20}, {20, 0, -20}, {20, 0, 20}}},
                              0,
                              WorldCollision::kLiquidSurface | 4U},
                             {{0, 1, 0},
                              {{{-20, 0, -20}, {20, 0, 20}, {-20, 0, 20}}},
                              0,
                              WorldCollision::kLiquidSurface | 4U}});
        }
    } else if (contact == 1) {
        for (auto* fixture : {&native, &replay}) {
            auto& collision = const_cast<WorldCollision&>(fixture->world.collision());
            // Synthetic defaults include wall queries; floor-only flag 4 does not.
            collision.build({{{0, 0, -1}, {{{-20, 0, 1}, {-20, 20, 1}, {20, 20, 1}}}, 0},
                             {{0, 0, -1}, {{{-20, 0, 1}, {20, 20, 1}, {20, 0, 1}}}, 0}});
            REQUIRE(collision.resolveWalls({10, 3, .9f}, 0.5f, 2.75f, 3.25f) != Vec3{10, 3, .9f});
        }
    }
    auto shot = native.launch(5, {10, .01f, 0}, {0, -10, 0});
    if (contact == 1) {
        shot.position = {10, 3, .9f};
        shot.velocity = Vec3{0, 0, 10};
    }
    shot.spec = &MissileSpec::of(character);
    shot.wallSound = wallSound;
    REQUIRE(native.arsenal.missiles().launch(shot));
    REQUIRE(replay.arsenal.missiles().launch(shot));
    const auto before = replay.arsenalState();
    const auto beforeEffects = replay.effectState();
    const auto uploads = replay.device.texturesCreated;
    native.attacks.updateProjectiles(seconds, native.players, native.targets);
    const auto result = replay.advance(seconds);
    REQUIRE(result.advanced);
    REQUIRE(result.impacts == 1);
    REQUIRE(replay.effects.count() == 1);
    CHECK(replay.arsenalState() == native.arsenalState());
    CHECK(replay.effectState() == native.effectState());
    CHECK(replay.enemyState() == native.enemyState());
    REQUIRE(Phase::supportsEffect(replay.arsenal, replay.resources[character == 7 ? 1 : 0]));
    if (liquid || wallSound != MissileWallSound::Silent) {
        REQUIRE(result.cues.size() == 1);
        const auto& cue = result.cues.front();
        CHECK(cue.position == replay.effects.effect(0).position);
        if (liquid) {
            CHECK(cue.name == "S_SPLASH");
            REQUIRE(cue.playerDistance);
            CHECK(*cue.playerDistance ==
                  glm::distance(replay.players[0].actor.followPoint(), cue.position));
            CHECK(cue.volume == LevelSoundscape::kSplashLevel / 255.0f);
        } else {
            constexpr std::array<std::string_view, 3> kSounds{"S_WEAPONHITWOOD", "S_3WAYAXE",
                                                              "S_5WAYAXE"};
            CHECK(cue.name == kSounds[static_cast<usize>(wallSound)]);
            CHECK_FALSE(cue.playerDistance);
        }
    } else {
        CHECK(result.cues.empty());
    }
    REQUIRE(replay.arsenal.missiles().restore(before.missiles, replay.missileResources));
    REQUIRE(replay.effects.restorePool(beforeEffects, replay.resources));
    const auto repeated = replay.advance(seconds);
    REQUIRE(repeated.advanced);
    CHECK(repeated.cues == result.cues);
    CHECK(replay.effectState() == native.effectState());
    for (s32 tick = 0; tick < 30; ++tick) {
        native.effects.capturePresentation();
        replay.effects.capturePresentation();
        native.effects.update(seconds);
        replay.effects.update(seconds);
        CHECK(replay.effectState() == native.effectState());
    }
    CHECK(replay.device.texturesCreated == uploads);
    CHECK(replay.helps.empty());
}

TEST_CASE("native chests closed gates and raised tent walls stop replayed shots without outputs",
          "[ordinary-projectile-phase][ordinary-world-impact]") {
    const s32 kind = GENERATE(0, 1, 2);
    const f32 seconds = GENERATE(1.0f / 60, 1.0f / 30);
    Fixture native, replay;
    native.impactWorld(kind);
    replay.impactWorld(kind);
    LevelFixturesCheckpoint nativeFixtures(native.fixtures), replayFixtures(replay.fixtures);
    REQUIRE(nativeFixtures.bindResources());
    REQUIRE(replayFixtures.bindResources());
    const auto before = replayFixtures.checkpoint();
    REQUIRE(before);
    CHECK(nativeFixtures.checkpoint() == before);
    const auto uploads = replay.device.texturesCreated;
    const auto stop = native.fixtures.inertStops().front();
    auto shot = native.launch(10000, stop.centre + Vec3{0, 1, -3}, {0, 0, 30});
    REQUIRE(native.arsenal.missiles().launch(shot));
    REQUIRE(replay.arsenal.missiles().launch(shot));
    bool stopped = false;
    for (s32 tick = 0; tick < 12; ++tick) {
        native.attacks.updateProjectiles(seconds, native.players, native.targets);
        const auto result = replay.advance(seconds);
        REQUIRE(result.advanced);
        stopped |= result.impacts != 0;
        CHECK(result.cues.empty());
        CHECK(replay.arsenalState() == native.arsenalState());
        CHECK(replayFixtures.checkpoint() == before);
        CHECK(nativeFixtures.checkpoint() == before);
    }
    REQUIRE(stopped);
    CHECK(replay.effects.count() == 0);
    CHECK(replay.players[0].actor.save().progress().inventory.keys ==
          native.players[0].actor.save().progress().inventory.keys);
    CHECK(replay.device.texturesCreated == uploads);
    CHECK(replay.helps.empty());
}

TEST_CASE("prepared world impacts continue complete native archive effects after correction",
          "[ordinary-projectile-phase][ordinary-world-impact][assets]") {
    const s32 character = GENERATE(0, 7);
    const f32 seconds = GENERATE(1.0f / 60, 1.0f / 30);
    Fixture native, replay;
    native.impactArt(true);
    replay.impactArt(true);
    native.missileResources[0].spec = &MissileSpec::of(character);
    replay.missileResources[0].spec = &MissileSpec::of(character);
    auto shot = native.launch(5, {10, .01f, 0}, {0, -10, 0});
    shot.spec = &MissileSpec::of(character);
    REQUIRE(native.arsenal.missiles().launch(shot));
    REQUIRE(replay.arsenal.missiles().launch(shot));
    const auto textures = replay.device.texturesCreated;
    native.attacks.updateProjectiles(seconds, native.players, native.targets);
    REQUIRE(replay.advance(seconds).advanced);
    REQUIRE(native.effects.count() == 1);
    CHECK(native.effectState() == replay.effectState());
    std::optional<EffectTrees::PoolCheckpoint> corrected;
    for (s32 tick = 0; tick < 120; ++tick) {
        native.effects.capturePresentation();
        replay.effects.capturePresentation();
        native.effects.update(seconds);
        replay.effects.update(seconds);
        CHECK(native.effectState() == replay.effectState());
        if (tick == 2) {
            corrected = native.effectState();
            REQUIRE(replay.effects.restorePool(*corrected, replay.resources));
            CHECK(native.effectState() == replay.effectState());
        }
    }
    REQUIRE(corrected);
    CHECK(replay.device.texturesCreated == textures);
}

TEST_CASE("ordinary projectile scenery admission uses native surviving endpoints only",
          "[ordinary-projectile-phase][projectile-scenery]") {
    const f32 seconds = GENERATE(1.0f / 60, 1.0f / 30);
    // Far, already started, hidden, touch-only, vertically separated, crossed
    // between endpoints, actual endpoint, retired enemy hit, exact vertical edge.
    const s32 variant = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8);
    Fixture native;
    Fixture replay;
    auto shot = native.launch(5, {10, 3, 0}, {0, 0, .5f / seconds});
    Vec3 at{10, 3, .5f};
    if (variant == 0) {
        at.x = 30;
    } else if (variant == 4) {
        at.y = 6;
    } else if (variant == 5) {
        shot.velocity = Vec3{0, 0, 6 / seconds};
        at.z = 3;
    } else if (variant == 7) {
        shot.position = {0, 3, 4};
        shot.velocity = Vec3{0, 0, 1};
        at = {0, 3, 5};
    } else if (variant == 8) {
        // Capture the native endpoint's vertical position instead of guessing
        // a single gravity step when its spatial update uses multiple substeps.
        PlayerMissiles probe;
        REQUIRE(probe.launch(shot));
        probe.update(seconds, nullptr);
        REQUIRE(probe.count() == 1);
        at = probe.missile(0).position + Vec3{0, 1.25f, 0};
        // Addition and the native subtraction round at different magnitudes.
        // Select the last representable centre inside the actual f32 predicate.
        if (at.y - probe.missile(0).position.y > 1.25f) {
            at.y = std::nextafter(at.y, probe.missile(0).position.y);
        }
        REQUIRE(at.y - probe.missile(0).position.y <= 1.25f);
        REQUIRE(std::nextafter(at.y, std::numeric_limits<f32>::infinity()) -
                    probe.missile(0).position.y >
                1.25f);
    }
    const s32 subtype = variant == 3 ? FallingScenery::kFallAway : FallingScenery::kShootFall;
    const s32 minPlayers = variant == 2 ? 2 : 1;
    native.addScenery(at, subtype, minPlayers);
    replay.addScenery(at, subtype, minPlayers);
    if (variant == 1) {
        REQUIRE(native.world.fallingScenery().shoot(at, 1).size() == 1);
        REQUIRE(replay.world.fallingScenery().shoot(at, 1).size() == 1);
    }
    REQUIRE(native.arsenal.missiles().launch(shot));
    REQUIRE(replay.arsenal.missiles().launch(shot));
    FallingSceneryCheckpoint nativeScenery(native.world.fallingScenery());
    FallingSceneryCheckpoint replayScenery(replay.world.fallingScenery());
    const auto before = replay.arsenalState();
    const auto beforeEnemies = replay.enemyState();
    const auto beforeScenery = replayScenery.checkpoint();
    REQUIRE(beforeScenery);
    const auto uploads = replay.device.texturesCreated;
    native.attacks.updateProjectiles(seconds, native.players, native.targets);
    const auto result = replay.advance(seconds);
    const bool contact = variant == 6 || variant == 8;
    CAPTURE(variant, seconds);
    REQUIRE(result.advanced == !contact);
    if (contact) {
        REQUIRE(native.world.fallingScenery().piece(0).started);
        CHECK(replayScenery.checkpoint() == beforeScenery);
        CHECK(replay.enemyState() == beforeEnemies);
        REQUIRE(replay.arsenal.missiles().restore(before.missiles, replay.missileResources));
        CHECK(replay.arsenalState() == before);
    } else {
        CHECK(replay.arsenalState() == native.arsenalState());
        CHECK(replay.enemyState() == native.enemyState());
        CHECK(replayScenery.checkpoint() == nativeScenery.checkpoint());
        CHECK(native.world.fallingScenery().piece(0).started == (variant == 1));
    }
    CHECK(replay.device.texturesCreated == uploads);
    CHECK(replay.helps.empty());
}

TEST_CASE("a later scenery endpoint refuses before an earlier swarm impact is dispatched",
          "[ordinary-projectile-phase][projectile-scenery]") {
    Fixture fixture;
    fixture.addScenery({10, 3, .5f});
    REQUIRE(fixture.arsenal.missiles().launch(fixture.launch(10000, {0, 3, 4}, {0, 0, 1})));
    REQUIRE(fixture.arsenal.missiles().launch(fixture.launch(5, {10, 3, 0}, {0, 0, 30})));
    const auto before = fixture.arsenalState();
    const auto enemies = fixture.enemyState();
    FallingSceneryCheckpoint scenery(fixture.world.fallingScenery());
    const auto pieces = scenery.checkpoint();
    REQUIRE(pieces);
    CHECK_FALSE(fixture.advance(1.0f / 60).advanced);
    CHECK(fixture.enemyState() == enemies);
    CHECK(scenery.checkpoint() == pieces);
    REQUIRE(fixture.arsenal.missiles().restore(before.missiles, fixture.missileResources));
    CHECK(fixture.arsenalState() == before);
}

TEST_CASE("ordinary projectile phase follows the complete native swarm-hit state",
          "[ordinary-projectile-phase]") {
    const f32 seconds = GENERATE(1.0f / 60, 1.0f / 30);
    const f32 damage = GENERATE(5.0f, 10000.0f);
    Fixture native;
    Fixture replay;
    REQUIRE(native.arsenal.missiles().launch(native.launch(damage)));
    REQUIRE(replay.arsenal.missiles().launch(replay.launch(damage)));
    const auto initialNative = native.enemyState();
    const auto initialReplay = replay.enemyState();
    CHECK(initialNative == initialReplay);
    bool hit = false;
    for (s32 tick = 0; tick < 30; ++tick) {
        native.attacks.updateProjectiles(seconds, native.players, native.targets);
        const auto result = replay.advance(seconds);
        REQUIRE(result.advanced);
        hit = hit || result.impacts != 0;
        CAPTURE(tick, result.impacts);
        CHECK(replay.arsenalState() == native.arsenalState());
        CHECK(replay.enemyState() == native.enemyState());
    }
    REQUIRE(hit);
    const auto after = replay.enemyState();
    REQUIRE_FALSE(after.losses.empty());
    REQUIRE_FALSE(after.feedback.empty());
    // Reward/feedback consumption belongs to the next opponent phase, not this
    // projectile phase. Neither side has awarded experience or shown a lesson.
    CHECK(replay.players[0].actor.save().experience() == 0);
    CHECK(replay.helps.empty());
}

TEST_CASE("ordinary projectile phase rejects a later unsupported impact before all damage",
          "[ordinary-projectile-phase]") {
    const bool explosive = GENERATE(false, true);
    Fixture fixture;
    fixture.impactArt();
    fixture.impactWorld(-1, explosive);
    // The later hit either reaches actual explosive geometry or available
    // native artwork absent from the trusted catalog. Neither may turn into a
    // silent floor hit or leave the earlier grunt damaged on refusal.
    if (!explosive) {
        fixture.resources.clear();
    }
    REQUIRE(fixture.arsenal.missiles().launch(fixture.launch(10000, {0, 3, 4}, {0, 0, 1})));
    REQUIRE(fixture.arsenal.missiles().launch(fixture.launch(10000, {10, 0.01f, 0}, {0, -10, 0})));
    const auto before = fixture.arsenalState();
    const auto enemies = fixture.enemyState();
    const auto effects = fixture.effects.checkpointPool();
    REQUIRE(effects);
    const auto result = fixture.advance(1.0f / 60);
    CHECK_FALSE(result.advanced);
    CHECK(result.cues.empty());
    CHECK(fixture.enemyState() == enemies);
    CHECK(fixture.effects.checkpointPool() == effects);
    CHECK_FALSE(fixture.world.hasWorldExplosions());
    CHECK(fixture.world.collision().solid(0));
    // The caller's full transaction also rewinds the consumed impacts/flights.
    // No queue has been erased and reported as a successful simulated tick.
    REQUIRE(fixture.arsenal.missiles().restore(before.missiles, fixture.missileResources));
    CHECK(fixture.arsenalState() == before);
    CHECK(fixture.helps.empty());
}

TEST_CASE("ordinary projectile phase refuses malformed or unsupported flight before mutation",
          "[ordinary-projectile-phase]") {
    const s32 variant = GENERATE(0, 1, 2, 3, 4, 5, 6);
    Fixture fixture;
    auto shot = fixture.launch();
    if (variant == 0) {
        shot.velocity = Vec3{1e6f, 0, 0};
    } else if (variant == 1) {
        shot.flags = powerup::kSuperShot;
    } else if (variant == 2) {
        shot.multiplayer = MultiplayerMode::Hurt;
    } else if (variant == 3) {
        shot.flags = 1; // elemental damage is not a plain weapon hit
    } else if (variant == 4) {
        shot.owner = 2; // valid input slot, but not a member of this transaction
    } else if (variant == 5) {
        shot.release = MissileRelease{0, 1, false, 0, 1U << 16};
    }
    REQUIRE(fixture.arsenal.missiles().launch(shot));
    const auto before = fixture.arsenalState();
    const auto enemies = fixture.enemyState();
    const f32 seconds = variant == 6 ? std::numeric_limits<f32>::infinity() : 1.0f / 60;
    CHECK_FALSE(fixture.advance(seconds).advanced);
    CHECK(fixture.arsenalState() == before);
    CHECK(fixture.enemyState() == enemies);
}

TEST_CASE("ordinary projectile work limits refuse aggregate work without changing native physics",
          "[ordinary-projectile-phase]") {
    Fixture fixture;
    auto shot = fixture.launch(0, {0, 100, 0}, {0, 0, 1});
    REQUIRE(fixture.arsenal.missiles().launch(shot));
    auto state = fixture.arsenalState();
    const f32 seconds = 1.0f / 30;
    // The trusted Warrior radius makes this exactly 256 native substeps.
    const f32 maximum = std::nextafter(static_cast<f32>(Phase::kMaxFlightSteps) *
                                           MissileSpec::of(0).radius * 0.5f / seconds,
                                       0.0f);
    state.missiles.missiles[0].velocity = {maximum, 0, 0};
    REQUIRE(state.valid());
    CHECK(Phase::supported(state, fixture.missileResources, seconds));
    state.missiles.missiles[0].velocity.x *= 2;
    REQUIRE(state.valid());
    CHECK_FALSE(Phase::supported(state, fixture.missileResources, seconds));
    state.missiles.missiles[0].velocity.x = maximum;
    const auto first = state.missiles.missiles[0];
    for (usize index = 1; index < Phase::kMaxTotalSteps / Phase::kMaxFlightSteps; ++index) {
        auto flight = first;
        flight.instance = static_cast<u64>(index + 1);
        state.missiles.missiles.push_back(flight);
    }
    state.missiles.nextInstance = static_cast<u64>(state.missiles.missiles.size() + 1);
    REQUIRE(state.valid());
    CHECK(Phase::supported(state, fixture.missileResources, seconds));
    auto extra = first;
    extra.instance = state.missiles.nextInstance++;
    state.missiles.missiles.push_back(extra);
    REQUIRE(state.valid());
    CHECK_FALSE(Phase::supported(state, fixture.missileResources, seconds));
}

TEST_CASE(
    "ordinary projectile admission preserves pending native impacts and rejects foreign specs",
    "[ordinary-projectile-phase]") {
    Fixture fixture;
    REQUIRE(fixture.arsenal.missiles().launch(fixture.launch()));
    auto state = fixture.arsenalState();
    PlayerMissiles::Checkpoint::Impact impact;
    impact.owner = 3;
    impact.target = 1000;
    impact.effectResource = 1;
    state.missiles.impacts.push_back(impact);
    REQUIRE(state.valid());
    CHECK(Phase::supported(state, fixture.missileResources, 1.0f / 60));
    auto resources = fixture.missileResources;
    resources[0].spec = &MissileSpec::potion();
    CHECK_FALSE(Phase::supported(state, resources, 1.0f / 60));
    CHECK_FALSE(Phase::supported(state, {}, 1.0f / 60));
    // This value-only check admits family ranges. advance separately validates
    // the actual native barrel/generator slot and its supported outcome.
    for (const s32 target : {-1, 0, 999, 2000, 2999, 7500, 7999}) {
        state.missiles.impacts.back().target = target;
        CHECK(Phase::supported(state, fixture.missileResources, 1.0f / 60));
    }
    for (const s32 target : {3000, 7000, 8000, 9000}) {
        state.missiles.impacts.back().target = target;
        CHECK_FALSE(Phase::supported(state, fixture.missileResources, 1.0f / 60));
    }
}

} // namespace
