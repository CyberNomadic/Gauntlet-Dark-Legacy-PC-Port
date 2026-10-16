#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <numeric>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>

#include "engine/world/WorldStateCheckpoint.h"

#include "FakeRenderDevice.h"
#include "SampleLevel.h"

namespace {
using namespace gdl;

TextureAnimationInfo cycle(std::string_view name, s32 texture, s32 source, s32 frames, s32 rate,
                           std::string_view frameName = "") {
    TextureAnimationInfo info;
    info.name = name;
    info.frameName = frameName;
    info.texture = texture;
    info.source = source;
    info.frames = frames;
    info.rate = rate;
    return info;
}

struct Fixture {
    test::FakeRenderDevice device;
    ModelSet models;
    TextureSet textures, lender;
    WorldLayout layout;
    WorldScene scene;
    WorldCollision collision;
    WorldAnimator animator;
    TextureAnimator textureClock;
    std::shared_ptr<const u8> binding = std::make_shared<const u8>(0);
    WorldStateCheckpoint checkpoint{scene, collision, animator, binding};

    explicit Fixture(std::string_view name, s32 variant = 0) {
        const auto dir = test::sampleLevel(name);
        if (variant == 3 || variant == 4) {
            auto authored = nlohmann::json::parse(readTextFile(dir / "world.json"));
            if (variant == 3) {
                authored["animations"][0]["track"]["values"][1] = 0.5f;
            } else {
                authored["objects"][0]["radius"] = 7.0f;
            }
            writeTextFile(dir / "world.json", authored.dump());
        }
        REQUIRE(models.load(dir));
        REQUIRE(textures.load(dir));
        REQUIRE(lender.load(test::sampleLender(std::string(name) + "-lender")));
        REQUIRE(layout.load(dir));
        const std::array<TextureSet*, 1> lenders{&lender};
        REQUIRE(scene.build(layout, models, textures, device, {}, lenders));
        animator.bind(layout);
        animator.apply(scene);
        std::vector<CollisionTriangle> triangles{
            {{0, 1, 0}, {Vec3{-2, 0, -2}, Vec3{2, 0, -2}, Vec3{0, 0, 2}}, 8, 4},
            {{0, 1, 0}, {Vec3{-2, 0, -2}, Vec3{2, 0, -2}, Vec3{0, 0, 2}}, 1000, 4}};
        if (variant == 1 || variant == 2) {
            triangles[variant == 1 ? 1 : 0].vertices[0].x -= 0.25f;
        }
        collision.build(std::move(triangles));
        const std::array<s32, 1> moving{8};
        collision.setMovingObjects(moving);
        collision.setObjectTransform(8, scene.worldTransform(8));
        std::array animations{cycle("TORCHB", 3, TextureAnimationInfo::kByName, 2, 2, "TORCH00"),
                              cycle("STONE", 0, 1, 2, 1),
                              cycle("GLASS", 1, TextureAnimationInfo::kScrollU, -4, 0)};
        if (variant == 5) {
            animations[0].rate = 3;
        }
        if (variant == 6) {
            animations[0].start = 1;
        }
        if (variant == 7) {
            animations[2].frames = 4;
        }
        if (variant == 8) {
            animations[0].frameName = "TORCH01";
        }
        textureClock.bind(animations, textures, device, lenders);
        textureClock.apply(scene);
        REQUIRE(checkpoint.bind(textureClock, layout));
    }

    void step(f32 seconds, u32 tick) {
        scene.capturePresentation();
        animator.step(seconds, scene);
        collision.setObjectTransform(8, scene.worldTransform(8));
        textureClock.advance(seconds);
        textureClock.apply(scene);
        scene.setObjectAlpha(9, static_cast<f32>(tick % 10) / 10);
        REQUIRE(scene.setObjectVisible(11, tick % 3 != 0));
        scene.setDarken(static_cast<f32>(tick % 5) / 10);
        const std::array<PointLight, 1> lights{
            {{{10, 1, 50}, {1, 0.5f, 0.25f}, 4, static_cast<f32>(tick % 7)}}};
        scene.setPointLights(lights);
        collision.setSolid(1000, tick % 2 == 0);
        collision.setContactOnly(1000, tick % 3 == 0);
        collision.setFloorExitBlocked(1000, tick % 5 != 0);
    }

