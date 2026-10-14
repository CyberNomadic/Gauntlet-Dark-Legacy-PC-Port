#include <algorithm>
#include <array>
#include <limits>

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
} // namespace
