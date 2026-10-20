#include <array>
#include <limits>
#include <memory>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/CrittersCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;

CrittersCheckpoint::State capture(const CrittersCheckpoint& owner) {
    auto state = owner.checkpoint();
    REQUIRE(state);
    return std::move(*state);
}
struct Population {
    test::FakeRenderDevice device;
    Critters critters;
    std::unique_ptr<CrittersCheckpoint> checkpoint;
    explicit Population(bool reverse = false) {
        const auto root = test::assetOrSkip("CRITTER/GOLEM.WAD").parent_path().parent_path();
        critters.open(device, root, nullptr, {}, 'G');
        critters.setLookouts({{{-20, 0, 0}, {-20, 0, 20}}, {1, 65535}});
        const std::array kinds{CombatantKind::Golem, CombatantKind::General,
                               CombatantKind::Gargoyle};
        for (usize i = 0; i < kinds.size(); ++i) {
            REQUIRE(critters.archiveFor(kinds[reverse ? kinds.size() - 1 - i : i]) != nullptr);
        }
        checkpoint = std::make_unique<CrittersCheckpoint>(critters);
    }
    void spawn() {
        REQUIRE(critters.spawnGolem({0, 0, 0}, 0) == 0);
        REQUIRE(critters.spawnGeneral({-20, 0, 0}, 0, 4) == 1);
        REQUIRE(critters.spawnGargoyle({20, 0, 0}, 0) == 2);
    }
};
std::array<EnemyView, 2> views() {
    std::array<EnemyView, 2> result;
    result[0].player = 3;
    result[0].position = {0, 0, 10};
    result[0].radius = 1;
    result[0].height = 6;
    result[1].player = 1;
    result[1].position = {20, 0, 15};
    result[1].radius = 1;
    result[1].height = 6;
    return result;
}

TEST_CASE(
    "empty critter population captures exact slots and atomically rejects a malformed last slot",
    "[critters-checkpoint]") {
    Critters native;
    CrittersCheckpoint owner(native);
    const auto baseline = capture(owner);
    REQUIRE(baseline.valid());
    auto bad = baseline;
    bad.actors.back().parts.front().arenaRandom = 0;
    CHECK_FALSE(bad.valid());
    CHECK_FALSE(owner.restore(bad));
    CHECK(capture(owner) == baseline);
    bad = baseline;
    bad.events.blows.push_back({0, 0, std::numeric_limits<f32>::quiet_NaN()});
    CHECK_FALSE(bad.valid());
    CHECK_FALSE(owner.restore(bad));
    CHECK(capture(owner) == baseline);
    auto first = owner.prepareRestore(baseline);
    auto sibling = owner.prepareRestore(baseline);
    REQUIRE(first);
    REQUIRE(sibling);
    REQUIRE(owner.commitRestore(std::move(*first)));
    CHECK_FALSE(owner.canCommit(*sibling));
    CHECK_FALSE(owner.commitRestore(std::move(*first)));
    auto stale = owner.prepareRestore(baseline);
    REQUIRE(stale);
    native.close();
    CHECK_FALSE(owner.canCommit(*stale));
    CHECK_FALSE(owner.checkpoint());
}

TEST_CASE("critter capture retains exact actor links and validates incoming bank and slot data",
          "[critters-checkpoint][checkpoint-capture-validation][assets]") {
    auto fixture = std::make_unique<Population>();
    fixture->spawn();
    const auto players = views();
    for (s32 tick = 0; tick < 12; ++tick) {
        fixture->critters.update(1, 1.0f / 60, players);
        const auto captured = capture(*fixture->checkpoint);
        const CrittersCheckpoint independent{fixture->critters};
        REQUIRE(captured.valid());
        CHECK(capture(independent) == captured);
    }
    const auto baseline = capture(*fixture->checkpoint);
    for (s32 mutation = 0; mutation < 5; ++mutation) {
        CAPTURE(mutation);
        auto invalid = baseline;
        switch (mutation) {
        case 0: invalid.actors.back().parts.front().arenaRandom = 0; break;
        case 1: invalid.actors.front().parts.front().id = 1; break;
        case 2:
            invalid.banks.front().clock.remainder = std::numeric_limits<f32>::quiet_NaN();
            break;
        case 3:
            invalid.events.blows.push_back({0, 0, std::numeric_limits<f32>::quiet_NaN()});
            break;
        case 4: invalid.actors.front().lookouts.reset(); break;
        default:
            break;
        }
        CHECK_FALSE(invalid.valid());
        CHECK_FALSE(fixture->checkpoint->prepareRestore(invalid));
        CHECK(capture(*fixture->checkpoint) == baseline);
    }
}