    WorldStateCheckpoint::State state() const {
        auto result = checkpoint.checkpoint();
        REQUIRE(result);
        return std::move(*result);
    }
};

void sameDraw(Fixture& first, Fixture& second, f32 alpha) {
    first.device.draws.clear();
    second.device.draws.clear();
    const CameraFrame camera = CameraFrame::at(Vec3{10, 4, 0});
    first.scene.draw(first.device, Mat4{1}, camera, alpha, -0.25f);
    second.scene.draw(second.device, Mat4{1}, camera, alpha, -0.25f);
    REQUIRE(first.device.draws.size() == second.device.draws.size());
    for (usize i = 0; i < first.device.draws.size(); ++i) {
        const auto& a = first.device.draws[i];
        const auto& b = second.device.draws[i];
        CHECK(a.transform == b.transform);
        CHECK(a.state.uvOffset == b.state.uvOffset);
        CHECK(a.state.darken == b.state.darken);
        REQUIRE(a.vertices.size() == b.vertices.size());
        for (usize j = 0; j < a.vertices.size(); ++j) {
            CHECK(a.vertices[j].position == b.vertices[j].position);
            CHECK(a.vertices[j].color == b.vertices[j].color);
        }
    }
}

TEST_CASE("native world history restores collision animation masks and rendered geometry together",
          "[local-rewind][world-local-rewind]") {
    Fixture reference("world-local-reference");
    Fixture replay("world-local-replay");
    struct Frame {
        WorldScene::LocalSnapshot scene;
        WorldCollision::LocalSnapshot collision;
        WorldAnimator::LocalSnapshot animator;
        TextureAnimator::Clock textures;
    };
    std::array<Frame, 16> history;
    std::array<Frame, 8> expected;
    Frame observed;
    const auto step = [](Fixture& pool, u32 tick, bool wrong) {
        if (tick % 11 == 0 || wrong) {
            pool.animator.fire(8, !wrong, wrong);
        }
        if (tick % 7 == 0) {
            pool.animator.cycle(8, !wrong);
        }
        pool.step(1.0f / 60, wrong ? tick + 9 : tick);
        if (tick % 13 == 0 || wrong) {
            const Mat4 transform =
                glm::scale(glm::translate(Mat4{1}, Vec3{0, wrong ? -30.0f : 4.0f, 50}),
                           Vec3{wrong ? 0.0f : 1.0f});
            pool.scene.setObjectTransform(8, transform, true);
            pool.collision.setObjectTransform(8, pool.scene.worldTransform(8));
        }
    };
    u32 tick = 0;
    for (u32 window = 0; window < 128; ++window) {
        CAPTURE(window, tick);
        auto& saved = history[window % history.size()];
        replay.scene.saveLocal(saved.scene);
        replay.collision.saveLocal(saved.collision);
        replay.animator.saveLocal(saved.animator);
        saved.textures = replay.textureClock.clock();
        const u32 distance = window % 8 + 1;
        // Compare native values under the same binding, not resource-normalized DTOs.
        for (u32 i = 0; i < distance; ++i) {
            step(replay, tick + i, false);
            replay.scene.saveLocal(expected[i].scene);
            replay.collision.saveLocal(expected[i].collision);
            replay.animator.saveLocal(expected[i].animator);
        }
        REQUIRE(replay.scene.restoreLocal(saved.scene));
        REQUIRE(replay.collision.restoreLocal(saved.collision));
        REQUIRE(replay.animator.restoreLocal(saved.animator));
        replay.textureClock.restoreClock(saved.textures);
        for (u32 i = 0; i < distance; ++i) {
            step(replay, tick + i, true);
        }
        // A draw must not leave stale composed transforms after correction.
        replay.scene.draw(replay.device, Mat4{1}, CameraFrame{}, 0.5f);
        REQUIRE(replay.scene.canRestoreLocal(saved.scene));
        REQUIRE(replay.collision.canRestoreLocal(saved.collision));
        REQUIRE(replay.animator.canRestoreLocal(saved.animator));
        REQUIRE(replay.scene.restoreLocal(saved.scene));
        REQUIRE(replay.collision.restoreLocal(saved.collision));
        REQUIRE(replay.animator.restoreLocal(saved.animator));
        replay.textureClock.restoreClock(saved.textures);
        CHECK(replay.state() == reference.state());
        sameDraw(reference, replay, 0.37f);
        for (u32 i = 0; i < distance; ++i) {
            step(reference, tick + i, false);
            step(replay, tick + i, false);
            replay.scene.saveLocal(observed.scene);
            replay.collision.saveLocal(observed.collision);
            replay.animator.saveLocal(observed.animator);
            CHECK(replay.scene.equalLocal(observed.scene, expected[i].scene));
            CHECK(replay.collision.equalLocal(observed.collision, expected[i].collision));
            CHECK(replay.animator.equalLocal(observed.animator, expected[i].animator));
            CHECK(replay.state() == reference.state());
            sameDraw(reference, replay, 0.37f);
            const Vec3 from{10, 4, 45}, to{10, 4, 55};
            CHECK(replay.collision.sweepWalls(from, to, 1, 3, 9) ==
                  reference.collision.sweepWalls(from, to, 1, 3, 9));
        }
        tick += distance;
    }
    CHECK(replay.device.textureUpdates == 0);
}

TEST_CASE("native world history rejects foreign owners and changed geometry bindings",
          "[local-rewind][world-local-rewind]") {
    Fixture a("world-local-owner"), b("world-local-other");
    WorldScene::LocalSnapshot scene;
    WorldCollision::LocalSnapshot collision;
    WorldAnimator::LocalSnapshot animator;
    CHECK_FALSE(a.scene.restoreLocal(scene));
    CHECK_FALSE(a.collision.restoreLocal(collision));
    CHECK_FALSE(a.animator.restoreLocal(animator));
    CHECK_FALSE(a.scene.equalLocal(scene, scene));
    CHECK_FALSE(a.collision.equalLocal(collision, collision));
    CHECK_FALSE(a.animator.equalLocal(animator, animator));
    a.scene.saveLocal(scene);
    a.collision.saveLocal(collision);
    a.animator.saveLocal(animator);
    CHECK(a.scene.equalLocal(scene, scene));
    CHECK(a.collision.equalLocal(collision, collision));
    CHECK(a.animator.equalLocal(animator, animator));
    CHECK_FALSE(b.scene.equalLocal(scene, scene));
    CHECK_FALSE(b.collision.equalLocal(collision, collision));
    CHECK_FALSE(b.animator.equalLocal(animator, animator));
    CHECK_FALSE(b.scene.restoreLocal(scene));
    CHECK_FALSE(b.collision.restoreLocal(collision));
    CHECK_FALSE(b.animator.restoreLocal(animator));
    a.collision.append(std::array{CollisionTriangle{.object = 9999}});
    CHECK_FALSE(a.collision.restoreLocal(collision));
    a.collision.saveLocal(collision);
    a.collision.setMovingObjects(std::array<s32, 1>{9999});
    CHECK_FALSE(a.collision.restoreLocal(collision));
    a.scene.clear();
    a.animator.clear();
    CHECK_FALSE(a.scene.equalLocal(scene, scene));
    CHECK_FALSE(a.animator.equalLocal(animator, animator));
    CHECK_FALSE(a.scene.restoreLocal(scene));
    CHECK_FALSE(a.animator.restoreLocal(animator));
}

TEST_CASE("native world history shares static placements without losing diagnostic edits",
          "[local-rewind][world-local-rewind]") {
    Fixture reference("world-local-static-reference"), replay("world-local-static-replay");
    const auto initial = replay.state();
    WorldScene::LocalSnapshot beforeDiagnostic;
    replay.scene.saveLocal(beforeDiagnostic);
    auto shifted = initial;
    shifted.placements[0].local[3].x += 2;
    shifted.placements[0].previous[3].x -= 3;
    shifted.placements[0].continuity = 9;
    REQUIRE_FALSE(replay.scene.moving(0));
    REQUIRE_FALSE(replay.scene.moving(9));
    REQUIRE(replay.scene.moving(8));
    REQUIRE(replay.scene.moving(10)); // The nonanimated blade follows its animated ancestor.
    REQUIRE(reference.checkpoint.restore(shifted));
    REQUIRE(replay.checkpoint.restore(shifted));
    WorldScene::LocalSnapshot unsettled;
    replay.scene.saveLocal(unsettled);
    // A diagnostic restore can recreate the retained static catalog without
    // changing its contents. Native equality must not compare allocation identity.
    REQUIRE(replay.checkpoint.restore(shifted));
    WorldScene::LocalSnapshot sameStaticValues;
    replay.scene.saveLocal(sameStaticValues);
    CHECK(replay.scene.equalLocal(unsettled, sameStaticValues));
    CHECK_FALSE(replay.scene.equalLocal(beforeDiagnostic, unsettled));
    const auto step = [](WorldScene& scene, u32 tick) {
        scene.capturePresentation();
        scene.setObjectTransform(8,
                                 glm::translate(Mat4{1}, Vec3{0, tick, 50}) *
                                     glm::rotate(Mat4{1}, tick * 0.1f, Vec3{0, 1, 0}),
                                 tick % 5 == 0);
        scene.setObjectAlpha(9, static_cast<f32>(tick % 10) / 10);
        REQUIRE(scene.setObjectVisible(11, tick % 3 != 0));
        scene.setTextureOffset(0, {tick * 0.007f, 0});
    };
    step(reference.scene, 1);
    step(replay.scene, 1);
    CHECK(replay.state() == reference.state());
    WorldScene::LocalSnapshot settled;
    replay.scene.saveLocal(settled);
    u32 tick = 2;
    for (u32 distance = 1; distance <= 8; ++distance) {
        CAPTURE(distance);
        WorldScene::LocalSnapshot saved;
        replay.scene.saveLocal(saved);
        for (u32 i = 0; i < distance; ++i) {
            step(replay.scene, tick + 100 + i);
        }
        auto discarded = replay.state();
        discarded.placements[0].local[3].x += 17;
        discarded.placements[0].previous[3].x -= 11;
        REQUIRE(replay.checkpoint.restore(discarded));
        replay.scene.draw(replay.device, Mat4{1}, CameraFrame{}, 0.5f);
        REQUIRE(replay.scene.restoreLocal(saved));
        CHECK(replay.state() == reference.state());
        sameDraw(reference, replay, 0.5f);
        for (u32 i = 0; i < distance; ++i, ++tick) {
            step(reference.scene, tick);
            step(replay.scene, tick);
            CHECK(replay.state() == reference.state());
            sameDraw(reference, replay, 0.5f);
        }
    }
    REQUIRE(replay.scene.restoreLocal(unsettled));
    CHECK(replay.state() == shifted);
    step(replay.scene, 1);
    const auto settledState = replay.state();
    REQUIRE(replay.scene.restoreLocal(settled));
    CHECK(replay.state() == settledState);
    REQUIRE(replay.scene.restoreLocal(beforeDiagnostic));
    CHECK(replay.state() == initial);
    CHECK(replay.scene.worldTransform(0) == initial.placements[0].local);
}

TEST_CASE("native world equality sees latent texture phases query masks and held animation",
          "[local-rewind][world-local-rewind]") {
    Fixture fixture("world-local-hidden-values");
    WorldScene::LocalSnapshot sceneBefore, sceneAfter, sceneRestored;
    WorldCollision::LocalSnapshot collisionBefore, collisionAfter, collisionRestored;
    WorldAnimator::LocalSnapshot animatorBefore, animatorAfter, animatorRestored;
    fixture.scene.saveLocal(sceneBefore);
    fixture.collision.saveLocal(collisionBefore);
    fixture.animator.saveLocal(animatorBefore);
    const auto transform = fixture.scene.worldTransform(8);
    const auto offset = fixture.scene.textureOffset(1);
    fixture.scene.setTextureScroll(1, offset, {0.25f, 0.5f}, {0.01f, 0.02f});
    fixture.scene.saveLocal(sceneAfter);
    CHECK(fixture.scene.textureOffset(1) == offset);
    CHECK_FALSE(fixture.scene.equalLocal(sceneBefore, sceneAfter));
    fixture.collision.setContactOnly(1000, true);
    fixture.collision.setFloorExitBlocked(1000, true);
    fixture.collision.saveLocal(collisionAfter);
    CHECK(fixture.collision.solid(1000));
    CHECK_FALSE(fixture.collision.equalLocal(collisionBefore, collisionAfter));
    fixture.animator.hold(8);
    fixture.animator.saveLocal(animatorAfter);
    CHECK(fixture.scene.worldTransform(8) == transform);
    CHECK_FALSE(fixture.animator.equalLocal(animatorBefore, animatorAfter));
    REQUIRE(fixture.scene.restoreLocal(sceneBefore));
    REQUIRE(fixture.collision.restoreLocal(collisionBefore));
    REQUIRE(fixture.animator.restoreLocal(animatorBefore));
    fixture.scene.saveLocal(sceneRestored);
    fixture.collision.saveLocal(collisionRestored);
    fixture.animator.saveLocal(animatorRestored);
    CHECK(fixture.scene.equalLocal(sceneBefore, sceneRestored));
    CHECK(fixture.collision.equalLocal(collisionBefore, collisionRestored));
    CHECK(fixture.animator.equalLocal(animatorBefore, animatorRestored));
}

TEST_CASE("world chrome coordinates stay finite for absent and collapsed mesh normals",
          "[world][world-chrome][world-local-rewind]") {
    const bool zeroNormal = GENERATE(false, true);
    const bool collapsed = GENERATE(false, true);
    const auto directory = test::sampleLevel("world-chrome-zero-normal");
    writeTextFile(directory / "models/006_LIT.obj",
                  std::string{"v 0 0 0 0.2 0.2 0.2\nv 1 0 0 0.2 0.2 0.2\n"
                              "v 0 1 0 0.2 0.2 0.2\n"} +
                      (zeroNormal ? "vn 0 0 0\n" : "vn 0 1 0\n") +
                      "usemtl tex0\nf 1//1 2//1 3//1\n");
    test::convertModelFixture(directory);
    writeTextFile(directory / "world.json", R"({"objects":[
      {"name":"LIT","position":[0,0,0],"next":-1,"child":-1,
       "flags":4098,"objectFlags":32768}]})");
    test::FakeRenderDevice device;
    ModelSet models;
    TextureSet textures;
    WorldLayout layout;
    WorldScene scene;
    REQUIRE(models.load(directory));
    REQUIRE(textures.load(directory));
    REQUIRE(layout.load(directory));
    REQUIRE(scene.build(layout, models, textures, device));
    REQUIRE(scene.moving(0));
    if (collapsed) {
        scene.setObjectTransform(0, glm::scale(Mat4{1}, Vec3{0}), true);
    }
    scene.draw(device, Mat4{1});
    REQUIRE(device.draws.size() == 1);
    REQUIRE(device.draws.front().vertices.size() == 3);
    const auto index = models.find("LIT");
    REQUIRE(index);
    const auto& mesh = models.mesh(*index);
    REQUIRE(mesh.prelit);
    for (const auto& vertex : device.draws.front().vertices) {
        CHECK(std::isfinite(vertex.uv.x));
        CHECK(std::isfinite(vertex.uv.y));
        CHECK(vertex.color == mesh.vertices.front().color);
        CHECK(vertex.uv == (zeroNormal || collapsed ? Vec2{0.5f} : Vec2{0.5f, 0}));
    }
}

