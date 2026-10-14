#include <array>
#include <limits>
#include <memory>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"
#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/CombatCapture.h"
#include "game/screens/PlayerRuntime.h"
#include "game/screens/ReplicaActors.h"
#include "game/screens/ReplicaProjectiles.h"
#include "game/screens/StageParticlePresentation.h"
#include "game/screens/StageSnapshotSampling.h"
#include "game/world/LevelWorld.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

std::unique_ptr<CombatSnapshot> picture(u64 tick) {
    auto result = std::make_unique<CombatSnapshot>();
    result->motion.epoch = result->motion.cameraContinuity = 1;
    result->motion.tick = tick;
    result->geometry = SceneGeometry{1, 3, 0, {}};
    REQUIRE(result->valid());
    return result;
}
void sameDraws(const std::vector<test::RecordedDraw>& actual,
               const std::vector<test::RecordedDraw>& expected, bool sameTextures = true) {
    REQUIRE(actual.size() == expected.size());
    for (usize i = 0; i < actual.size(); ++i) {
        CAPTURE(i);
        if (sameTextures) {
            CHECK(actual[i].texture == expected[i].texture);
        }
        CHECK(actual[i].state.blend == expected[i].state.blend);
        CHECK(actual[i].state.depthWrite == expected[i].state.depthWrite);
        REQUIRE(actual[i].vertices.size() == expected[i].vertices.size());
        for (s32 column = 0; column < 4; ++column) {
            CHECK(glm::length(actual[i].transform[column] - expected[i].transform[column]) <
                  0.0001f);
        }
        for (usize vertex = 0; vertex < actual[i].vertices.size(); ++vertex) {
            CHECK(glm::distance(actual[i].vertices[vertex].position,
                                expected[i].vertices[vertex].position) < 0.0001f);
            CHECK(actual[i].vertices[vertex].color == expected[i].vertices[vertex].color);
        }
    }
}

struct EffectFixture {
    test::FakeRenderDevice device;
    ItemArchive archive;
    LevelWorld world;
    EffectTrees effects, empty;
    PlayerMissiles playerMissiles;
    EnemyMissiles enemyMissiles;
    ProjectileResources resources;
    std::shared_ptr<const void> lifetime = std::make_shared<const u8>(0);
    EffectFixture() {
        const auto root = test::sampleLevel("stage-native-roster");
        writeTextFile(root / "world.json", R"({"objects":[
            {"name":"FLOOR","position":[0,0,0],"flags":4}]})");
        writeTextFile(root / "animations.json", R"({
          "particles":[{"preset":2,"enables":16929,"texture":"STONE",
            "particleLife":[0.5,0],"rate":[30,30,30,30]}],
          "trees":[{"name":"FIRST","nodes":[
            {"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]},
            {"name":"FIRE","particle":0,"parent":0,"position":[0,1,0]}],"sequences":[]},
            {"name":"SECOND","nodes":[
            {"name":"ROOT","object":"WALL","parent":-1,"position":[0,2,0]},
            {"name":"FIRE","particle":0,"parent":0,"position":[0,1,0]}],"sequences":[]}]})");
        REQUIRE(archive.load(root));
        REQUIRE(world.load(device, root, {.name = "TEST", .directory = "."}));
        REQUIRE(resources.addTree(1, archive, "FIRST", device));
        REQUIRE(resources.addTree(2, archive, "SECOND", device));
    }
    std::unique_ptr<CombatSnapshot> capture(u64 tick) const {
        auto result = picture(tick);
        REQUIRE(
            ProjectileCapture::append(*result, resources, playerMissiles, enemyMissiles, &effects));
        return result;
    }
    void compare(const CombatSnapshot& before, const CombatSnapshot& current) {
        const auto sidecar = StageParticlePresentation::capture(1, current.motion.tick, lifetime,
                                                                {world, empty, effects});
        REQUIRE(sidecar);
        const auto uploads = device.texturesCreated;
        const auto updates = device.textureUpdates;
        for (const f32 alpha : {0.0f, 0.5f, 1.0f}) {
            CAPTURE(alpha);
            EnemyPresentation presentation;
            const auto shown = StageSnapshotSampling::sample(before, current, alpha, &presentation);
            REQUIRE(shown);
            REQUIRE(presentation.matches(*shown));
            REQUIRE(shown->projectiles.size() == current.projectiles.size());
            if (!current.projectiles.empty()) {
                CHECK(shown->projectiles[0].resource == current.projectiles[0].resource);
            }
            const CameraFrame camera;
            device.draws.clear();
            effects.draw(device, Mat4{1}, {}, &camera, alpha);
            const auto expected = device.draws;
            ReplicaProjectiles replica;
            REQUIRE(replica.begin(1));
            REQUIRE(replica.show(*shown, resources));
            device.draws.clear();
            replica.draw(device, resources, Mat4{1}, {}, camera, TreeModel::Pass::All, false);
            REQUIRE(sidecar->draw(StageParticlePresentation::Phase::Effects, device, Mat4{1},
                                  camera, alpha));
            sameDraws(device.draws, expected);
        }
        CHECK(device.texturesCreated == uploads);
        CHECK(device.textureUpdates == updates);
    }
};
} // namespace

