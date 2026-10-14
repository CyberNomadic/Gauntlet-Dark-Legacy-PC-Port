#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <tuple>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/world/SafeRocksCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;

SafeRocksCheckpoint::State captured(const SafeRocksCheckpoint& owner) {
    const auto state = owner.checkpoint();
    REQUIRE(state);
    REQUIRE(state->valid());
    return *state;
}

void checkTargets(std::span<const CombatArenaTarget> actual,
                  std::span<const CombatArenaTarget> expected) {
    REQUIRE(actual.size() == expected.size());
    for (usize index = 0; index < actual.size(); ++index) {
        CHECK(std::tie(actual[index].index, actual[index].placement, actual[index].active) ==
              std::tie(expected[index].index, expected[index].placement, expected[index].active));
    }
}

struct Fixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    ItemArchive items;
    SafeRocks rocks;
    SafeRocksCheckpoint owner{rocks};

    explicit Fixture(std::string_view name) {
        const auto directory = test::scratchDirectory(name);
        writeTextFile(directory / "world.json", R"({
          "objects":[{"name":"WALL","position":[0,0,0]}],
          "itemInfos":[
            {"type":10,"subtype":41,"name":"ROCK","radius":2.3,"height":5,
             "collisionType":1,"hitPoints":40,"armor":10},
            {"type":10,"subtype":43,"name":"OVERRIDE","radius":1,"height":2,
             "hitPoints":20,"armor":-1},
            {"type":10,"subtype":41,"name":"ZERO","radius":1,"height":1,
             "hitPoints":0}],
          "itemInstances":[
            {"info":0,"minPlayers":1,"position":[7,30,-1],"rotation":[0,0.4,0],
             "params":[41,0,3,0,0,0,0,0,0,0,0,0]},
            {"info":0,"minPlayers":2,"position":[20,30,-1],
             "params":[0,0,2,0,0,0,0,0,0,0,0,0]},
            {"info":1,"position":[40,30,-1],
             "params":[41,0,3,0,0,0,0,0,0,0,0,0]},
            {"info":2,"position":[60,30,-1],
             "params":[0,0,3,0,0,0,0,0,0,0,0,0]}]})");
        REQUIRE(layout.load(directory));
        REQUIRE(rocks.bind(device, layout, items));
        rocks.setPlayerCount(1);
        REQUIRE(rocks.size() == 4);
        REQUIRE(rocks.bodyResources().empty()); // Missing art does not remove gameplay cover.
    }
};

void mutate(SafeRocks& rocks, s32 tick, f32 seconds) {
    switch (tick) {
    case 0: REQUIRE_FALSE(rocks.strike(0, 50)); break;
    case 1: REQUIRE_FALSE(rocks.strike(0, 5)); break;
    case 2: REQUIRE_FALSE(rocks.strike(0, 49)); break;
    case 3: REQUIRE(rocks.strike(0, 1000)); break;
    case 4: rocks.hideForEruptions(); break;
    case 5: rocks.scheduleActivation(0, 0.5f); break;
    case 10: rocks.scheduleActivation(0, 0.2f); break;
    case 30: rocks.setPlayerCount(2); break;
    case 31: rocks.scheduleActivation(1, 0.1f); break;
    case 45: rocks.activate(2); break;
    case 46: REQUIRE_FALSE(rocks.strike(2, 1000)); break;
    case 50: rocks.activate(3); break; // Native zero-health tier-three residue is valid.
    case 60: REQUIRE(rocks.strike(0, 1000)); break;
    default: break;
    }
    rocks.update(seconds);
    if (tick % 7 == 0) {
        rocks.capturePresentation();
    }
}