TEST_CASE("Ghost Town world chrome draws contain finite texture coordinates",
          "[assets][world-chrome][world-local-rewind]") {
    const auto directory = test::assetOrSkip("LEVELS/LEVELG2/WORLDS.PS2").parent_path();
    test::FakeRenderDevice device;
    ModelSet models;
    TextureSet textures;
    WorldLayout layout;
    WorldScene scene;
    REQUIRE(models.load(directory));
    REQUIRE(textures.load(directory));
    REQUIRE(layout.load(directory));
    REQUIRE(scene.build(layout, models, textures, device));
    scene.draw(device, Mat4{1}, CameraFrame::at(Vec3{0, 20, 0}), 0.5f);
    REQUIRE_FALSE(device.draws.empty());
    for (usize draw = 0; draw < device.draws.size(); ++draw) {
        const auto& vertices = device.draws[draw].vertices;
        for (usize vertex = 0; vertex < vertices.size(); ++vertex) {
            if (!std::isfinite(vertices[vertex].uv.x) || !std::isfinite(vertices[vertex].uv.y)) {
                CAPTURE(draw, vertex);
                FAIL("non-finite world texture coordinate");
            }
        }
    }
}

TEST_CASE("native level geometry reports reusable local history costs",
          "[.][assets][local-rewind-timing][world-local-rewind-timing]") {
    const auto level = GENERATE("G2", "G4", "L1");
    const auto directory =
        test::assetOrSkip(std::string{"LEVELS/LEVEL"} + level + "/WORLDS.PS2").parent_path();
    test::FakeRenderDevice device;
    device.recordDraws = false;
    ModelSet models;
    TextureSet textures;
    WorldLayout layout;
    WorldScene scene;
    WorldCollision collision;
    WorldAnimator animator;
    REQUIRE(models.load(directory));
    REQUIRE(textures.load(directory));
    REQUIRE(layout.load(directory));
    REQUIRE(scene.build(layout, models, textures, device));
    REQUIRE(collision.load(directory, layout));
    animator.bind(layout);
    animator.apply(scene);
    std::vector<s32> moving;
    for (usize i = 0; i < layout.objects().size(); ++i) {
        if (scene.moving(i)) {
            moving.push_back(static_cast<s32>(i));
        }
    }
    collision.setMovingObjects(moving);
    std::cout << level << "_geometry_objects=" << layout.objects().size()
              << " moving_placements=" << moving.size()
              << " static_placements=" << layout.objects().size() - moving.size() << '\n';
    const auto step = [&] {
        scene.capturePresentation();
        animator.step(1.0f / 60, scene);
        for (const s32 object : moving) {
            collision.setObjectTransform(object, scene.worldTransform(static_cast<usize>(object)));
        }
    };
    step();
    struct Frame {
        WorldScene::LocalSnapshot scene;
        WorldCollision::LocalSnapshot collision;
        WorldAnimator::LocalSnapshot animator;
    };
    std::array<Frame, 16> history;
    const auto save = [&](Frame& frame) {
        scene.saveLocal(frame.scene);
        collision.saveLocal(frame.collision);
        animator.saveLocal(frame.animator);
    };
    for (auto& frame : history) {
        save(frame);
    }
    using Clock = std::chrono::steady_clock;
    const auto elapsed = [](auto start) {
        return std::chrono::duration<f64, std::milli>(Clock::now() - start).count();
    };
    std::array<f64, 256> saves{}, restores{}, replays{};
    for (usize i = 0; i < saves.size(); ++i) {
        auto& frame = history[i % history.size()];
        auto start = Clock::now();
        save(frame);
        saves[i] = elapsed(start);
        for (usize j = 0; j < 8; ++j) {
            step();
        }
        start = Clock::now();
        const bool restored = scene.restoreLocal(frame.scene) &&
                              collision.restoreLocal(frame.collision) &&
                              animator.restoreLocal(frame.animator);
        restores[i] = elapsed(start);
        REQUIRE(restored);
        start = Clock::now();
        for (usize j = 0; j < 8; ++j) {
            step();
        }
        replays[i] = elapsed(start);
    }
    const auto report = [&](std::string_view operation, auto values) {
        std::ranges::sort(values);
        std::cout << level << "_geometry_" << operation << " mean_ms="
                  << std::accumulate(values.begin(), values.end(), 0.0) / values.size()
                  << " p99_ms=" << values[values.size() * 99 / 100] << " max_ms=" << values.back()
                  << '\n';
    };
    report("save", saves);
    report("restore", restores);
    report("replay8", replays);
}

