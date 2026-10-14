#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/players/CursorAim.h"
#include "game/screens/CombatCapture.h"
#include "game/screens/ReplicaView.h"
#include "game/screens/StageParticlePresentation.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("replica viewport uses the local display without host aspect masks",
          "[netplay][replica-view]") {
    const auto wide = ReplicaView::viewport(1920, 1080);
    REQUIRE(wide);
    CHECK(wide->x == 0);
    CHECK(wide->y == 0);
    CHECK(wide->width == Approx(1920));
    CHECK(wide->height == Approx(1080));
    const auto narrow = ReplicaView::viewport(800, 600);
    REQUIRE(narrow);
    CHECK(narrow->x == 0);
    CHECK(narrow->y == 0);
    CHECK(narrow->width == Approx(800));
    CHECK(narrow->height == Approx(600));
    CHECK(ReplicaView::viewport(640, 448) == Rect{0, 0, 640, 448});
    for (const f32 bad : {0.0f, -1.0f, std::numeric_limits<f32>::infinity(),
                          std::numeric_limits<f32>::quiet_NaN()}) {
        CHECK_FALSE(ReplicaView::viewport(bad, 600));
        CHECK_FALSE(ReplicaView::viewport(800, bad));
    }
    CHECK_FALSE(ReplicaView::viewport(65537, 600));
}

