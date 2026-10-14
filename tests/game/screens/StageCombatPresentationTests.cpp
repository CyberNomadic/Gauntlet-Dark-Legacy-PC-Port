#include <algorithm>
#include <limits>
#include <memory>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"
#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/PlayScene.h"
#include "game/screens/StageCombatPresentation.h"
#include "game/screens/StageParticlePresentation.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

CombatSnapshot picture(u64 tick, f32 height) {
    CombatSnapshot result;
    result.motion.epoch = result.motion.cameraContinuity = 1;
    result.motion.tick = tick;
    result.motion.camera.position = {0, height + 10, -10};
    for (const usize seat : {usize{0}, usize{2}}) {
        result.motion.players[seat] = SeatMotion{1, 1, {static_cast<f32>(seat), height, 0}, 0};
        result.players[seat] =
            PlayerCombatState{1000, ReplicaPlayerLife::Standing, false, true, {4, 1, 1, height, 1}};
    }
    result.geometry = SceneGeometry{42, 1, 0, {{0}}};
    result.geometry->objects[0].local[3].y = height;
    result.worldTextureFrame = height;
    EnemyCombatState enemy;
    enemy.instance = 7;
    enemy.health = enemy.fullHealth = 50;
    enemy.position = {5, height, 0};
    enemy.animation = {2, 1, 1, height, 1};
    result.enemies.push_back(enemy);
    ProjectileState shot;
    shot.instance = shot.resource = 1;
    shot.placement[3].y = height;
    shot.age = height;
    result.projectiles.push_back(shot);
    PickupState item;
    item.instance = item.resource = 1;
    item.placement[3].y = height;
    item.textureFrame = height;
    result.pickups.push_back(item);
    FixtureState fixture;
    fixture.instance = fixture.resource = 1;
    fixture.placement[3].y = height;
    fixture.textureClock = height;
    result.fixtures.push_back(fixture);
    REQUIRE(result.valid());
    return result;
}
EnemyPresentation provenance(const CombatSnapshot& state) {
    EnemyPresentation result;
    result.epoch = state.motion.epoch;
    result.tick = state.motion.tick;
    result.shownGeometry = state.geometry;
    REQUIRE(result.matches(state));
    return result;
}

struct CaptureFixture {
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive archive;
    TreeModel model;
    Enemies enemies;
    EnemyMissiles enemyMissiles;
    PlayerMissiles playerMissiles;
    EffectTrees effects;
    Chests chests;
    LockedGates gates;
    Generators generators;
    Breakables barrels;
    Traps traps;
    SafeRocks rocks;
    Rubble rubble;
    Critters critters;
    Bosses bosses;
    std::array<PlayerRuntime, 2> party;
    ProjectileResources projectiles;
    PickupResources pickups;
    FixtureResources fixtures;
    FighterResources fighters;

    CaptureFixture() {
        const auto root = test::sampleLevel("stage-combat-presentation");
        writeTextFile(root / "world.json", R"({"objects":[
            {"name":"FLOOR","position":[0,0,0]}]})");
        writeTextFile(root / "animations.json", R"({"trees":[{"name":"SHOT",
            "nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
            "sequences":[]}]})");
        REQUIRE(world.load(device, root, {.name = "TEST", .directory = "."}));
        REQUIRE(archive.load(root));
        REQUIRE(model.bind(archive.trees.tree(0), archive.models, archive.textures, device));
        constexpr std::array kSeats{0, 2};
        for (usize i = 0; i < party.size(); ++i) {
            party[i].actor.spawn(kSeats[i], {}, nullptr, {static_cast<f32>(i) * 5, 0, 0}, 0);
            party[i].actor.save().progress().health = 1000;
        }
    }
    MotionSnapshot motion(u64 tick) const {
        MotionSnapshot value;
        value.epoch = value.cameraContinuity = 1;
        value.tick = tick;
        for (const auto& player : party) {
            value.players[static_cast<usize>(player.actor.player())] =
                SeatMotion{1, 1, player.actor.position(), player.actor.yaw()};
        }
        return value;
    }
    StageCombatPresentation::Sources sources() const {
        return {party,
                world,
                enemies,
                playerMissiles,
                enemyMissiles,
                effects,
                {chests, gates, world.triggers(), generators, barrels, traps, rocks, rubble},
                critters,
                bosses};
    }
    StageCombatPresentation::Resources resources() const {
        return {projectiles, pickups, fixtures, fighters};
    }
    CombatSnapshot authority() const {
        const auto state = CombatCapture::capture(motion(10), party, enemies, nullptr, &world);
        REQUIRE(state);
        return *state;
    }
};