TEST_CASE("world core checkpoints preserve native clocks, preceding poses and appended collision",
          "[world][world-state-checkpoint]") {
    const u32 rate = GENERATE(30u, 60u);
    Fixture authority("world-core-authority");
    Fixture replay("world-core-replay");
    const f32 seconds = 1.0f / static_cast<f32>(rate);
    for (u32 tick = 1; tick <= 7; ++tick) {
        authority.step(seconds, tick);
    }
    const auto baseline = authority.state();
    const auto clock = authority.textureClock.checkpoint();
    const auto texturesBefore = replay.device.texturesCreated;
    REQUIRE(replay.checkpoint.restore(baseline));
    REQUIRE(replay.textureClock.restore(clock));
    CHECK(replay.state() == baseline);
    CHECK(replay.device.texturesCreated == texturesBefore);
    CHECK(replay.scene.textureOf(3) != authority.scene.textureOf(3));
    sameDraw(authority, replay, 0.0f);
    sameDraw(authority, replay, 0.5f);
    sameDraw(authority, replay, 1.0f);
    CHECK(replay.state() == baseline); // Draw caches are not simulation changes.
    for (u32 tick = 8; tick <= rate * 2; ++tick) {
        CAPTURE(rate, tick);
        authority.step(seconds, tick);
        replay.step(seconds, tick);
        CHECK(replay.state() == authority.state());
        CHECK(replay.textureClock.checkpoint() == authority.textureClock.checkpoint());
    }
    REQUIRE(authority.checkpoint.restore(baseline));
    REQUIRE(authority.textureClock.restore(clock));
    REQUIRE(replay.checkpoint.restore(baseline));
    REQUIRE(replay.textureClock.restore(clock));
    CHECK(authority.state() == baseline);
    CHECK(replay.state() == baseline);
    CHECK(replay.device.texturesCreated == texturesBefore);
}