TEST_CASE("native mixed critter pools resume every slot queue and shared texture bank",
          "[critters-checkpoint][assets]") {
    const s32 rate = GENERATE(30, 60);
    auto first = std::make_unique<Population>();
    auto replay = std::make_unique<Population>(true);
    first->spawn();
    auto players = views();
    for (s32 frame = 0; frame < 30; ++frame) {
        first->critters.update(60 / rate, 1.0f / static_cast<f32>(rate), players);
    }
    EnemyHit hit;
    hit.player = 3;
    hit.damage = 11;
    hit.flags = 0x10;
    hit.level = 30;
    hit.close = true;
    first->critters.hurt(0, hit);
    first->critters.blind(2, 7);
    first->critters.freeze(0, 3);
    const auto baseline = capture(*first->checkpoint);
    REQUIRE_FALSE(baseline.events.losses.empty());
    REQUIRE(replay->checkpoint->restore(baseline));
    REQUIRE(replay->critters.count() == 3);
    const u32 uploads = replay->device.texturesCreated;
    for (s32 frame = 0; frame < 120; ++frame) {
        CAPTURE(rate, frame);
        players[0].position.x = frame > 40 ? 4.0f : 0.0f;
        const bool stop = frame >= 25 && frame < 40;
        const s32 ticks = frame == 45 ? 0 : 60 / rate;
        first->critters.update(ticks, 1.0f / static_cast<f32>(rate), players, stop);
        replay->critters.update(ticks, 1.0f / static_cast<f32>(rate), players, stop);
        CHECK(capture(*replay->checkpoint) == capture(*first->checkpoint));
    }
    CHECK(replay->device.texturesCreated == uploads);
    CHECK(replay->device.textureUpdates == 0);
    CHECK(replay->device.draws.empty());
}

TEST_CASE("critter pool rewind covers newly spawned slots damage queues and bank clocks together",
          "[critters-checkpoint][assets]") {
    auto fixture = std::make_unique<Population>();
    fixture->spawn();
    const auto before = capture(*fixture->checkpoint);
    auto undo = fixture->checkpoint->prepareRestore(before);
    REQUIRE(undo);
    REQUIRE(fixture->critters.spawnGeneral({-5, 0, 3}, 0) == 3);
    EnemyHit hit;
    hit.player = 1;
    hit.damage = 12;
    hit.level = 30;
    hit.close = true;
    fixture->critters.hurt(0, hit);
    fixture->critters.update(2, 1.0f / 30, views());
    REQUIRE(capture(*fixture->checkpoint) != before);
    REQUIRE(fixture->checkpoint->canCommit(*undo));
    REQUIRE(fixture->checkpoint->commitRestore(std::move(*undo)));
    CHECK(capture(*fixture->checkpoint) == before);
    CHECK(fixture->critters.count() == 3);
    CHECK(fixture->critters.takeLosses().empty());
}

TEST_CASE(
    "critter prepared population rejects foreign catalogs routes late banks and closed owners",
    "[critters-checkpoint][assets]") {
    auto fixture = std::make_unique<Population>();
    fixture->spawn();
    const auto before = capture(*fixture->checkpoint);
    auto bad = before;
    bad.banks.back().clock.remainder = std::numeric_limits<f32>::quiet_NaN();
    CHECK_FALSE(fixture->checkpoint->restore(bad));
    CHECK(capture(*fixture->checkpoint) == before);
    bad = before;
    bad.banks.front().name = "OTHER";
    CHECK_FALSE(fixture->checkpoint->restore(bad));
    CHECK(capture(*fixture->checkpoint) == before);
    bad = before;
    bad.actors.back().parts.front().actor.definition = {9999, 0};
    CHECK_FALSE(fixture->checkpoint->restore(bad));
    CHECK(capture(*fixture->checkpoint) == before);
    auto undo = fixture->checkpoint->prepareRestore(before);
    REQUIRE(undo);
    fixture->critters.setLookouts({{{5, 0, 5}}, {-1}});
    CHECK_FALSE(fixture->checkpoint->canCommit(*undo));
    fixture->critters.setLookouts(before.lookouts);
    // Definitions equal again, but resource teardown must never revive a token.
    fixture->critters.close();
    CHECK_FALSE(fixture->checkpoint->canCommit(*undo));
    CHECK_FALSE(fixture->checkpoint->restore(before));
}
} // namespace