TEST_CASE("stage capture resolves native source pointers without allocating render resources",
          "[netplay][stage-presentation]") {
    auto fixture = std::make_unique<CaptureFixture>();
    auto authority = fixture->authority();
    const auto first = StageCombatPresentation::capture(authority, fixture->motion(11),
                                                        fixture->sources(), fixture->resources());
    REQUIRE(first);
    REQUIRE(first->geometry);
    CHECK(first->geometry->layout == fixture->world.scene().geometry().layout);
    CHECK(first->players[0]->health == 1000);
    CHECK(first->players[2]->health == 1000);

    MissileLaunch launch;
    launch.owner = 2;
    launch.spec = &MissileSpec::of(0);
    launch.model = &fixture->model;
    launch.position = {0, 5, 0};
    launch.velocity = Vec3{0, 0, 20};
    launch.release = MissileRelease{0, 12};
    REQUIRE(fixture->playerMissiles.launch(launch));
    const auto textures = fixture->device.texturesCreated;
    const auto updates = fixture->device.textureUpdates;
    CHECK_FALSE(StageCombatPresentation::capture(authority, fixture->motion(11), fixture->sources(),
                                                 fixture->resources()));
    CHECK(fixture->playerMissiles.count() == 1);
    REQUIRE(fixture->projectiles.addModel(1, fixture->model));
    const auto captured = StageCombatPresentation::capture(
        authority, fixture->motion(11), fixture->sources(), fixture->resources());
    REQUIRE(captured);
    REQUIRE(captured->projectiles.size() == 1);
    CHECK(captured->projectiles[0].instance == fixture->playerMissiles.missile(0).instance);
    CHECK(captured->projectiles[0].placement ==
          PlayerMissiles::transformOf(fixture->playerMissiles.missile(0)));
    // Native generation alone cannot certify a session release tick/grant.
    CHECK_FALSE(captured->projectiles[0].releaseOrigin());
    CHECK(fixture->device.texturesCreated == textures);
    CHECK(fixture->device.textureUpdates == updates);
    CHECK(fixture->party[1].actor.save().health() == 1000);
}

TEST_CASE("stage capture refuses missing portal and external presentation ownership",
          "[netplay][stage-presentation]") {
    auto fixture = std::make_unique<CaptureFixture>();
    auto authority = fixture->authority();
    SECTION("ordinary portal") {
        FixtureState portal;
        portal.source = FixtureSource::Portal;
        portal.instance = portal.resource = 1;
        authority.fixtures.push_back(portal);
    }
    SECTION("secret portal") {
        FixtureState portal;
        portal.source = FixtureSource::SecretPortal;
        portal.instance = portal.resource = 1;
        authority.fixtures.push_back(portal);
    }
    SECTION("arrival") {
        ProjectileState arrival;
        arrival.source = ProjectileSource::Arrival;
        arrival.instance = arrival.resource = 1;
        authority.projectiles.push_back(arrival);
    }
    REQUIRE(authority.valid());
    CHECK_FALSE(StageCombatPresentation::capture(authority, fixture->motion(11), fixture->sources(),
                                                 fixture->resources()));
    const ExitPortals unbound;
    auto incomplete = fixture->sources();
    incomplete.fixtures.portals = &unbound;
    CHECK_FALSE(StageCombatPresentation::capture(authority, fixture->motion(11), incomplete,
                                                 fixture->resources()));
}

TEST_CASE("stage capture refuses unrepresented live particles without banning empty emitters",
          "[netplay][stage-presentation]") {
    auto fixture = std::make_unique<CaptureFixture>();
    const auto authority = fixture->authority();
    ParticleDescriptor descriptor;
    descriptor.texture = "STONE";
    descriptor.emitFrames = ParticleDescriptor::kEndless;
    descriptor.rate = {1, 1, 1, 1};
    descriptor.particleLife = 30;
    REQUIRE(fixture->effects.startParticles(fixture->device, fixture->archive, descriptor, 0,
                                            Mat4{1}) != 0);
    REQUIRE(fixture->effects.effect(0).trails.particleCount() == 0);
    CHECK(StageCombatPresentation::capture(authority, fixture->motion(11), fixture->sources(),
                                           fixture->resources()));
    fixture->effects.update(0.1f);
    REQUIRE(fixture->effects.effect(0).trails.particleCount() > 0);
    const auto count = fixture->effects.effect(0).trails.particleCount();
    CHECK_FALSE(StageCombatPresentation::capture(authority, fixture->motion(11), fixture->sources(),
                                                 fixture->resources()));
    CHECK(fixture->effects.effect(0).trails.particleCount() == count);
    auto lifetime = std::make_shared<const u8>(0);
    const auto particles = StageParticlePresentation::capture(
        1, 11, lifetime,
        {fixture->world, fixture->playerMissiles.visuals(), fixture->effects, nullptr, {}});
    REQUIRE(particles);
    CHECK(StageCombatPresentation::capture(authority, fixture->motion(11), fixture->sources(),
                                           fixture->resources(), particles.get()));
    CHECK_FALSE(StageCombatPresentation::capture(authority, fixture->motion(12), fixture->sources(),
                                                 fixture->resources(), particles.get()));
    lifetime.reset();
    CHECK_FALSE(StageCombatPresentation::capture(authority, fixture->motion(11), fixture->sources(),
                                                 fixture->resources(), particles.get()));
    fixture->effects.clear();
    CHECK(StageCombatPresentation::capture(authority, fixture->motion(11), fixture->sources(),
                                           fixture->resources()));
}