TEST_CASE("world core restore retains cycle outputs and raw masks without redispatch",
          "[world][world-state-checkpoint]") {
    Fixture fixture("world-core-events");
    fixture.step(3.0f / 30.0f, 3);
    REQUIRE_FALSE(fixture.animator.cycleEvents().empty());
    fixture.collision.setSolid(1000, false);
    fixture.collision.setContactOnly(1000, true);
    fixture.collision.setFloorExitBlocked(1000, true);
    const auto baseline = fixture.state();
    REQUIRE(baseline.blockedFloorExits == std::vector<s32>{1000});
    CHECK_FALSE(fixture.collision.floorExitBlocked(1000)); // Hidden masks the public query.
    fixture.step(1.0f / 30.0f, 10);
    REQUIRE(fixture.checkpoint.restore(baseline));
    CHECK(fixture.state() == baseline);
    CHECK(fixture.animator.cycleEvents().size() == baseline.cycles.size());
    fixture.collision.setSolid(1000, true);
    CHECK(fixture.collision.floorExitBlocked(1000));
    CHECK(fixture.collision.contactOnly(1000));
}

TEST_CASE("world core preparations are inert, single-use and bound to the owning catalog",
          "[world][world-state-checkpoint]") {
    Fixture first("world-core-owner-one");
    Fixture second("world-core-owner-two");
    const auto baseline = first.state();
    auto prepared = first.checkpoint.prepareRestore(baseline);
    auto sibling = first.checkpoint.prepareRestore(baseline);
    REQUIRE(prepared);
    REQUIRE(sibling);
    CHECK(first.state() == baseline);
    CHECK_FALSE(second.checkpoint.canCommit(*prepared));
    CHECK_FALSE(second.checkpoint.commitRestore(std::move(*prepared)));
    REQUIRE(first.checkpoint.canCommit(*prepared));
    first.step(1.0f / 30.0f, 1);
    REQUIRE(first.checkpoint.commitRestore(std::move(*prepared)));
    CHECK(first.state() == baseline);
    CHECK_FALSE(first.checkpoint.canCommit(*prepared));
    CHECK_FALSE(first.checkpoint.canCommit(*sibling));
    auto stale = first.checkpoint.prepareRestore(baseline);
    REQUIRE(stale);
    first.binding = std::make_shared<const u8>(0);
    CHECK_FALSE(first.checkpoint.canCommit(*stale));
    CHECK_FALSE(first.checkpoint.checkpoint());
    REQUIRE(first.checkpoint.bind(first.textureClock, first.layout));
    CHECK_FALSE(first.checkpoint.canCommit(*stale));
    auto rebound = first.checkpoint.prepareRestore(baseline);
    REQUIRE(rebound);
    REQUIRE(first.checkpoint.bind(first.textureClock, first.layout));
    CHECK_FALSE(first.checkpoint.canCommit(*rebound));
}

TEST_CASE("world core rejects malformed state before changing any participating owner",
          "[world][world-state-checkpoint]") {
    const u32 mutation = GENERATE(0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u);
    Fixture fixture("world-core-malformed");
    fixture.step(1.0f / 30.0f, 1);
    const auto baseline = fixture.state();
    auto invalid = baseline;
    switch (mutation) {
    case 0: invalid.placements[8].local[3].x = std::numeric_limits<f32>::quiet_NaN(); break;
    case 1: invalid.placements[8].previous[0].w = 1; break;
    case 2: invalid.catalog[0] ^= 1; break;
    case 3: invalid.resourceCount += 1; break;
    case 4: invalid.slots.front().frame = invalid.resourceCount; break;
    case 5: invalid.moving.front().object = 1000; break;
    case 6: invalid.moving.front().transform[0].x = std::numeric_limits<f32>::infinity(); break;
    case 7: invalid.hidden.push_back(2000); break;
    case 8: invalid.animations.front().frames += 1; break;
    case 9: invalid.animations.front().frame = std::numeric_limits<f32>::infinity(); break;
    case 10: invalid.cycles.push_back({1000, true}); break;
    case 11: invalid.hidden = {1000, 1000}; break;
    case 12: invalid.units.front().alpha = 2; break;
    case 13: invalid.light.points.front().radius = -1; break;
    }
    CAPTURE(mutation);
    CHECK_FALSE(fixture.checkpoint.prepareRestore(invalid));
    CHECK_FALSE(fixture.checkpoint.restore(invalid));
    CHECK(fixture.state() == baseline);
}