TEST_CASE("native stage effect births removals and morphs keep meshes with their particle owner",
          "[netplay][stage-native-sampling][stage-particle-presentation]") {
    auto fixture = std::make_unique<EffectFixture>();
    const auto empty = fixture->capture(10);
    EffectTrees::Setting setting;
    setting.seconds = 3;
    SECTION("birth and retirement") {
        const auto id = fixture->effects.startSet(fixture->device, fixture->archive, "FIRST",
                                                  {3, 4, 5}, setting);
        REQUIRE(id != 0);
        fixture->effects.update(1.0f / 30);
        const auto born = fixture->capture(11);
        REQUIRE(born->projectiles.size() == 1);
        fixture->compare(*empty, *born);
        fixture->effects.capturePresentation();
        fixture->effects.stop(id);
        fixture->effects.update(1.0f / 60);
        const auto retired = fixture->capture(12);
        CHECK(retired->projectiles.empty());
        fixture->compare(*born, *retired);
    }
    SECTION("same instance morph") {
        setting.then = "SECOND";
        setting.morphIn = 0.05f;
        REQUIRE(fixture->effects.startSet(fixture->device, fixture->archive, "FIRST", {3, 4, 5},
                                          setting) != 0);
        fixture->effects.update(1.0f / 30);
        const auto first = fixture->capture(11);
        REQUIRE(first->projectiles.size() == 1);
        fixture->effects.capturePresentation();
        fixture->effects.update(1.0f / 30);
        const auto morphed = fixture->capture(12);
        REQUIRE(morphed->projectiles.size() == 1);
        CHECK(first->projectiles[0].instance == morphed->projectiles[0].instance);
        REQUIRE(first->projectiles[0].resource != morphed->projectiles[0].resource);
        fixture->compare(*first, *morphed);
    }
}

