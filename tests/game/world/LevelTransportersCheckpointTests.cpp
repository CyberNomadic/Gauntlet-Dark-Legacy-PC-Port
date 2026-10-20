#include <array>
#include <limits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/world/LevelTransportersCheckpoint.h"
#include "game/world/LevelWorld.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct TransportFixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    WorldCollision collision;
    ItemArchive items;
    LevelTransporters pads;
    LevelTransportersCheckpoint checkpoint{pads};
    s32 players = 1;

    explicit TransportFixture(std::string_view name, bool artwork = true, s32 variant = 0,
                              s32 party = 1)
        : players(party) {
        const auto directory = test::sampleLevel(name);
        writeTextFile(directory / "world.json", R"({
          "objects":[{"name":"FLOOR","position":[0,0,0],"flags":4102}],
          "itemInfos":[{"type":11,"name":"TRANS","radius":1.6,"height":2}],
          "itemInstances":[
            {"info":0,"position":[2,0,3],"params":[10,0,0,0,20,0,0,0]},
            {"info":0,"position":[-4,0,5],"params":[20,0,0,0,10,0,0,0]},
            {"info":0,"position":[-8,0,-8],"params":[30,0,0,0,30,0,0,0]},
            {"info":0,"minPlayers":2,"position":[8,0,8],"params":[20,0,0,0,10,0,0,0]}]})");
        writeTextFile(directory / "animations.json", R"({
          "textureAnimations":[{"name":"STONE","texture":0,"source":1,"frames":2,"rate":1}],
          "trees":[{"name":"TRANS",
            "nodes":[{"name":"BODY","object":"WALL","parent":-1,"position":[0,0,0]}],
            "sequences":[{"name":"ACTIVE","frames":4,"frameRate":30,
              "tracks":[{"node":0,"flags":16,"frames":[0,3],"values":[0,2]}]}]}]})");
        if (variant == 1) {
            // Same named asset, vertex/part counts and tree: different actual geometry.
            writeTextFile(directory / "models/000_WALL.obj",
                          "v 0 0 0\nv 3 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nvn 0 1 0\n"
                          "usemtl tex0\nf 1/1/1 2/2/1 3/3/1\n");
            test::convertModelFixture(directory);
        } else if (variant == 2) {
            writeTextFile(directory / "animations.json", R"({
              "textureAnimations":[{"name":"STONE","texture":0,"source":1,"frames":2,"rate":1}],
              "trees":[{"name":"TRANS",
                "nodes":[{"name":"BODY","object":"WALL","parent":-1,"position":[0,0,0]}],
                "sequences":[{"name":"ACTIVE","frames":4,"frameRate":30,
                  "tracks":[{"node":0,"flags":16,"frames":[0,3],"values":[0,3]}]}]}]})");
        }
        REQUIRE(layout.load(directory));
        if (artwork) {
            REQUIRE(items.load(directory));
        }
        collision.build(
            {CollisionTriangle{.vertices = {Vec3{-40, 0, -40}, Vec3{40, 0, 40}, Vec3{40, 0, -40}},
                               .object = 0},
             CollisionTriangle{.vertices = {Vec3{-40, 0, -40}, Vec3{-40, 0, 40}, Vec3{40, 0, 40}},
                               .object = 0}});
        collision.setMovingObjects(std::array<s32, 1>{0});
        bind();
    }
    void bind() {
        pads.bind(device, layout, AssetScope(items), players, &collision);
        REQUIRE(pads.size() == (players == 1 ? 3 : 4));
        REQUIRE(checkpoint.bindResources());
    }
    LevelTransportersCheckpoint::State state() const {
        auto result = checkpoint.checkpoint();
        REQUIRE(result);
        return std::move(*result);
    }
};