TEST_CASE("world core refuses unknown texture resources and changed collision topology",
          "[world][world-state-checkpoint]") {
    Fixture fixture("world-core-topology");
    const auto baseline = fixture.state();
    fixture.scene.setTextureFrame(0, &fixture.device.whiteTexture());
    CHECK_FALSE(fixture.checkpoint.checkpoint());
    REQUIRE(fixture.checkpoint.restore(baseline));
    auto pending = fixture.checkpoint.prepareRestore(baseline);
    REQUIRE(pending);
    const std::array extra{
        CollisionTriangle{{0, 1, 0}, {Vec3{10, 0, 10}, Vec3{12, 0, 10}, Vec3{10, 0, 12}}, 2000, 4}};
    fixture.collision.append(extra);
    CHECK_FALSE(fixture.checkpoint.canCommit(*pending));
    CHECK_FALSE(fixture.checkpoint.restore(baseline));
    CHECK_FALSE(fixture.checkpoint.checkpoint());
}

TEST_CASE("world core state rejects bounded rosters without truncating them",
          "[world][world-state-checkpoint]") {
    WorldStateCheckpoint::State state;
    REQUIRE(state.valid());
    state.hidden.resize(WorldStateCheckpoint::State::kMaxObjects + 1);
    CHECK_FALSE(state.valid());
    state = {};
    state.resourceCount = static_cast<u32>(WorldStateCheckpoint::State::kMaxTextures + 1);
    CHECK_FALSE(state.valid());
    state = {};
    state.cycles.resize(WorldStateCheckpoint::State::kMaxEvents + 1);
    CHECK_FALSE(state.valid());
}

TEST_CASE("compact world catalogs reject equal-count but different authored resources",
          "[world][world-state-checkpoint]") {
    const s32 variant = GENERATE(1, 2, 3, 4, 5, 6, 7, 8);
    CAPTURE(variant);
    Fixture source("world-core-catalog-source");
    Fixture other("world-core-catalog-other", variant);
    const auto sourceState = source.state();
    const auto before = other.state();
    CHECK(sourceState.placements.size() == before.placements.size());
    CHECK(sourceState.moving.size() == before.moving.size());
    CHECK(sourceState.animations.size() == before.animations.size());
    CHECK(sourceState.resourceCount == before.resourceCount);
    CHECK(sourceState.catalog != before.catalog);
    CHECK_FALSE(other.checkpoint.prepareRestore(sourceState));
    CHECK_FALSE(other.checkpoint.restore(sourceState));
    CHECK(other.state() == before);
}

TEST_CASE("compact world state keeps every placement and appended collision owner",
          "[world][world-state-checkpoint]") {
    Fixture source("world-core-full-placement");
    Fixture other("world-core-full-placement-other");
    REQUIRE_FALSE(source.scene.moving(0));
    const auto initial = source.state();
    auto shifted = initial;
    shifted.placements[0].local[3].x += 2;
    shifted.placements[0].previous[3].x -= 3;
    shifted.placements[0].continuity = 9;
    REQUIRE(source.checkpoint.restore(shifted));
    CHECK(source.state() == shifted);
    REQUIRE(other.checkpoint.restore(source.state()));
    CHECK(other.state() == shifted);
    CHECK(other.scene.worldTransform(0) == shifted.placements[0].local);
    CHECK(source.checkpoint.hasCollisionObject(8));
    CHECK(source.checkpoint.hasCollisionObject(1000));
    CHECK_FALSE(source.checkpoint.hasCollisionObject(0));
    CHECK_FALSE(source.checkpoint.hasCollisionObject(1001));
    const auto owners = source.checkpoint.collisionObjects();
    CHECK(std::vector<s32>(owners.begin(), owners.end()) == std::vector<s32>{8, 1000});
    REQUIRE(source.checkpoint.restore(initial));
    CHECK(source.state() == initial);
}

TEST_CASE("catalog fingerprints are independent of mutable clocks transforms and masks",
          "[world][world-state-checkpoint]") {
    Fixture fixture("world-core-late-catalog");
    const auto initial = fixture.state();
    for (u32 tick = 1; tick < 20; ++tick) {
        fixture.step(1.0f / 60, tick);
    }
    const auto advanced = fixture.state();
    CHECK(advanced.catalog == initial.catalog);
    REQUIRE(fixture.checkpoint.bind(fixture.textureClock, fixture.layout));
    CHECK(fixture.state() == advanced);
    REQUIRE(fixture.checkpoint.restore(initial));
    CHECK(fixture.state() == initial);
}

TEST_CASE("world texture lookup preserves first authored occurrence and repeated cycle ids",
          "[world][world-state-checkpoint]") {
    Fixture authority("world-core-texture-ids");
    Fixture replay("world-core-texture-ids-other");
    const auto initial = authority.state();
    REQUIRE(replay.state() == initial);

    // Independent linear oracle: native base slots first, then authored cycles.
    // Aliases retain the first id even when their references repeat or reorder.
    std::vector<const Texture*> resources;
    const auto add = [&](const Texture* texture) {
        if (texture && std::ranges::find(resources, texture) == resources.end()) {
            resources.push_back(texture);
        }
    };
    for (const auto& slot : initial.slots) {
        authority.scene.setTextureFrame(slot.index, nullptr);
        const auto* texture = authority.scene.textureOf(slot.index);
        REQUIRE((texture != nullptr) == slot.texture.has_value());
        add(texture);
    }
    for (usize entry = 0; entry < authority.textureClock.size(); ++entry) {
        for (usize frame = 0;; ++frame) {
            const auto* texture = authority.textureClock.cycleFrame(entry, frame);
            if (!texture) {
                break;
            }
            add(texture);
        }
    }
    REQUIRE(resources.size() == initial.resourceCount);
    REQUIRE(resources.size() >= 2);
    REQUIRE(authority.checkpoint.restore(initial));
    const auto uploads = replay.device.texturesCreated;
    const auto slot = initial.slots.front().index;
    std::vector<const Texture*> frames;
    std::vector<u32> expected;
    for (usize i = 0; i < 512; ++i) {
        const auto id = static_cast<u32>((resources.size() - 1) - (i % resources.size()));
        frames.push_back(resources[id]);
        expected.push_back(id);
    }
    authority.scene.setTextureFrame(slot, resources.back());
    authority.scene.setTextureCycle(slot, frames, 3.25f, -0.5f);
    const auto captured = authority.state();
    CHECK(captured.catalog == initial.catalog);
    CHECK(captured.resourceCount == initial.resourceCount);
    CHECK(captured.slots.front().frame == initial.resourceCount - 1);
    CHECK(captured.slots.front().cycle == expected);
    REQUIRE(replay.checkpoint.restore(captured));
    CHECK(replay.state() == captured);
    CHECK(replay.scene.textureOf(slot) != authority.scene.textureOf(slot));
    CHECK(replay.device.texturesCreated == uploads);
    sameDraw(authority, replay, 0.5f);

    // A lookup miss must not insert a new catalog id, including in long cycles.
    frames.back() = &authority.device.whiteTexture();
    authority.scene.setTextureCycle(slot, frames, 3.25f, -0.5f);
    CHECK_FALSE(authority.checkpoint.checkpoint());
    REQUIRE(authority.checkpoint.restore(captured));
    CHECK(authority.state() == captured);
}