TEST_CASE("native stage sampling uses current discrete owners and compatible continuous fields",
          "[netplay][stage-native-sampling]") {
    auto previous = picture(10);
    auto current = picture(11);
    ProjectileState shot;
    shot.source = ProjectileSource::WorldEffect;
    shot.resource = shot.instance = 1;
    shot.animation = {0, 0, 1, 1, 1};
    previous->projectiles.push_back(shot);
    shot.placement[3].x = 4;
    shot.animation.frame = 3;
    shot.alpha = 0.25f;
    current->projectiles.push_back(shot);
    previous->geometry->objects = {{0}, {1}};
    current->geometry->objects = {{1}, {2}};
    current->geometry->objects[0].local[3].x = 4;
    current->geometry->objects[0].visible = false;
    current->geometry->objects[1].visible = false;
    FixtureState fixture;
    fixture.source = FixtureSource::Switch;
    fixture.instance = fixture.resource = 1;
    fixture.pose = {0, 0, 1, 1, 1};
    previous->fixtures.push_back(fixture);
    fixture.placement[3].x = 4;
    fixture.pose.frame = 3;
    current->fixtures.push_back(fixture);
    for (const f32 alpha : {0.0f, 0.5f, 1.0f}) {
        CAPTURE(alpha);
        const auto result = StageSnapshotSampling::sample(*previous, *current, alpha);
        REQUIRE(result);
        CHECK(result->projectiles[0].placement[3].x == Approx(4 * alpha));
        CHECK(result->projectiles[0].animation.frame == Approx(1 + 2 * alpha));
        CHECK(result->projectiles[0].alpha == 0.25f); // native opacity is current, not delayed
        REQUIRE(result->geometry->objects.size() == 2);
        CHECK(result->geometry->objects[0].index == 1);
        CHECK(result->geometry->objects[0].local[3].x == Approx(4 * alpha));
        CHECK_FALSE(result->geometry->objects[0].visible);
        CHECK(result->geometry->objects[1].index == 2);
        CHECK_FALSE(result->geometry->objects[1].visible);
        CHECK(result->fixtures[0].placement[3].x == 4); // native switches draw discretely
        CHECK(result->fixtures[0].pose.frame == 3);
    }
    current->projectiles[0].animation.generation = 2;
    current->projectiles[0].animation.frame = 0;
    const auto cut = StageSnapshotSampling::sample(*previous, *current, 0.5f);
    REQUIRE(cut);
    CHECK(cut->projectiles[0].animation.generation == 2);
    CHECK(cut->projectiles[0].animation.frame == 0);
    // The transport sampler deliberately retains its old lower-tick policy.
    const auto delayed = CombatPlayback::samplePair(*previous, *current, 0.5f);
    REQUIRE(delayed);
    CHECK(delayed->projectiles[0].animation.generation == 1);
    CHECK(delayed->geometry->objects[0].index == 0);
}