TEST_CASE("replica Stage lights shade the complete draw and revoke to borrowed world lighting",
          "[netplay][replica-view][stage-point-lights]") {
    const auto directory = test::sampleLevel("replica-stage-point-lights");
    writeTextFile(directory / "world.json", R"({"objects":[
      {"name":"LIT","position":[0,0,0],"flags":2}]})");
    writeTextFile(directory / "animations.json", R"({"trees":[
      {"name":"FIGURE","nodes":[{"name":"ROOT","object":"WALL","parent":-1,
        "position":[0,0,0]}],"sequences":[{"name":"STILL","frames":1,"frameRate":30}]}]})");
    test::FakeRenderDevice device;
    LevelRef info;
    info.name = "TEST";
    info.directory = ".";
    LevelWorld world;
    REQUIRE(world.load(device, directory, info));
    const std::array baselineLights{PointLight{{0, 3, 0}, {0, .4f, 0}, 20, 2}};
    world.setPointLights(baselineLights);
    world.setAmbientOffset(-.1f);
    ItemArchive archive;
    REQUIRE(archive.load(directory));
    const auto tree = archive.trees.find("FIGURE");
    REQUIRE(tree);
    TreeModel model;
    REQUIRE(model.bind(archive.trees.tree(*tree), archive.models, archive.textures, device));
    ProjectileResources shots;
    REQUIRE(shots.addModel(1, model));
    PickupResources pickups;
    REQUIRE(pickups.bind(device, std::array{&archive}));
    Enemies enemies;
    FixtureResources fixtures;
    MatchContext context;
    context.epoch = context.scene = 1;
    context.owners = {1, 0, 0, 0};
    context.grants = {1, 0, 0, 0};
    auto view = std::make_unique<ReplicaView>();
    REQUIRE(view->begin(context, world, enemies, shots, pickups, fixtures));
    auto snapshot = std::make_unique<CombatSnapshot>();
    snapshot->motion.epoch = snapshot->motion.cameraContinuity = 1;
    snapshot->motion.tick = 10;
    snapshot->motion.camera = {{0, 10, -20}, .3f, 0, 0};
    snapshot->motion.players[0] = SeatMotion{1, 1, {}, 0};
    snapshot->players[0] = PlayerCombatState{};
    snapshot->players[0]->life = ReplicaPlayerLife::InTower;
    snapshot->geometry = world.scene().geometry();
    auto& shot = snapshot->projectiles.emplace_back();
    shot.instance = shot.resource = 1;
    shot.placement = glm::translate(Mat4{1}, Vec3{4, 0, 0});
    auto& pickup = snapshot->pickups.emplace_back();
    pickup.instance = pickup.resource = 1;
    pickup.placement = glm::translate(Mat4{1}, Vec3{2, 0, 0});
    REQUIRE(snapshot->valid());
    REQUIRE(view->show(*snapshot));
    const GameConfig video;
    const auto camera = CameraFrame::of(snapshot->motion.camera);
    const auto clip =
        snapshot->motion.camera.clipTransform(video.horizontalFovRadians(), 640, 448, Mat4{1});
    const auto expected = [&](std::span<const PointLight> points) {
        auto geometry = world.scene();
        geometry.setPointLights(points);
        auto lit = world.lighting();
        auto full = world.fullLighting();
        lit.points.assign(points.begin(), points.end());
        full.points.assign(points.begin(), points.end());
        device.draws.clear();
        geometry.drawOpaque(device, clip, camera, -1, 0);
        pickups.draw(device, pickup, clip, lit, camera, TreeModel::Pass::Opaque);
        shots.draw(device, shot, clip, full, camera, TreeModel::Pass::Opaque);
        REQUIRE(device.draws.size() == 3);
        return device.draws;
    };
    const auto checkDraw = [&](const std::vector<test::RecordedDraw>& native) {
        device.draws.clear();
        view->draw(device, Mat4{1}, 640, 448, 0);
        REQUIRE(device.draws.size() == native.size());
        for (usize row = 0; row < native.size(); ++row) {
            CAPTURE(row);
            CHECK(device.draws[row].vertices == native[row].vertices);
            CHECK(device.draws[row].texture == native[row].texture);
            CHECK(device.draws[row].transform == native[row].transform);
        }
    };
    const auto baseline = expected(baselineLights);
    checkDraw(baseline);
    EffectTrees projectiles, effects;
    std::shared_ptr<const void> lifetime = std::make_shared<const u8>(0);
    std::vector<PointLight> predicted{{{2, 3, 0}, {1, 0, 0}, 20, 2}};
    StageParticlePresentation::Sources sources{world, projectiles, effects};
    sources.lights = predicted;
    EnemyPresentation presentation;
    presentation.epoch = snapshot->motion.epoch;
    presentation.tick = snapshot->motion.tick;
    presentation.shownGeometry = snapshot->geometry;
    presentation.particleTick = snapshot->motion.tick + 1;
    presentation.particles = StageParticlePresentation::capture(
        presentation.epoch, presentation.particleTick, lifetime, sources);
    REQUIRE(presentation.particles);
    const auto illuminated = expected(predicted);
    for (usize row = 0; row < illuminated.size(); ++row) {
        CAPTURE(row);
        REQUIRE(illuminated[row].vertices != baseline[row].vertices);
    }
    const auto textures = device.texturesCreated;
    const auto uploads = device.textureUpdates;
    predicted.front().position = {100, 100, 100};
    predicted.clear(); // the accepted endpoint has its own copy
    for (const f32 alpha : {0.0f, .5f, 1.0f, .25f}) {
        presentation.particleBlend = alpha;
        REQUIRE(view->show(*snapshot, &presentation));
        checkDraw(illuminated);
    }
    CHECK(world.lighting().points.front().position == baselineLights.front().position);
    CHECK(world.lighting().points.front().color == baselineLights.front().color);
    CHECK(world.fullLighting().points.front().color == baselineLights.front().color);
    // Both explicit authoritative replacement and marker expiry remove the
    // speculative illumination before any subsequent geometry/actor draws.
    REQUIRE(view->show(*snapshot));
    checkDraw(baseline);
    REQUIRE(view->show(*snapshot, &presentation));
    checkDraw(illuminated);
    ++presentation.particleTick;
    CHECK_FALSE(view->show(*snapshot, &presentation));
    --presentation.particleTick;
    checkDraw(illuminated); // rejected input cannot partially replace the picture
    lifetime.reset();
    CHECK_FALSE(view->show(*snapshot, &presentation));
    checkDraw(baseline);
    lifetime = std::make_shared<const u8>(0);
    sources.lights = {};
    presentation.particles = StageParticlePresentation::capture(
        presentation.epoch, presentation.particleTick, lifetime, sources);
    REQUIRE(presentation.particles);
    REQUIRE(view->show(*snapshot, &presentation));
    checkDraw(expected({})); // empty is a complete current list, not fallback
    REQUIRE(view->show(*snapshot));
    checkDraw(baseline);
    CHECK(device.texturesCreated == textures);
    CHECK(device.textureUpdates == uploads);
}