void sameDraws(test::FakeRenderDevice& host, test::FakeRenderDevice& guest,
               const LevelTransporters& native, const LevelTransporters& restored, f32 alpha) {
    host.draws.clear();
    guest.draws.clear();
    native.draw(host, Mat4{1}, {}, alpha);
    restored.draw(guest, Mat4{1}, {}, alpha);
    REQUIRE(host.draws.size() == guest.draws.size());
    for (usize index = 0; index < host.draws.size(); ++index) {
        const auto& expected = host.draws[index];
        const auto& actual = guest.draws[index];
        CHECK(actual.vertices == expected.vertices);
        CHECK(actual.transform == expected.transform);
        CHECK(actual.state.depthTest == expected.state.depthTest);
        CHECK(actual.state.depthWrite == expected.state.depthWrite);
        CHECK(actual.state.blend == expected.state.blend);
        REQUIRE(actual.texture != expected.texture); // Independent native resource owners.
        const auto* expectedTexture = dynamic_cast<const test::FakeTexture*>(expected.texture);
        const auto* actualTexture = dynamic_cast<const test::FakeTexture*>(actual.texture);
        REQUIRE(expectedTexture);
        REQUIRE(actualTexture);
        CHECK(actualTexture->pixels == expectedTexture->pixels);
    }
}

TEST_CASE("native transporter history rewinds floor contacts animation and held presentation",
          "[local-rewind][transporters-local-rewind]") {
    const bool artwork = GENERATE(false, true);
    TransportFixture native("trans-local-native", artwork);
    TransportFixture replay("trans-local-replay", artwork);
    std::array<LevelTransporters::LocalSnapshot, 16> history;
    WorldCollision::LocalSnapshot floor;
    const auto* model = &replay.pads.pad(0).model;
    const auto uploads = replay.device.texturesCreated;
    const auto step = [](TransportFixture& fixture, u32 tick, bool wrong) {
        fixture.pads.capturePresentation();
        fixture.collision.setObjectTransform(
            0, glm::rotate(glm::translate(Mat4{1}, Vec3{1, wrong ? -8.0f : static_cast<f32>(tick) * 0.01f, 2}),
                           static_cast<f32>(tick % 30) * 0.02f, Vec3{0, 1, 0}));
        fixture.collision.setSolid(0, tick % 3 != 0);
        fixture.pads.syncFloors(fixture.collision);
        if (wrong || tick % 5 != 0) {
            fixture.pads.animate(wrong ? 0.2f : 1.0f / 60);
        }
    };
    u32 tick = 0;
    for (u32 window = 0; window < 80; ++window) {
        CAPTURE(window, tick, artwork);
        auto& saved = history[window % history.size()];
        replay.pads.saveLocal(saved);
        replay.collision.saveLocal(floor);
        const u32 distance = window % 8 + 1;
        for (u32 i = 0; i < distance; ++i) {
            step(replay, tick + i, true);
        }
        REQUIRE(replay.collision.restoreLocal(floor));
        REQUIRE(replay.pads.restoreLocal(saved));
        CHECK(replay.state() == native.state());
        sameDraws(native.device, replay.device, native.pads, replay.pads, 0.35f);
        for (u32 i = 0; i < distance; ++i) {
            step(native, tick + i, false);
            step(replay, tick + i, false);
            CHECK(replay.state() == native.state());
            CHECK(replay.pads.contact(native.pads.pad(0).position, 0.5f, 2) == 0);
            for (f32 const alpha : {-1.0f, 0.0f, 0.35f, 1.0f}) {
                sameDraws(native.device, replay.device, native.pads, replay.pads, alpha);
            }
        }
        tick += distance;
    }
    CHECK(&replay.pads.pad(0).model == model);
    CHECK(replay.device.texturesCreated == uploads);
    CHECK(replay.device.textureUpdates == 0);
    CHECK_FALSE(native.pads.restoreLocal(history[0]));
    replay.bind();
    CHECK_FALSE(replay.pads.restoreLocal(history[0]));
    replay.pads.saveLocal(history[0]);
    replay.pads.clear();
    CHECK_FALSE(replay.pads.restoreLocal(history[0]));
}