TEST_CASE("stage presentation samples every visual cohort on the same native tick interval",
          "[netplay][stage-presentation]") {
    const auto previous = picture(11, 0);
    auto current = picture(12, 2);
    current.players[2]->health = 900;
    for (const f32 fraction : {0.0f, 0.25f, 0.5f, 1.0f}) {
        CAPTURE(fraction);
        auto shown = picture(8, 20);
        shown.musicArea = 3;
        shown.musicLevel = 128;
        shown.hud.emplace();
        shown.hud->players[0] = HudPlayer{.name = "LOCAL", .gold = 99, .health = 1000};
        shown.hud->players[2] = HudPlayer{.name = "REMOTE", .gold = 13, .health = 1000};
        auto presentation = provenance(shown);
        REQUIRE(StageCombatPresentation::present(shown, presentation, previous, current, fraction));
        CHECK(shown.valid());
        CHECK(presentation.matches(shown));
        const f32 height = 2 * fraction;
        CHECK(shown.motion.tick == 8);
        CHECK(shown.motion.camera.position.y == Approx(10 + height));
        CHECK(shown.motion.players[0]->position.y == Approx(height));
        CHECK(shown.motion.players[2]->position.y == Approx(height));
        CHECK(shown.enemies[0].position.y == Approx(height));
        CHECK(shown.geometry->objects[0].local[3].y == Approx(height));
        CHECK(shown.projectiles[0].placement[3].y == Approx(height));
        CHECK(shown.pickups[0].placement[3].y == Approx(height));
        CHECK(shown.fixtures[0].placement[3].y == Approx(height));
        CHECK(shown.worldTextureFrame == Approx(height));
        CHECK(shown.players[2]->health == 1000);
        CHECK(shown.hud->players[2]->gold == 13);
        CHECK(shown.musicArea == 3);
        CHECK(shown.musicLevel == 128);
        CHECK_FALSE(shown.players[0]->movement.enabled);
        CHECK_FALSE(shown.players[2]->movement.enabled);
        CHECK_FALSE(shown.opponents);
        CHECK_FALSE(shown.combatState);
    }
}

TEST_CASE("stage presentation refuses stale identities atomically rather than grafting one seat",
          "[netplay][stage-presentation]") {
    const auto previous = picture(11, 0);
    auto current = picture(12, 2);
    auto shown = picture(8, 20);
    auto presentation = provenance(shown);
    f32 blend = 0.5f;
    SECTION("remote grant changed") {
        current.motion.players[2]->grant = 2;
    }
    SECTION("local continuity changed") {
        current.motion.players[0]->continuity = 2;
    }
    SECTION("camera cut") {
        current.motion.cameraContinuity = 2;
    }
    SECTION("new layout") {
        current.geometry->layout = 43;
    }
    SECTION("remote actor disappeared") {
        current.motion.players[2].reset();
        current.players[2].reset();
    }
    SECTION("wrong epoch") {
        current.motion.epoch = 2;
    }
    SECTION("gap") {
        current.motion.tick = 14;
    }
    SECTION("nonfinite blend") {
        blend = std::numeric_limits<f32>::quiet_NaN();
    }
    SECTION("future display") {
        shown.motion.tick = presentation.tick = 13;
    }
    SECTION("damaged provenance") {
        presentation.tick = 7;
    }
    const auto unchanged = CombatPacket::encode(shown);
    REQUIRE(unchanged);
    const auto stamp = presentation.tick;
    CHECK_FALSE(StageCombatPresentation::present(shown, presentation, previous, current, blend));
    CHECK(CombatPacket::encode(shown) == unchanged);
    CHECK(presentation.tick == stamp);
    CHECK(presentation.enemies.empty());
}