TEST_CASE("guest level textures advance with host time and hold without new simulation",
          "[netplay][replica-view][assets][guest-textures]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root));
    Enemies enemies;
    enemies.open(device, root, nullptr, 4, {}, 1);
    ProjectileResources shots;
    PickupResources pickups;
    FixtureResources fixtures;
    MatchContext context;
    context.epoch = context.scene = 1;
    context.owners = {1, 0, 0, 0};
    context.grants = {1, 0, 0, 0};
    ReplicaView view;
    REQUIRE(view.begin(context, world, enemies, shots, pickups, fixtures));
    REQUIRE(view.setPlayer(0, PlayerFigure::load(device, root, {}, false)));
    CombatSnapshot snapshot;
    snapshot.motion.epoch = snapshot.motion.cameraContinuity = 1;
    snapshot.motion.players[0] = SeatMotion{1, 1, {}, 0};
    snapshot.players[0] = PlayerCombatState{};
    snapshot.players[0]->life = ReplicaPlayerLife::InTower;
    snapshot.geometry = world.scene().geometry();
    auto reference = world.scene();
    auto clock = world.textureAnimator();
    REQUIRE(clock.size() > 0);
    bool changed = false;
    for (const u32 tick : {0U, 1U, 17U, 80U, 1U, 70U}) {
        snapshot.motion.tick += 3;
        snapshot.worldTextureFrame = static_cast<f32>(tick);
        REQUIRE(view.show(snapshot));
        view.draw(device, Mat4{1}, 640, 448, 999); // render clock is deliberately unrelated
        clock = world.textureAnimator();
        clock.step(tick);
        clock.apply(reference);
        for (usize index = 0; index < clock.size(); ++index) {
            const auto slot = clock.slot(index);
            CHECK(view.geometry().textureOf(slot) == reference.textureOf(slot));
            CHECK(view.geometry().textureOffset(slot) == reference.textureOffset(slot));
            changed |= reference.textureOf(slot) != world.scene().textureOf(slot) ||
                       reference.textureOffset(slot) != world.scene().textureOffset(slot);
        }
    }
    CHECK(changed);
    CHECK(world.textureAnimator().frame() == 0);
    view.draw(device, Mat4{1}, 640, 448, 5000);
    for (usize index = 0; index < clock.size(); ++index) {
        const auto slot = clock.slot(index);
        CHECK(view.geometry().textureOf(slot) == reference.textureOf(slot));
        CHECK(view.geometry().textureOffset(slot) == reference.textureOffset(slot));
    }
}