TEST_CASE("transporter checkpoint resumes independent native floor clock and loop presentation",
          "[rollback][transporter-checkpoint]") {
    const s32 rate = GENERATE(30, 60);
    const bool artwork = GENERATE(false, true);
    CAPTURE(rate, artwork);
    TransportFixture host("trans-checkpoint-host", artwork);
    TransportFixture guest("trans-checkpoint-guest", artwork);
    const f32 seconds = 1.0f / static_cast<f32>(rate);
    REQUIRE(host.pads.pad(0).support == 0);
    for (s32 tick = 0; tick < 5; ++tick) {
        host.pads.animate(seconds);
    }
    host.pads.animate(0.007f); // Fractional clock credit, not a displayed-frame reconstruction.
    const auto baseline = host.state();
    CHECK(baseline.pads[0].destination == 1);
    CHECK(baseline.pads[1].destination == 0);
    CHECK_FALSE(baseline.pads[2].destination);
    const auto textures = guest.device.texturesCreated;
    const auto uploads = guest.device.textureUpdates;
    const auto* address = &guest.pads.pad(0);
    REQUIRE(guest.checkpoint.restore(baseline));
    CHECK(guest.state() == baseline);
    if (artwork) {
        REQUIRE(baseline.pads[0].animation.playing);
        REQUIRE(baseline.pads[0].pose);
        REQUIRE_FALSE(host.device.texturesCreated == 0);
    }
    bool looped = false;
    u64 generation = host.pads.pad(0).animation.generation();
    for (s32 tick = 0; tick < rate; ++tick) {
        CAPTURE(tick);
        const Mat4 moved =
            glm::rotate(glm::translate(Mat4{1}, Vec3{1, static_cast<f32>(tick) / 10, 2}),
                        static_cast<f32>(tick) / 20, Vec3{0, 1, 0});
        host.collision.setSolid(0, tick % 3 != 0);
        guest.collision.setSolid(0, tick % 3 != 0);
        host.collision.setObjectTransform(0, moved);
        guest.collision.setObjectTransform(0, moved);
        host.pads.syncFloors(host.collision);
        guest.pads.syncFloors(guest.collision);
        host.pads.animate(seconds);
        guest.pads.animate(seconds);
        CHECK(guest.state() == host.state());
        CHECK(guest.pads.contact(host.pads.pad(0).position, 0.5f, 2) == 0);
        looped |= generation != host.pads.pad(0).animation.generation();
        generation = host.pads.pad(0).animation.generation();
        for (const f32 alpha : {-1.0f, 0.0f, 0.35f, 1.0f}) {
            sameDraws(host.device, guest.device, host.pads, guest.pads, alpha);
        }
        if (tick % 7 == 0) {
            host.pads.capturePresentation();
            guest.pads.capturePresentation();
            const auto held = host.state();
            REQUIRE_FALSE(held.presentationAdvanced);
            REQUIRE(guest.checkpoint.restore(held));
            sameDraws(host.device, guest.device, host.pads, guest.pads, 0.1f);
        }
    }
    CHECK(looped == artwork);
    CHECK(&guest.pads.pad(0) == address);
    CHECK(guest.device.texturesCreated == textures);
    CHECK(guest.device.textureUpdates == uploads);
}

TEST_CASE("transporter restore preserves historical placement before its next native carry",
          "[rollback][transporter-checkpoint]") {
    TransportFixture host("trans-checkpoint-lag-host");
    TransportFixture guest("trans-checkpoint-lag-guest");
    const auto placement = glm::translate(Mat4{1}, Vec3{4, 2, 6});
    host.collision.setObjectTransform(0, placement);
    host.pads.syncFloors(host.collision);
    host.pads.animate(0.025f);
    const auto baseline = host.state();
    const auto later = glm::rotate(glm::translate(Mat4{1}, Vec3{9, 11, 13}), 0.75f, Vec3{0, 1, 0});
    // Scripted world motion can leave pads at their preceding carry phase.
    host.collision.setObjectTransform(0, later);
    guest.collision.setObjectTransform(0, later);
    guest.collision.setSolid(0, false);
    REQUIRE(guest.checkpoint.restore(baseline));
    CHECK(guest.state() == baseline);
    CHECK(guest.pads.pad(0).transform != later * baseline.pads[0].supportLocal);
    sameDraws(host.device, guest.device, host.pads, guest.pads, 0.4f);
    guest.pads.syncFloors(guest.collision);
    host.pads.syncFloors(host.collision);
    CHECK(guest.state() == host.state());
    CHECK(guest.pads.pad(0).transform == later * baseline.pads[0].supportLocal);
}