TEST_CASE("native stage swarm sampling matches real 30 Hz poses on both 60 Hz half steps",
          "[netplay][stage-native-sampling][assets]") {
    test::FakeRenderDevice device;
    const auto root =
        test::assetOrSkip("MONSTERS/GRU/ANIM.PS2").parent_path().parent_path().parent_path();
    Enemies enemies;
    enemies.open(device, root, nullptr, 1, {}, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemyView player;
    player.player = 0;
    player.position = {0, 0, 20};
    player.radius = 1;
    player.height = 6;
    const std::array players{player};
    EnemySpawn spawn;
    spawn.placed = true;
    spawn.algorithm = 0;
    const auto id = enemies.spawn(spawn, players);
    REQUIRE(id);
    for (s32 step = 0; step < 40; ++step) {
        enemies.update(2, 1.0f / 30, players);
    }
    const auto capture = [&](u64 tick) {
        const auto base = picture(tick);
        auto state = CombatCapture::capture(base->motion, {}, enemies);
        REQUIRE(state);
        return std::make_unique<CombatSnapshot>(std::move(*state));
    };
    auto previous = capture(100);
    ReplicaActors replica;
    REQUIRE(replica.begin(1));
    bool firstHalf = false, secondHalf = false, changedGeneration = false;
    for (u64 tick = 101; tick <= 112; ++tick) {
        if (tick == 105) {
            enemies.hurt(*id, {.damage = 1, .player = 0});
        }
        enemies.update(1, 1.0f / 60, players, {}, nullptr, 1, tick >= 110);
        auto current = capture(tick);
        const auto unchanged = enemies.checkpoint();
        REQUIRE(unchanged);
        const auto anchors = StageSnapshotSampling::captureSwarm(enemies);
        REQUIRE(anchors.enemies.size() == 1);
        firstHalf |= anchors.enemies[0].blendStart == 0;
        secondHalf |= anchors.enemies[0].blendStart == 0.5f;
        changedGeneration |=
            previous->enemies[0].animation.generation != current->enemies[0].animation.generation;
        for (const f32 alpha : {0.0f, 0.5f, 1.0f}) {
            CAPTURE(tick, alpha);
            EnemyPresentation provenance;
            const auto sampled =
                StageSnapshotSampling::sample(*previous, *current, alpha, &provenance, &anchors);
            REQUIRE(sampled);
            REQUIRE(provenance.enemies.size() == 1);
            REQUIRE(provenance.enemies[0].nativePose);
            REQUIRE(provenance.matches(*sampled));
            const CameraFrame camera;
            device.draws.clear();
            enemies.draw(device, Mat4{1}, {}, &device.whiteTexture(), nullptr, &camera, alpha);
            const auto expected = device.draws;
            REQUIRE_FALSE(expected.empty());
            const auto uploads = device.texturesCreated;
            const auto updates = device.textureUpdates;
            REQUIRE(replica.show(*sampled, enemies, &provenance));
            device.draws.clear();
            replica.draw(device, enemies, Mat4{1}, {}, camera, 0, &device.whiteTexture());
            sameDraws(device.draws, expected);
            CHECK(device.texturesCreated == uploads);
            CHECK(device.textureUpdates == updates);
            if (tick == 101 && alpha == 0) {
                // Structurally valid local metadata must still bind its native
                // sequence to the trusted artwork before any shown owner changes.
                const auto* tree =
                    enemies.appearanceTree({static_cast<s32>(sampled->enemies[0].kind),
                                            static_cast<s32>(sampled->enemies[0].tier),
                                            static_cast<s32>(sampled->enemies[0].variant), false});
                REQUIRE(tree != nullptr);
                auto malformed = std::make_unique<CombatSnapshot>(*sampled);
                auto invalid = provenance;
                const auto sequence = static_cast<u32>(tree->sequences.size());
                malformed->enemies[0].animation.sequence = sequence;
                invalid.enemies[0].from.animation.sequence = sequence;
                invalid.enemies[0].to.animation.sequence = sequence;
                REQUIRE(malformed->valid());
                REQUIRE(invalid.matches(*malformed));
                CHECK_FALSE(replica.acceptsPresentation(*malformed, enemies, invalid));
                CHECK_FALSE(replica.show(*malformed, enemies, &invalid));
                device.draws.clear();
                replica.draw(device, enemies, Mat4{1}, {}, camera, 0, &device.whiteTexture());
                sameDraws(device.draws, expected);
                auto stale = provenance;
                ++stale.enemies[0].to.animation.generation;
                CHECK_FALSE(stale.matches(*sampled));
            }
        }
        CHECK(enemies.checkpoint() == unchanged);
        previous = std::move(current);
    }
    CHECK(firstHalf);
    CHECK(secondHalf);
    CHECK(changedGeneration);
}

TEST_CASE(
    "native player stage poses retain the exact final transition interval without draw history",
    "[netplay][stage-native-sampling][native-player-presentation][assets]") {
    const s32 character = GENERATE(0, 2, 6);
    CAPTURE(character);
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    Enemies enemies;
    std::array<PlayerRuntime, 1> players;
    auto& player = players[0];
    CharacterSave save;
    save.character = character;
    player.actor.spawn(0, save, nullptr, {}, 0);
    player.figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(player.figure);
    for (s32 warmup = 0; warmup < 24; ++warmup) {
        player.figure->animate(0, 1, 1.0f / 60);
    }
    ReplicaActors replica;
    REQUIRE(replica.begin(1));
    REQUIRE(replica.setPlayer(0, 7, PlayerFigure::load(device, root, save, false)));
    const auto capture = [&](u64 tick) {
        auto where = picture(tick);
        where->motion.players[0] = SeatMotion{7, 1, player.actor.position(), player.actor.yaw()};
        auto captured = CombatCapture::capture(where->motion, players, enemies);
        REQUIRE(captured);
        auto result = std::make_unique<CombatSnapshot>(std::move(*captured));
        result->worldTextureFrame = 12 + static_cast<f32>(tick) * 0.5f;
        return result;
    };
    auto previous = capture(0);
    bool completed = false;
    bool generationCut = false;
    for (u64 tick = 1; tick <= 120; ++tick) {
        // Native locomotion cuts IN without a stance blend. Releasing the
        // stick blends BACK to Ready and eventually crosses transition end.
        player.figure->animate(tick <= 32 ? 1.0f : 0.0f, 1, 1.0f / 60);
        auto current = capture(tick);
        const auto& before = *previous->players[0];
        const auto& after = *current->players[0];
        if (before.animation.generation != after.animation.generation) {
            generationCut = true;
            const auto anchors = StageSnapshotSampling::captureParty(players);
            REQUIRE(anchors);
            EnemyPresentation provenance;
            const auto sampled = StageSnapshotSampling::sample(*previous, *current, 0, &provenance,
                                                               nullptr, &*anchors);
            REQUIRE(sampled);
            REQUIRE(provenance.players[0]);
            TreePose expected;
            player.figure->animator().evaluatePresentation(expected, 0);
            const auto checkpoint = expected.checkpoint();
            REQUIRE(checkpoint);
            CHECK(provenance.players[0]->pose == *checkpoint);
            CHECK(sampled->players[0]->animation.generation == after.animation.generation);
        }
        completed = before.transitionPose && !after.transitionPose &&
                    before.animation.generation == after.animation.generation &&
                    before.animation.action == after.animation.action;
        if (!completed) {
            previous = std::move(current);
            continue;
        }
        CAPTURE(tick);
        REQUIRE(before.animation.transition < 1);
        REQUIRE(after.animation.transition == 1);
        REQUIRE(after.rollback);
        const auto exact = player.figure->animator().checkpoint();
        const auto revision = player.figure->animationRevision();
        const auto anchors = StageSnapshotSampling::captureParty(players);
        REQUIRE(anchors);
        REQUIRE(anchors->players[0]);
        const auto uploads = device.texturesCreated;
        const auto updates = device.textureUpdates;
        const auto camera = CameraFrame::at({0, 20, 30});
        // First draw is fractional on a fresh replica. Then jump backward,
        // repeat and skip fractions without changing the simulation clock.
        for (const f32 alpha : {0.5f, 0.0f, 0.25f, 0.25f, 1.0f, 0.5f}) {
            CAPTURE(alpha);
            EnemyPresentation provenance;
            const auto sampled = StageSnapshotSampling::sample(*previous, *current, alpha,
                                                               &provenance, nullptr, &*anchors);
            REQUIRE(sampled);
            REQUIRE(provenance.players[0]);
            CHECK(provenance.matches(*sampled));
            CHECK(sampled->players[0]->rollback == after.rollback);
            CHECK_FALSE(sampled->players[0]->transitionPose);
            CHECK(sampled->players[0]->animation.transition == 1);
            TreePose expected;
            player.figure->animator().evaluatePresentation(expected, alpha);
            TreePose actual;
            REQUIRE(actual.restore(*player.figure->actionTree(), provenance.players[0]->pose));
            REQUIRE(actual.matrices().size() == expected.matrices().size());
            for (usize node = 0; node < actual.matrices().size(); ++node) {
                for (s32 column = 0; column < 4; ++column) {
                    CHECK(glm::length(actual.matrices()[node][column] -
                                      expected.matrices()[node][column]) < 0.0001f);
                }
            }
            REQUIRE(replica.acceptsPresentation(*sampled, enemies, provenance));
            REQUIRE(replica.show(*sampled, enemies, &provenance));
            device.draws.clear();
            replica.draw(device, enemies, Mat4{1}, {}, camera, sampled->worldTextureFrame, nullptr);
            const auto observed = device.draws;
            REQUIRE_FALSE(observed.empty());
            device.draws.clear();
            player.figure->draw(device, Mat4{1}, Mat4{1}, {}, 1, false, &camera, alpha);
            // Independent native/replica figures own different texture handles.
            sameDraws(observed, device.draws, false);
        }
        CHECK(player.figure->animator().checkpoint() == exact);
        CHECK(player.figure->animationRevision() == revision);
        CHECK(device.texturesCreated == uploads);
        CHECK(device.textureUpdates == updates);
        // Missing native endpoints cannot pair the fractional transitioning
        // cursor with a completed rollback checkpoint/source-less pose.
        const auto conservative = StageSnapshotSampling::sample(*previous, *current, 0.5f);
        REQUIRE(conservative);
        CHECK(conservative->players[0]->animation.transition == 1);
        break;
    }
    REQUIRE(completed);
    REQUIRE(generationCut);
}

TEST_CASE("native party anchors reject mismatched seats and expire with their stage resources",
          "[netplay][stage-native-sampling][native-player-presentation][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root));
    std::array<PlayerRuntime, 1> players;
    auto& player = players[0];
    player.actor.spawn(2, {}, nullptr, {}, 0);
    player.figure = PlayerFigure::load(device, root, {}, false);
    REQUIRE(player.figure);
    player.figure->animate(1, 1, 1.0f / 60);
    const auto exact = player.figure->animator().checkpoint();
    const EffectTrees empty;
    auto lifetime = std::shared_ptr<const void>{std::make_shared<const u8>(0)};
    const auto sidecar = StageParticlePresentation::capture(
        1, 1, lifetime, {world, empty, empty, nullptr, {}, true, nullptr, players});
    REQUIRE(sidecar);
    REQUIRE(sidecar->party() != nullptr);
    CHECK_FALSE(sidecar->party()->players[0]);
    REQUIRE(sidecar->party()->players[2]);
    CHECK(player.figure->animator().checkpoint() == exact);
    lifetime.reset();
    CHECK_FALSE(sidecar->matches(1, 1));
    CHECK(sidecar->party() == nullptr);
    std::array<PlayerRuntime, 2> duplicate;
    duplicate[0].actor.spawn(2, {}, nullptr, {}, 0);
    duplicate[1].actor.spawn(2, {}, nullptr, {}, 0);
    CHECK_FALSE(StageSnapshotSampling::captureParty(duplicate));
}

TEST_CASE("native stage sampling refuses malformed fractions without publishing provenance",
          "[netplay][stage-native-sampling]") {
    const auto previous = picture(10);
    const auto current = picture(11);
    for (const f32 alpha : {-0.1f, 1.1f, std::numeric_limits<f32>::quiet_NaN()}) {
        EnemyPresentation provenance;
        provenance.epoch = 7;
        CHECK_FALSE(StageSnapshotSampling::sample(*previous, *current, alpha, &provenance));
        CHECK(provenance.epoch == 0);
    }
}

TEST_CASE("native stage uses current enemy life pickup and fixture rosters at every fraction",
          "[netplay][stage-native-sampling]") {
    const auto previous = picture(10);
    const auto current = picture(11);
    EnemyCombatState enemy;
    enemy.instance = 1;
    enemy.health = enemy.fullHealth = 10;
    enemy.animation = {0, 0, 1, 2, 1};
    previous->enemies.push_back(enemy);
    enemy.life = ReplicaEnemyLife::Dying;
    enemy.health = 0;
    enemy.position.x = 5;
    enemy.animation = {static_cast<u32>(EnemyAction::Dying), 0, 2, 0, 1};
    current->enemies.push_back(enemy);
    enemy.instance = 2;
    previous->enemies.push_back(enemy); // removed in current, not held for another draw
    PickupState pickup;
    pickup.instance = pickup.resource = 1;
    previous->pickups.push_back(pickup);
    pickup.instance = 2;
    current->pickups.push_back(pickup);
    FixtureState fixture;
    fixture.instance = fixture.resource = 1;
    previous->fixtures.push_back(fixture);
    fixture.source = FixtureSource::ChestPreview;
    current->fixtures.push_back(fixture);
    for (const f32 alpha : {0.0f, 0.5f, 1.0f}) {
        const auto sampled = StageSnapshotSampling::sample(*previous, *current, alpha);
        REQUIRE(sampled);
        REQUIRE(sampled->enemies.size() == 1);
        CHECK(sampled->enemies[0].life == ReplicaEnemyLife::Dying);
        CHECK(sampled->enemies[0].health == 0);
        CHECK(sampled->enemies[0].position.x == 5);
        CHECK(sampled->enemies[0].animation.generation == 2);
        REQUIRE(sampled->pickups.size() == 1);
        CHECK(sampled->pickups[0].instance == 2);
        REQUIRE(sampled->fixtures.size() == 1);
        CHECK(sampled->fixtures[0].source == FixtureSource::ChestPreview);
    }
}