TEST_CASE("replica view stays blank until an entire trusted sample can be drawn",
          "[netplay][replica-view][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root));
    world.setPlayerCount(1);
    Enemies enemies;
    enemies.open(device, root, nullptr, 4, {}, 1);
    ProjectileResources resources;
    PickupResources pickups;
    REQUIRE(pickups.bind(device, world.placedItems().archives()));
    FixtureResources fixtures;
    const Generators generators;
    const SafeRocks rocks;
    const Breakables barrels;
    const Traps traps;
    const Rubble rubble;
    REQUIRE(fixtures.bind(device, world.placedItems().archives(), generators, rocks));
    MatchContext context;
    context.epoch = 1;
    context.scene = 1;
    context.owners = {1, 0, 0, 0};
    context.grants = {1, 0, 0, 0};
    ReplicaView view;
    CHECK_FALSE(view.cursorAim(0, {0.5f, 0.5f}));
    const Mat4 projection = glm::orthoLH_ZO(0.0f, 1920.0f, 1080.0f, 0.0f, 0.0f, 1.0f);
    view.draw(device, projection, 1920, 1080, 0);
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws[0].vertices[0].color == Color::black());
    CHECK_FALSE(device.draws[0].state.depthTest);
    const LevelWorld empty;
    CHECK_FALSE(view.begin(context, empty, enemies, resources, pickups, fixtures));
    REQUIRE(view.begin(context, world, enemies, resources, pickups, fixtures));
    CHECK_FALSE(view.begin(context, world, enemies, resources, pickups, fixtures));

    std::array<PlayerRuntime, 1> players;
    auto& player = players[0];
    CharacterSave save;
    save.progress().experience = levelExperience(80);
    save.progress().inventory.addPowerup(powerup::kSpecial, powerup::kPhoenix, 0, 60);
    ItemArchive weapons;
    REQUIRE(weapons.load(root / "WEAPONS"));
    player.actor.spawn(0, save, nullptr, {}, 0);
    player.figure = PlayerFigure::load(device, root, save, false);
    REQUIRE(player.figure);
    player.figure->setCompanionPowerups(device, world.powerups(), save.progress().inventory,
                                        &weapons);
    player.figure->animate(0, 1, 1.0f / 30);
    MotionSnapshot motion;
    motion.epoch = 1;
    motion.tick = 5;
    motion.cameraContinuity = 1;
    motion.camera = {{0, 10, -20}, 0.3f, 0, 0};
    motion.aspect = 4.0f / 3;
    motion.players[0] = SeatMotion{1, 1, {}, 0};
    auto captured = CombatCapture::capture(motion, players, enemies);
    REQUIRE(captured);
    captured->geometry = world.scene().geometry();
    REQUIRE(PickupCapture::append(*captured, world.placedItems(), pickups));
    const Chests chests;
    const LockedGates gates;
    REQUIRE(FixtureCapture::append(
        *captured, fixtures,
        {chests, gates, world.triggers(), generators, barrels, traps, rocks, rubble}));
    REQUIRE_FALSE(captured->fixtures.empty());
    REQUIRE_FALSE(captured->pickups.empty());
    CHECK_FALSE(view.show(*captured)); // do not draw a partial party
    CHECK(view.shown() == nullptr);
    REQUIRE(view.setPlayer(0, PlayerFigure::load(device, root, save, false)));
    CHECK_FALSE(view.show(*captured)); // unknown timed companion cannot trigger file loading
    REQUIRE(view.bindCompanions(device, world.powerups(), &weapons));
    const PartyHud hud;
    captured->hud = HudCapture::capture(players, hud, true);
    REQUIRE(captured->hud);
    CHECK_FALSE(view.show(*captured)); // HUD artwork is also loaded before snapshot admission.
    REQUIRE(view.loadHud(device, root, nullptr));
    GameConfig video;
    video.camera.horizontalFovDegrees = 85;
    SECTION("first gameplay sample with post-processing off") {}
    SECTION("guest enables its own full post-processing chain") {
        video.display.bloom = true;
        video.display.ambientOcclusion = true;
        video.display.depthOfField = true;
    }
    SECTION("host projection changes do not change local presentation") {
        captured->motion.aspect = 21.0f / 9;
        captured->motion.horizontalFov = glm::radians(45.0f);
    }
    SECTION("two quick pauses before the first gameplay sample arrives") {
        context.transition = MatchTransition::Resume;
        ++context.epoch;
        REQUIRE(view.resume(context));
        ++context.epoch;
        REQUIRE(view.resume(context));
        CHECK(view.shown() == nullptr);
        captured->motion.epoch = context.epoch;
    }
    REQUIRE(view.show(*captured));
    CHECK(view.actors().visiblePlayers() == 1);
    CHECK(view.pickupCount() == captured->pickups.size());
    CHECK(view.fixtureCount() == captured->fixtures.size());
    const auto image = CombatPacket::encode(*view.shown());
    const auto textureCount = device.texturesCreated;
    {
        // A structurally valid pose from another tree must be refused before
        // even the viewport geometry changes, not halfway through actor draw.
        auto candidate = *captured;
        ++candidate.motion.tick;
        candidate.geometry->darken = 0.25f;
        EnemyPresentation presentation;
        presentation.epoch = candidate.motion.epoch;
        presentation.tick = candidate.motion.tick;
        presentation.shownGeometry = candidate.geometry;
        TreePose pose;
        player.figure->animator().evaluatePresentation(pose, 1);
        const auto checkpoint = pose.checkpoint();
        REQUIRE(checkpoint);
        presentation.players[0] = EnemyPresentation::PlayerPose{
            candidate.motion.players[0]->grant, candidate.motion.players[0]->continuity,
            candidate.players[0]->life, candidate.players[0]->animation, *checkpoint};
        presentation.players[0]->pose.tree = "OTHER_TREE";
        REQUIRE(presentation.matches(candidate));
        const auto darken = view.geometry().geometry().darken;
        CHECK_FALSE(view.show(candidate, &presentation));
        CHECK(view.geometry().geometry().darken == darken);
        CHECK(CombatPacket::encode(*view.shown()) == image);
        CHECK(device.texturesCreated == textureCount);
    }
    CHECK_FALSE(view.bindCompanions(device, world.powerups(), &weapons));
    CHECK_FALSE(view.loadHud(device, root, nullptr));
    auto invalid = *captured;
    invalid.hud->players[0]->keys = 10;
    CHECK_FALSE(view.show(invalid));
    CHECK(CombatPacket::encode(*view.shown()) == image);
    invalid = *captured;
    invalid.hud->hourglass = HudHourglass{0.25f, 0};
    REQUIRE(invalid.valid());
    CHECK_FALSE(view.show(invalid)); // No timer archive: do not load from snapshot reception.
    CHECK(CombatPacket::encode(*view.shown()) == image);
    invalid = *captured;
    invalid.hud->bossBars.push_back({true, false, {256, 256}});
    REQUIRE(invalid.valid());
    CHECK_FALSE(view.show(invalid)); // No boss layout was loaded for this scene.
    CHECK(CombatPacket::encode(*view.shown()) == image);
    invalid = *captured;
    invalid.players[0]->companions[1]->animation.sequence = 65535;
    CHECK_FALSE(view.show(invalid));
    CHECK(CombatPacket::encode(*view.shown()) == image);
    invalid = *captured;
    invalid.hud->scroll = HudScroll{4095, 0, -1, 255};
    REQUIRE(invalid.valid());
    CHECK_FALSE(view.show(invalid));
    CHECK(CombatPacket::encode(*view.shown()) == image);
    invalid = *captured;
    invalid.hud->help = HudHelp{};
    invalid.hud->help->id = 10; // Not in the native help-message roster.
    REQUIRE(HelpMessages::specOf(10) == nullptr);
    REQUIRE(invalid.valid());
    CHECK_FALSE(view.show(invalid));
    CHECK(CombatPacket::encode(*view.shown()) == image);
    invalid = *captured;
    invalid.geometry.reset();
    CHECK_FALSE(view.show(invalid));
    invalid = *captured;
    ++invalid.geometry->layout;
    CHECK_FALSE(view.show(invalid));
    invalid = *captured;
    invalid.motion.players[0]->grant = 2;
    CHECK_FALSE(view.show(invalid));
    invalid = *captured;
    invalid.players[0]->animation.sequence = 65535;
    CHECK_FALSE(view.show(invalid));
    invalid = *captured;
    invalid.motion.tick = 4;
    CHECK_FALSE(view.show(invalid));
    invalid = *captured;
    invalid.motion.epoch = 2;
    CHECK_FALSE(view.show(invalid));
    invalid = *captured;
    invalid.pickups[0].resource = 65535;
    CHECK_FALSE(view.show(invalid));
    CHECK(view.pickupCount() == captured->pickups.size());
    invalid = *captured;
    invalid.fixtures[0].resource = 65535;
    CHECK_FALSE(view.show(invalid));
    CHECK(view.fixtureCount() == captured->fixtures.size());
    invalid = *captured;
    TransporterState pad;
    pad.instance = 1;
    pad.resource = 65535;
    invalid.transporters.push_back(pad);
    REQUIRE(invalid.valid());
    CHECK(view.rejection(invalid) == "transporter resources");
    CHECK_FALSE(view.show(invalid));
    CHECK(CombatPacket::encode(*view.shown()) == image);
    invalid = *captured;
    FighterMeshState fighter;
    fighter.incarnation = fighter.part = fighter.resource = 1;
    invalid.fighters.push_back(fighter);
    REQUIRE(invalid.valid());
    CHECK_FALSE(view.show(invalid));
    CHECK(CombatPacket::encode(*view.shown()) == image);
    invalid = *captured;
    ProjectileState shot;
    shot.instance = 1;
    shot.continuity = 1;
    shot.resource = 42;
    invalid.projectiles.push_back(shot);
    REQUIRE(invalid.valid());
    CHECK_FALSE(view.show(invalid));
    invalid = *captured;
    EnemyCombatState enemy;
    enemy.instance = 1;
    enemy.kind = kGruntKind;
    enemy.health = enemy.fullHealth = 50;
    enemy.animation = {0, 0, 1, 0, 1};
    invalid.enemies.push_back(enemy);
    REQUIRE(invalid.valid());
    CHECK_FALSE(view.show(invalid)); // never load a kind in response to a packet
    CHECK_FALSE(enemies.kindLoaded(kGruntKind));
    CHECK(device.texturesCreated == textureCount);
    REQUIRE(CombatPacket::encode(*view.shown()) == image);

    device.draws.clear();
    view.draw(device, projection, 1920, 1080, 0, nullptr, nullptr, video);
    REQUIRE(device.draws.size() > 2);
    const Mat4 expected =
        motion.camera.clipTransform(video.horizontalFovRadians(), 1920, 1080, projection);
    for (const Vec2 cursor : {Vec2{0.2f, 0.3f}, Vec2{0.8f, 0.7f}}) {
        const auto target = view.cursorAim(0, cursor);
        const auto expectedTarget =
            cursorAimDirection(cursor, expected, motion.players[0]->position);
        REQUIRE(target);
        REQUIRE(expectedTarget);
        CHECK(glm::distance(*target, *expectedTarget) < 0.0001f);
    }
    CHECK_FALSE(view.cursorAim(1, {0.5f, 0.5f}));
    CHECK_FALSE(view.cursorAim(255, {0.5f, 0.5f}));
    CHECK_FALSE(view.cursorAim(0, {-1, 0}));
    // Networking may accept the next sample before the next render. Mouse aiming
    // must use both the camera and the player position from the picture on screen.
    const Vec2 pointer{0.8f, 0.7f};
    const auto displayedAim = view.cursorAim(0, pointer);
    auto pending = *captured;
    pending.motion.players[0]->position += Vec3{15, 0, 20};
    pending.motion.camera.position += Vec3{7, 0, 0};
    REQUIRE(view.show(pending));
    CHECK(view.cursorAim(0, pointer) == displayedAim);
    REQUIRE(view.show(*captured));
    for (s32 column = 0; column < 4; ++column) {
        for (s32 row = 0; row < 4; ++row) {
            CHECK(device.draws.front().transform[column][row] ==
                  Approx(expected[column][row]).margin(0.0001f));
        }
    }
    CHECK(device.draws.back().state.depthTest);
    const Mat4 hudProjection = makeVirtualScreenTransform(projection, 512, 384, 1920, 1080);
    CHECK(device.draws.back().transform == hudProjection);
    if (video.display.bloom) {
        REQUIRE(device.bloomDrawOffsets.size() == 1);
        REQUIRE(device.ambientOcclusionDrawOffsets.size() == 1);
        REQUIRE(device.depthOfFieldDrawOffsets.size() == 1);
        CHECK(device.ambientOcclusionDrawOffsets[0] < device.bloomDrawOffsets[0]);
        CHECK(device.bloomDrawOffsets[0] == device.depthOfFieldDrawOffsets[0]);
        CHECK(device.depthOfFieldDrawOffsets[0] < device.draws.size());
        const Mat4 clipToView = motion.camera.view() * glm::inverse(expected);
        CHECK(device.ambientOcclusionSettings[0].clipToView == clipToView);
        CHECK(device.depthOfFieldSettings[0].clipToView == clipToView);
        const f32 distance = (motion.camera.view() * Vec4{motion.players[0]->position, 1}).z;
        CHECK(device.depthOfFieldSettings[0].focusEnd == Approx(std::max(20.0f, distance + 15)));
    } else {
        CHECK(device.bloomDrawOffsets.empty());
        CHECK(device.ambientOcclusionDrawOffsets.empty());
        CHECK(device.depthOfFieldDrawOffsets.empty());
    }
    CHECK(CombatPacket::encode(*view.shown()) == image); // Video choices cannot change simulation.
    CHECK(device.texturesCreated == textureCount);
    CHECK(enemies.count() == 0);
    CHECK(enemies.takeBlows().empty());
    {
        EffectTrees projectiles, effects;
        auto lifetime = std::make_shared<const u8>(0);
        EnemyPresentation presentation;
        presentation.epoch = captured->motion.epoch;
        presentation.tick = captured->motion.tick;
        presentation.shownGeometry = captured->geometry;
        presentation.particleTick = captured->motion.tick + 1;
        presentation.particleBlend = 0.5f;
        presentation.particles = StageParticlePresentation::capture(
            presentation.epoch, presentation.particleTick, lifetime,
            {world, projectiles, effects, nullptr, {}});
        REQUIRE(presentation.particles);
        REQUIRE(view.show(*captured, &presentation));
        ++presentation.particleTick;
        CHECK_FALSE(view.show(*captured, &presentation));
        --presentation.particleTick;
        lifetime.reset();
        CHECK_FALSE(view.show(*captured, &presentation));
        CHECK(CombatPacket::encode(*view.shown()) == image);
        view.draw(device, projection, 1920, 1080, 0); // expired artwork is never dereferenced
        REQUIRE(view.show(*captured)); // authoritative-only pictures drop the old sidecar
    }
    auto resumed = context;
    resumed.epoch = captured->motion.epoch + 1;
    resumed.transition = MatchTransition::Resume;
    auto wrongScene = resumed;
    ++wrongScene.scene;
    CHECK_FALSE(view.resume(wrongScene));
    auto wrongGrant = resumed;
    ++wrongGrant.grants[0];
    CHECK_FALSE(view.resume(wrongGrant));
    CHECK_FALSE(view.begin(resumed, world, enemies, resources, pickups, fixtures));
    const auto* figure = view.actors().playerFigure(0);
    REQUIRE(view.resume(resumed));
    CHECK_FALSE(view.show(*captured));
    CHECK(CombatPacket::encode(*view.shown()) == image);
    auto first = *captured;
    first.motion.epoch = resumed.epoch;
    first.motion.tick = 0;
    invalid = first;
    invalid.fixtures[0].resource = 65535;
    CHECK_FALSE(view.show(invalid));
    CHECK(CombatPacket::encode(*view.shown()) == image);
    REQUIRE(view.show(first));
    CHECK(view.actors().playerFigure(0) == figure);
    CHECK(view.actors().visiblePlayers() == 1);
    CHECK(device.texturesCreated == textureCount);
    view.clear();
    CHECK_FALSE(view.cursorAim(0, {0.5f, 0.5f}));
    CHECK(view.shown() == nullptr);
    CHECK(view.actors().visiblePlayers() == 0);
    device.draws.clear();
    view.draw(device, projection, 1920, 1080, 0);
    CHECK(device.draws.size() == 1);
}
} // namespace