TEST_CASE("world state flat membership checks preserve unordered native rosters",
          "[world][world-state-checkpoint]") {
    WorldStateCheckpoint::State state;
    state.placements.resize(513);
    state.units = {{512, 1, true}, {0, 0.5f, false}, {42, 0, true}};
    state.hidden = {1'000'000, -1, 8, 0};
    state.contactOnly = {500, 0, -1, 2};
    state.blockedFloorExits = {300, 1'000'000, 0};
    state.moving = {{1'000'000, Mat4{1}}, {0, Mat4{1}}, {800, Mat4{1}}};
    state.animations = {{512, 3, 0}, {0, 3, 1}};
    state.cycles = {{0, false}, {512, true}, {0, true}};
    const auto original = state;
    REQUIRE(state.valid());
    CHECK(state == original);
    SECTION("duplicate units") {
        state.units.back().object = state.units.front().object;
    }
    SECTION("out of range unit") {
        state.units.front().object = 513;
    }
    SECTION("duplicate hidden") {
        state.hidden.back() = state.hidden.front();
    }
    SECTION("duplicate contact only") {
        state.contactOnly.back() = state.contactOnly.front();
    }
    SECTION("duplicate exit lock") {
        state.blockedFloorExits.back() = state.blockedFloorExits.front();
    }
    SECTION("negative exit lock") {
        state.blockedFloorExits.back() = -1;
    }
    SECTION("mask beyond maximum") {
        state.hidden.front() = 1'000'001;
    }
    SECTION("duplicate movers") {
        state.moving.back().object = state.moving.front().object;
    }
    SECTION("out of range animation") {
        state.animations.front().object = 513;
    }
    SECTION("negative event") {
        state.cycles.back().object = -1;
    }
    SECTION("out of range event") {
        state.cycles.back().object = 513;
    }
    SECTION("unit without animation is not an event owner") {
        state.cycles.back().object = 42;
    }
    CHECK_FALSE(state.valid());
}

TEST_CASE("world capture and preparation still validate derived moving triangles",
          "[world][world-state-checkpoint]") {
    Fixture fixture("world-core-moving-validation");
    const auto baseline = fixture.state();
    REQUIRE(baseline.moving.size() == 1);
    auto invalid = baseline;
    // Every transform scalar is in range, but applying it to the bound local
    // vertices leaves the validated spatial range. Refuse before any mutation.
    invalid.moving.front().transform = glm::scale(Mat4{1}, Vec3{1'000'000});
    REQUIRE(invalid.valid());
    CHECK_FALSE(fixture.checkpoint.prepareRestore(invalid));
    CHECK(fixture.state() == baseline);
    fixture.collision.setObjectTransform(8, invalid.moving.front().transform);
    CHECK_FALSE(fixture.checkpoint.checkpoint());
    REQUIRE(fixture.checkpoint.restore(baseline));
    CHECK(fixture.state() == baseline);
    auto collapsed = baseline;
    collapsed.moving.front().transform = glm::scale(Mat4{1}, Vec3{0});
    REQUIRE(fixture.checkpoint.restore(collapsed));
    CHECK(fixture.state() == collapsed);
    REQUIRE(fixture.checkpoint.restore(baseline));
    CHECK(fixture.state() == baseline);
}

TEST_CASE("prepared world commits own every value after the incoming state is destroyed",
          "[world][world-state-checkpoint]") {
    Fixture source("world-core-prepared-values-source");
    Fixture target("world-core-prepared-values-target");
    source.step(3.0f / 30, 3);
    source.collision.setContactOnly(1000, true);
    source.collision.setFloorExitBlocked(1000, true);
    const auto expected = source.state();
    REQUIRE_FALSE(expected.cycles.empty());
    REQUIRE_FALSE(expected.light.points.empty());
    REQUIRE(
        std::ranges::any_of(expected.slots, [](const auto& slot) { return !slot.cycle.empty(); }));
    const auto before = target.state();
    const auto uploads = target.device.texturesCreated;
    std::optional<WorldStateCheckpoint::Prepared> pending;
    {
        auto incoming = expected;
        pending = target.checkpoint.prepareRestore(incoming);
        REQUIRE(pending);
        CHECK(target.state() == before);
        // Mutate existing storage, then release every vector before committing.
        // Both aliasing that storage and retaining a reference to the State fail.
        incoming.placements.front().local[3].x += 123;
        incoming.units.front().alpha = 0;
        incoming.slots.front().scrollVelocity = {7, 8};
        for (auto& slot : incoming.slots) {
            std::ranges::fill(slot.cycle, incoming.resourceCount);
        }
        incoming.light.points.front().radius = 900;
        incoming.moving.front().transform[3].x += 456;
        incoming.animations.front().frame = 0;
        incoming.cycles.front().wrapped = !incoming.cycles.front().wrapped;
        incoming = {};
    }
    target.step(1.0f / 30, 11);
    CHECK(target.state() != before);
    REQUIRE(target.checkpoint.canCommit(*pending));
    REQUIRE(target.checkpoint.commitRestore(std::move(*pending)));
    CHECK(target.state() == expected);
    CHECK(target.device.texturesCreated == uploads);
    CHECK_FALSE(target.checkpoint.canCommit(*pending));
    sameDraw(source, target, 0.5f);
}