TEST_CASE("safe rock checkpoints resume all native damage and eruption states",
          "[safe-rocks-checkpoint]") {
    Fixture host("safe-rock-checkpoint-host");
    Fixture guest("safe-rock-checkpoint-guest");
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    const auto uploads = guest.device.texturesCreated;
    std::array<bool, 4> tiers{};
    bool rescheduled = false;
    bool reactivated = false;
    for (s32 tick = 0; tick < 100; ++tick) {
        CAPTURE(tick, seconds);
        const auto before = captured(host.owner);
        REQUIRE(guest.owner.restore(before));
        auto undo = host.owner.prepareRestore(before);
        REQUIRE(undo);
        mutate(host.rocks, tick, seconds);
        const auto expected = captured(host.owner);
        tiers[static_cast<usize>(expected.rocks[0].tier)] = true;
        rescheduled |= tick == 10 && expected.rocks[0].activationDelay > 0;
        reactivated |=
            tick > 10 && tick < 60 && !expected.rocks[0].dormant && expected.rocks[0].health == 120;
        mutate(guest.rocks, tick, seconds);
        CHECK(captured(guest.owner) == expected);
        checkTargets(guest.rocks.arenaTargets(), host.rocks.arenaTargets());
        checkTargets(guest.rocks.eruptionTargets(), host.rocks.eruptionTargets());
        CHECK(guest.rocks.attackAnchors() == host.rocks.attackAnchors());
        CHECK(guest.rocks.obstacles().size() == host.rocks.obstacles().size());
        CHECK(guest.rocks.blocksBreath({7, 32, -10}, {7, 32, 10}) ==
              host.rocks.blocksBreath({7, 32, -10}, {7, 32, 10}));
        REQUIRE(host.owner.commitRestore(std::move(*undo)));
        CHECK(captured(host.owner) == before);
        mutate(host.rocks, tick, seconds);
        CHECK(captured(host.owner) == expected);
    }
    CHECK(std::ranges::all_of(tiers, [](bool present) { return present; }));
    CHECK(rescheduled);
    CHECK(reactivated);
    CHECK(host.rocks.rock(0).health == 0);  // An expired activation is not fired twice.
    CHECK(host.rocks.rock(2).health == 60); // Negative armour remains indestructible.
    CHECK(host.rocks.rock(3).health == 0);
    CHECK(host.rocks.rock(3).tier == 3);
    CHECK(guest.device.texturesCreated == uploads);
}

TEST_CASE("safe rock checkpoint restores are atomic and fenced by bindings and siblings",
          "[safe-rocks-checkpoint]") {
    Fixture fixture("safe-rock-checkpoint-fences");
    const auto before = captured(fixture.owner);
    auto invalid = before;
    const s32 variant = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9);
    switch (variant) {
    case 0: invalid.rocks[0].source.position.x += 1; break;
    case 1: invalid.rocks[0].definition.name = "FOREIGN"; break;
    case 2: invalid.rocks[0].tier = 4; break;
    case 3: invalid.rocks[0].health = std::numeric_limits<s32>::max(); break;
    case 4: invalid.rocks[0].activationDelay = -1; break;
    case 5: invalid.textures.remainder = std::numeric_limits<f32>::quiet_NaN(); break;
    case 6: invalid.textureSlots = {0, 0}; break;
    case 7: invalid.rocks[0].models[3] = true; break;
    case 8: invalid.rocks[0].obstacle.height += 1; break;
    case 9: invalid.rocks[0].position.z += 1; break;
    }
    CHECK_FALSE(fixture.owner.restore(invalid));
    CHECK(captured(fixture.owner) == before);
    auto plan = fixture.owner.prepareRestore(before);
    auto sibling = fixture.owner.prepareRestore(before);
    REQUIRE(plan);
    REQUIRE(sibling);
    SafeRocksCheckpoint another(fixture.rocks);
    CHECK_FALSE(another.commitRestore(std::move(*plan)));
    REQUIRE(fixture.owner.commitRestore(std::move(*plan)));
    CHECK_FALSE(fixture.owner.canCommit(*sibling));
    plan = fixture.owner.prepareRestore(before);
    REQUIRE(plan);
    REQUIRE(another.restore(before));
    CHECK_FALSE(fixture.owner.canCommit(*plan));
    plan = fixture.owner.prepareRestore(before);
    REQUIRE(plan);
    fixture.rocks.bindAnimations(fixture.device, fixture.items);
    CHECK_FALSE(fixture.owner.canCommit(*plan));
    plan = fixture.owner.prepareRestore(captured(fixture.owner));
    REQUIRE(plan);
    fixture.rocks.clear();
    CHECK_FALSE(fixture.owner.canCommit(*plan));
    REQUIRE(captured(fixture.owner).rocks.empty());
}