TEST_CASE("transporter restore is atomic for malformed links poses clocks resources and limits",
          "[rollback][transporter-checkpoint]") {
    const TransportFixture fixture("trans-checkpoint-invalid");
    const auto baseline = fixture.state();
    for (s32 mutation = 0; mutation < 16; ++mutation) {
        CAPTURE(mutation);
        auto invalid = baseline;
        switch (mutation) {
        case 0: invalid.pads[0].position.y = std::numeric_limits<f32>::quiet_NaN(); break;
        case 1: invalid.pads[0].transform[0][0] = std::numeric_limits<f32>::infinity(); break;
        case 2: invalid.pads[0].destination = 0; break;
        case 3: invalid.pads[0].destination = invalid.pads.size(); break;
        case 4: invalid.pads[0].source.info.radius += 1; break;
        case 5: invalid.pads[0].supportLocal[3].x += 1; break;
        case 6: invalid.pads[0].anchorLocal.z += 1; break;
        case 7: invalid.pads[0].animation.sequence = 65535; break;
        case 8: invalid.pads[0].pose->nodes[0].rest.x += 1; break;
        case 9: invalid.pads[0].previousFrame = -1; break;
        case 10: invalid.pads[0].previousGeneration = std::numeric_limits<u64>::max(); break;
        case 11: invalid.textures.remainder = 1; break;
        case 12: invalid.catalog[0] ^= 1; break;
        case 13: invalid.pads.resize(LevelTransportersCheckpoint::State::kMaxPads + 1); break;
        case 14: invalid.pads[0].animation.frame = 100; break;
        case 15: invalid.pads[0].source.instance.params[4] = 30; break;
        default:
            break;
        }
        CHECK_FALSE(fixture.checkpoint.prepareRestore(invalid));
        CHECK(fixture.state() == baseline);
    }
    TransportFixture mesh("trans-checkpoint-wrong-mesh", true, 1);
    TransportFixture track("trans-checkpoint-wrong-track", true, 2);
    TransportFixture party("trans-checkpoint-wrong-party", true, 0, 2);
    TransportFixture absent("trans-checkpoint-no-art", false);
    CHECK(mesh.state().catalog != baseline.catalog);
    CHECK(track.state().catalog != baseline.catalog);
    CHECK_FALSE(mesh.checkpoint.restore(baseline));
    CHECK_FALSE(track.checkpoint.restore(baseline));
    CHECK_FALSE(party.checkpoint.restore(baseline));
    CHECK_FALSE(absent.checkpoint.restore(baseline));
    // Duplicate authored ids retain the native first matching non-self row.
    CHECK(party.state().pads[0].destination == 1);
    CHECK(party.state().pads[3].destination == 0);
    auto wrongOrder = party.state();
    wrongOrder.pads[0].destination = 3;
    CHECK_FALSE(party.checkpoint.restore(wrongOrder));
}