TEST_CASE("prepared moving caches match native floor ray and cylinder queries after rewind",
          "[world][world-state-checkpoint][world-collapsed-collision]") {
    Fixture native("world-core-borrowed-native");
    Fixture replay("world-core-borrowed-replay");
    for (auto* fixture : {&native, &replay}) {
        fixture->collision.build(
            {{{0, 1, 0}, {Vec3{-2, 0, -2}, Vec3{2, 0, -2}, Vec3{0, 0, 2}}, 8, 4}});
        // Appended item collision retains its own non-layout object identity.
        const std::array appended{
            CollisionTriangle{{0, 0, 1}, {Vec3{-2, -2, 0}, Vec3{2, -2, 0}, Vec3{0, 2, 0}}, 1000, 2},
            CollisionTriangle{
                {0, 1, 0}, {Vec3{48, -3, -2}, Vec3{52, -3, -2}, Vec3{50, -3, 2}}, 2000, 4}};
        fixture->collision.append(appended);
        fixture->collision.setMovingObjects(std::array<s32, 2>{8, 1000});
        REQUIRE(fixture->checkpoint.bind(fixture->textureClock, fixture->layout));
        CHECK(fixture->checkpoint.hasCollisionObject(1000));
        CHECK(fixture->checkpoint.hasCollisionObject(2000));
    }
    const auto initial = native.state();
    const Mat4 placed = glm::translate(Mat4{1}, Vec3{7, 3, 9}) *
                        glm::rotate(Mat4{1}, 0.4f, Vec3{0, 1, 0}) *
                        glm::scale(Mat4{1}, Vec3{1.5f, 0.75f, 2});
    const Mat4 collapsed = glm::translate(Mat4{1}, Vec3{7, 3, 9}) * glm::scale(Mat4{1}, Vec3{0});
    const std::array transforms{Mat4{1}, placed, collapsed, placed, Mat4{1}};
    const auto uploads = replay.device.texturesCreated;
    for (usize step = 0; step < transforms.size(); ++step) {
        CAPTURE(step);
        const auto& transform = transforms[step];
        for (const s32 object : {8, 1000}) {
            native.collision.setObjectTransform(object, transform);
        }
        const auto saved = native.state();
        auto pending = replay.checkpoint.prepareRestore(saved);
        REQUIRE(pending);
        // A native update after preparation may replace the old derived cache;
        // it cannot change the saved cache or the borrowed immutable locals.
        for (const s32 object : {8, 1000}) {
            replay.collision.setObjectTransform(object, glm::translate(Mat4{1}, Vec3{-7, 6, -9}));
        }
        REQUIRE(replay.checkpoint.commitRestore(std::move(*pending)));
        CHECK(replay.state() == saved);
        CHECK(saved.catalog == initial.catalog);
        CHECK(replay.collision.triangleCount() == 3);
        CHECK(replay.device.texturesCreated == uploads);

        const Vec3 centre{transform[3]};
        const bool collapsedNow = step == 2;
        const Vec3 normal =
            collapsedNow ? Vec3{0, 0, 1} : glm::normalize(Mat3{transform} * Vec3{0, 0, 1});
        const auto nativeFloor = native.collision.floorAt(centre, 1, 1);
        const auto replayFloor = replay.collision.floorAt(centre, 1, 1);
        REQUIRE(replayFloor.has_value() == nativeFloor.has_value());
        CHECK(nativeFloor.has_value() == !collapsedNow);
        if (nativeFloor) {
            CHECK(nativeFloor->object == 8);
            CHECK(nativeFloor->objectFlags == 4);
            CHECK(glm::abs(nativeFloor->y - centre.y) < 0.00001f);
            CHECK(replayFloor->object == nativeFloor->object);
            CHECK(replayFloor->objectFlags == nativeFloor->objectFlags);
            CHECK(replayFloor->normal == nativeFloor->normal);
            CHECK(replayFloor->y == nativeFloor->y);
        }
        const Vec3 from = centre + normal;
        const Vec3 to = centre - normal;
        const auto nativeRay = native.collision.pickSurface(from, to);
        CHECK(nativeRay.has_value() == !collapsedNow);
        CHECK(replay.collision.pickSurface(from, to) == nativeRay);
        CHECK(native.collision.wallBetween(from, to) == !collapsedNow);
        CHECK(replay.collision.wallBetween(from, to) == !collapsedNow);
        CHECK(native.collision.wallBetween(from, to, 0.25f) == !collapsedNow);
        CHECK(replay.collision.wallBetween(from, to, 0.25f) == !collapsedNow);
        const Vec3 body = centre + normal * 0.1f;
        const auto nativeContacts =
            native.collision.surfaceContacts(body, 0.5f, centre.y - 0.5f, centre.y + 0.5f);
        const auto replayContacts =
            replay.collision.surfaceContacts(body, 0.5f, centre.y - 0.5f, centre.y + 0.5f);
        CHECK(std::ranges::any_of(nativeContacts, [](const auto& contact) {
                  return contact.object == 1000;
              }) == !collapsedNow);
        REQUIRE(replayContacts.size() == nativeContacts.size());
        for (usize contact = 0; contact < nativeContacts.size(); ++contact) {
            CHECK(replayContacts[contact].object == nativeContacts[contact].object);
            CHECK(replayContacts[contact].point == nativeContacts[contact].point);
        }
        const auto nativeResolved =
            native.collision.resolveWalls(body, 0.5f, centre.y - 0.5f, centre.y + 0.5f);
        CHECK((nativeResolved != body) == !collapsedNow);
        CHECK(replay.collision.resolveWalls(body, 0.5f, centre.y - 0.5f, centre.y + 0.5f) ==
              nativeResolved);
        CHECK(replay.collision.sweepWalls(from, to, 0.5f, centre.y - 0.5f, centre.y + 0.5f) ==
              native.collision.sweepWalls(from, to, 0.5f, centre.y - 0.5f, centre.y + 0.5f));
    }
    // The static appended floor and the original local triangles survive every
    // prepared swap, including the zero-scale interval and subsequent expansion.
    const auto floor = replay.collision.floorAt({50, -3, 0}, 1, 1);
    REQUIRE(floor);
    CHECK(floor->object == 2000);
    CHECK(floor->objectFlags == 4);
    CHECK(replay.state() == initial);
}
} // namespace