TEST_CASE("empty safe rock rosters retain their native animation binding",
          "[safe-rocks-checkpoint][assets]") {
    const auto directory = test::scratchDirectory("safe-rock-empty-animation-binding");
    writeTextFile(
        directory / "world.json",
        R"({"objects":[{"name":"FLOOR","position":[0,0,0]}],"itemInfos":[],"itemInstances":[]})");
    WorldLayout hostLayout;
    WorldLayout guestLayout;
    REQUIRE(hostLayout.load(directory));
    REQUIRE(guestLayout.load(directory));
    const auto archive = test::assetOrSkip("ITEMS/LEVELK5/objects.ngc").parent_path();
    ItemArchive hostItems;
    ItemArchive guestItems;
    REQUIRE(hostItems.load(archive));
    REQUIRE(guestItems.load(archive));
    test::FakeRenderDevice hostDevice;
    test::FakeRenderDevice guestDevice;
    SafeRocks host;
    SafeRocks guest;
    CHECK_FALSE(host.bind(hostDevice, hostLayout, hostItems));
    CHECK_FALSE(guest.bind(guestDevice, guestLayout, guestItems));
    REQUIRE(host.size() == 0);
    REQUIRE(guest.size() == 0);
    SafeRocksCheckpoint hostOwner(host);
    SafeRocksCheckpoint guestOwner(guest);
    const auto unbound = captured(guestOwner);
    CHECK_FALSE(unbound.animationItems);

    // PlayScene performs this bind unconditionally after opening opponents,
    // even for ordinary levels without arena cover or resolved texture clocks.
    host.bindAnimations(hostDevice, hostItems);
    host.update(0.075f);
    const auto authority = captured(hostOwner);
    REQUIRE(authority.animationItems);
    REQUIRE(authority.rocks.empty());
    REQUIRE(authority.animations.empty());
    CHECK_FALSE(guestOwner.restore(authority));
    CHECK(captured(guestOwner) == unbound);

    guest.bindAnimations(guestDevice, guestItems);
    const auto uploads = guestDevice.texturesCreated;
    const auto updates = guestDevice.textureUpdates;
    REQUIRE(guestOwner.restore(authority));
    CHECK(captured(guestOwner) == authority);
    CHECK(guestDevice.texturesCreated == uploads);
    CHECK(guestDevice.textureUpdates == updates);
}

TEST_CASE("native arena cover checkpoints retain all authored tiers and stable resource addresses",
          "[safe-rocks-checkpoint][assets]") {
    const auto name = GENERATE("B6", "A5", "I5");
    CAPTURE(name);
    const std::string relative = std::string{"LEVELS/LEVEL"} + name + "/WORLDS.PS2";
    const std::string itemRelative = std::string{"ITEMS/LEVEL"} + name + "/objects.ngc";
    WorldLayout layout;
    ItemArchive items;
    test::FakeRenderDevice device;
    REQUIRE(layout.load(test::assetOrSkip(relative).parent_path()));
    REQUIRE(items.load(test::assetOrSkip(itemRelative).parent_path()));
    SafeRocks rocks;
    REQUIRE(rocks.bind(device, layout, items));
    rocks.bindAnimations(device, items);
    rocks.setPlayerCount(4);
    SafeRocksCheckpoint owner(rocks);
    const auto before = captured(owner);
    const auto resources = rocks.bodyResources();
    REQUIRE_FALSE(resources.empty());
    const auto uploads = device.texturesCreated;
    rocks.draw(device, Mat4{1}, {});
    const auto draws = device.draws;
    REQUIRE_FALSE(draws.empty());
    auto undo = owner.prepareRestore(before);
    REQUIRE(undo);
    for (usize index = 0; index < rocks.size(); ++index) {
        REQUIRE(rocks.strike(index, 10000));
    }
    rocks.hideForEruptions();
    rocks.scheduleActivation(0, 0.5f);
    rocks.update(0.13f);
    REQUIRE(captured(owner) != before);
    REQUIRE(owner.commitRestore(std::move(*undo)));
    CHECK(captured(owner) == before);
    const auto restoredResources = rocks.bodyResources();
    REQUIRE(restoredResources.size() == resources.size());
    for (usize index = 0; index < resources.size(); ++index) {
        CHECK(restoredResources[index].index == resources[index].index);
        CHECK(restoredResources[index].tier == resources[index].tier);
        CHECK(restoredResources[index].model == resources[index].model);
        CHECK(restoredResources[index].textures == resources[index].textures);
    }
    device.draws.clear();
    rocks.draw(device, Mat4{1}, {});
    REQUIRE(device.draws.size() == draws.size());
    for (usize index = 0; index < draws.size(); ++index) {
        CHECK(device.draws[index].texture == draws[index].texture);
        CHECK(device.draws[index].transform == draws[index].transform);
        CHECK(device.draws[index].vertices == draws[index].vertices);
        CHECK(device.draws[index].state == draws[index].state);
    }
    CHECK(device.texturesCreated == uploads);
}