TEST_CASE("direct stage pair sampling matches the transported playback interpolation",
          "[netplay][stage-presentation][combat-playback]") {
    const auto first = picture(10, 0);
    auto last = picture(14, 4);
    last.motion.camera.yaw = -3;
    last.motion.players[2]->yaw = 3;
    last.geometry->objects[0].local =
        glm::translate(Mat4{1}, Vec3{0, 4, 0}) * glm::rotate(Mat4{1}, 0.5f, Vec3{0, 1, 0});
    last.projectiles.push_back(last.projectiles.front());
    last.projectiles.back().instance = 2;
    CombatPlayback playback;
    REQUIRE(playback.begin(1, 1));
    for (const auto* state : std::array<const CombatSnapshot*, 2>{&first, &last}) {
        const auto packets = CombatReplica::packets(*state);
        REQUIRE(packets);
        for (const auto& packet : *packets) {
            playback.receive(1, packet);
        }
        REQUIRE(playback.latest());
        CHECK(playback.latest()->motion.tick == state->motion.tick);
    }
    for (const f32 fraction : {0.0f, 0.125f, 0.5f, 0.875f, 1.0f}) {
        CAPTURE(fraction);
        EnemyPresentation directPresentation;
        const auto direct = CombatPlayback::samplePair(first, last, fraction, &directPresentation);
        REQUIRE(direct);
        const f32 offset = fraction * 4;
        const auto whole = static_cast<u64>(offset);
        EnemyPresentation networkPresentation;
        const auto network =
            playback.sample(10 + whole, offset - static_cast<f32>(whole), &networkPresentation);
        REQUIRE(network);
        REQUIRE(CombatPacket::encode(*network));
        CHECK(CombatPacket::encode(*direct) == CombatPacket::encode(*network));
        CHECK(directPresentation.matches(*direct));
        CHECK(networkPresentation.matches(*network));
        CHECK(direct->projectiles.size() == (fraction < 1 ? 1 : 2));
    }
}

TEST_CASE("native G1 stage capture includes its portals fixtures pickups and all party seats",
          "[netplay][stage-presentation][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G1");
    REQUIRE(level);
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    GameConfig config;
    GameContext context;
    context.config = &config;
    context.levels = &catalog;
    context.unpackedRoot = root;
    CharacterSave save;
    save.name = "TEST";
    save.autoAttack = false;
    save.progress().health = 1000;
    const std::array party{PartyMember{0, save}, PartyMember{2, save}};
    auto play = std::make_unique<PlayScene>();
    REQUIRE(play->open(device, context, world, party, {.welcome = false}));
    ProjectileResources projectiles;
    PickupResources pickups;
    FixtureResources fixtures;
    FighterResources fighters;
    REQUIRE(play->bindReplicationResources(projectiles, pickups, fixtures));
    REQUIRE(fighters.bind(play->critters(), play->bosses()));
    for (s32 tick = 0; tick < 400; ++tick) {
        REQUIRE(play->update(1.0 / 60, {}) == PlayOutcome::Running);
    }
    REQUIRE_FALSE(play->spawning());
    MotionSnapshot motion;
    motion.epoch = motion.cameraContinuity = 1;
    motion.tick = 400;
    motion.camera = play->viewCamera();
    for (const auto& player : play->participants()) {
        motion.players[static_cast<usize>(player.actor.player())] =
            SeatMotion{1, 1, player.actor.position(), player.actor.yaw()};
    }
    const auto authority =
        CombatCapture::capture(motion, play->participants(), play->enemies(), nullptr, &world);
    REQUIRE(authority);
    const StageCombatPresentation::Sources sources{
        play->participants(),
        world,
        play->enemies(),
        play->missiles(),
        play->enemyMissiles(),
        play->effects(),
        {play->chests(), play->gates(), world.triggers(), play->generators(), play->barrels(),
         play->traps(), play->safeRocks(), play->rubble(), &play->statues(), &play->portals()},
        play->critters(),
        play->bosses()};
    const auto textures = device.texturesCreated;
    const auto captured = StageCombatPresentation::capture(
        *authority, motion, sources, {projectiles, pickups, fixtures, fighters});
    REQUIRE(captured);
    REQUIRE(captured->geometry);
    CHECK(captured->players[0]);
    CHECK(captured->players[2]);
    CHECK_FALSE(captured->pickups.empty());
    CHECK_FALSE(captured->fixtures.empty());
    CHECK(std::ranges::any_of(captured->fixtures, [](const auto& value) {
        return value.source == FixtureSource::Portal;
    }));
    CHECK(device.texturesCreated == textures);
    const auto noPortals = StageCombatPresentation::Sources{
        sources.party,
        sources.world,
        sources.enemies,
        sources.playerMissiles,
        sources.enemyMissiles,
        sources.effects,
        {play->chests(), play->gates(), world.triggers(), play->generators(), play->barrels(),
         play->traps(), play->safeRocks(), play->rubble(), &play->statues()},
        sources.critters,
        sources.bosses};
    CHECK_FALSE(StageCombatPresentation::capture(*captured, motion, noPortals,
                                                 {projectiles, pickups, fixtures, fighters}));
}
} // namespace