TEST_CASE("transporter preparations own their state and fence owners rebinds and siblings",
          "[rollback][transporter-checkpoint]") {
    TransportFixture fixture("trans-checkpoint-tokens");
    TransportFixture other("trans-checkpoint-tokens-other");
    const auto original = fixture.state();
    auto saved = original;
    auto prepared = fixture.checkpoint.prepareRestore(saved);
    auto sibling = fixture.checkpoint.prepareRestore(saved);
    REQUIRE(prepared);
    REQUIRE(sibling);
    saved = {};
    fixture.pads.animate(0.09f);
    fixture.collision.setObjectTransform(0, glm::translate(Mat4{1}, Vec3{1, 2, 3}));
    fixture.pads.syncFloors(fixture.collision);
    fixture.pads.draw(fixture.device, Mat4{1}, {}, 0.25f);
    CHECK_FALSE(other.checkpoint.commitRestore(std::move(*prepared)));
    REQUIRE(fixture.checkpoint.canCommit(*prepared));
    REQUIRE(fixture.checkpoint.commitRestore(std::move(*prepared)));
    CHECK(fixture.state() == original);
    CHECK_FALSE(fixture.checkpoint.canCommit(*prepared));
    CHECK_FALSE(fixture.checkpoint.canCommit(*sibling));
    prepared = fixture.checkpoint.prepareRestore(original);
    REQUIRE(prepared);
    REQUIRE(fixture.checkpoint.bindResources());
    CHECK_FALSE(fixture.checkpoint.canCommit(*prepared));
    prepared = fixture.checkpoint.prepareRestore(original);
    REQUIRE(prepared);
    fixture.pads.clear();
    CHECK_FALSE(fixture.checkpoint.checkpoint());
    CHECK_FALSE(fixture.checkpoint.canCommit(*prepared));
    fixture.collision.setObjectTransform(0, Mat4{1});
    fixture.bind();
    CHECK_FALSE(fixture.checkpoint.commitRestore(std::move(*prepared)));
    CHECK(fixture.state() == original);
}

TEST_CASE("native transporter catalogs restore all thirty six pads across seven levels",
          "[rollback][transporter-checkpoint][assets]") {
    const s32 rate = GENERATE(30, 60);
    const auto root =
        test::assetOrSkip("LEVELS/LEVELA1/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    usize count = 0;
    usize levels = 0;
    for (const auto& realm : catalog.realms()) {
        for (const auto& name : realm.levels) {
            const auto ref = catalog.byName(name);
            REQUIRE(ref);
            WorldLayout census;
            REQUIRE(census.load(root / ref->directory));
            bool padsPresent = false;
            for (const auto& info : census.itemInfos()) {
                padsPresent |= info.type == LevelTransporters::kItemType;
            }
            if (!padsPresent) {
                continue;
            }
            CAPTURE(name, rate);
            test::FakeRenderDevice device;
            LevelWorld hostWorld;
            LevelWorld guestWorld;
            REQUIRE(hostWorld.load(device, root, *ref));
            REQUIRE(guestWorld.load(device, root, *ref));
            LevelTransporters host;
            LevelTransporters guest;
            host.bind(device, hostWorld.layout(), hostWorld.itemScope(), 1, &hostWorld.collision());
            guest.bind(device, guestWorld.layout(), guestWorld.itemScope(), 1,
                       &guestWorld.collision());
            LevelTransportersCheckpoint hostState{host};
            LevelTransportersCheckpoint guestState{guest};
            REQUIRE(hostState.bindResources());
            REQUIRE(guestState.bindResources());
            const f32 seconds = 1.0f / static_cast<f32>(rate);
            for (s32 tick = 0; tick < 13; ++tick) {
                host.animate(seconds);
            }
            host.animate(0.007f);
            const auto seed = hostState.checkpoint();
            REQUIRE(seed);
            const auto textures = device.texturesCreated;
            const auto uploads = device.textureUpdates;
            REQUIRE(guestState.restore(*seed));
            CHECK(guestState.checkpoint() == seed);
            for (s32 tick = 0; tick < rate; ++tick) {
                host.animate(seconds);
                guest.animate(seconds);
                CHECK(hostState.checkpoint() == guestState.checkpoint());
            }
            CHECK(device.texturesCreated == textures);
            CHECK(device.textureUpdates == uploads);
            for (usize index = 0; index < host.size(); ++index) {
                CAPTURE(index);
                CHECK(host.pad(index).model.bound());
                CHECK(guest.pad(index).animation.playing());
            }
            count += host.size();
            ++levels;
        }
    }
    CHECK(count == 36);
    CHECK(levels == 7);
}
} // namespace