struct PlagueFixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    ItemArchive items;
    ItemArchive boss;
    SafeRocks rocks;
    SafeRocksCheckpoint owner{rocks};
    u32 first = 0;
    usize drawCount = 0;
    usize animatedDrawCount = 0;

    PlagueFixture() {
        REQUIRE(layout.load(test::assetOrSkip("LEVELS/LEVELK5/WORLDS.PS2").parent_path()));
        REQUIRE(items.load(test::assetOrSkip("ITEMS/LEVELK5/objects.ngc").parent_path()));
        REQUIRE(boss.load(test::assetOrSkip("MONSTERS/PBOSS/objects.ngc").parent_path()));
        REQUIRE(rocks.bind(device, layout, items));
        rocks.setPlayerCount(1);
        const std::array lenders{&boss.textures};
        rocks.bindAnimations(device, items, lenders);
        const auto texture = boss.textures.find("BLOB_BOSS00");
        REQUIRE(texture);
        first = *texture;
        REQUIRE(rocks.size() == 3);
        const auto state = captured(owner);
        REQUIRE(state.resolvedAnimations == 1);
        REQUIRE(state.animations.size() == 1);
        REQUIRE(state.animations[0].frames == 15);
        REQUIRE(state.animations[0].rate == 3);
        REQUIRE(state.animations[0].texture >= 0);
        const auto animatedSlot = static_cast<u32>(state.animations[0].texture);
        // A native rock can contain several material parts. Count the authored
        // parts and the animated slot rather than assuming one draw per rock.
        for (const auto& rock : state.rocks) {
            std::optional<u32> model;
            const auto base = rock.definition.name + std::to_string(rock.tier);
            for (const auto* suffix : {"", "L1", "L1ROOT"}) {
                model = items.models.find(base + suffix);
                if (model) {
                    break;
                }
            }
            REQUIRE(model);
            const auto& parts = items.models.mesh(*model).parts;
            REQUIRE_FALSE(parts.empty());
            drawCount += parts.size();
            animatedDrawCount +=
                static_cast<usize>(std::ranges::count(parts, animatedSlot, &MeshPart::texture));
        }
        REQUIRE(animatedDrawCount > 0);
    }
    void drawFrame(u32 frame, f32 alpha) {
        device.draws.clear();
        rocks.draw(device, Mat4{1}, {}, alpha);
        const Texture* expected = &boss.textures.texture(device, first + frame);
        REQUIRE(device.draws.size() == drawCount);
        CHECK(static_cast<usize>(std::ranges::count(
                  device.draws, expected, &test::RecordedDraw::texture)) == animatedDrawCount);
    }
};

TEST_CASE(
    "safe rock checkpoints preserve borrowed boss texture clocks across corrections and holds",
    "[safe-rocks-checkpoint][assets]") {
    PlagueFixture host;
    PlagueFixture guest;
    const auto uploads = guest.device.texturesCreated;
    const auto updates = guest.device.textureUpdates;
    std::array<bool, 15> seen{};
    for (s32 tick = 0; tick < 190; ++tick) {
        CAPTURE(tick);
        host.rocks.update(1.0f / 60);
        if (tick % 11 == 0) {
            host.rocks.capturePresentation();
        }
        const auto state = captured(host.owner);
        REQUIRE(guest.owner.restore(state));
        CHECK(captured(guest.owner) == state);
        const u32 frame = (state.textures.frame / 3) % 15;
        seen[frame] = true;
        host.drawFrame(frame, -1);
        guest.drawFrame(frame, -1);
        // Independent archives have distinct texture pointers, but resolve the
        // same exact borrowed native image. Comparing pixels verifies the lender.
        for (usize index = 0; index < host.device.draws.size(); ++index) {
            const auto& a =
                dynamic_cast<const test::FakeTexture&>(*host.device.draws[index].texture);
            const auto& b =
                dynamic_cast<const test::FakeTexture&>(*guest.device.draws[index].texture);
            CHECK((a.pixels == b.pixels));
            CHECK(host.device.draws[index].vertices == guest.device.draws[index].vertices);
        }
        auto undo = guest.owner.prepareRestore(state);
        REQUIRE(undo);
        guest.rocks.update(0.071f);
        guest.rocks.hideForEruptions();
        REQUIRE(guest.owner.commitRestore(std::move(*undo)));
        CHECK(captured(guest.owner) == state);
        guest.drawFrame(frame, -1);
        if (state.textures.advance == 0) {
            for (const f32 alpha : {0.0f, 0.5f, 1.0f}) {
                guest.drawFrame(frame, alpha);
            }
        }
    }
    CHECK(std::ranges::all_of(seen, [](bool present) { return present; }));
    CHECK(guest.device.texturesCreated == uploads);
    CHECK(guest.device.textureUpdates == updates);
}
} // namespace
